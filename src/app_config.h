#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * 全局编译配置。
 *
 * 这些宏都可以在 Makefile 命令行中覆盖，例如：
 *   make DEVICE_WIDTH=2540 DEVICE_HEIGHT=3072 RS422_DEVICE=/dev/ttyS1
 *
 * 注意：这里配置的是 ARM 应用看到的图像/内存布局，不等同于 FPGA RTL 内部
 * 的所有缓存深度。最终地址和尺寸需要与 FPGA、驱动、上位机协议三方保持一致。
 */

#ifndef DEVICE_WIDTH
/* FPGA 内存中一行的总像素数，包含有效图像之外的边界/偏移区域。 */
#define DEVICE_WIDTH 3072u
#endif

#ifndef DEVICE_HEIGHT
/* FPGA 内存中一帧的总行数，包含有效图像之外的边界/偏移区域。 */
#define DEVICE_HEIGHT 7716u
#endif

#ifndef IMAGE_WIDTH
/* 上位机和模板算法实际使用的有效图像宽度。 */
#define IMAGE_WIDTH 3072u
#endif

#ifndef IMAGE_HEIGHT
/* 上位机和模板算法实际使用的有效图像高度。 */
#define IMAGE_HEIGHT 7716u
#endif

#ifndef ROW_OFFSET
/* 有效图像区域在 FPGA 整幅图中的起始行偏移。 */
#define ROW_OFFSET 0u
#endif

#ifndef COL_OFFSET
/* 有效图像区域在 FPGA 整幅图中的起始列偏移。 */
#define COL_OFFSET 0u
#endif

#ifndef FPGA_IMAGE_PTR
/*
 * FPGA 写入原始图像的物理起始地址。
 * 当前值是早期占位配置；开发板上的 /dev/uio0 实际显示为 pu-pa-ddr，
 * 后续应与 FPGA/设备树确认最终共享 DDR 基址和分区。
 */
#define FPGA_IMAGE_PTR 0x20000000u
#endif

#ifndef FPGA_OFFSET_PTR
/* offset 模板写入 FPGA/PA 可访问内存的物理起始地址。 */
#define FPGA_OFFSET_PTR 0x26000000u
#endif

#ifndef FPGA_GAIN_PTR
/* gain 模板写入 FPGA/PA 可访问内存的物理起始地址。 */
#define FPGA_GAIN_PTR 0x28000000u
#endif

#ifndef FPGA_MEM_MAP_SIZE
/* ARM 通过 /dev/mem mmap 的总窗口大小，需覆盖 image/offset/gain 三段区域。 */
#define FPGA_MEM_MAP_SIZE 0x10000000u
#endif

#ifndef TEMPLATE_OFFSET_FILE
/* offset 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_OFFSET_FILE "/usr/local/offset.raw"
#endif

#ifndef TEMPLATE_GAIN_FILE
/* gain 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_GAIN_FILE "/usr/local/gain.raw"
#endif

/* 有效图像字节数：只包含上位机关心的 IMAGE_WIDTH x IMAGE_HEIGHT。 */
#define ACTIVE_IMAGE_BYTES ((size_t)IMAGE_WIDTH * IMAGE_HEIGHT * sizeof(uint16_t))

/* FPGA 整幅图字节数：包含 ROW_OFFSET/COL_OFFSET 对应的无效或保留区域。 */
#define DEVICE_IMAGE_BYTES ((size_t)DEVICE_WIDTH * DEVICE_HEIGHT * sizeof(uint16_t))
