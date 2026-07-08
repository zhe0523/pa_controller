#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "command_handler.h"
#include "fpga_mem.h"
#include "image_frame.h"
#include "log.h"
#include "pa_pu.h"
#include "rs422.h"
#include "template_builder.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  /* 信号处理函数只置标志位，实际资源释放放在 main 主流程中完成。 */
  g_stop = 1;
}

static void print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [--stdio] [-d rs422_device] [-b baud]\n", program);
  fprintf(stderr, "  --stdio: read commands from stdin and write responses to stdout\n");
  fprintf(stderr, "  default device: %s\n", RS422_DEVICE);
  fprintf(stderr, "  default baud:   %d\n", RS422_BAUD);
}

static const char* select_rs422_device(const char* requested) {
  if (requested != NULL && access(requested, R_OK | W_OK) == 0) {
    return requested;
  }

  /*
   * 目标板没有 ttyPS*，常见 UART 设备是 ttyS0~ttyS3。
   * 这里优先跳过常被 console 占用的 ttyS0，先尝试 ttyS1/2/3。
   */
  static const char* candidates[] = {
    "/dev/ttyS1",
    "/dev/ttyS2",
    "/dev/ttyS3",
    "/dev/ttyS0",
  };

  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    if (access(candidates[i], R_OK | W_OK) == 0) {
      log_warn("rs422 device %s not found, auto selected %s", requested, candidates[i]);
      return candidates[i];
    }
  }

  return requested;
}

int main(int argc, char* argv[]) {
  /* 支持 Ctrl+C / kill 优雅退出，避免 mmap 和串口 fd 泄漏。 */
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  const char* rs422_device = RS422_DEVICE;
  int rs422_baud = RS422_BAUD;
  bool use_stdio = false;

  /* 解析运行参数：--stdio 用于开发板无独立 422 口时模拟上位机命令。 */
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--stdio") == 0) {
      use_stdio = true;
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      rs422_device = argv[++i];
    } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      rs422_baud = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    } else {
      print_usage(argv[0]);
      return 1;
    }
  }

  log_info("pa_controller start");
  log_info("image=%ux%u active=%ux%u offset=(%u,%u)",
           DEVICE_WIDTH, DEVICE_HEIGHT, IMAGE_WIDTH, IMAGE_HEIGHT, COL_OFFSET, ROW_OFFSET);
  log_info("fiber image header=%u bytes", DETECTOR_IMAGE_HEADER_BYTES);

  fpga_mem_t fpga_mem;
  /* 映射 FPGA 图像/模板内存。模板生成和光口传图都依赖这块共享内存。 */
  if (fpga_mem_open(&fpga_mem) != 0) {
    return 1;
  }

  /* 映射 PA 控制寄存器。当前开发板阶段可能只是占位映射，真板需确认 UIO 节点。 */
  if (pa_pu_open(PA_PU_BASE_ADDR, PA_PU_MAP_SIZE) != 0) {
    fpga_mem_close(&fpga_mem);
    return 1;
  }

  /*
   * 启动时尝试加载磁盘上的 offset/gain 模板。
   * 文件不存在不阻断启动，上位机可通过 MAKE_OFFSET / MAKE_GAIN 现场生成。
   */
  if (template_load_files(&fpga_mem) == 0) {
    pa_pu_configure_templates();
  } else {
    log_warn("template files not fully loaded");
  }

  command_context_t ctx = {
    .fpga_mem = &fpga_mem,
    .should_quit = false,
  };

  char command[256];
  char response[512];

  if (use_stdio) {
    /*
     * 开发板阶段没有独立 RS422 口时，用标准输入输出测试同一套命令处理逻辑。
     * 日志仍然输出到 stderr，stdout 只输出协议响应，便于后续脚本化测试。
     */
    log_info("stdio command mode enabled");
    while (!g_stop && !ctx.should_quit && fgets(command, sizeof(command), stdin) != NULL) {
      /* fgets 会保留换行符，命令分发前先统一去掉 CR/LF。 */
      command[strcspn(command, "\r\n")] = '\0';
      if (command[0] == '\0') {
        continue;
      }

      log_info("rx: %s", command);
      if (command_handle(&ctx, command, response, sizeof(response)) == 0 && response[0] != '\0') {
        fputs(response, stdout);
        fflush(stdout);
        log_info("tx: %s", response);
      }
    }

    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    log_info("pa_controller stop");
    return 0;
  }

  rs422_t rs422;
  /* 串口模式：正式上位机通过 RS422 发来的每一行命令都会进入同一个 command_handle。 */
  rs422_device = select_rs422_device(rs422_device);
  if (rs422_open(&rs422, rs422_device, rs422_baud) != 0) {
    log_error("no rs422 device opened, use -d /dev/ttySx to specify the 422 uart");
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }

  while (!g_stop && !ctx.should_quit) {
    int n = rs422_read_line(&rs422, command, sizeof(command));
    if (n < 0) {
      log_error("rs422 read failed: %d", errno);
      break;
    }
    if (n == 0) {
      /* VMIN=0/VTIME>0 时 read 可能超时返回 0，这不是错误。 */
      continue;
    }

    log_info("rx: %s", command);
    if (command_handle(&ctx, command, response, sizeof(response)) == 0 && response[0] != '\0') {
      rs422_write_text(&rs422, response);
      log_info("tx: %s", response);
    }
  }

  rs422_close(&rs422);
  pa_pu_close();
  fpga_mem_close(&fpga_mem);
  log_info("pa_controller stop");
  return 0;
}
