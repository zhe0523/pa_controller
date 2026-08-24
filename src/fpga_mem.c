#include "fpga_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

static const char* uio_device_name(const char* device) {
  const char* slash = strrchr(device, '/');
  return slash != NULL ? slash + 1 : device;
}

static int read_uio_map_value(const char* device, const char* name, unsigned long long* value_out) {
  char path[160];
  char text[64];

  if (device == NULL || name == NULL || value_out == NULL) {
    return -1;
  }

  /*
   * 共享 DDR 的物理地址和窗口大小以设备树 UIO map0 为准。
   * 这样设备树调整内存布局后，应用只需要继续指向正确的 /dev/uioX。
   */
  int written = snprintf(path,
                         sizeof(path),
                         "/sys/class/uio/%s/maps/map0/%s",
                         uio_device_name(device),
                         name);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    log_error("uio sysfs path too long device=%s name=%s", device, name);
    return -1;
  }

  FILE* fp = fopen(path, "r");
  if (fp == NULL) {
    log_error("open %s failed: %d", path, errno);
    return -1;
  }

  if (fscanf(fp, "%63s", text) != 1) {
    log_error("read %s failed", path);
    fclose(fp);
    return -1;
  }
  fclose(fp);

  errno = 0;
  char* end = NULL;
  unsigned long long value = strtoull(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0') {
    log_error("parse %s value=%s failed errno=%d", path, text, errno);
    return -1;
  }

  *value_out = value;
  return 0;
}

static int read_uio_map_info(const char* device, uint32_t* phys_base_out, size_t* size_out) {
  unsigned long long addr = 0;
  unsigned long long size = 0;

  if (read_uio_map_value(device, "addr", &addr) != 0 ||
      read_uio_map_value(device, "size", &size) != 0) {
    return -1;
  }

  if (addr > 0xffffffffull || size == 0 || size > (unsigned long long)((size_t)-1)) {
    log_error("invalid uio map device=%s addr=0x%llx size=0x%llx", device, addr, size);
    return -1;
  }

  *phys_base_out = (uint32_t)addr;
  *size_out = (size_t)size;
  return 0;
}

static int map_uio_region(const char* device, size_t size, uint8_t** ptr_out) {
  /* 所有共享 DDR 都按 UIO map0 映射，返回 fd 供 fpga_mem_close 释放。 */
  int fd = open(device, O_RDWR | O_CLOEXEC);
  if (fd == -1) {
    log_error("open %s failed: %d", device, errno);
    return -1;
  }

  void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    log_error("mmap %s size=0x%lx failed: %d", device, (unsigned long)size, errno);
    close(fd);
    return -1;
  }

  *ptr_out = (uint8_t*)ptr;
  return fd;
}

static void unmap_region(uint8_t** ptr, size_t* size, int* fd) {
  if (ptr != NULL && *ptr != NULL) {
    munmap(*ptr, *size);
    *ptr = NULL;
  }

  if (fd != NULL && *fd != -1) {
    close(*fd);
    *fd = -1;
  }

  if (size != NULL) {
    *size = 0;
  }
}

static int fpga_mem_open_uio(fpga_mem_t* mem) {
  mem->image_fd = -1;
  mem->offset_fd = -1;
  mem->gain_fd = -1;
  mem->image_pool_fd = -1;

  /*
   * 当前板上只有 uio0/uio1/uio2：
   * uio0/uio1 用于 offset/gain 模板区，uio2 用于 Static Idle 实际输出图环形池。
   * 物理地址和 size 直接读取 UIO map0，避免 Makefile 与设备树重复维护内存布局。
   */
  if (read_uio_map_info(FPGA_OFFSET_UIO_DEVICE, &mem->offset_phys_base, &mem->offset_map_size) != 0 ||
      read_uio_map_info(FPGA_GAIN_UIO_DEVICE, &mem->gain_phys_base, &mem->gain_map_size) != 0 ||
      read_uio_map_info(FPGA_IMAGE_UIO_DEVICE, &mem->image_phys_base, &mem->image_map_size) != 0) {
    return -1;
  }
  mem->image_pool = NULL;
  mem->image_pool_phys_base = mem->image_phys_base;
  mem->image_pool_map_size = mem->image_map_size;

  mem->offset_fd = map_uio_region(FPGA_OFFSET_UIO_DEVICE, mem->offset_map_size, &mem->offset_template);
  if (mem->offset_fd == -1) {
    return -1;
  }

  mem->gain_fd = map_uio_region(FPGA_GAIN_UIO_DEVICE, mem->gain_map_size, &mem->gain_template);
  if (mem->gain_fd == -1) {
    return -1;
  }

  mem->image_fd = map_uio_region(FPGA_IMAGE_UIO_DEVICE, mem->image_map_size, &mem->image);
  if (mem->image_fd == -1) {
    return -1;
  }

  /*
   * Static Idle 输出图环形池就是 image/uio2。保留 image_pool 字段是为了工作模式代码语义清晰，
   * 但不再额外暴露第二个 uio2 配置。
   */
  mem->image_pool = mem->image;
  mem->image_pool_fd = -1;

  log_info("fpga memory mapped by uio image=%s/%p phys=0x%08x size=0x%lx offset=%s/%p phys=0x%08x size=0x%lx gain=%s/%p phys=0x%08x size=0x%lx",
           FPGA_IMAGE_UIO_DEVICE,
           mem->image,
           mem->image_phys_base,
           (unsigned long)mem->image_map_size,
           FPGA_OFFSET_UIO_DEVICE,
           mem->offset_template,
           mem->offset_phys_base,
           (unsigned long)mem->offset_map_size,
           FPGA_GAIN_UIO_DEVICE,
           mem->gain_template,
           mem->gain_phys_base,
           (unsigned long)mem->gain_map_size);
  return 0;
}

