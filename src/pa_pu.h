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
 * 当前 /dev/uio0~2 已用于共享 DDR，因此默认留空，走 /dev/mem + PA_PU_BASE_ADDR。
 * 如果后续设备树把 PA 寄存器单独暴露为 UIO，再在 make 中覆盖此项。
 */
#define PA_PU_UIO_DEVICE ""
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
  /* offset 校正模式：0=静态模板，1=动态模板/动态 offset。 */
  uint8_t offset_corr_mode;
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

enum {
  /* dynamic 模块当前协议最多支持 10 个步骤，每步由 high/low 两个 32bit 配置字组成。 */
  PA_PU_DYNC_STEP_COUNT = 10,
};

/*
 * dynamic_ctrl 动态流程配置。
 *
 * step_cfg_h bit31 表示该 step 使能，bit[7:0] 为 req_code；
 * step_cfg_l 当前按协议表表示该 step 的等待时间，单位 ms。
 */
typedef struct {
  /* 动态流程循环次数，0 表示一直运行到收到 DYNC_STOP。 */
  uint32_t cycle_num;
  /* 动态模式写图环形缓冲起始物理地址。 */
  uint32_t image_start_addr;
  /* 动态模式写图环形缓冲结束物理地址。 */
  uint32_t image_end_addr;
  /* 每个动态步骤的 high 配置字。 */
  uint32_t step_cfg_h[PA_PU_DYNC_STEP_COUNT];
  /* 每个动态步骤的 low 配置字。 */
  uint32_t step_cfg_l[PA_PU_DYNC_STEP_COUNT];
} pa_pu_dync_config_t;

/* STATUS 命令返回给上位机的核心 PA 状态快照。 */
typedef struct {
  /* PA 版本号。 */
  uint32_t pa_version;
  /* PA 构建信息。 */
  uint32_t pa_build_information;
  /* 当前 PA bitstream 适配的主板版本号。 */
  uint32_t adapted_main_board_version;
  /* 当前 PA bitstream 适配的 GIC 板版本号。 */
  uint32_t adapted_gic_board_version;
  /* 当前 PA bitstream 适配的 ROIC 板版本号。 */
  uint32_t adapted_roic_board_version;
  /* 预留板卡 0 版本号。 */
  uint32_t adapted_reserved_board_0_version;
  /* 预留板卡 1 版本号。 */
  uint32_t adapted_reserved_board_1_version;
  /* 预留板卡 2 版本号。 */
  uint32_t adapted_reserved_board_2_version;
  /* PA/PU 通信模块版本号。 */
  uint32_t pa_pu_com_version;
  /* 复位初始化状态，文档 bit0~bit3 分别表示主逻辑、panel、DDR、SFP reset done。 */
  uint32_t rst_init_state;
  /* 图像写出状态机状态。 */
  uint32_t img_wr_state;
  /* 图像写出完成标志。 */
  uint32_t img_wr_end;
  /* dynamic 模式下图像写出模块返回的最终输出图 DDR 地址。 */
  uint32_t img_wr_final_img_addr;
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
  /* dynamic 模块状态，高电平表示 busy。 */
  uint32_t dync_state;
  /* dynamic 操作完成标志。 */
  uint32_t dync_end;
  /* dynamic 调试输出。 */
  uint32_t dync_debug_out;
} pa_pu_status_t;

/* 打开 PA 寄存器映射：优先 UIO，失败后回退 /dev/mem + base_addr。 */
int pa_pu_open(uintptr_t base_addr, size_t map_size);

/* 关闭 PA 寄存器映射并释放 fd/mmap。 */
void pa_pu_close(void);

/* 判断 PA 寄存器当前是否已经映射。 */
bool pa_pu_is_open(void);

/* 判断是否已打开 /dev/pa_irq；未打开时等待中断会自动回退到 INT_VECTOR 轮询。 */
bool pa_pu_irq_driver_is_open(void);

/* 调试用：打开后等待中断路径会同步打印 trace，用于定位硬卡死位置。 */
void pa_pu_set_trace(bool enabled);

/* 读一个 32 位 PA 寄存器；reg 使用 pa_pu_regs.h 中的偏移。 */
uint32_t pa_pu_read(uint16_t reg);

/* 写一个 32 位 PA 寄存器；reg 使用 pa_pu_regs.h 中的偏移。 */
void pa_pu_write(uint16_t reg, uint32_t value);

/* 读取 STATUS 命令需要的非清除类状态寄存器集合。 */
void pa_pu_read_status(pa_pu_status_t* status);

/* 调试用：打印 PA/PU 寄存器值；会跳过 INT_VECTOR 等 read-clear 寄存器。 */
void pa_pu_dump_all_registers(const char* reason);

/*
 * 调试命令用：打印寄存器快照，但跳过 read-clear 寄存器。
 * 返回实际读取并打印的寄存器数量；skipped_out 返回被跳过的数量。
 */
size_t pa_pu_dump_safe_registers(const char* reason, size_t* skipped_out);

/* 读取 INT_VECTOR。当前硬件语义为 read-clear，调用者会消费并清除 pending 中断。 */
uint32_t pa_pu_read_int_vector(void);

/*
 * 等待 INT_VECTOR 中指定完成 bit：优先使用 /dev/pa_irq，未打开时回退到寄存器轮询。
 * 返回 1 表示读到目标 bit，0 表示超时，-1 表示参数错误；int_vector_out 返回最后一次读到的快照。
 */
int pa_pu_wait_int_vector(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out);

/*
 * 等待 INT_VECTOR 中指定完成 bit 全部出现。
 * 返回 1 表示累计读到全部目标 bit，0 表示超时，-1 表示参数错误；int_vector_out 返回累计快照。
 */
int pa_pu_wait_int_vector_all(uint32_t mask, unsigned timeout_ms, uint32_t* int_vector_out);

/* start 类命令写启动寄存器前调用，清掉上一轮遗留的驱动事件或 INT_VECTOR sticky。 */
void pa_pu_prepare_irq_wait(void);

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

/* 按调用者给定参数配置 dynamic 模块步骤表和图像环形地址范围。 */
void pa_pu_configure_dync(const pa_pu_dync_config_t* config);

/* 写 DYNC_STR，启动 dynamic 流程。 */
void pa_pu_start_dync(void);

/* 写 DYNC_STOP，停止 dynamic 流程。 */
void pa_pu_stop_dync(void);

/* 只配置 IMG_WR_STR_ADDR，不触发 IMG_WR_STR；image_addr 必须是 FPGA 可访问的 DDR 物理地址。 */
void pa_pu_configure_image_write(uint32_t image_addr);

/* 连续写 IMG_CORR_STR、IMG_WR_STR、GIC_STR；调用前必须已经配置 GIC、IMG_WR_ADDR 和 IMG_CORR。 */
void pa_pu_start_capture_triplet(void);

/* 配置原始图像地址并写 IMG_WR_STR，启动一次图像写出/光口传图。 */
void pa_pu_start_image_write(uint32_t image_addr);
