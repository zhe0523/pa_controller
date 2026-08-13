#include "pa_pu.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

/* 当前 PA 寄存器映射的文件描述符：可能是 /dev/uioX，也可能是 /dev/mem。 */
static int g_fd = -1;

/* PA 寄存器映射后的用户态虚拟基址。volatile 防止编译器合并 MMIO 访问。 */
static volatile uint8_t* g_base = NULL;

/* 映射窗口长度，用于寄存器越界检查和 munmap。 */
static size_t g_map_size = 0;

/* 记录当前是否通过 UIO 打开。当前项目暂时不用 UIO fd 中断，只用寄存器 INT_VECTOR。 */
static bool g_using_uio = false;

/* /dev/pa_irq 驱动 fd。存在时由驱动负责读取 read-clear 的 INT_VECTOR。 */
static int g_irq_fd = -1;

typedef struct {
  uint32_t int_vector;
  uint32_t reserved;
  uint64_t count;
  uint64_t timestamp_ns;
} pa_irq_event_t;

static int pa_pu_open_uio(size_t map_size) {
  if (PA_PU_UIO_DEVICE[0] == '\0') {
    return -1;
  }

  g_fd = open(PA_PU_UIO_DEVICE, O_RDWR | O_CLOEXEC);
  if (g_fd == -1) {
    log_warn("open %s failed, fallback to /dev/mem: %d", PA_PU_UIO_DEVICE, errno);
    return -1;
  }

  /*
   * UIO 的 map0 由设备树/驱动描述物理地址，应用不再关心 PA 寄存器物理基址。
   * 这对应目标板上的 /dev/uio0。
   */
  void* ptr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
  if (ptr == MAP_FAILED) {
    log_error("mmap %s failed: %d", PA_PU_UIO_DEVICE, errno);
    close(g_fd);
    g_fd = -1;
    return -1;
  }

  g_base = (volatile uint8_t*)ptr;
  g_map_size = map_size;
  g_using_uio = true;
  log_info("pa_pu mapped by uio device=%s size=0x%lx", PA_PU_UIO_DEVICE, (unsigned long)map_size);
  return 0;
}

static int pa_pu_open_devmem(uintptr_t base_addr, size_t map_size) {
  g_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (g_fd == -1) {
    log_error("open /dev/mem for pa_pu failed: %d", errno);
    return -1;
  }

  void* ptr = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, (off_t)base_addr);
  if (ptr == MAP_FAILED) {
    log_error("mmap pa_pu 0x%lx failed: %d", (unsigned long)base_addr, errno);
    close(g_fd);
    g_fd = -1;
    return -1;
  }

  g_base = (volatile uint8_t*)ptr;
  g_map_size = map_size;
  g_using_uio = false;
  log_info("pa_pu mapped by /dev/mem base=0x%lx size=0x%lx", (unsigned long)base_addr, (unsigned long)map_size);
  return 0;
}

