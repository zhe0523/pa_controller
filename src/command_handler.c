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
           "OK STATUS int_vector=0x%08x pa_version=0x%08x com_version=0x%08x rst_state=0x%08x wr_state=%u wr_end=%u corr_state=%u corr_end=%u gic_state=%u gic_end=%u gic_dfx=0x%02x roic_state=%u roic_end=%u roic_dfx=0x%02x\r\n",
           status.int_vector,
           status.pa_version,
           status.pa_pu_com_version,
           status.rst_init_state,
           status.img_wr_state,
           status.img_wr_end,
           status.img_corr_state,
           status.img_corr_end,
           status.gic_state,
           status.gic_end,
           status.gic_dfx,
           status.roic_state,
           status.roic_end,
           status.roic_dfx);
}

static void write_no_hw_status_response(char* response, size_t response_size) {
  snprintf(response, response_size,
           "OK STATUS int_vector=0x00000000 pa_version=0x00000000 com_version=0x00000000 rst_state=0x00000000 wr_state=0 wr_end=0 corr_state=0 corr_end=0 gic_state=0 gic_end=0 gic_dfx=0x00 roic_state=0 roic_end=0 roic_dfx=0x00\r\n");
}

static bool reject_no_hw(const command_context_t* ctx, char* response, size_t response_size) {
  if (ctx != NULL && !ctx->hardware_enabled) {
    snprintf(response, response_size, "ERR NO_HW\r\n");
    return true;
  }
  return false;
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
    if (ctx->hardware_enabled) {
      write_status_response(response, response_size);
    } else {
      write_no_hw_status_response(response, response_size);
    }
    return 0;
  }

  if (cmd_is(command, "LOAD_TEMPLATE")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 只重新下发模板地址/尺寸配置，不重新生成或加载模板内容。 */
    pa_pu_configure_templates();
    snprintf(response, response_size, "OK CONFIG_TEMPLATE\r\n");
    return 0;
  }

  if (cmd_is(command, "CONFIG_GIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /*
     * 下发 GIC 默认时序、行范围和 binning 配置。
     * 默认值来自 app_config.h，可在 make 命令中用 GIC_DEFAULT_* 覆盖。
     */
    pa_pu_configure_gic_defaults();
    snprintf(response, response_size, "OK CONFIG_GIC\r\n");
    return 0;
  }

  if (cmd_is(command, "START_GIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动一次 GIC 操作，完成后通过 STATUS 的 gic_state/gic_end/gic_dfx 确认。 */
    pa_pu_start_gic();
    snprintf(response, response_size, "OK START_GIC\r\n");
    return 0;
  }

  if (cmd_is(command, "STOP_GIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 主要用于 xao scan 模式；其它模式下是否有效由 FPGA 决定。 */
    pa_pu_stop_gic();
    snprintf(response, response_size, "OK STOP_GIC\r\n");
    return 0;
  }

  if (cmd_is(command, "CONFIG_ROIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /*
     * 下发 ROIC 芯片寄存器默认值和列范围配置，但不立即启动。
     * ROIC_DEFAULT_REG_* 现在是占位值，真板联调前应按 panel 参数覆盖。
     */
    pa_pu_configure_roic_defaults();
    snprintf(response, response_size, "OK CONFIG_ROIC\r\n");
    return 0;
  }

  if (cmd_is(command, "START_ROIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动一次 ROIC 配置操作，完成后通过 STATUS 的 roic_state/roic_end/roic_dfx 确认。 */
    pa_pu_start_roic();
    snprintf(response, response_size, "OK START_ROIC\r\n");
    return 0;
  }

  if (cmd_is(command, "START_CORR")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动 FPGA 图像校正，完成后应通过 STATUS 确认。 */
    pa_pu_start_correction();
    snprintf(response, response_size, "OK START_CORR\r\n");
    return 0;
  }

  if (cmd_is(command, "SEND_IMAGE") || cmd_is(command, "SEND_SINGLE") || cmd_is(command, "START_CONTINUOUS")) {
    /*
     * 图像数据不经过 ARM 发送。这里仅通知 PA 端从 FPGA 图像物理地址启动写图流程，
     * 光口传输由 PA/FPGA 逻辑完成。
     *
     * SEND_IMAGE 是早期调试命令；SEND_SINGLE / START_CONTINUOUS 是当前 Qt 上位机
     * 的客户入口命令。当前 FPGA 侧尚未区分单帧和持续上图，因此两者暂时都触发
     * 同一次写图流程，后续硬件支持持续模式后只需要在这里拆分实现。
     */
    if (ctx->hardware_enabled) {
      pa_pu_start_image_write(FPGA_IMAGE_PTR);
    }
    snprintf(response, response_size, "OK %s addr=0x%08x\r\n", command, FPGA_IMAGE_PTR);
    return 0;
  }

  if (cmd_is(command, "STOP_TRANSFER")) {
    /*
     * 当前 PA/FPGA 暂未提供明确的停流寄存器。先兼容上位机按钮流程：
     * ARM 确认收到停止请求，但不额外操作硬件。
     */
    snprintf(response, response_size, "OK STOP_TRANSFER\r\n");
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
