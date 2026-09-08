#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_config.h"
#include "calibration_builder.h"
#include "command_handler.h"
#include "config_store.h"
#include "fpga_mem.h"
#include "image_frame.h"
#include "log.h"
#include "pa_pu.h"
#include "pa_protocol.h"
#include "rs422.h"
#include "template_builder.h"
#include "work_mode.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  /* 信号处理函数只置标志位，实际资源释放放在 main 主流程中完成。 */
  g_stop = 1;
}

static void print_usage(const char* program) {
  fprintf(stderr, "Usage: %s [--stdio|--binary] [-d rs422_device] [-b baud]\n", program);
  fprintf(stderr, "  --stdio: read commands from stdin and write responses to stdout\n");
  fprintf(stderr, "  --binary: use formal binary protocol on RS422\n");
  fprintf(stderr, "  default device: %s\n", RS422_DEVICE);
  fprintf(stderr, "  default baud:   %d\n", RS422_BAUD);
}

static void install_signal_handlers(void) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);

  /*
   * 不设置 SA_RESTART。这样 fgets/read/poll/usleep 被 Ctrl+C 打断后会返回 EINTR，
   * 主循环和硬件等待函数才能尽快看到退出条件并释放资源。
   */
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);
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

static const char* default_work_mode_name(uint32_t mode) {
  switch (mode) {
    case WORK_MODE_IDLE:
      return "Idle";
    case WORK_MODE_AED:
      return "AED";
    case WORK_MODE_SYNC_OUT:
      return "SyncOut";
    case WORK_MODE_SYNC_IN:
      return "SyncIn";
    case WORK_MODE_PREP:
      return "Prep";
    case WORK_MODE_CONTINUOUS:
      return "Continuous";
    case WORK_MODE_INNER:
      return "Inner";
    case WORK_MODE_FREE_SYNC:
      return "FreeSync";
    case WORK_MODE_DDR:
      return "DDR";
    default:
      return "Unknown";
  }
}

typedef struct {
  command_context_t* command_context;
  rs422_t* port;
} binary_loop_context_t;

static int handle_binary_frame(const pa_protocol_frame_view_t* request, void* user) {
  binary_loop_context_t* loop = (binary_loop_context_t*)user;
  if (loop == NULL || loop->command_context == NULL || loop->port == NULL) {
    return -1;
  }

  uint8_t response[PA_PROTOCOL_MAX_FRAME];
  size_t response_length = 0u;
  int result = command_handle_binary(loop->command_context,
                                     request,
                                     response,
                                     sizeof(response),
                                     &response_length);
  if (result != 0 || response_length == 0u) {
    log_error("binary command failed cmd=0x%04x seq=%u result=%d",
              request->cmd, request->seq, result);
    return -1;
  }
  if (rs422_write_bytes(loop->port, response, response_length) != 0) {
    log_error("binary response write failed cmd=0x%04x seq=%u",
              request->cmd, request->seq);
    return -1;
  }
  log_info("binary request cmd=0x%04x seq=%u response=%u bytes",
           request->cmd, request->seq, (unsigned)response_length);
  return 0;
}

