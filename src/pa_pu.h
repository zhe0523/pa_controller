#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pa_pu_regs.h"

#ifndef PA_PU_MAP_SIZE
/* PA 寄存器映射窗口大小。当前 pa_pu_com.v 寄存器范围小于 4KB，因此默认 0x1000。 */
#define PA_PU_MAP_SIZE 0x1000u
#endif

#ifndef PA_PU_UIO_DEVICE
/*
 * PA 寄存器 UIO 设备名。
 * 注意：当前开发板上的 /dev/uio0 名字是 pu-pa-ddr，更像共享 DDR，不是 PA 寄存器。
 * 真板阶段建议把 PA 寄存器单独暴露为 /dev/uio1 或明确命名的 UIO 节点。
 */
#define PA_PU_UIO_DEVICE "/dev/uio0"
#endif

/*
 * 图像校正参数。
 *
 * ARM 通过这些字段一次性配置 PA/FPGA 校正模块：
 * 原始图像 -> offset 扣除 -> gain 校正 -> 可选坏点校正 -> 后续写图/传图。
 */
typedef struct {
  /* 图像分包数/并行包数，具体硬件含义由 IMG_CORR 模块定义。 */
  uint16_t pkg_num;
  /* 有效图像行数。 */
  uint16_t row_num;
  /* 有效图像列数。 */
  uint16_t col_num;
  /* 是否启用 offset 校正。 */
  bool offset_enable;
  /* offset 模板物理地址，必须是 PA/FPGA 可访问地址。 */
  uint32_t offset_template_addr;
  /* offset 扣除后的附加补偿值，公式需 FPGA 确认。 */
  uint16_t offset_adder_value;
  /* 是否启用 gain 校正。 */
  bool gain_enable;
  /* gain 模板物理地址，必须是 PA/FPGA 可访问地址。 */
  uint32_t gain_template_addr;
  /* gain 输出限幅值，避免校正后溢出。 */
  uint16_t gain_clipping_value;
  /* 是否启用坏点校正；当前项目先默认关闭。 */
  bool defect_enable;
} pa_pu_corr_config_t;

/* GIC 采集时序和行方向 ROI 配置。 */
typedef struct {
  uint8_t req_code;
  bool dout_enable;
  uint32_t line_time_ns;
  uint32_t oe_raising_edge_ns;
  uint32_t oe_falling_edge_ns;
  uint16_t start_row;
  uint16_t end_row;
  uint8_t binning_mode;
} pa_pu_gic_config_t;

/* ROIC 寄存器配置和列方向 ROI 配置。 */
typedef struct {
  uint16_t reg_00;
  uint16_t reg_02;
  uint16_t reg_05;
  uint16_t reg_06;
  uint16_t reg_07;
  uint16_t reg_09;
  uint16_t reg_0a;
  uint16_t reg_0b;
  uint16_t reg_0c;
  uint16_t reg_0d;
  uint16_t reg_0e;
  uint16_t reg_0f;
  uint16_t reg_10;
  uint16_t reg_11;
  uint16_t reg_17;
  uint16_t reg_24;
  uint16_t reg_28;
  uint16_t reg_2d;
  uint16_t reg_3b;
  uint16_t start_col;
  uint16_t end_col;
  uint8_t binning_mode;
} pa_pu_roic_config_t;

/* STATUS 命令返回给上位机的核心 PA 状态快照。 */
typedef struct {
  /* 中断向量寄存器，bit 定义见 PA_PU_IRQ_*。 */
  uint32_t int_vector;
  /* PA 版本号。 */
  uint32_t pa_version;
  /* PA/PU 通信模块版本号。 */
  uint32_t pa_pu_com_version;
  /* 复位初始化状态，文档 bit0~bit3 分别表示主逻辑、panel、DDR、SFP reset done。 */
  uint32_t rst_init_state;
  /* 图像写出状态机状态。 */
  uint32_t img_wr_state;
  /* 图像写出完成标志。 */
  uint32_t img_wr_end;
  /* 图像校正状态机状态。 */
  uint32_t img_corr_state;
  /* 图像校正完成标志。 */
  uint32_t img_corr_end;
  /* GIC 模块状态，高电平表示 busy。 */
  uint32_t gic_state;
  /* GIC 操作完成标志。 */
  uint32_t gic_end;
  /* GIC 调试/错误状态。 */
  uint32_t gic_dfx;
  /* ROIC 模块状态，高电平表示 busy。 */
  uint32_t roic_state;
  /* ROIC 操作完成标志。 */
  uint32_t roic_end;
  /* ROIC 调试/保留状态。 */
  uint32_t roic_dfx;
} pa_pu_status_t;

/* 打开 PA 寄存器映射：优先 UIO，失败后回退 /dev/mem + base_addr。 */
int pa_pu_open(uintptr_t base_addr, size_t map_size);

/* 关闭 PA 寄存器映射并释放 fd/mmap。 */
void pa_pu_close(void);

/* 判断 PA 寄存器当前是否已经映射。 */
bool pa_pu_is_open(void);

/* 读一个 32 位 PA 寄存器；reg 使用 pa_pu_regs.h 中的偏移。 */
uint32_t pa_pu_read(uint16_t reg);

/* 写一个 32 位 PA 寄存器；reg 使用 pa_pu_regs.h 中的偏移。 */
void pa_pu_write(uint16_t reg, uint32_t value);

/* 读取 STATUS 命令需要的状态寄存器集合。 */
void pa_pu_read_status(pa_pu_status_t* status);

/*
 * 等待一次 UIO 中断。
 * 返回值：>0 表示收到中断，0 表示超时，-1 表示 read/poll 错误，-2 表示当前不是 UIO 映射。
 */
int pa_pu_wait_irq(int timeout_ms, uint32_t* irq_count);

/* 使用默认模板地址和默认图像尺寸配置 PA 校正模块。 */
void pa_pu_configure_templates(void);

/* 按调用者给定参数配置 PA 校正模块。 */
void pa_pu_configure_correction(const pa_pu_corr_config_t* config);

/* 使用 app_config.h 中的默认参数配置 GIC。 */
void pa_pu_configure_gic_defaults(void);

/* 按调用者给定参数配置 GIC。 */
void pa_pu_configure_gic(const pa_pu_gic_config_t* config);

/* 写 GIC_STR，启动一次 GIC 操作。 */
void pa_pu_start_gic(void);

/* 写 GIC_STOP，停止 GIC 操作；主要用于 xao scan。 */
void pa_pu_stop_gic(void);

/* 使用 app_config.h 中的默认参数配置 ROIC。 */
void pa_pu_configure_roic_defaults(void);

/* 按调用者给定参数配置 ROIC。 */
void pa_pu_configure_roic(const pa_pu_roic_config_t* config);

/* 写 ROIC_STR，启动一次 ROIC 配置操作。 */
void pa_pu_start_roic(void);

/* 写 IMG_CORR_STR，启动一次图像校正。 */
void pa_pu_start_correction(void);

/* 配置原始图像地址并写 IMG_WR_STR，启动一次图像写出/光口传图。 */
void pa_pu_start_image_write(uint32_t image_addr);