static void pa_pu_open_irq_driver(void) {
  if (g_irq_fd != -1) {
    return;
  }

  g_irq_fd = open(PA_IRQ_DEVICE, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (g_irq_fd == -1) {
    log_warn("open %s failed, fallback to INT_VECTOR polling: %d", PA_IRQ_DEVICE, errno);
    return;
  }

  log_info("pa irq driver opened device=%s", PA_IRQ_DEVICE);
}

int pa_pu_open(uintptr_t base_addr, size_t map_size) {
  if (g_base != NULL) {
    return 0;
  }

  /* 优先走 UIO：地址由设备树/驱动维护，用户态只关心 /dev/uioX。 */
  if (pa_pu_open_uio(map_size) == 0) {
    pa_pu_open_irq_driver();
    return 0;
  }

  /* UIO 不存在时才回退 /dev/mem，主要用于早期 bring-up。 */
  int ret = pa_pu_open_devmem(base_addr, map_size);
  if (ret == 0) {
    pa_pu_open_irq_driver();
  }
  return ret;
}

void pa_pu_close(void) {
  if (g_irq_fd != -1) {
    close(g_irq_fd);
    g_irq_fd = -1;
  }

  if (g_base != NULL) {
    munmap((void*)g_base, g_map_size);
    g_base = NULL;
    g_map_size = 0;
    g_using_uio = false;
  }

  if (g_fd != -1) {
    close(g_fd);
    g_fd = -1;
  }
}

bool pa_pu_is_open(void) {
  return g_base != NULL;
}

bool pa_pu_irq_driver_is_open(void) {
  return g_irq_fd != -1;
}

uint32_t pa_pu_read(uint16_t reg) {
  if (g_base == NULL || (size_t)reg + sizeof(uint32_t) > g_map_size) {
    return 0;
  }
  /* PA 寄存器统一按 32 位访问。 */
  return *(volatile uint32_t*)(g_base + reg);
}

void pa_pu_write(uint16_t reg, uint32_t value) {
  if (g_base == NULL || (size_t)reg + sizeof(uint32_t) > g_map_size) {
    return;
  }
  /* 对 start 类寄存器通常写 1 触发硬件动作，是否自清由 FPGA 决定。 */
  *(volatile uint32_t*)(g_base + reg) = value;
}

void pa_pu_read_status(pa_pu_status_t* status) {
  if (status == NULL) {
    return;
  }

  memset(status, 0, sizeof(*status));

  /* 这里只读非 read-clear 状态，避免 STATUS 消费完成中断。 */
  status->pa_version = pa_pu_read(PA_PU_PA_VERSION_REG);
  status->pa_build_information = pa_pu_read(PA_PU_PA_BUILD_INFORMATION_REG);
  status->adapted_main_board_version = pa_pu_read(PA_PU_ADAPTED_MAIN_BOARD_VERSION_REG);
  status->adapted_gic_board_version = pa_pu_read(PA_PU_ADAPTED_GIC_BOARD_VERSION_REG);
  status->adapted_roic_board_version = pa_pu_read(PA_PU_ADAPTED_ROIC_BOARD_VERSION_REG);
  status->adapted_reserved_board_0_version = pa_pu_read(PA_PU_ADAPTED_RESERVED_BOARD_0_VERSION_REG);
  status->adapted_reserved_board_1_version = pa_pu_read(PA_PU_ADAPTED_RESERVED_BOARD_1_VERSION_REG);
  status->adapted_reserved_board_2_version = pa_pu_read(PA_PU_ADAPTED_RESERVED_BOARD_2_VERSION_REG);
  status->pa_pu_com_version = pa_pu_read(PA_PU_COM_VERSION_REG);
  status->rst_init_state = pa_pu_read(PA_PU_RST_INIT_STATE_REG);
  status->img_wr_state = pa_pu_read(PA_PU_IMG_WR_STATE_REG);
  status->img_wr_end = pa_pu_read(PA_PU_IMG_WR_END_REG);
  status->img_corr_state = pa_pu_read(PA_PU_IMG_CORR_STATE_REG);
  status->img_corr_end = pa_pu_read(PA_PU_IMG_CORR_END_REG);
  status->gic_state = pa_pu_read(PA_PU_GIC_STATE_REG);
  status->gic_end = pa_pu_read(PA_PU_GIC_END_REG);
  status->gic_dfx = pa_pu_read(PA_PU_GIC_DFX_REG);
  status->roic_state = pa_pu_read(PA_PU_ROIC_STATE_REG);
  status->roic_end = pa_pu_read(PA_PU_ROIC_END_REG);
  status->roic_dfx = pa_pu_read(PA_PU_ROIC_DFX_REG);
}

uint32_t pa_pu_read_int_vector(void) {
  return pa_pu_read(PA_PU_INT_VECTOR_REG);
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void pa_pu_drain_irq_driver(void) {
  if (g_irq_fd == -1) {
    return;
  }

  for (;;) {
    pa_irq_event_t event;
    ssize_t n = read(g_irq_fd, &event, sizeof(event));
    if (n == (ssize_t)sizeof(event)) {
      log_warn("drain stale pa irq int_vector=0x%08x count=%llu",
               event.int_vector,
               (unsigned long long)event.count);
      continue;
    }
    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    if (n == -1 && errno == EINTR) {
      return;
    }
    if (n > 0) {
      log_warn("short read while draining %s: %ld", PA_IRQ_DEVICE, (long)n);
    } else if (n == -1) {
      log_warn("read %s while draining failed, fallback to polling: %d", PA_IRQ_DEVICE, errno);
      close(g_irq_fd);
      g_irq_fd = -1;
    }
    return;
  }
}

void pa_pu_prepare_irq_wait(void) {
  if (g_irq_fd != -1) {
    pa_pu_drain_irq_driver();
    return;
  }

  /*
   * 回退轮询模式下，先读一次 read-clear 的 INT_VECTOR，避免上一轮遗留 bit
   * 被误判成本轮 start 命令完成。
   */
  uint32_t stale = pa_pu_read_int_vector();
  if (stale != 0) {
    log_warn("clear stale INT_VECTOR before start: 0x%08x", stale);
  }
}

static int pa_pu_wait_irq_driver(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  const uint64_t start_ms = monotonic_ms();
  uint32_t last_vector = 0;

  while (g_irq_fd != -1) {
    const uint64_t now_ms = monotonic_ms();
    if (now_ms - start_ms >= timeout_ms) {
      if (int_vector_out != NULL) {
        *int_vector_out = last_vector;
      }
      return 0;
    }

    uint64_t remain_ms = timeout_ms - (now_ms - start_ms);
    int poll_timeout = remain_ms > INT32_MAX ? INT32_MAX : (int)remain_ms;
    if (poll_timeout <= 0) {
      poll_timeout = 1;
    }

    struct pollfd pfd = {
      .fd = g_irq_fd,
      .events = POLLIN,
    };

    int pret = poll(&pfd, 1, poll_timeout);
    if (pret == 0) {
      if (int_vector_out != NULL) {
        *int_vector_out = last_vector;
      }
      return 0;
    }
    if (pret < 0) {
      if (errno == EINTR) {
        return -1;
      }
      log_warn("poll %s failed, fallback to INT_VECTOR polling: %d", PA_IRQ_DEVICE, errno);
      close(g_irq_fd);
      g_irq_fd = -1;
      return -2;
    }

    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      log_warn("poll %s revents=0x%x, fallback to INT_VECTOR polling", PA_IRQ_DEVICE, pfd.revents);
      close(g_irq_fd);
      g_irq_fd = -1;
      return -2;
    }

    if ((pfd.revents & POLLIN) == 0) {
      continue;
    }

    for (;;) {
      pa_irq_event_t event;
      ssize_t n = read(g_irq_fd, &event, sizeof(event));
      if (n == (ssize_t)sizeof(event)) {
        last_vector = event.int_vector;
        if (last_vector != 0) {
          log_info("pa irq int_vector=0x%08x count=%llu",
                   last_vector,
                   (unsigned long long)event.count);
        }
        if ((last_vector & mask) != 0) {
          if (int_vector_out != NULL) {
            *int_vector_out = last_vector;
          }
          return 1;
        }
        if (last_vector != 0) {
          log_warn("pa irq int_vector=0x%08x does not match mask=0x%08x", last_vector, mask);
        }
        continue;
      }
      if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      }
      if (n == -1 && errno == EINTR) {
        return -1;
      }
      if (n > 0) {
        log_warn("short read from %s: %ld", PA_IRQ_DEVICE, (long)n);
      } else if (n == -1) {
        log_warn("read %s failed, fallback to INT_VECTOR polling: %d", PA_IRQ_DEVICE, errno);
      }
      close(g_irq_fd);
      g_irq_fd = -1;
      return -2;
    }
  }

  if (int_vector_out != NULL) {
    *int_vector_out = last_vector;
  }
  return -2;
}