int main(int argc, char* argv[]) {
  /* 支持 Ctrl+C / kill 优雅退出，避免 mmap 和串口 fd 泄漏。 */
  install_signal_handlers();

  const char* rs422_device = RS422_DEVICE;
  int rs422_baud = RS422_BAUD;
  bool use_stdio = false;
  bool use_binary = false;

  /* 解析运行参数：--stdio 用于开发板无独立 422 口时模拟上位机命令。 */
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--stdio") == 0) {
      use_stdio = true;
    } else if (strcmp(argv[i], "--binary") == 0) {
      use_binary = true;
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

  log_info("pa_controller start app_version=%s build_time=%s", APP_VERSION, APP_BUILD_TIME);
  log_info("work mode default auto_start=%u default_mode=%u/%s",
           (unsigned)WORK_MODE_AUTO_START,
           (unsigned)WORK_MODE_DEFAULT_MODE,
           default_work_mode_name(WORK_MODE_DEFAULT_MODE));
  log_info("image=%ux%u active=%ux%u offset=(%u,%u)",
           DEVICE_WIDTH, DEVICE_HEIGHT, IMAGE_WIDTH, IMAGE_HEIGHT, COL_OFFSET, ROW_OFFSET);
  log_info("fiber image header=%u bytes", DETECTOR_IMAGE_HEADER_BYTES);

  fpga_mem_t fpga_mem;
  memset(&fpga_mem, 0, sizeof(fpga_mem));

  /* 映射 FPGA 图像/模板内存。模板生成和光口传图都依赖这块共享内存。 */
  if (fpga_mem_open(&fpga_mem) != 0) {
    return 1;
  }

  /* 映射 PA 控制寄存器。当前开发板阶段通过 /dev/mem 访问 AXI-lite 地址空间。 */
  if (pa_pu_open(PA_PU_BASE_ADDR, PA_PU_MAP_SIZE) != 0) {
    fpga_mem_close(&fpga_mem);
    return 1;
  }

  if (use_stdio && use_binary) {
    fprintf(stderr, "--stdio and --binary cannot be used together\n");
    return 1;
  }

  pa_runtime_config_t runtime_config;
  int config_result = config_store_load_or_create(APP_CONFIG_FILE, &fpga_mem, &runtime_config);
  if (config_result < 0) {
    log_error("runtime config unavailable path=%s", APP_CONFIG_FILE);
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }
  char config_summary[256];
  if (config_store_summary(&runtime_config, config_summary, sizeof(config_summary)) == 0) {
    log_info("runtime config path=%s source=%s %s", APP_CONFIG_FILE,
             config_result == 1 ? "defaults-created" : "file", config_summary);
  }

  /* 开机先把配置文件描述的基础寄存器状态下发一次。 */
  pa_pu_configure_gic(&runtime_config.gic);
  pa_pu_configure_roic(&runtime_config.roic);
  pa_pu_configure_correction(&runtime_config.corr);

  /*
   * 启动时尝试加载磁盘上的 offset/gain 模板。
   * 文件不存在不阻断启动，上位机可通过 MAKE_OFFSET / MAKE_GAIN 现场生成。
   */
  if (template_load_files_from_paths(&fpga_mem,
                                     runtime_config.offset_file,
                                     runtime_config.gain_file) != 0) {
    log_warn("template files not fully loaded");
  }

  /*
   * 工作模式上下文始终初始化，方便通过 START_WORK/CONFIG_STATIC_IDLE 手动进入。
   * 是否开机自动启动由 WORK_MODE_AUTO_START/WORK_MODE_DEFAULT_MODE 控制。
   */
  work_mode_context_t work_mode;
  if (work_mode_init(&work_mode, &fpga_mem) != 0) {
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }
  dynamic_mode_config_t runtime_dynamic_config;
  if (dynamic_mode_default_config(&fpga_mem, &runtime_dynamic_config) != 0) {
    log_error("failed to build dynamic runtime config");
    work_mode_stop(&work_mode);
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }
  runtime_dynamic_config.dync = runtime_config.dync;
  runtime_dynamic_config.start_timeout_ms = runtime_config.dynamic_start_timeout_ms;
  runtime_dynamic_config.state_poll_interval_ms = runtime_config.dynamic_state_poll_interval_ms;
  runtime_dynamic_config.stop_timeout_ms = runtime_config.dynamic_stop_timeout_ms;
  if (work_mode_update_static_idle_config(&work_mode, &runtime_config.static_idle) != 0 ||
      work_mode_update_dynamic_settings(&work_mode, &runtime_dynamic_config) != 0) {
    log_error("runtime work mode config rejected");
    work_mode_stop(&work_mode);
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }
  if (WORK_MODE_AUTO_START != 0) {
    if (WORK_MODE_DEFAULT_MODE != WORK_MODE_IDLE &&
        WORK_MODE_DEFAULT_MODE != WORK_MODE_CONTINUOUS) {
      log_error("default work mode not implemented mode=%u/%s",
                (unsigned)WORK_MODE_DEFAULT_MODE,
                default_work_mode_name(WORK_MODE_DEFAULT_MODE));
      pa_pu_close();
      fpga_mem_close(&fpga_mem);
      return 1;
    }
    int start_ret = WORK_MODE_DEFAULT_MODE == WORK_MODE_CONTINUOUS
        ? work_mode_start_dynamic(&work_mode)
        : work_mode_start(&work_mode);
    if (start_ret != 0) {
      log_error("work mode thread start failed");
      pa_pu_close();
      fpga_mem_close(&fpga_mem);
      return 1;
    }
  } else {
    log_info("work mode auto start disabled, use START_WORK for Static Idle or START_CONTINUOUS for Dynamic");
  }

  command_context_t ctx = {
    .fpga_mem = &fpga_mem,
    .work_mode = &work_mode,
    .runtime_config = &runtime_config,
    .config_file = APP_CONFIG_FILE,
    .should_quit = false,
  };

  char command[1024];
  char response[2048];

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

    if (g_stop) {
      log_info("stop signal received");
    }

    calibration_task_shutdown();
    work_mode_stop(&work_mode);
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    log_info("pa_controller stop");
    log_shutdown();
    return 0;
  }

  rs422_t rs422;
  /* 串口模式：正式上位机通过 RS422 发来的每一行命令都会进入同一个 command_handle。 */
  rs422_device = select_rs422_device(rs422_device);
  if (rs422_open(&rs422, rs422_device, rs422_baud) != 0) {
    log_error("no rs422 device opened, use -d /dev/ttySx to specify the 422 uart");
    calibration_task_shutdown();
    work_mode_stop(&work_mode);
    pa_pu_close();
    fpga_mem_close(&fpga_mem);
    return 1;
  }

  if (use_binary) {
    log_info("binary rs422 protocol mode enabled");
    pa_protocol_parser_t parser;
    pa_protocol_parser_init(&parser);
    binary_loop_context_t loop = {
      .command_context = &ctx,
      .port = &rs422,
    };
    uint8_t input[512];
    while (!g_stop && !ctx.should_quit) {
      int n = rs422_read_bytes(&rs422, input, sizeof(input));
      if (n < 0) {
        log_error("binary rs422 read failed: %d", errno);
        break;
      }
      if (n == 0) {
        continue;
      }
      if (pa_protocol_parser_feed(&parser,
                                  input,
                                  (size_t)n,
                                  handle_binary_frame,
                                  &loop) != PA_PROTOCOL_OK) {
        log_error("binary protocol parser stopped");
        break;
      }
    }
  } else while (!g_stop && !ctx.should_quit) {
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

  if (g_stop) {
    log_info("stop signal received");
  }

  rs422_close(&rs422);
  calibration_task_shutdown();
  work_mode_stop(&work_mode);
  pa_pu_close();
  fpga_mem_close(&fpga_mem);
  log_info("pa_controller stop");
  log_shutdown();
  return 0;
}
