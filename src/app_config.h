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

#ifndef APP_VERSION
/* pa_controller 应用软件版本号。发布时可通过 make APP_VERSION=x.y.z 覆盖。 */
#define APP_VERSION "0.1.0"
#endif

#ifndef DEVICE_WIDTH
/* FPGA 内存中一行的总像素数，包含有效图像之外的边界/偏移区域。 */
#define DEVICE_WIDTH 3072u
#endif

#ifndef DEVICE_HEIGHT
/* FPGA 内存中一帧的总行数，包含有效图像之外的边界/偏移区域。 */
#define DEVICE_HEIGHT 7680u
#endif

#ifndef IMAGE_WIDTH
/* 上位机和模板算法实际使用的有效图像宽度。 */
#define IMAGE_WIDTH 3072u
#endif

#ifndef IMAGE_HEIGHT
/* 上位机和模板算法实际使用的有效图像高度。 */
#define IMAGE_HEIGHT 7680u
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
 * 对应设备树 pu-pa-ddr2，当前用于收取图片以制作暗场/亮场模板。
 */
#define FPGA_IMAGE_PTR 0x28000000u
#endif

#ifndef FPGA_OFFSET_PTR
/* offset/暗场模板写入 FPGA/PA 可访问内存的物理起始地址，对应 pu-pa-ddr。 */
#define FPGA_OFFSET_PTR 0x3C000000u
#endif

#ifndef FPGA_GAIN_PTR
/* gain/亮场模板写入 FPGA/PA 可访问内存的物理起始地址，对应 pu-pa-ddr1。 */
#define FPGA_GAIN_PTR 0x38000000u
#endif

#ifndef FPGA_MEM_MAP_SIZE
/* 兼容旧 /dev/mem 映射的总窗口大小；当前默认使用三个 UIO 节点分别映射。 */
#define FPGA_MEM_MAP_SIZE 0x10000000u
#endif

#ifndef FPGA_IMAGE_UIO_DEVICE
#define FPGA_IMAGE_UIO_DEVICE "/dev/uio2"
#endif

#ifndef FPGA_OFFSET_UIO_DEVICE
#define FPGA_OFFSET_UIO_DEVICE "/dev/uio0"
#endif

#ifndef FPGA_GAIN_UIO_DEVICE
#define FPGA_GAIN_UIO_DEVICE "/dev/uio1"
#endif

#ifndef FPGA_IMAGE_UIO_SIZE
#define FPGA_IMAGE_UIO_SIZE 0x10000000u
#endif

#ifndef FPGA_OFFSET_UIO_SIZE
#define FPGA_OFFSET_UIO_SIZE 0x04000000u
#endif

#ifndef FPGA_GAIN_UIO_SIZE
#define FPGA_GAIN_UIO_SIZE 0x04000000u
#endif

#ifndef FPGA_MEM_USE_DEVMEM_FALLBACK
/* UIO 映射失败时是否回退 /dev/mem；默认关闭，避免误碰裸物理地址。 */
#define FPGA_MEM_USE_DEVMEM_FALLBACK 0u
#endif

#ifndef GAIN_TEMPLATE_REPEAT_COUNT
/* 新亮场 UIO 窗口为 64MB，默认只存放一份有效 gain 模板。 */
#define GAIN_TEMPLATE_REPEAT_COUNT 1u
#endif

#ifndef PA_PU_IRQ_TIMEOUT_MS
/* start 类命令等待 INT_VECTOR 对应完成 bit 的默认超时时间。 */
#define PA_PU_IRQ_TIMEOUT_MS 5000u
#endif

#ifndef PA_PU_IRQ_POLL_INTERVAL_US
/* 轮询 INT_VECTOR 的间隔。读取 INT_VECTOR 会清除已置位中断。 */
#define PA_PU_IRQ_POLL_INTERVAL_US 1000u
#endif

#ifndef PA_IRQ_DEVICE
/* 正式 PA/PU F2P 中断驱动节点；不存在时自动回退到 INT_VECTOR 轮询。 */
#define PA_IRQ_DEVICE "/dev/pa_irq"
#endif

#ifndef TEMPLATE_OFFSET_FILE
/* offset 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_OFFSET_FILE "/usr/local/offset.raw"
#endif

#ifndef TEMPLATE_GAIN_FILE
/* gain 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_GAIN_FILE "/usr/local/gain.raw"
#endif

#ifndef GIC_DEFAULT_REQ_CODE
/* 默认 GIC 请求模式：0 串行扫描，1 并行扫描，2 xao 扫描。 */
#define GIC_DEFAULT_REQ_CODE 0u
#endif