int pa_pu_wait_int_vector(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  if (mask == 0) {
    return -1;
  }

  if (g_irq_fd != -1) {
    int driver_ret = pa_pu_wait_irq_driver(mask, timeout_ms, int_vector_out);
    if (driver_ret != -2) {
      return driver_ret;
    }
  }

  const uint64_t start_ms = monotonic_ms();
  uint32_t last_vector = 0;

  for (;;) {
    /*
     * INT_VECTOR 是 read-clear：每次读取都会消费当前 pending 中断。
     * 如果读到了非目标 bit，也保留在 last_vector 中返回给调用方诊断。
     */
    last_vector = pa_pu_read_int_vector();
    if (last_vector != 0) {
      log_info("poll INT_VECTOR=0x%08x", last_vector);
    }
    if ((last_vector & mask) != 0) {
      if (int_vector_out != NULL) {
        *int_vector_out = last_vector;
      }
      return 1;
    }

    const uint64_t now_ms = monotonic_ms();
    if (now_ms - start_ms >= timeout_ms) {
      if (int_vector_out != NULL) {
        *int_vector_out = last_vector;
      }
      return 0;
    }

    if (usleep(PA_PU_IRQ_POLL_INTERVAL_US) != 0 && errno == EINTR) {
      return -1;
    }
  }
}

