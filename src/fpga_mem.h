#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * FPGA/PA 共享图像内存映射。
 *
 * 当前目标板通过三个 UIO 节点分别映射：
 *   /dev/uio0: offset/暗场模板
 *   /dev/uio1: gain/亮场模板
 *   /dev/uio2: 原始图像采集缓冲区
 */
typedef struct {
  /* 原始图像缓冲区虚拟地址，对应 FPGA_IMAGE_PTR。 */
  uint8_t* image;
  /* offset 模板缓冲区虚拟地址，对应 FPGA_OFFSET_PTR。 */
  uint8_t* offset_template;
  /* gain 模板缓冲区虚拟地址，对应 FPGA_GAIN_PTR。 */
  uint8_t* gain_template;
  size_t image_map_size;
  size_t offset_map_size;
  size_t gain_map_size;
  int image_fd;
  int offset_fd;
  int gain_fd;
} fpga_mem_t;

/* 打开并映射 FPGA/PA 共享内存。 */
int fpga_mem_open(fpga_mem_t* mem);

/* 关闭共享内存映射。 */
void fpga_mem_close(fpga_mem_t* mem);

/* 判断共享内存是否已经成功映射。 */
bool fpga_mem_is_open(const fpga_mem_t* mem);
