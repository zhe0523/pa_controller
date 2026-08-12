#include "fpga_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

static int map_uio_region(const char* device, size_t size, uint8_t** ptr_out) {
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

  mem->offset_map_size = FPGA_OFFSET_UIO_SIZE;
  mem->gain_map_size = FPGA_GAIN_UIO_SIZE;
  mem->image_map_size = FPGA_IMAGE_UIO_SIZE;

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

  log_info("fpga memory mapped by uio image=%s/%p offset=%s/%p gain=%s/%p",
           FPGA_IMAGE_UIO_DEVICE,
           mem->image,
           FPGA_OFFSET_UIO_DEVICE,
           mem->offset_template,
           FPGA_GAIN_UIO_DEVICE,
           mem->gain_template);
  return 0;
}

#if FPGA_MEM_USE_DEVMEM_FALLBACK
static int fpga_mem_open_devmem(fpga_mem_t* mem) {
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd == -1) {
    log_error("open /dev/mem failed: %d", errno);
    return -1;
  }

  void* ptr = mmap(NULL, FPGA_MEM_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, FPGA_IMAGE_PTR);
  if (ptr == MAP_FAILED) {
    log_error("mmap fpga memory 0x%x failed: %d", FPGA_IMAGE_PTR, errno);
    close(fd);
    return -1;
  }

  mem->image = (uint8_t*)ptr;
  mem->offset_template = mem->image + (FPGA_OFFSET_PTR - FPGA_IMAGE_PTR);
  mem->gain_template = mem->image + (FPGA_GAIN_PTR - FPGA_IMAGE_PTR);
  mem->image_map_size = FPGA_MEM_MAP_SIZE;
  mem->image_fd = fd;
  mem->offset_fd = -1;
  mem->gain_fd = -1;

  log_info("fpga memory mapped by /dev/mem image=%p offset=%p gain=%p", mem->image, mem->offset_template, mem->gain_template);
  return 0;
}
#endif

static bool fpga_mem_layout_is_valid(const fpga_mem_t* mem) {
  const size_t gain_bytes = (size_t)GAIN_TEMPLATE_REPEAT_COUNT * DEVICE_IMAGE_BYTES;

  if (mem->image_map_size < DEVICE_IMAGE_BYTES) {
    log_error("image uio window too small: need=%lu size=%lu",
              (unsigned long)DEVICE_IMAGE_BYTES,
              (unsigned long)mem->image_map_size);
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

  if (mem->offset_fd == -1 && mem->gain_fd == -1 && mem->image_fd != -1 && mem->offset_template != NULL) {
    unmap_region(&mem->image, &mem->image_map_size, &mem->image_fd);
  } else {
    unmap_region(&mem->image, &mem->image_map_size, &mem->image_fd);
    unmap_region(&mem->offset_template, &mem->offset_map_size, &mem->offset_fd);
    unmap_region(&mem->gain_template, &mem->gain_map_size, &mem->gain_fd);
  }

  memset(mem, 0, sizeof(*mem));
  mem->image_fd = -1;
  mem->offset_fd = -1;
  mem->gain_fd = -1;
}

bool fpga_mem_is_open(const fpga_mem_t* mem) {
  return mem != NULL && mem->image != NULL && mem->offset_template != NULL && mem->gain_template != NULL;
}