#ifndef GIC_DEFAULT_DOUT_EN
/* 串行扫描模式下的 GIC 数据输出使能。 */
#define GIC_DEFAULT_DOUT_EN 1u
#endif

#ifndef GIC_DEFAULT_LINE_TIME_NS
/* 默认 GIC 行时间，单位 ns；现场应按 panel 时序覆盖。 */
#define GIC_DEFAULT_LINE_TIME_NS 0u
#endif

#ifndef GIC_DEFAULT_OE_RISE_NS
/* 默认 GIC OE 上升沿时间，单位 ns；现场应按 panel 时序覆盖。 */
#define GIC_DEFAULT_OE_RISE_NS 0u
#endif

#ifndef GIC_DEFAULT_OE_FALL_NS
/* 默认 GIC OE 下降沿时间，单位 ns；现场应按 panel 时序覆盖。 */
#define GIC_DEFAULT_OE_FALL_NS 0u
#endif

#ifndef GIC_DEFAULT_START_ROW
#define GIC_DEFAULT_START_ROW 0u
#endif

#ifndef GIC_DEFAULT_END_ROW
#define GIC_DEFAULT_END_ROW (IMAGE_HEIGHT - 1u)
#endif

#ifndef GIC_DEFAULT_BINNING
#define GIC_DEFAULT_BINNING 1u
#endif

#ifndef ROIC_DEFAULT_START_COL
#define ROIC_DEFAULT_START_COL 0u
#endif

#ifndef ROIC_DEFAULT_END_COL
#define ROIC_DEFAULT_END_COL (IMAGE_WIDTH - 1u)
#endif

#ifndef ROIC_DEFAULT_BINNING
#define ROIC_DEFAULT_BINNING 1u
#endif

#ifndef ROIC_DEFAULT_REG_00
/* ROIC 芯片寄存器默认值需由 panel 测试方确认，当前只提供可覆盖占位值。 */
#define ROIC_DEFAULT_REG_00 0u
#endif
#ifndef ROIC_DEFAULT_REG_02
#define ROIC_DEFAULT_REG_02 0u
#endif
#ifndef ROIC_DEFAULT_REG_05
#define ROIC_DEFAULT_REG_05 0u
#endif
#ifndef ROIC_DEFAULT_REG_06
#define ROIC_DEFAULT_REG_06 0u
#endif
#ifndef ROIC_DEFAULT_REG_07
#define ROIC_DEFAULT_REG_07 0u
#endif
#ifndef ROIC_DEFAULT_REG_09
#define ROIC_DEFAULT_REG_09 0u
#endif
#ifndef ROIC_DEFAULT_REG_0A
#define ROIC_DEFAULT_REG_0A 0u
#endif
#ifndef ROIC_DEFAULT_REG_0B
#define ROIC_DEFAULT_REG_0B 0u
#endif
#ifndef ROIC_DEFAULT_REG_0C
#define ROIC_DEFAULT_REG_0C 0u
#endif
#ifndef ROIC_DEFAULT_REG_0D
#define ROIC_DEFAULT_REG_0D 0u
#endif
#ifndef ROIC_DEFAULT_REG_0E
#define ROIC_DEFAULT_REG_0E 0u
#endif
#ifndef ROIC_DEFAULT_REG_0F
#define ROIC_DEFAULT_REG_0F 0u
#endif
#ifndef ROIC_DEFAULT_REG_10
#define ROIC_DEFAULT_REG_10 0u
#endif
#ifndef ROIC_DEFAULT_REG_11
#define ROIC_DEFAULT_REG_11 0u
#endif
#ifndef ROIC_DEFAULT_REG_17
#define ROIC_DEFAULT_REG_17 0u
#endif
#ifndef ROIC_DEFAULT_REG_24
#define ROIC_DEFAULT_REG_24 0u
#endif
#ifndef ROIC_DEFAULT_REG_28
#define ROIC_DEFAULT_REG_28 0u
#endif
#ifndef ROIC_DEFAULT_REG_2D
#define ROIC_DEFAULT_REG_2D 0u
#endif
#ifndef ROIC_DEFAULT_REG_3B
#define ROIC_DEFAULT_REG_3B 0u
#endif

/* 有效图像字节数：只包含上位机关心的 IMAGE_WIDTH x IMAGE_HEIGHT。 */
#define ACTIVE_IMAGE_BYTES ((size_t)IMAGE_WIDTH * IMAGE_HEIGHT * sizeof(uint16_t))

/* FPGA 整幅图字节数：包含 ROW_OFFSET/COL_OFFSET 对应的无效或保留区域。 */
#define DEVICE_IMAGE_BYTES ((size_t)DEVICE_WIDTH * DEVICE_HEIGHT * sizeof(uint16_t))
