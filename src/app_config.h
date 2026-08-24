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

#ifndef APP_BUILD_TIME
/* pa_controller 编译时间；Makefile 会注入更易读的本地时间字符串。 */
#define APP_BUILD_TIME __DATE__ " " __TIME__
#endif

#ifndef WORK_MODE_AUTO_START
/*
 * 程序启动后是否自动启动工作模式线程。
 * 0：只初始化寄存器/DDR/命令入口，不启动后台自清空；1：按 WORK_MODE_DEFAULT_MODE 启动。
 */
#define WORK_MODE_AUTO_START 0u
#endif

#ifndef WORK_MODE_DEFAULT_MODE
/*
 * 启动时默认进入的工作模式编号，沿用上一代 WorkMode 枚举。
 * 当前 ARM 侧只实现 0=Idle/Static Idle；其它模式会初始化但不会自动启动。
 */
#define WORK_MODE_DEFAULT_MODE 0u
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

#ifndef FPGA_MEM_MAP_SIZE
/* /dev/mem fallback 映射窗口大小；正常使用三个 UIO 节点分别映射。 */
#define FPGA_MEM_MAP_SIZE 0x1F000000u
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

#ifndef DDR_IMAGE_FRAME_ALIGN
/* 图像池中每张图的首地址/步进对齐，IMG_WR_STR_ADDR 协议要求至少 1KB 对齐。 */
#define DDR_IMAGE_FRAME_ALIGN 4096u
#endif

#ifndef DDR_IMAGE_POOL_FRAME_COUNT
/* Static Idle 输出图环形池最大帧数；当前 uio2 空间只能完整放 9 张图。 */
#define DDR_IMAGE_POOL_FRAME_COUNT 9u
#endif

#ifndef STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU
/*
 * Static Idle 第一帧 light 模板写入策略。
 * 0：FPGA 直接写 uio0/offset；1：FPGA 先写 uio2，再由 ARM 复制到 uio0。
 */
#define STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU 0u
#endif

#ifndef FPGA_MEM_USE_DEVMEM_FALLBACK
/* UIO 映射失败时是否回退 /dev/mem；默认关闭，避免误碰裸物理地址。 */
#define FPGA_MEM_USE_DEVMEM_FALLBACK 0u
#endif

#ifndef CORR_DEFAULT_ROW_NUM
/* CONFIG_CORR 默认校正行数，写入 IMG_ROW_NUM。 */
#define CORR_DEFAULT_ROW_NUM 7680u
#endif

#ifndef CORR_DEFAULT_COL_NUM
/* CONFIG_CORR 默认校正列数，写入 IMG_COL_NUM。 */
#define CORR_DEFAULT_COL_NUM 3072u
#endif

#ifndef CORR_DEFAULT_PKG_NUM
/*
 * CONFIG_CORR 不带参数时使用的默认图像校正分包数量。
 * 写入 IMG_PKG_NUM 寄存器，按 16bit 图像字节数除以 1KB 计算：
 *   img_pkg_num = 行 * 列 * 2 / 1024
 */
#define CORR_DEFAULT_PKG_NUM ((CORR_DEFAULT_ROW_NUM * CORR_DEFAULT_COL_NUM * 2u) / 1024u)
#endif

#ifndef CORR_DEFAULT_OFFSET_EN
/* CONFIG_CORR 默认是否启用 offset/暗场校正，写入 IMG_CORR_OFFSET_EN。 */
#define CORR_DEFAULT_OFFSET_EN 1u
#endif

#ifndef CORR_DEFAULT_OFFSET_ADDR
/* CONFIG_CORR 默认模板地址在启动后由 UIO map0 填充；这里只保留无硬件上下文占位值。 */
#define CORR_DEFAULT_OFFSET_ADDR 0u
#endif

#ifndef CORR_DEFAULT_OFFSET_ADDER_VALUE
/* CONFIG_CORR 默认 offset 校正附加值，写入 IMG_CORR_OFFSET_ADDER_VALUE。 */
#define CORR_DEFAULT_OFFSET_ADDER_VALUE 100u
#endif

#ifndef CORR_DEFAULT_OFFSET_CORR_MODE
/* CONFIG_CORR 默认 offset 校正模式，0=静态 offset，1=动态 offset。 */
#define CORR_DEFAULT_OFFSET_CORR_MODE 0u
#endif

