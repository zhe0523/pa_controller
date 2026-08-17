#include "fpga_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

static bool uio_device_index(const char* device, unsigned* index_out) {
  /* 从 /dev/uioN 解析出 N，用于定位 /sys/class/uio/uioN/maps/map0。 */
  const char* prefix = "/dev/uio";
  char* end = NULL;
  unsigned long value = 0;

  if (device == NULL || strncmp(device, prefix, strlen(prefix)) != 0 || index_out == NULL) {
    return false;
  }

  errno = 0;
  value = strtoul(device + strlen(prefix), &end, 10);
  if (errno != 0 || end == device + strlen(prefix) || *end != '\0' || value > UINT_MAX) {
    return false;
  }

  *index_out = (unsigned)value;
  return true;
}

static bool read_uio_map_value(const char* device, const char* name, unsigned long* value_out) {
  unsigned index = 0;
  char path[96];
  FILE* fp = NULL;
  unsigned long value = 0;

  if (value_out == NULL || !uio_device_index(device, &index)) {
    return false;
  }

  snprintf(path, sizeof(path), "/sys/class/uio/uio%u/maps/map0/%s", index, name);
  fp = fopen(path, "r");
  if (fp == NULL) {
    log_error("open %s failed: %d", path, errno);
    return false;
  }

  if (fscanf(fp, "%lx", &value) != 1) {
    log_error("read %s failed", path);
    fclose(fp);
    return false;
  }

  fclose(fp);
  *value_out = value;
  return true;
}

static bool read_uio_region_info(const char* device, uint32_t* phys_out, size_t* size_out) {
  unsigned long phys = 0;
  unsigned long size = 0;

  if (!read_uio_map_value(device, "addr", &phys) ||
      !read_uio_map_value(device, "size", &size) ||
      phys > UINT32_MAX ||
      size == 0) {
    return false;
  }

  if (phys_out != NULL) {
    *phys_out = (uint32_t)phys;
  }
  if (size_out != NULL) {
    *size_out = (size_t)size;
  }
  return true;
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
   * 物理地址和 size 以设备树暴露到 UIO sysfs 的 map0 为准。
   */
  if (!read_uio_region_info(FPGA_OFFSET_UIO_DEVICE, &mem->offset_phys_base, &mem->offset_map_size) ||
      !read_uio_region_info(FPGA_GAIN_UIO_DEVICE, &mem->gain_phys_base, &mem->gain_map_size) ||
      !read_uio_region_info(FPGA_IMAGE_UIO_DEVICE, &mem->image_phys_base, &mem->image_map_size)) {
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
   * 当前设备树把 offset/gain/image 放在三段独立 UIO 区域，并且 image 不再覆盖 offset/gain。
   * 单个 /dev/mem 窗口很容易算错偏移，因此正式路径只支持 UIO sysfs 描述的布局。
   */
  log_error("split DDR layout requires UIO mapping; /dev/mem fallback is not supported");
  return -1;
}
#endif

static bool fpga_mem_layout_is_valid(const fpga_mem_t* mem) {
  const size_t gain_bytes = (size_t)GAIN_TEMPLATE_REPEAT_COUNT * DEVICE_IMAGE_BYTES;

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

  if (mem->gain_map_size != 0 && mem->gain_map_size < gain_bytes) {
    log_error("gain uio window too small: need=%lu size=%lu",
              (unsigned long)gain_bytes,
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
