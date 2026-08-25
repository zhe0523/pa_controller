#include "pa_pu.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
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

/* 调试 trace：只在命令显式打开时同步打印，避免正常运行刷屏。 */
static bool g_trace = false;

typedef struct {
  uint32_t int_vector;
  uint32_t reserved;
  uint64_t count;
  uint64_t timestamp_ns;
} pa_irq_event_t;

void pa_pu_set_trace(bool enabled) {
  g_trace = enabled;
}

static void pa_pu_trace(const char* tag, uint32_t mask, uint32_t value) {
  if (!g_trace) {
    return;
  }
  fprintf(stderr, "[TRACE] pa_pu %s mask=0x%08x value=0x%08x irq_fd=%d\n", tag, mask, value, g_irq_fd);
  fflush(stderr);
}

typedef struct {
  const char* name;
  uint16_t reg;
} pa_pu_reg_desc_t;

static const pa_pu_reg_desc_t k_pa_pu_reg_descs[] = {
  {"int_vector", PA_PU_INT_VECTOR_REG},
  {"pa_version", PA_PU_PA_VERSION_REG},
  {"pa_build_information", PA_PU_PA_BUILD_INFORMATION_REG},
  {"adapted_main_board_version", PA_PU_ADAPTED_MAIN_BOARD_VERSION_REG},
  {"adapted_gic_board_version", PA_PU_ADAPTED_GIC_BOARD_VERSION_REG},
  {"adapted_roic_board_version", PA_PU_ADAPTED_ROIC_BOARD_VERSION_REG},
  {"adapted_reserved_board_0_version", PA_PU_ADAPTED_RESERVED_BOARD_0_VERSION_REG},
  {"adapted_reserved_board_1_version", PA_PU_ADAPTED_RESERVED_BOARD_1_VERSION_REG},
  {"adapted_reserved_board_2_version", PA_PU_ADAPTED_RESERVED_BOARD_2_VERSION_REG},
  {"pa_pu_com_version", PA_PU_COM_VERSION_REG},
  {"rst_init_state", PA_PU_RST_INIT_STATE_REG},
  {"gic_str", PA_PU_GIC_STR_REG},
  {"gic_stop", PA_PU_GIC_STOP_REG},
  {"gic_req_code", PA_PU_GIC_REQ_CODE_REG},
  {"gic_dout_en", PA_PU_GIC_DOUT_EN_REG},
  {"gic_line_time", PA_PU_GIC_LINE_TIME_REG},
  {"gic_oe_raising_edge", PA_PU_GIC_OE_RAISING_EDGE_REG},
  {"gic_oe_falling_edge", PA_PU_GIC_OE_FALLING_EDGE_REG},
  {"gic_str_row_num", PA_PU_GIC_STR_ROW_NUM_REG},
  {"gic_end_row_num", PA_PU_GIC_END_ROW_NUM_REG},
  {"gic_binning_mode", PA_PU_GIC_BINNING_MODE_REG},
  {"gic_state", PA_PU_GIC_STATE_REG},
  {"gic_end", PA_PU_GIC_END_REG},
  {"gic_dfx", PA_PU_GIC_DFX_REG},
  {"gic_debug_in", PA_PU_GIC_DEBUG_IN_REG},
  {"gic_debug_out", PA_PU_GIC_DEBUG_OUT_REG},
  {"roic_str", PA_PU_ROIC_STR_REG},
  {"roic_req_code", PA_PU_ROIC_REQ_CODE_REG},
  {"roic_reg_00", PA_PU_ROIC_REG_00_REG},
  {"roic_reg_02", PA_PU_ROIC_REG_02_REG},
  {"roic_reg_05", PA_PU_ROIC_REG_05_REG},
  {"roic_reg_06", PA_PU_ROIC_REG_06_REG},
  {"roic_reg_07", PA_PU_ROIC_REG_07_REG},
  {"roic_reg_09", PA_PU_ROIC_REG_09_REG},
  {"roic_reg_0a", PA_PU_ROIC_REG_0A_REG},
  {"roic_reg_0b", PA_PU_ROIC_REG_0B_REG},
  {"roic_reg_0c", PA_PU_ROIC_REG_0C_REG},
  {"roic_reg_0d", PA_PU_ROIC_REG_0D_REG},
  {"roic_reg_0e", PA_PU_ROIC_REG_0E_REG},
  {"roic_reg_0f", PA_PU_ROIC_REG_0F_REG},
  {"roic_reg_10", PA_PU_ROIC_REG_10_REG},
  {"roic_reg_11", PA_PU_ROIC_REG_11_REG},
  {"roic_reg_17", PA_PU_ROIC_REG_17_REG},
  {"roic_reg_24", PA_PU_ROIC_REG_24_REG},
  {"roic_reg_28", PA_PU_ROIC_REG_28_REG},
  {"roic_reg_2d", PA_PU_ROIC_REG_2D_REG},
  {"roic_reg_3b", PA_PU_ROIC_REG_3B_REG},
  {"roic_str_col_num", PA_PU_ROIC_STR_COL_NUM_REG},
  {"roic_end_col_num", PA_PU_ROIC_END_COL_NUM_REG},
  {"roic_binning_mode", PA_PU_ROIC_BINNING_MODE_REG},
  {"roic_state", PA_PU_ROIC_STATE_REG},
  {"roic_end", PA_PU_ROIC_END_REG},
  {"roic_dfx", PA_PU_ROIC_DFX_REG},
  {"roic_debug_in", PA_PU_ROIC_DEBUG_IN_REG},
  {"roic_debug_out", PA_PU_ROIC_DEBUG_OUT_REG},
  {"img_wr_str", PA_PU_IMG_WR_STR_REG},
  {"img_wr_str_addr", PA_PU_IMG_WR_STR_ADDR_REG},
  {"img_wr_state", PA_PU_IMG_WR_STATE_REG},
  {"img_wr_end", PA_PU_IMG_WR_END_REG},
  {"img_wr_final_img_addr", PA_PU_IMG_WR_FINAL_IMG_ADDR_REG},
  {"img_wr_dfx", PA_PU_IMG_WR_DFX_REG},
  {"img_wr_debug_in", PA_PU_IMG_WR_DEBUG_IN_REG},
  {"img_wr_debug_out", PA_PU_IMG_WR_DEBUG_OUT_REG},
  {"img_corr_str", PA_PU_IMG_CORR_STR_REG},
  {"img_pkg_num", PA_PU_IMG_PKG_NUM_REG},
  {"img_row_num", PA_PU_IMG_ROW_NUM_REG},
  {"img_col_num", PA_PU_IMG_COL_NUM_REG},
  {"img_corr_offset_en", PA_PU_IMG_CORR_OFFSET_EN_REG},
  {"img_corr_offset_temp_str_addr", PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG},
  {"img_corr_offset_adder_value", PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG},
  {"img_corr_gain_en", PA_PU_IMG_CORR_GAIN_EN_REG},
  {"img_corr_gain_temp_str_addr", PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG},
  {"img_corr_gain_clipping_value", PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG},
  {"img_corr_defect_en", PA_PU_IMG_CORR_DEFECT_EN_REG},
  {"img_offset_corr_mode", PA_PU_IMG_OFFSET_CORR_MODE_REG},
  {"img_corr_state", PA_PU_IMG_CORR_STATE_REG},
  {"img_corr_end", PA_PU_IMG_CORR_END_REG},
  {"img_corr_dfx", PA_PU_IMG_CORR_DFX_REG},
  {"img_corr_debug_in", PA_PU_IMG_CORR_DEBUG_IN_REG},
  {"img_corr_debug_out", PA_PU_IMG_CORR_DEBUG_OUT_REG},
  {"dync_str", PA_PU_DYNC_STR_REG},
  {"dync_stop", PA_PU_DYNC_STOP_REG},
  {"dync_cycle_num", PA_PU_DYNC_CYCLE_NUM_REG},
  {"dync_img_str_addr", PA_PU_DYNC_IMG_STR_ADDR_REG},
  {"dync_img_end_addr", PA_PU_DYNC_IMG_END_ADDR_REG},
  {"dync_step_0_cfg_h", PA_PU_DYNC_STEP_0_CFG_H_REG},
  {"dync_step_0_cfg_l", PA_PU_DYNC_STEP_0_CFG_L_REG},
  {"dync_step_1_cfg_h", PA_PU_DYNC_STEP_1_CFG_H_REG},
  {"dync_step_1_cfg_l", PA_PU_DYNC_STEP_1_CFG_L_REG},
  {"dync_step_2_cfg_h", PA_PU_DYNC_STEP_2_CFG_H_REG},
  {"dync_step_2_cfg_l", PA_PU_DYNC_STEP_2_CFG_L_REG},
  {"dync_step_3_cfg_h", PA_PU_DYNC_STEP_3_CFG_H_REG},
  {"dync_step_3_cfg_l", PA_PU_DYNC_STEP_3_CFG_L_REG},
  {"dync_step_4_cfg_h", PA_PU_DYNC_STEP_4_CFG_H_REG},
  {"dync_step_4_cfg_l", PA_PU_DYNC_STEP_4_CFG_L_REG},
  {"dync_step_5_cfg_h", PA_PU_DYNC_STEP_5_CFG_H_REG},
  {"dync_step_5_cfg_l", PA_PU_DYNC_STEP_5_CFG_L_REG},
  {"dync_step_6_cfg_h", PA_PU_DYNC_STEP_6_CFG_H_REG},
  {"dync_step_6_cfg_l", PA_PU_DYNC_STEP_6_CFG_L_REG},
  {"dync_step_7_cfg_h", PA_PU_DYNC_STEP_7_CFG_H_REG},
  {"dync_step_7_cfg_l", PA_PU_DYNC_STEP_7_CFG_L_REG},
  {"dync_step_8_cfg_h", PA_PU_DYNC_STEP_8_CFG_H_REG},
  {"dync_step_8_cfg_l", PA_PU_DYNC_STEP_8_CFG_L_REG},
  {"dync_step_9_cfg_h", PA_PU_DYNC_STEP_9_CFG_H_REG},
  {"dync_step_9_cfg_l", PA_PU_DYNC_STEP_9_CFG_L_REG},
  {"dync_end", PA_PU_DYNC_END_REG},
  {"dync_state", PA_PU_DYNC_STATE_REG},
  {"dync_debug_in", PA_PU_DYNC_DEBUG_IN_REG},
  {"dync_debug_out", PA_PU_DYNC_DEBUG_OUT_REG},
  {"img_upload_str", PA_PU_IMG_UPLOAD_STR_REG},
  {"img_upload_str_addr", PA_PU_IMG_UPLOAD_STR_ADDR_REG},
  {"img_upload_pkg_num", PA_PU_IMG_UPLOAD_PKG_NUM_REG},
  {"img_upload_row_num", PA_PU_IMG_UPLOAD_ROW_NUM_REG},
  {"img_upload_col_num", PA_PU_IMG_UPLOAD_COL_NUM_REG},
  {"img_upload_state", PA_PU_IMG_UPLOAD_STATE_REG},
  {"img_upload_end", PA_PU_IMG_UPLOAD_END_REG},
  {"img_upload_dfx", PA_PU_IMG_UPLOAD_DFX_REG},
};

