#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * FPGA/PA 共享图像内存映射。
 *
 * 当前目标板通过三个 UIO 节点分别映射：
 *   /dev/uio0: 暗场图/offset 模板区
 *   /dev/uio1: 亮场图/gain 模板区
 *   /dev/uio2: 实际输出图环形图像池
 *
 * 物理地址和映射大小来自 app_config.h/Makefile，必须和设备树 UIO 布局保持一致。
 */
typedef struct {
  /* 实际输出图缓冲区虚拟地址，对应 FPGA_IMAGE_UIO_DEVICE。 */
  uint8_t* image;
  /* offset 模板缓冲区虚拟地址，对应 FPGA_OFFSET_UIO_DEVICE。 */
  uint8_t* offset_template;
  /* gain 模板缓冲区虚拟地址，对应 FPGA_GAIN_UIO_DEVICE。 */
  uint8_t* gain_template;
  /* Static Idle DDR 图像池虚拟地址；当前与 image 指向同一个 uio2 映射。 */
  uint8_t* image_pool;
  /* 三块 UIO 的物理基地址，写给 FPGA 寄存器时使用。 */
  uint32_t image_phys_base;
  uint32_t offset_phys_base;
  uint32_t gain_phys_base;
  /* Static Idle DDR 图像池物理基地址，写 IMG_WR_STR_ADDR 时使用。 */
  uint32_t image_pool_phys_base;
  /* /dev/uio2 图像缓冲区映射大小。 */
  size_t image_map_size;
  /* /dev/uio0 offset 模板区映射大小。 */
  size_t offset_map_size;
  /* /dev/uio1 gain 模板区映射大小。 */
  size_t gain_map_size;
  /* /dev/uio2 DDR 图像池映射大小。 */
  size_t image_pool_map_size;
  /* 各 UIO 节点 fd，close 时按映射来源逐个释放。 */
  int image_fd;
  int offset_fd;
  int gain_fd;
  int image_pool_fd;
} fpga_mem_t;

/* 打开并映射 FPGA/PA 共享内存。 */
int fpga_mem_open(fpga_mem_t* mem);

/* 关闭共享内存映射。 */
void fpga_mem_close(fpga_mem_t* mem);

/* 判断共享内存是否已经成功映射。 */
bool fpga_mem_is_open(const fpga_mem_t* mem);
