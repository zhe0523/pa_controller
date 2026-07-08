#include "command_handler.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "log.h"
#include "pa_pu.h"
#include "template_builder.h"

static bool cmd_is(const char* command, const char* expected) {
  return strcmp(command, expected) == 0;
}

static void write_status_response(char* response, size_t response_size) {
  pa_pu_status_t status;
  pa_pu_read_status(&status);

  /* STATUS 响应保持短格式，方便串口助手查看，也方便上位机按字段解析。 */
  snprintf(response, response_size,
           "OK STATUS int=0x%08x pa=0x%08x com=0x%08x rst=0x%08x wr_state=%u wr_end=%u corr_state=%u corr_end=%u\r\n",
           status.int_vector,
           status.pa_version,
           status.pa_pu_com_version,
           status.rst_init_state,
           status.img_wr_state,
           status.img_wr_end,
           status.img_corr_state,
           status.img_corr_end);
}

int command_handle(command_context_t* ctx, const char* command, char* response, size_t response_size) {
  if (ctx == NULL || command == NULL || response == NULL || response_size == 0) {
    return -1;
  }

  response[0] = '\0';

  /*
   * 当前使用可读性高的 ASCII 行协议，便于串口助手调试。
   * 后续上位机协议确定后，可以只替换本文件，不影响 PA/模板底层模块。
   */
  if (cmd_is(command, "PING")) {
    /* 心跳命令：只验证通信链路和应用主循环是否正常。 */
    snprintf(response, response_size, "OK PONG\r\n");
    return 0;
  }

  if (cmd_is(command, "STATUS")) {
    /* 读取 PA/FPGA 当前状态，用于上位机刷新状态栏或调试。 */
    write_status_response(response, response_size);
    return 0;
  }

  if (cmd_is(command, "WAIT_IRQ")) {
    /*
     * 阻塞等待一次 FPGA->ARM 中断。
     * 当前超时时间固定 5 秒，后续正式协议可扩展成 WAIT_IRQ <timeout_ms>。
     */
    uint32_t irq_count = 0;
    int ret = pa_pu_wait_irq(5000, &irq_count);
    if (ret > 0) {
      pa_pu_status_t status;
      pa_pu_read_status(&status);
      snprintf(response, response_size, "OK IRQ count=%u int=0x%08x\r\n", irq_count, status.int_vector);
    } else if (ret == 0) {
      snprintf(response, response_size, "ERR IRQ_TIMEOUT\r\n");
    } else if (ret == -2) {
      snprintf(response, response_size, "ERR IRQ_NOT_UIO\r\n");
    } else {
      snprintf(response, response_size, "ERR IRQ_WAIT\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "LOAD_TEMPLATE")) {
    /* 从文件系统加载 offset/gain 模板，适合设备重启后恢复已有校正模板。 */
    if (template_load_files(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK LOAD_TEMPLATE\r\n");
    } else {
      snprintf(response, response_size, "ERR LOAD_TEMPLATE\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_OFFSET")) {
    /* 用当前暗场图像生成 offset 模板；上位机应先确保当前帧是有效暗场。 */
    if (template_make_offset(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK MAKE_OFFSET\r\n");
    } else {
      snprintf(response, response_size, "ERR MAKE_OFFSET\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_GAIN")) {
    /* 用当前亮场图像和已有 offset 模板生成 gain 模板；需先执行或加载 offset。 */
    if (template_make_gain(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK MAKE_GAIN\r\n");
    } else {
      snprintf(response, response_size, "ERR MAKE_GAIN\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "CONFIG_TEMPLATE")) {
    /* 只重新下发模板地址/尺寸配置，不重新生成或加载模板内容。 */
    pa_pu_configure_templates();
    snprintf(response, response_size, "OK CONFIG_TEMPLATE\r\n");
    return 0;
  }

  if (cmd_is(command, "START_CORR")) {
    /* 启动 FPGA 图像校正，完成后应通过 STATUS 或 WAIT_IRQ 确认。 */
    pa_pu_start_correction();
    snprintf(response, response_size, "OK START_CORR\r\n");
    return 0;
  }

  if (cmd_is(command, "SEND_IMAGE")) {
    /*
     * 图像数据不经过 ARM 发送。这里仅通知 PA 端从 FPGA 图像物理地址启动写图流程，
     * 光口传输由 PA/FPGA 逻辑完成。
     */
    pa_pu_start_image_write(FPGA_IMAGE_PTR);
    snprintf(response, response_size, "OK SEND_IMAGE addr=0x%08x\r\n", FPGA_IMAGE_PTR);
    return 0;
  }

  if (cmd_is(command, "QUIT")) {
    /* 只退出 ARM 应用，不复位 FPGA/PA。 */
    ctx->should_quit = true;
    snprintf(response, response_size, "OK QUIT\r\n");
    return 0;
  }

  log_warn("unknown command: %s", command);
  snprintf(response, response_size, "ERR UNKNOWN\r\n");
  return 0;
}