static size_t pa_pu_reg_desc_count(void) {
  return sizeof(k_pa_pu_reg_descs) / sizeof(k_pa_pu_reg_descs[0]);
}

static bool pa_pu_reg_is_read_clear(uint16_t reg) {
  /*
   * INT_VECTOR 是硬件 read-clear 寄存器，读一次会清除 pending 中断。
   * 调试快照不能碰它，否则可能把现场中断消费掉。
   */
  return reg == PA_PU_INT_VECTOR_REG;
}

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

static bool pa_pu_write_verify(uint16_t reg, uint32_t value, const char* name) {
  /*
   * PA/PU 配置寄存器经 AXI-lite 写入 FPGA。现场观察到连续快速写 IMG_CORR 尺寸时，
   * 偶发出现 IMG_ROW_NUM 读回等于 IMG_PKG_NUM 的情况；对关键配置做写后读回，
   * 如果未锁存成功就短延迟重写，避免带着错误尺寸启动硬件状态机。
   */
  enum { kMaxAttempts = 3 };
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    pa_pu_write(reg, value);
    uint32_t readback = pa_pu_read(reg);
    if (readback == value) {
      if (attempt > 1) {
        log_warn("pa_pu write verify recovered reg=%s offset=0x%04x value=0x%08x attempts=%d",
                 name != NULL ? name : "unknown",
                 reg,
                 value,
                 attempt);
      }
      return true;
    }

    log_warn("pa_pu write verify mismatch reg=%s offset=0x%04x expect=0x%08x read=0x%08x attempt=%d",
             name != NULL ? name : "unknown",
             reg,
             value,
             readback,
             attempt);
    usleep(1000u);
  }

  return false;
}