int pa_pu_wait_int_vector_all(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  const uint64_t start_ms = monotonic_ms();
  uint32_t accumulated_vector = 0;

  if (mask == 0) {
    return -1;
  }

  while ((accumulated_vector & mask) != mask) {
    const uint64_t now_ms = monotonic_ms();
    if (now_ms - start_ms >= timeout_ms) {
      if (int_vector_out != NULL) {
        *int_vector_out = accumulated_vector;
      }
      return 0;
    }

    uint32_t current_vector = 0;
    unsigned remain_ms = (unsigned)(timeout_ms - (now_ms - start_ms));
    int ret = pa_pu_wait_int_vector(mask & ~accumulated_vector, remain_ms, &current_vector);
    if (current_vector != 0) {
      accumulated_vector |= current_vector;
      log_info("accumulated INT_VECTOR=0x%08x wait_mask=0x%08x", accumulated_vector, mask);
    }

    if ((accumulated_vector & mask) == mask) {
      if (int_vector_out != NULL) {
        *int_vector_out = accumulated_vector;
      }
      return 1;
    }
    if (ret < 0) {
      if (int_vector_out != NULL) {
        *int_vector_out = accumulated_vector;
      }
      return ret;
    }
    if (ret == 0 && current_vector == 0) {
      if (int_vector_out != NULL) {
        *int_vector_out = accumulated_vector;
      }
      return 0;
    }
  }

  if (int_vector_out != NULL) {
    *int_vector_out = accumulated_vector;
  }
  return 1;
}

void pa_pu_configure_correction(const pa_pu_corr_config_t* config) {
  if (config == NULL) {
    return;
  }

  /* 这些写寄存器顺序按“尺寸 -> offset -> gain -> defect”组织，便于对照结构体字段。 */
  pa_pu_write(PA_PU_IMG_PKG_NUM_REG, config->pkg_num);
  pa_pu_write(PA_PU_IMG_ROW_NUM_REG, config->row_num);
  pa_pu_write(PA_PU_IMG_COL_NUM_REG, config->col_num);
  pa_pu_write(PA_PU_IMG_CORR_OFFSET_EN_REG, config->offset_enable ? 1u : 0u);
  pa_pu_write(PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG, config->offset_template_addr);
  pa_pu_write(PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG, config->offset_adder_value);
  pa_pu_write(PA_PU_IMG_CORR_GAIN_EN_REG, config->gain_enable ? 1u : 0u);
  pa_pu_write(PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG, config->gain_template_addr);
  pa_pu_write(PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG, config->gain_clipping_value);
  pa_pu_write(PA_PU_WR_IMG_CORR_DEFECT_EN_REG, config->defect_enable ? 1u : 0u);
}

