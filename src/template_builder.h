#pragma once

#include "fpga_mem.h"

/*
 * 模板文件和 FPGA 模板内存之间的转换接口。
 *
 * 文件中只保存有效图像区域 IMAGE_WIDTH x IMAGE_HEIGHT；
 * 写入 FPGA 内存时会铺回 DEVICE_WIDTH x DEVICE_HEIGHT 的整幅图布局，
 * 并按照 ROW_OFFSET/COL_OFFSET 放到正确位置。
 */

/* 从 TEMPLATE_OFFSET_FILE / TEMPLATE_GAIN_FILE 加载模板到 FPGA/PA 共享内存。 */
int template_load_files(fpga_mem_t* mem);

/* 用当前原始图像生成 offset 模板，并同时写入文件和 FPGA 模板内存。 */
int template_make_offset(fpga_mem_t* mem);

/* 用当前原始图像和 offset 模板生成 gain 模板，并同时写入文件和 FPGA 模板内存。 */
int template_make_gain(fpga_mem_t* mem);