static void pa_pu_trace_write_reg(const char* scope, const char* name, uint16_t reg, uint32_t value) {
  /*
   * 寄存器访问卡死时，普通函数级 trace 只能看到“进入配置函数”。
   * 这里在关键写寄存器前后输出更细粒度信息，用于判断 AXI-lite 总线具体卡在哪个地址。
   */
  if (!g_trace) {
    return;
  }
  printf("[TRACE] pa_pu %s write %s offset=0x%04x value=0x%08x\n",
         scope != NULL ? scope : "reg",
         name != NULL ? name : "unknown",
         reg,
         value);
  fflush(stdout);
}

static void pa_pu_write_traced(const char* scope, const char* name, uint16_t reg, uint32_t value) {
  pa_pu_trace_write_reg(scope, name, reg, value);
  pa_pu_write(reg, value);
  if (g_trace) {
    printf("[TRACE] pa_pu %s write_done %s offset=0x%04x\n",
           scope != NULL ? scope : "reg",
           name != NULL ? name : "unknown",
           reg);
    fflush(stdout);
  }
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
  status->img_wr_final_img_addr = pa_pu_read(PA_PU_IMG_WR_FINAL_IMG_ADDR_REG);
  status->img_corr_state = pa_pu_read(PA_PU_IMG_CORR_STATE_REG);
  status->img_corr_end = pa_pu_read(PA_PU_IMG_CORR_END_REG);
  status->gic_state = pa_pu_read(PA_PU_GIC_STATE_REG);
  status->gic_end = pa_pu_read(PA_PU_GIC_END_REG);
  status->gic_dfx = pa_pu_read(PA_PU_GIC_DFX_REG);
  status->roic_state = pa_pu_read(PA_PU_ROIC_STATE_REG);
  status->roic_end = pa_pu_read(PA_PU_ROIC_END_REG);
  status->roic_dfx = pa_pu_read(PA_PU_ROIC_DFX_REG);
  status->dync_state = pa_pu_read(PA_PU_DYNC_STATE_REG);
  status->dync_end = pa_pu_read(PA_PU_DYNC_END_REG);
  status->dync_debug_out = pa_pu_read(PA_PU_DYNC_DEBUG_OUT_REG);
  status->img_upload_state = pa_pu_read(PA_PU_IMG_UPLOAD_STATE_REG);
  status->img_upload_end = pa_pu_read(PA_PU_IMG_UPLOAD_END_REG);
  status->img_upload_dfx = pa_pu_read(PA_PU_IMG_UPLOAD_DFX_REG);
}