void pa_pu_configure_templates(void) {
  /* 当前默认配置：启用 offset/gain，关闭坏点校正，图像尺寸来自 app_config.h。 */
  pa_pu_corr_config_t config = {
    .pkg_num = 8,
    .row_num = IMAGE_HEIGHT,
    .col_num = IMAGE_WIDTH,
    .offset_enable = true,
    .offset_template_addr = FPGA_OFFSET_PTR,
    .offset_adder_value = 0,
    .gain_enable = true,
    .gain_template_addr = FPGA_GAIN_PTR,
    .gain_clipping_value = 0xffff,
    .defect_enable = false,
  };
  pa_pu_configure_correction(&config);
}

void pa_pu_configure_gic_defaults(void) {
  pa_pu_gic_config_t config = {
    .req_code = GIC_DEFAULT_REQ_CODE,
    .dout_enable = GIC_DEFAULT_DOUT_EN != 0,
    .line_time_ns = GIC_DEFAULT_LINE_TIME_NS,
    .oe_raising_edge_ns = GIC_DEFAULT_OE_RISE_NS,
    .oe_falling_edge_ns = GIC_DEFAULT_OE_FALL_NS,
    .start_row = GIC_DEFAULT_START_ROW,
    .end_row = GIC_DEFAULT_END_ROW,
    .binning_mode = GIC_DEFAULT_BINNING,
  };
  pa_pu_configure_gic(&config);
}

void pa_pu_configure_gic(const pa_pu_gic_config_t* config) {
  if (config == NULL) {
    return;
  }

  pa_pu_write(PA_PU_GIC_REQ_CODE_REG, config->req_code);
  pa_pu_write(PA_PU_GIC_DOUT_EN_REG, config->dout_enable ? 1u : 0u);
  pa_pu_write(PA_PU_GIC_LINE_TIME_REG, config->line_time_ns);
  pa_pu_write(PA_PU_GIC_OE_RAISING_EDGE_REG, config->oe_raising_edge_ns);
  pa_pu_write(PA_PU_GIC_OE_FALLING_EDGE_REG, config->oe_falling_edge_ns);
  pa_pu_write(PA_PU_GIC_STR_ROW_NUM_REG, config->start_row);
  pa_pu_write(PA_PU_GIC_END_ROW_NUM_REG, config->end_row);
  pa_pu_write(PA_PU_GIC_BINNING_MODE_REG, config->binning_mode);
}

void pa_pu_start_gic(void) {
  pa_pu_write(PA_PU_GIC_STR_REG, 1);
}

void pa_pu_stop_gic(void) {
  pa_pu_write(PA_PU_GIC_STOP_REG, 1);
}