#ifndef CORR_DEFAULT_GAIN_EN
/* CONFIG_CORR 默认是否启用 gain/亮场校正，写入 IMG_CORR_GAIN_EN。 */
#define CORR_DEFAULT_GAIN_EN 1u
#endif

#ifndef CORR_DEFAULT_GAIN_ADDR
/* CONFIG_CORR 默认模板地址在启动后由 UIO map0 填充；这里只保留无硬件上下文占位值。 */
#define CORR_DEFAULT_GAIN_ADDR 0u
#endif

#ifndef CORR_DEFAULT_GAIN_CLIPPING_VALUE
/* CONFIG_CORR 默认 gain 校正限幅值，写入 IMG_CORR_GAIN_CLIPPING_VALUE。 */
#define CORR_DEFAULT_GAIN_CLIPPING_VALUE 55000u
#endif

#ifndef CORR_DEFAULT_DEFECT_EN
/* CONFIG_CORR 默认是否启用坏点校正，写入 IMG_CORR_DEFECT_EN。 */
#define CORR_DEFAULT_DEFECT_EN 0u
#endif

#ifndef PA_PU_IRQ_TIMEOUT_MS
/* start 类命令等待 INT_VECTOR 对应完成 bit 的默认超时时间，单位 ms。 */
#define PA_PU_IRQ_TIMEOUT_MS 1000u
#endif

#ifndef PA_PU_IRQ_POLL_INTERVAL_US
/* 轮询 INT_VECTOR 的间隔。读取 INT_VECTOR 会清除已置位中断。 */
#define PA_PU_IRQ_POLL_INTERVAL_US 1000u
#endif

#ifndef PA_PU_IRQ_POLL_BUSY_WAIT
/* 无中断驱动回退轮询时是否用忙等替代 usleep，1=忙等，0=usleep。 */
#define PA_PU_IRQ_POLL_BUSY_WAIT 1u
#endif

#ifndef PA_PU_IRQ_POLL_BUSY_SPINS
/* 忙等轮询每轮空转次数，仅用于调试规避 Linux sleep 唤醒卡死问题。 */
#define PA_PU_IRQ_POLL_BUSY_SPINS 20000u
#endif

#ifndef PA_IRQ_DEVICE
/* 正式 PA/PU F2P 中断驱动节点；不存在时自动回退到 INT_VECTOR 轮询。 */
#define PA_IRQ_DEVICE "/dev/pa_irq"
#endif

#ifndef STATIC_IDLE_CLEAN_INTERVAL_MS
/* Static Idle 空闲自清空间隔，单位 ms。 */
#define STATIC_IDLE_CLEAN_INTERVAL_MS 50u
#endif

#ifndef STATIC_IDLE_CLEAN_LOG_ENABLE
/* Static Idle 自清空正常流程日志开关；0 仅打印错误，1 打印 start/wait/done。 */
#define STATIC_IDLE_CLEAN_LOG_ENABLE 0u
#endif

#ifndef STATIC_IDLE_EXPOSURE_MS
/* Static Idle 收到采图请求后的曝光窗口时间，单位 ms。 */
#define STATIC_IDLE_EXPOSURE_MS 50u
#endif

#ifndef STATIC_IDLE_DARK_WINDOW_MS
/* Static Idle 亮场采图完成后的暗场窗口时间，单位 ms。 */
#define STATIC_IDLE_DARK_WINDOW_MS 300u
#endif

#ifndef TEMPLATE_OFFSET_FILE
/* offset 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_OFFSET_FILE "/usr/local/offset.raw"
#endif

#ifndef TEMPLATE_GAIN_FILE
/* gain 模板落盘文件，现场生成后重启仍可加载。 */
#define TEMPLATE_GAIN_FILE "/usr/local/gain.raw"
#endif

#ifndef CAL_GAIN_DIR
/* gain/defect 多灰阶校准的均值图中间文件目录。 */
#define CAL_GAIN_DIR "/usr/local/calib"
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
#define GIC_DEFAULT_LINE_TIME_NS 25600u
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
#define GIC_DEFAULT_BINNING 0u
#endif

#ifndef ROIC_DEFAULT_START_COL
#define ROIC_DEFAULT_START_COL 0u
#endif

#ifndef ROIC_DEFAULT_END_COL
#define ROIC_DEFAULT_END_COL (IMAGE_WIDTH - 1u)
#endif

#ifndef ROIC_DEFAULT_BINNING
#define ROIC_DEFAULT_BINNING 0u
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