void pa_pu_dump_all_registers(const char* reason) {
  const char* text = reason != NULL ? reason : "manual";
  size_t count = 0;
  size_t skipped = 0;

  log_error("pa_pu register dump begin reason=%s total=%lu", text, (unsigned long)pa_pu_reg_desc_count());
  for (size_t i = 0; i < pa_pu_reg_desc_count(); ++i) {
    const pa_pu_reg_desc_t* desc = &k_pa_pu_reg_descs[i];
    if (pa_pu_reg_is_read_clear(desc->reg)) {
      ++skipped;
      log_error("pa_pu reg %-34s offset=0x%04x skipped=read_clear",
                desc->name,
                desc->reg);
      continue;
    }

    uint32_t value = pa_pu_read(desc->reg);
    ++count;
    log_error("pa_pu reg %-34s offset=0x%04x value=0x%08x",
              desc->name,
              desc->reg,
              value);
  }
  log_error("pa_pu register dump end reason=%s count=%lu skipped=%lu",
            text,
            (unsigned long)count,
            (unsigned long)skipped);
}

size_t pa_pu_dump_safe_registers(const char* reason, size_t* skipped_out) {
  const char* text = reason != NULL ? reason : "manual";
  size_t count = 0;
  size_t skipped = 0;

  log_info("pa_pu safe register dump begin reason=%s total=%lu",
           text,
           (unsigned long)pa_pu_reg_desc_count());
  for (size_t i = 0; i < pa_pu_reg_desc_count(); ++i) {
    const pa_pu_reg_desc_t* desc = &k_pa_pu_reg_descs[i];
    if (pa_pu_reg_is_read_clear(desc->reg)) {
      ++skipped;
      log_info("pa_pu reg %-34s offset=0x%04x skipped=read_clear",
               desc->name,
               desc->reg);
      continue;
    }

    uint32_t value = pa_pu_read(desc->reg);
    ++count;
    log_info("pa_pu reg %-34s offset=0x%04x value=0x%08x",
             desc->name,
             desc->reg,
             value);
  }
  log_info("pa_pu safe register dump end reason=%s count=%lu skipped=%lu",
           text,
           (unsigned long)count,
           (unsigned long)skipped);

  if (skipped_out != NULL) {
    *skipped_out = skipped;
  }
  return count;
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

static int pa_pu_poll_delay(void) {
  /*
   * 无 /dev/pa_irq 时的轮询等待。现场曾观察到 FPGA 某次启动后 Linux usleep
   * 不再返回，因此默认使用纯 CPU 忙等做对比测试；正式版本可再切回 usleep。
   */
  if (PA_PU_IRQ_POLL_BUSY_WAIT != 0) {
    volatile uint32_t spin = 0;
    for (uint32_t i = 0; i < PA_PU_IRQ_POLL_BUSY_SPINS; ++i) {
      spin += i;
    }
    (void)spin;
    return 0;
  }

  if (usleep(PA_PU_IRQ_POLL_INTERVAL_US) != 0 && errno == EINTR) {
    return -1;
  }
  return 0;
}

static void pa_pu_drain_irq_driver(void) {
  if (g_irq_fd == -1) {
    return;
  }

  for (;;) {
    pa_irq_event_t event;
    ssize_t n = read(g_irq_fd, &event, sizeof(event));
    if (n == (ssize_t)sizeof(event)) {
#if PA_PU_IRQ_EVENT_LOG_ENABLE
      log_warn("drain stale pa irq int_vector=0x%08x count=%llu",
               event.int_vector,
               (unsigned long long)event.count);
#endif
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
#if PA_PU_IRQ_EVENT_LOG_ENABLE
  if (stale != 0) {
    log_warn("clear stale INT_VECTOR before start: 0x%08x", stale);
  }
#else
  (void)stale;
#endif
}

static int pa_pu_wait_irq_driver(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  const uint64_t start_ms = monotonic_ms();
  uint32_t last_vector = 0;
  uint32_t seen_vector = 0;
#if PA_PU_IRQ_EVENT_LOG_ENABLE
  uint32_t reported_mismatch = 0;
#endif

  pa_pu_trace("wait_irq_driver_enter", mask, timeout_ms);
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

    pa_pu_trace("wait_irq_driver_before_poll", mask, (uint32_t)poll_timeout);
    int pret = poll(&pfd, 1, poll_timeout);
    pa_pu_trace("wait_irq_driver_poll_return", mask, (uint32_t)pret);
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
        seen_vector |= last_vector;
        pa_pu_trace("wait_irq_driver_event", mask, last_vector);
        if ((last_vector & mask) != 0) {
          if (int_vector_out != NULL) {
            *int_vector_out = seen_vector;
          }
          return 1;
        }
#if PA_PU_IRQ_EVENT_LOG_ENABLE
        if (last_vector != 0 && last_vector != reported_mismatch) {
          log_warn("pa irq int_vector=0x%08x does not match mask=0x%08x", last_vector, mask);
          reported_mismatch = last_vector;
        }
#endif
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
    *int_vector_out = seen_vector != 0 ? seen_vector : last_vector;
  }
  return -2;
}

int pa_pu_wait_int_vector(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  if (mask == 0) {
    return -1;
  }

  pa_pu_trace("wait_int_vector_enter", mask, timeout_ms);
  if (g_irq_fd != -1) {
    int driver_ret = pa_pu_wait_irq_driver(mask, timeout_ms, int_vector_out);
    pa_pu_trace("wait_int_vector_driver_ret", mask, (uint32_t)driver_ret);
    if (driver_ret != -2) {
      return driver_ret;
    }
  }

  const uint64_t start_ms = monotonic_ms();
  uint32_t last_vector = 0;
  uint32_t seen_vector = 0;
#if PA_PU_IRQ_EVENT_LOG_ENABLE
  uint32_t reported_mismatch = 0;
#endif

  for (;;) {
    /*
     * INT_VECTOR 是 read-clear：每次读取都会消费当前 pending 中断。
     * 如果读到了非目标 bit，也保留在 last_vector 中返回给调用方诊断。
     */
    pa_pu_trace("wait_int_vector_read_begin", mask, PA_PU_INT_VECTOR_REG);
    last_vector = pa_pu_read_int_vector();
    pa_pu_trace("wait_int_vector_read_done", mask, last_vector);
    seen_vector |= last_vector;
    if (last_vector != 0 || g_trace) {
      pa_pu_trace("wait_int_vector_poll_value", mask, last_vector);
    }
#if PA_PU_IRQ_EVENT_LOG_ENABLE
    if (last_vector != 0 && (last_vector & mask) == 0 && last_vector != reported_mismatch) {
      log_warn("poll INT_VECTOR=0x%08x does not match mask=0x%08x", last_vector, mask);
      reported_mismatch = last_vector;
    } else if (last_vector != 0 && mask != PA_PU_IRQ_GIC_END) {
      log_info("poll INT_VECTOR=0x%08x", last_vector);
    }
#endif
    if ((last_vector & mask) != 0) {
      if (int_vector_out != NULL) {
        *int_vector_out = seen_vector;
      }
      return 1;
    }

    pa_pu_trace("wait_int_vector_before_clock", mask, last_vector);
    const uint64_t now_ms = monotonic_ms();
    pa_pu_trace("wait_int_vector_after_clock", mask, (uint32_t)(now_ms - start_ms));
    if (now_ms - start_ms >= timeout_ms) {
      if (int_vector_out != NULL) {
        *int_vector_out = seen_vector != 0 ? seen_vector : last_vector;
      }
      return 0;
    }

    pa_pu_trace("wait_int_vector_before_delay", mask, PA_PU_IRQ_POLL_BUSY_WAIT != 0 ? PA_PU_IRQ_POLL_BUSY_SPINS : PA_PU_IRQ_POLL_INTERVAL_US);
    if (pa_pu_poll_delay() != 0) {
      return -1;
    }
    pa_pu_trace("wait_int_vector_after_delay", mask, PA_PU_IRQ_POLL_BUSY_WAIT != 0 ? PA_PU_IRQ_POLL_BUSY_SPINS : PA_PU_IRQ_POLL_INTERVAL_US);
  }
}

int pa_pu_wait_int_vector_all(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out) {
  const uint64_t start_ms = monotonic_ms();
  uint32_t accumulated_vector = 0;

  if (mask == 0) {
    return -1;
  }

  pa_pu_trace("wait_all_enter", mask, timeout_ms);
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
    pa_pu_trace("wait_all_before_wait_one", mask & ~accumulated_vector, accumulated_vector);
    int ret = pa_pu_wait_int_vector(mask & ~accumulated_vector, remain_ms, &current_vector);
    pa_pu_trace("wait_all_after_wait_one", mask & ~accumulated_vector, current_vector);
    if (current_vector != 0) {
      accumulated_vector |= current_vector;
#if PA_PU_IRQ_EVENT_LOG_ENABLE
      if ((current_vector & mask) == 0 || (accumulated_vector & mask) != mask) {
        log_warn("accumulated INT_VECTOR=0x%08x wait_mask=0x%08x current=0x%08x",
                 accumulated_vector,
                 mask,
                 current_vector);
      }
#endif
    }

    if ((accumulated_vector & mask) == mask) {
      if (int_vector_out != NULL) {
        *int_vector_out = accumulated_vector;
      }
      pa_pu_trace("wait_all_done", mask, accumulated_vector);
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

  /*
   * 这些写寄存器顺序按“尺寸 -> offset -> gain -> defect”组织，便于对照结构体字段。
   * 尺寸寄存器会直接决定 IMG_CORR/IMG_WR 本轮处理长度，必须确认读回正确后再启动 STR。
   */
  bool size_ok = true;
  pa_pu_trace_write_reg("config_corr", "img_pkg_num", PA_PU_IMG_PKG_NUM_REG, config->pkg_num);
  size_ok &= pa_pu_write_verify(PA_PU_IMG_PKG_NUM_REG, config->pkg_num, "img_pkg_num");
  pa_pu_trace_write_reg("config_corr", "img_row_num", PA_PU_IMG_ROW_NUM_REG, config->row_num);
  size_ok &= pa_pu_write_verify(PA_PU_IMG_ROW_NUM_REG, config->row_num, "img_row_num");
  pa_pu_trace_write_reg("config_corr", "img_col_num", PA_PU_IMG_COL_NUM_REG, config->col_num);
  size_ok &= pa_pu_write_verify(PA_PU_IMG_COL_NUM_REG, config->col_num, "img_col_num");
  uint32_t pkg_readback = pa_pu_read(PA_PU_IMG_PKG_NUM_REG);
  uint32_t row_readback = pa_pu_read(PA_PU_IMG_ROW_NUM_REG);
  uint32_t col_readback = pa_pu_read(PA_PU_IMG_COL_NUM_REG);
  if (!size_ok ||
      pkg_readback != config->pkg_num ||
      row_readback != config->row_num ||
      col_readback != config->col_num) {
    log_warn("img corr size readback mismatch pkg=%u/0x%08x row=%u/0x%08x col=%u/0x%08x",
             config->pkg_num,
             pkg_readback,
             config->row_num,
             row_readback,
             config->col_num,
             col_readback);
  }
  pa_pu_trace_write_reg("config_corr", "img_corr_offset_en", PA_PU_IMG_CORR_OFFSET_EN_REG, config->offset_enable ? 1u : 0u);
  pa_pu_write_verify(PA_PU_IMG_CORR_OFFSET_EN_REG, config->offset_enable ? 1u : 0u, "img_corr_offset_en");
  pa_pu_trace_write_reg("config_corr", "img_corr_offset_temp_str_addr", PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG, config->offset_template_addr);
  pa_pu_write_verify(PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG, config->offset_template_addr, "img_corr_offset_temp_str_addr");
  pa_pu_trace_write_reg("config_corr", "img_corr_offset_adder_value", PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG, config->offset_adder_value);
  pa_pu_write_verify(PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG, config->offset_adder_value, "img_corr_offset_adder_value");
  pa_pu_trace_write_reg("config_corr", "img_offset_corr_mode", PA_PU_IMG_OFFSET_CORR_MODE_REG, config->offset_corr_mode);
  pa_pu_write_verify(PA_PU_IMG_OFFSET_CORR_MODE_REG, config->offset_corr_mode, "img_offset_corr_mode");
  pa_pu_trace_write_reg("config_corr", "img_corr_gain_en", PA_PU_IMG_CORR_GAIN_EN_REG, config->gain_enable ? 1u : 0u);
  pa_pu_write_verify(PA_PU_IMG_CORR_GAIN_EN_REG, config->gain_enable ? 1u : 0u, "img_corr_gain_en");
  pa_pu_trace_write_reg("config_corr", "img_corr_gain_temp_str_addr", PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG, config->gain_template_addr);
  pa_pu_write_verify(PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG, config->gain_template_addr, "img_corr_gain_temp_str_addr");
  pa_pu_trace_write_reg("config_corr", "img_corr_gain_clipping_value", PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG, config->gain_clipping_value);
  pa_pu_write_verify(PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG, config->gain_clipping_value, "img_corr_gain_clipping_value");
  pa_pu_trace_write_reg("config_corr", "img_corr_defect_en", PA_PU_WR_IMG_CORR_DEFECT_EN_REG, config->defect_enable ? 1u : 0u);
  pa_pu_write_verify(PA_PU_WR_IMG_CORR_DEFECT_EN_REG, config->defect_enable ? 1u : 0u, "img_corr_defect_en");
}

void pa_pu_configure_templates(void) {
  /* 使用 app_config.h/Makefile 中的统一默认图像校正配置。 */
  pa_pu_corr_config_t config = {
    .pkg_num = CORR_DEFAULT_PKG_NUM,
    .row_num = CORR_DEFAULT_ROW_NUM,
    .col_num = CORR_DEFAULT_COL_NUM,
    .offset_enable = CORR_DEFAULT_OFFSET_EN != 0,
    .offset_template_addr = CORR_DEFAULT_OFFSET_ADDR,
    .offset_adder_value = CORR_DEFAULT_OFFSET_ADDER_VALUE,
    .offset_corr_mode = CORR_DEFAULT_OFFSET_CORR_MODE,
    .gain_enable = CORR_DEFAULT_GAIN_EN != 0,
    .gain_template_addr = CORR_DEFAULT_GAIN_ADDR,
    .gain_clipping_value = CORR_DEFAULT_GAIN_CLIPPING_VALUE,
    .defect_enable = CORR_DEFAULT_DEFECT_EN != 0,
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

  pa_pu_write_traced("config_gic", "gic_req_code", PA_PU_GIC_REQ_CODE_REG, config->req_code);
  pa_pu_write_traced("config_gic", "gic_dout_en", PA_PU_GIC_DOUT_EN_REG, config->dout_enable ? 1u : 0u);
  pa_pu_write_traced("config_gic", "gic_line_time", PA_PU_GIC_LINE_TIME_REG, config->line_time_ns);
  pa_pu_write_traced("config_gic", "gic_oe_raising_edge", PA_PU_GIC_OE_RAISING_EDGE_REG, config->oe_raising_edge_ns);
  pa_pu_write_traced("config_gic", "gic_oe_falling_edge", PA_PU_GIC_OE_FALLING_EDGE_REG, config->oe_falling_edge_ns);
  pa_pu_write_traced("config_gic", "gic_str_row_num", PA_PU_GIC_STR_ROW_NUM_REG, config->start_row);
  pa_pu_write_traced("config_gic", "gic_end_row_num", PA_PU_GIC_END_ROW_NUM_REG, config->end_row);
  pa_pu_write_traced("config_gic", "gic_binning_mode", PA_PU_GIC_BINNING_MODE_REG, config->binning_mode);
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

void pa_pu_configure_dync(const pa_pu_dync_config_t* config) {
  static const uint16_t step_h_regs[PA_PU_DYNC_STEP_COUNT] = {
    PA_PU_DYNC_STEP_0_CFG_H_REG,
    PA_PU_DYNC_STEP_1_CFG_H_REG,
    PA_PU_DYNC_STEP_2_CFG_H_REG,
    PA_PU_DYNC_STEP_3_CFG_H_REG,
    PA_PU_DYNC_STEP_4_CFG_H_REG,
    PA_PU_DYNC_STEP_5_CFG_H_REG,
    PA_PU_DYNC_STEP_6_CFG_H_REG,
    PA_PU_DYNC_STEP_7_CFG_H_REG,
    PA_PU_DYNC_STEP_8_CFG_H_REG,
    PA_PU_DYNC_STEP_9_CFG_H_REG,
  };
  static const uint16_t step_l_regs[PA_PU_DYNC_STEP_COUNT] = {
    PA_PU_DYNC_STEP_0_CFG_L_REG,
    PA_PU_DYNC_STEP_1_CFG_L_REG,
    PA_PU_DYNC_STEP_2_CFG_L_REG,
    PA_PU_DYNC_STEP_3_CFG_L_REG,
    PA_PU_DYNC_STEP_4_CFG_L_REG,
    PA_PU_DYNC_STEP_5_CFG_L_REG,
    PA_PU_DYNC_STEP_6_CFG_L_REG,
    PA_PU_DYNC_STEP_7_CFG_L_REG,
    PA_PU_DYNC_STEP_8_CFG_L_REG,
    PA_PU_DYNC_STEP_9_CFG_L_REG,
  };

  if (config == NULL) {
    return;
  }

  /*
   * dynamic 模块一次性下发循环次数、图像环形缓冲范围和步骤表。
   * 调用者需要先保证 GIC/ROIC/IMG_CORR/IMG_WR 等基础模块参数已经配置到期望值。
   */
  pa_pu_write(PA_PU_DYNC_CYCLE_NUM_REG, config->cycle_num);
  pa_pu_write(PA_PU_DYNC_IMG_STR_ADDR_REG, config->image_start_addr);
  pa_pu_write(PA_PU_DYNC_IMG_END_ADDR_REG, config->image_end_addr);
  for (size_t i = 0; i < PA_PU_DYNC_STEP_COUNT; ++i) {
    pa_pu_write(step_h_regs[i], config->step_cfg_h[i]);
    pa_pu_write(step_l_regs[i], config->step_cfg_l[i]);
  }
}

void pa_pu_start_dync(void) {
  pa_pu_write(PA_PU_DYNC_STR_REG, 1);
}

void pa_pu_stop_dync(void) {
  pa_pu_write(PA_PU_DYNC_STOP_REG, 1);
}

void pa_pu_configure_img_upload(const pa_pu_img_upload_config_t* config) {
  if (config == NULL) {
    return;
  }

  /*
   * 图片上传模块只需要 DDR 首地址和本次上传尺寸。
   * STR 单独写，便于 CONFIG_IMG_UPLOAD 后先读回寄存器再触发。
   */
  pa_pu_write_traced("config_img_upload", "img_upload_str_addr", PA_PU_IMG_UPLOAD_STR_ADDR_REG, config->image_addr);
  pa_pu_write_traced("config_img_upload", "img_upload_pkg_num", PA_PU_IMG_UPLOAD_PKG_NUM_REG, config->pkg_num);
  pa_pu_write_traced("config_img_upload", "img_upload_row_num", PA_PU_IMG_UPLOAD_ROW_NUM_REG, config->row_num);
  pa_pu_write_traced("config_img_upload", "img_upload_col_num", PA_PU_IMG_UPLOAD_COL_NUM_REG, config->col_num);
}

void pa_pu_start_img_upload(void) {
  pa_pu_write(PA_PU_IMG_UPLOAD_STR_REG, 1);
}

void pa_pu_configure_image_write(uint32_t image_addr) {
  /* 这里只写首地址，真正开始写图由 IMG_WR_STR 触发。 */
  pa_pu_write_traced("config_img_wr", "img_wr_str_addr", PA_PU_IMG_WR_STR_ADDR_REG, image_addr);
}

void pa_pu_start_capture_triplet(void) {
  /*
   * 软件侧尽量连续写三个启动寄存器，实现“同时启动”的业务语义。
   * FPGA 侧最终时序仍以各模块 STR 采样和内部同步逻辑为准。
   */
  pa_pu_write_traced("start_triplet", "img_corr_str", PA_PU_IMG_CORR_STR_REG, 1);
  pa_pu_write_traced("start_triplet", "img_wr_str", PA_PU_IMG_WR_STR_REG, 1);
  pa_pu_write_traced("start_triplet", "gic_str", PA_PU_GIC_STR_REG, 1);
}

void pa_pu_start_image_write(uint32_t image_addr) {
  /* 先配置原始图像地址，再触发写图。光口传输本身由 PA/FPGA 完成。 */
  pa_pu_configure_image_write(image_addr);
  pa_pu_write(PA_PU_IMG_WR_STR_REG, 1);
}