void pa_pu_configure_roic_defaults(void) {
  pa_pu_roic_config_t config = {
    .reg_00 = ROIC_DEFAULT_REG_00,
    .reg_02 = ROIC_DEFAULT_REG_02,
    .reg_05 = ROIC_DEFAULT_REG_05,
    .reg_06 = ROIC_DEFAULT_REG_06,
    .reg_07 = ROIC_DEFAULT_REG_07,
    .reg_09 = ROIC_DEFAULT_REG_09,
    .reg_0a = ROIC_DEFAULT_REG_0A,
    .reg_0b = ROIC_DEFAULT_REG_0B,
    .reg_0c = ROIC_DEFAULT_REG_0C,
    .reg_0d = ROIC_DEFAULT_REG_0D,
    .reg_0e = ROIC_DEFAULT_REG_0E,
    .reg_0f = ROIC_DEFAULT_REG_0F,
    .reg_10 = ROIC_DEFAULT_REG_10,
    .reg_11 = ROIC_DEFAULT_REG_11,
    .reg_17 = ROIC_DEFAULT_REG_17,
    .reg_24 = ROIC_DEFAULT_REG_24,
    .reg_28 = ROIC_DEFAULT_REG_28,
    .reg_2d = ROIC_DEFAULT_REG_2D,
    .reg_3b = ROIC_DEFAULT_REG_3B,
    .start_col = ROIC_DEFAULT_START_COL,
    .end_col = ROIC_DEFAULT_END_COL,
    .binning_mode = ROIC_DEFAULT_BINNING,
  };
  pa_pu_configure_roic(&config);
}

void pa_pu_configure_roic(const pa_pu_roic_config_t* config) {
  if (config == NULL) {
    return;
  }

  pa_pu_write(PA_PU_ROIC_REQ_CODE_REG, PA_PU_ROIC_REQ_CONFIG_ONCE);
  pa_pu_write(PA_PU_ROIC_REG_00_REG, config->reg_00);
  pa_pu_write(PA_PU_ROIC_REG_02_REG, config->reg_02);
  pa_pu_write(PA_PU_ROIC_REG_05_REG, config->reg_05);
  pa_pu_write(PA_PU_ROIC_REG_06_REG, config->reg_06);
  pa_pu_write(PA_PU_ROIC_REG_07_REG, config->reg_07);
  pa_pu_write(PA_PU_ROIC_REG_09_REG, config->reg_09);
  pa_pu_write(PA_PU_ROIC_REG_0A_REG, config->reg_0a);
  pa_pu_write(PA_PU_ROIC_REG_0B_REG, config->reg_0b);
  pa_pu_write(PA_PU_ROIC_REG_0C_REG, config->reg_0c);
  pa_pu_write(PA_PU_ROIC_REG_0D_REG, config->reg_0d);
  pa_pu_write(PA_PU_ROIC_REG_0E_REG, config->reg_0e);
  pa_pu_write(PA_PU_ROIC_REG_0F_REG, config->reg_0f);
  pa_pu_write(PA_PU_ROIC_REG_10_REG, config->reg_10);
  pa_pu_write(PA_PU_ROIC_REG_11_REG, config->reg_11);
  pa_pu_write(PA_PU_ROIC_REG_17_REG, config->reg_17);
  pa_pu_write(PA_PU_ROIC_REG_24_REG, config->reg_24);
  pa_pu_write(PA_PU_ROIC_REG_28_REG, config->reg_28);
  pa_pu_write(PA_PU_ROIC_REG_2D_REG, config->reg_2d);
  pa_pu_write(PA_PU_ROIC_REG_3B_REG, config->reg_3b);
  pa_pu_write(PA_PU_ROIC_STR_COL_NUM_REG, config->start_col);
  pa_pu_write(PA_PU_ROIC_END_COL_NUM_REG, config->end_col);
  pa_pu_write(PA_PU_ROIC_BINNING_MODE_REG, config->binning_mode);
}

void pa_pu_start_roic(void) {
  pa_pu_write(PA_PU_ROIC_STR_REG, 1);
}

void pa_pu_start_correction(void) {
  /* 写 1 后由 FPGA 开始图像校正；完成状态通过 IMG_CORR_END/中断读取。 */
  pa_pu_write(PA_PU_IMG_CORR_STR_REG, 1);
}

void pa_pu_start_image_write(uint32_t image_addr) {
  /* 先配置原始图像地址，再触发写图。光口传输本身由 PA/FPGA 完成。 */
  pa_pu_write(PA_PU_IMG_WR_STR_ADDR_REG, image_addr);
  pa_pu_write(PA_PU_IMG_WR_STR_REG, 1);
}