#if FPGA_MEM_USE_DEVMEM_FALLBACK
static int fpga_mem_open_devmem(fpga_mem_t* mem) {
  (void)mem;
  /*
   * 当前三块 DDR 是离散 UIO 区域，不能再按一个 /dev/mem 连续窗口推导偏移。
   * 正式路径使用 UIO 节点，物理地址和窗口大小由设备树 UIO map0 给出。
   */
  log_error("split DDR layout requires UIO mapping; /dev/mem fallback is not supported");
  return -1;
}
#endif

static bool fpga_mem_layout_is_valid(const fpga_mem_t* mem) {
  /* 启动阶段做一次保守检查，避免后续模板生成或采图写地址时越过映射窗口。 */
  if (mem->image_map_size < DEVICE_IMAGE_BYTES) {
    log_error("image uio window too small: need=%lu size=%lu",
              (unsigned long)DEVICE_IMAGE_BYTES,
              (unsigned long)mem->image_map_size);
    return false;
  }

  if (mem->image_pool_map_size < ACTIVE_IMAGE_BYTES || mem->image_pool == NULL || mem->image_pool_phys_base == 0) {
    log_error("image pool invalid: need=%lu size=%lu phys=0x%08x",
              (unsigned long)ACTIVE_IMAGE_BYTES,
              (unsigned long)mem->image_pool_map_size,
              mem->image_pool_phys_base);
    return false;
  }

  if (mem->offset_map_size < DEVICE_IMAGE_BYTES) {
    log_error("offset uio window too small: need=%lu size=%lu",
              (unsigned long)DEVICE_IMAGE_BYTES,
              (unsigned long)mem->offset_map_size);
    return false;
  }

  if (mem->gain_map_size != 0 && mem->gain_map_size < DEVICE_IMAGE_BYTES) {
    log_error("gain uio window too small: need=%lu size=%lu",
              (unsigned long)DEVICE_IMAGE_BYTES,
              (unsigned long)mem->gain_map_size);
    return false;
  }

  return true;
}

int fpga_mem_open(fpga_mem_t* mem) {
  if (mem == NULL) {
    return -1;
  }

  memset(mem, 0, sizeof(*mem));
  mem->image_fd = -1;
  mem->offset_fd = -1;
  mem->gain_fd = -1;
  mem->image_pool_fd = -1;

  if (fpga_mem_open_uio(mem) == 0 && fpga_mem_layout_is_valid(mem)) {
    return 0;
  }

  fpga_mem_close(mem);
#if FPGA_MEM_USE_DEVMEM_FALLBACK
  log_warn("fpga uio mmap failed, fallback to /dev/mem");
  if (fpga_mem_open_devmem(mem) == 0 && fpga_mem_layout_is_valid(mem)) {
    return 0;
  }
  fpga_mem_close(mem);
  return -1;
#else
  return -1;
#endif
}

void fpga_mem_close(fpga_mem_t* mem) {
  if (mem == NULL) {
    return;
  }

  bool image_pool_alias_image = mem->image_pool != NULL && mem->image_pool == mem->image;
  if (mem->offset_fd == -1 && mem->gain_fd == -1 && mem->image_fd != -1 && mem->offset_template != NULL) {
    unmap_region(&mem->image, &mem->image_map_size, &mem->image_fd);
  } else {
    unmap_region(&mem->image, &mem->image_map_size, &mem->image_fd);
    unmap_region(&mem->offset_template, &mem->offset_map_size, &mem->offset_fd);
    unmap_region(&mem->gain_template, &mem->gain_map_size, &mem->gain_fd);
    if (!image_pool_alias_image) {
      unmap_region(&mem->image_pool, &mem->image_pool_map_size, &mem->image_pool_fd);
    }
  }

  memset(mem, 0, sizeof(*mem));
  mem->image_fd = -1;
  mem->offset_fd = -1;
  mem->gain_fd = -1;
  mem->image_pool_fd = -1;
}

bool fpga_mem_is_open(const fpga_mem_t* mem) {
  return mem != NULL &&
         mem->image != NULL &&
         mem->offset_template != NULL &&
         mem->gain_template != NULL &&
         mem->image_pool != NULL &&
         mem->image_pool_phys_base != 0;
}
