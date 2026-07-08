#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * FPGA/PA 共享图像内存映射。
 *
 * 当前实现通过 /dev/mem 映射 FPGA_IMAGE_PTR 开始的一段连续物理内存，
 * 并在这个窗口内按固定偏移访问 image / offset / gain 三个区域。
 *
 * 后续目标板建议改成通过名为 pu-pa-ddr 的 UIO 节点映射共享 DDR，
 * 避免应用直接依赖 /dev/mem 权限和裸物理地址。
 */
typedef struct {
  /* 原始图像缓冲区虚拟地址，对应 FPGA_IMAGE_PTR。 */
  uint8_t* image;
  /* offset 模板缓冲区虚拟地址，对应 FPGA_OFFSET_PTR。 */
  uint8_t* offset_template;
  /* gain 模板缓冲区虚拟地址，对应 FPGA_GAIN_PTR。 */
  uint8_t* gain_template;
  /* mmap 的总长度，用于 munmap。 */
  size_t map_size;
} fpga_mem_t;

/* 打开并映射 FPGA/PA 共享内存。 */
int fpga_mem_open(fpga_mem_t* mem);

/* 关闭共享内存映射。 */
void fpga_mem_close(fpga_mem_t* mem);

/* 判断共享内存是否已经成功映射。 */
bool fpga_mem_is_open(const fpga_mem_t* mem);
