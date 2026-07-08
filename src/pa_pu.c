#include "pa_pu.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

/* 当前 PA 寄存器映射的文件描述符：可能是 /dev/uioX，也可能是 /dev/mem。 */
static int g_fd = -1;

/* PA 寄存器映射后的用户态虚拟基址。volatile 防止编译器合并 MMIO 访问。 */
static volatile uint8_t* g_base = NULL;

/* 映射窗口长度，用于寄存器越界检查和 munmap。 */
static size_t g_map_size = 0;

/* 记录当前是否通过 UIO 打开；只有 UIO 模式才能用 read/poll 等待中断。 */
static bool g_using_uio = false;

static int pa_pu_open_uio(size_t map_size) {
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

int pa_pu_open(uintptr_t base_addr, size_t map_size) {
  if (g_base != NULL) {
    return 0;
  }

  /* 优先走 UIO：地址由设备树/驱动维护，用户态只关心 /dev/uioX。 */
  if (pa_pu_open_uio(map_size) == 0) {
    return 0;
  }

  /* UIO 不存在时才回退 /dev/mem，主要用于早期 bring-up。 */
  return pa_pu_open_devmem(base_addr, map_size);
}

void pa_pu_close(void) {
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

  /* 这里只读上位机当前最需要的状态，避免 STATUS 响应过长。 */
  status->int_vector = pa_pu_read(PA_PU_INT_VECTOR_REG);
  status->pa_version = pa_pu_read(PA_PU_PA_VERSION_REG);
  status->pa_pu_com_version = pa_pu_read(PA_PU_COM_VERSION_REG);
  status->rst_init_state = pa_pu_read(PA_PU_RST_INIT_STATE_REG);
  status->img_wr_state = pa_pu_read(PA_PU_IMG_WR_STATE_REG);
  status->img_wr_end = pa_pu_read(PA_PU_IMG_WR_END_REG);
  status->img_corr_state = pa_pu_read(PA_PU_IMG_CORR_STATE_REG);
  status->img_corr_end = pa_pu_read(PA_PU_IMG_CORR_END_REG);
}

int pa_pu_wait_irq(int timeout_ms, uint32_t* irq_count) {
  if (!g_using_uio || g_fd < 0) {
    return -2;
  }

  /* UIO 的中断事件通过 fd 可读体现，因此可以直接 poll。 */
  struct pollfd pfd = {
    .fd = g_fd,
    .events = POLLIN,
  };

  int ret = poll(&pfd, 1, timeout_ms);
  if (ret <= 0) {
    return ret;
  }

  uint32_t count = 0;
  if (read(g_fd, &count, sizeof(count)) != (ssize_t)sizeof(count)) {
    return -1;
  }

  /*
   * 标准 UIO 中断在 read 后需要写 1 重新使能。
   * 如果驱动不需要，该写入通常会被忽略或返回错误；这里不阻断主流程。
   */
  uint32_t enable = 1;
  (void)write(g_fd, &enable, sizeof(enable));

  if (irq_count != NULL) {
    *irq_count = count;
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

void pa_pu_start_correction(void) {
  /* 写 1 后由 FPGA 开始图像校正；完成状态通过 IMG_CORR_END/中断读取。 */
  pa_pu_write(PA_PU_IMG_CORR_STR_REG, 1);
}

void pa_pu_start_image_write(uint32_t image_addr) {
  /* 先配置原始图像地址，再触发写图。光口传输本身由 PA/FPGA 完成。 */
  pa_pu_write(PA_PU_IMG_WR_STR_ADDR_REG, image_addr);
  pa_pu_write(PA_PU_IMG_WR_STR_REG, 1);
}
