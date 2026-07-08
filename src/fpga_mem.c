#include "fpga_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

/* /dev/mem 文件描述符；当前实现假设 image/offset/gain 在一段连续物理窗口内。 */
static int g_mem_fd = -1;

int fpga_mem_open(fpga_mem_t* mem) {
  if (mem == NULL) {
    return -1;
  }

  memset(mem, 0, sizeof(*mem));

  /* 早期 bring-up 先使用 /dev/mem；量产建议改成 UIO 或专用字符设备。 */
  g_mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (g_mem_fd == -1) {
    log_error("open /dev/mem failed: %d", errno);
    return -1;
  }

  /* 从 FPGA_IMAGE_PTR 开始映射一整段窗口，offset/gain 通过窗口内偏移访问。 */
  void* ptr = mmap(NULL, FPGA_MEM_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_mem_fd, FPGA_IMAGE_PTR);
  if (ptr == MAP_FAILED) {
    log_error("mmap fpga memory 0x%x failed: %d", FPGA_IMAGE_PTR, errno);
    close(g_mem_fd);
    g_mem_fd = -1;
    return -1;
  }

  mem->image = (uint8_t*)ptr;

  /* 三段物理地址必须位于同一个 mmap 窗口内，否则这里的指针换算会失效。 */
  mem->offset_template = mem->image + (FPGA_OFFSET_PTR - FPGA_IMAGE_PTR);
  mem->gain_template = mem->image + (FPGA_GAIN_PTR - FPGA_IMAGE_PTR);
  mem->map_size = FPGA_MEM_MAP_SIZE;

  log_info("fpga memory mapped image=%p offset=%p gain=%p", mem->image, mem->offset_template, mem->gain_template);
  return 0;
}

void fpga_mem_close(fpga_mem_t* mem) {
  if (mem != NULL && mem->image != NULL) {
    munmap(mem->image, mem->map_size);
    memset(mem, 0, sizeof(*mem));
  }

  if (g_mem_fd != -1) {
    close(g_mem_fd);
    g_mem_fd = -1;
  }
}

bool fpga_mem_is_open(const fpga_mem_t* mem) {
  return mem != NULL && mem->image != NULL;
}
