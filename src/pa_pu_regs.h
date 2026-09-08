#pragma once

#include <stdint.h>

/*
 * PA/PU 寄存器地址表。
 *
 * 来源：
 * - fpga/pa_pu_com_definition.xlsx
 * - fpga/pa_pu_com_definition_add.xlsx
 * - fpga/dynamic.txt
 * - fpga/pa_pu_com.v
 *
 * RTL 中使用 upm_pu_awaddr[15:0] / upm_pu_araddr[15:0] 与这些地址比较，
 * 因此这里的值必须和 FPGA 文档保持一致。
 *
 * 寄存器访问统一通过 pa_pu_read()/pa_pu_write() 做 32 位 MMIO。
 */

enum {
  /* general：全局中断、版本和复位初始化状态。 */
  /* 0x0000, pa->pu, 32bit：中断向量寄存器，按 bit 表示各模块完成/初始化事件，xlsx 标注 read clear。 */
  PA_PU_INT_VECTOR_REG = 0x0000,
  /* 0x0008, pa->pu, 24bit：PA 版本号，bit23~16 主版本，bit15~8 次版本，bit7~0 修订号。 */
  PA_PU_PA_VERSION_REG = 0x0008,
  /* 0x0010, pa->pu, 32bit：PA 构建信息，bit31~24 年，bit23~16 月，bit15~8 日，bit7~0 子编号。 */
  PA_PU_PA_BUILD_INFORMATION_REG = 0x0010,
  /* 0x0018, pa->pu, 24bit：当前 PA bitstream 适配的主板版本号。 */
  PA_PU_ADAPTED_MAIN_BOARD_VERSION_REG = 0x0018,
  /* 0x0020, pa->pu, 24bit：当前 PA bitstream 适配的 GIC 板版本号。 */
  PA_PU_ADAPTED_GIC_BOARD_VERSION_REG = 0x0020,
  /* 0x0028, pa->pu, 24bit：当前 PA bitstream 适配的 ROIC 板版本号。 */
  PA_PU_ADAPTED_ROIC_BOARD_VERSION_REG = 0x0028,
  /* 0x0030, pa->pu, 24bit：预留板卡 0 版本号，当前无业务使用。 */
  PA_PU_ADAPTED_RESERVED_BOARD_0_VERSION_REG = 0x0030,
  /* 0x0038, pa->pu, 24bit：预留板卡 1 版本号，当前无业务使用。 */
  PA_PU_ADAPTED_RESERVED_BOARD_1_VERSION_REG = 0x0038,
  /* 0x0040, pa->pu, 24bit：预留板卡 2 版本号，当前无业务使用。 */
  PA_PU_ADAPTED_RESERVED_BOARD_2_VERSION_REG = 0x0040,
  /* 0x0048, pa->pu, 24bit：PA/PU 通信模块版本号，用于确认寄存器接口版本。 */
  PA_PU_COM_VERSION_REG = 0x0048,
  /* 0x0050, pa->pu, 32bit：复位初始化状态，bit0 主逻辑，bit1 panel，bit2 DDR，bit3 SFP。 */
  PA_PU_RST_INIT_STATE_REG = 0x0050,

  /* gic_drive：GIC 采集时序、ROI 行范围和 binning 配置。 */
  /* 0x0200, pu->pa, 1bit：写 1 启动一次 GIC 操作，高有效，通常由 RTL 自动清零。 */
  PA_PU_GIC_STR_REG = 0x0200,
  /* 0x0208, pu->pa, 1bit：写 1 停止 GIC 操作，仅 xao scan 模式下使用。 */
  PA_PU_GIC_STOP_REG = 0x0208,
  /* 0x0210, pu->pa, 8bit：GIC 请求模式，0 串行扫描 半清，1 并行扫描 并清，2 xao 扫描。 */
  PA_PU_GIC_REQ_CODE_REG = 0x0210,
  /* 0x0218, pu->pa, 1bit：GIC 数据输出使能，高有效，仅串行扫描模式使用。 */
  PA_PU_GIC_DOUT_EN_REG = 0x0218,
  /* 0x0220, pu->pa, 32bit：GIC 行时间，xlsx 单位为 ns。 */
  PA_PU_GIC_LINE_TIME_REG = 0x0220,
  /* 0x0228, pu->pa, 32bit：OE 上升沿时间，xlsx 单位为 ns。 */
  PA_PU_GIC_OE_RAISING_EDGE_REG = 0x0228,
  /* 0x0230, pu->pa, 32bit：OE 下降沿时间，xlsx 单位为 ns。 */
  PA_PU_GIC_OE_FALLING_EDGE_REG = 0x0230,
  /* 0x0238, pu->pa, 16bit：GIC 起始行号，从 0 开始，用于 ROI/行范围配置。 */
  PA_PU_GIC_STR_ROW_NUM_REG = 0x0238,
  /* 0x0240, pu->pa, 16bit：GIC 结束行号，用于 ROI/行范围配置。 */
  PA_PU_GIC_END_ROW_NUM_REG = 0x0240,
  /* 0x0248, pu->pa, 8bit：GIC binning 模式，新表定义 0=1x1，1=2x2，...，7=8x8，其它按 1x1。 */
  PA_PU_GIC_BINNING_MODE_REG = 0x0248,
  /* 0x03a0, pa->pu, 1bit：GIC 模块状态，高电平表示 busy。 */
  PA_PU_GIC_STATE_REG = 0x03a0,
  /* 0x03a8, pa->pu, 1bit：GIC 操作完成标志，高有效。 */
  PA_PU_GIC_END_REG = 0x03a8,
  /* 0x03c0, pa->pu, 8bit：GIC 调试/错误状态，bit0/bit2 为 OE 时序错误，bit1 为 ROI 配置错误。 */
  PA_PU_GIC_DFX_REG = 0x03c0,
  /* 0x03c8, pu->pa, 32bit：GIC 调试输入寄存器，仅调试使用。 */
  PA_PU_GIC_DEBUG_IN_REG = 0x03c8,
  /* 0x03d0, pa->pu, 32bit：GIC 调试输出寄存器，仅调试使用。 */
  PA_PU_GIC_DEBUG_OUT_REG = 0x03d0,

  /* roic_drive：ROIC 配置寄存器和列方向 ROI/binning 配置。 */
  /* 0x0400, pu->pa, 1bit：写 1 启动一次 ROIC 配置操作，高有效，RTL 自动清零。 */
  PA_PU_ROIC_STR_REG = 0x0400,
  /* 0x0408, pu->pa, 8bit：ROIC 请求码，1=配置一次 ROIC，其它预留。 */
  PA_PU_ROIC_REQ_CODE_REG = 0x0408,
  /* 0x0410, pu->pa, 16bit：写入 ROIC 芯片 0x00 寄存器配置值，初值由 panel 测试方提供。 */
  PA_PU_ROIC_REG_00_REG = 0x0410,
  /* 0x0418, pu->pa, 16bit：写入 ROIC 芯片 0x02 寄存器配置值。 */
  PA_PU_ROIC_REG_02_REG = 0x0418,
  /* 0x0420, pu->pa, 16bit：写入 ROIC 芯片 0x05 寄存器配置值。 */
  PA_PU_ROIC_REG_05_REG = 0x0420,
  /* 0x0428, pu->pa, 16bit：写入 ROIC 芯片 0x06 寄存器配置值。 */
  PA_PU_ROIC_REG_06_REG = 0x0428,
  /* 0x0430, pu->pa, 16bit：写入 ROIC 芯片 0x07 寄存器配置值。 */
  PA_PU_ROIC_REG_07_REG = 0x0430,
  /* 0x0438, pu->pa, 16bit：写入 ROIC 芯片 0x09 寄存器配置值。 */
  PA_PU_ROIC_REG_09_REG = 0x0438,
  /* 0x0440, pu->pa, 16bit：写入 ROIC 芯片 0x0a 寄存器配置值。 */
  PA_PU_ROIC_REG_0A_REG = 0x0440,
  /* 0x0448, pu->pa, 16bit：写入 ROIC 芯片 0x0b 寄存器配置值。 */
  PA_PU_ROIC_REG_0B_REG = 0x0448,
  /* 0x0450, pu->pa, 16bit：写入 ROIC 芯片 0x0c 寄存器配置值。 */
  PA_PU_ROIC_REG_0C_REG = 0x0450,
  /* 0x0458, pu->pa, 16bit：写入 ROIC 芯片 0x0d 寄存器配置值。 */
  PA_PU_ROIC_REG_0D_REG = 0x0458,
  /* 0x0460, pu->pa, 16bit：写入 ROIC 芯片 0x0e 寄存器配置值。 */
  PA_PU_ROIC_REG_0E_REG = 0x0460,
  /* 0x0468, pu->pa, 16bit：写入 ROIC 芯片 0x0f 寄存器配置值。 */
  PA_PU_ROIC_REG_0F_REG = 0x0468,
  /* 0x0470, pu->pa, 16bit：写入 ROIC 芯片 0x10 寄存器配置值。 */
  PA_PU_ROIC_REG_10_REG = 0x0470,
  /* 0x0478, pu->pa, 16bit：写入 ROIC 芯片 0x11 寄存器配置值。 */
  PA_PU_ROIC_REG_11_REG = 0x0478,
  /* 0x0480, pu->pa, 16bit：写入 ROIC 芯片 0x17 寄存器配置值。 */
  PA_PU_ROIC_REG_17_REG = 0x0480,
  /* 0x0488, pu->pa, 16bit：写入 ROIC 芯片 0x24 寄存器配置值。 */
  PA_PU_ROIC_REG_24_REG = 0x0488,
  /* 0x0490, pu->pa, 16bit：写入 ROIC 芯片 0x28 寄存器配置值。 */
  PA_PU_ROIC_REG_28_REG = 0x0490,
  /* 0x0498, pu->pa, 16bit：写入 ROIC 芯片 0x2d 寄存器配置值。 */
  PA_PU_ROIC_REG_2D_REG = 0x0498,
  /* 0x04a0, pu->pa, 16bit：写入 ROIC 芯片 0x3b 寄存器配置值。 */
  PA_PU_ROIC_REG_3B_REG = 0x04a0,
  /* 0x04a8, pu->pa, 16bit：ROIC 起始列号，从 0 开始，用于列方向 ROI。 */
  PA_PU_ROIC_STR_COL_NUM_REG = 0x04a8,
  /* 0x04b0, pu->pa, 16bit：ROIC 结束列号，用于列方向 ROI。 */
  PA_PU_ROIC_END_COL_NUM_REG = 0x04b0,
  /* 0x04b8, pu->pa, 8bit：ROIC binning 模式，新表定义 0=1x1，1=2x2，...，7=8x8，其它按 1x1。 */
  PA_PU_ROIC_BINNING_MODE_REG = 0x04b8,
  /* 0x05a0, pa->pu, 1bit：ROIC 模块状态，高电平表示 busy。 */
  PA_PU_ROIC_STATE_REG = 0x05a0,
  /* 0x05a8, pa->pu, 1bit：ROIC 操作完成标志，高有效。 */
  PA_PU_ROIC_END_REG = 0x05a8,
  /* 0x05c0, pa->pu, 8bit：ROIC 调试/保留状态。 */
  PA_PU_ROIC_DFX_REG = 0x05c0,
  /* 0x05c8, pu->pa, 32bit：ROIC 调试输入寄存器，仅调试使用。 */
  PA_PU_ROIC_DEBUG_IN_REG = 0x05c8,
  /* 0x05d0, pa->pu, 32bit：ROIC 调试输出寄存器，仅调试使用。 */
  PA_PU_ROIC_DEBUG_OUT_REG = 0x05d0,

  /* img_wr：通知 PA/FPGA 从 DDR 指定地址启动原始图像写出/光口传图。 */
  /* 0x0600, pu->pa, 1bit：写 1 启动原始图像写出，光口传图由 FPGA/PA 执行，RTL 自动清零。 */
  PA_PU_IMG_WR_STR_REG = 0x0600,
  /* 0x0608, pu->pa, 32bit：原始图像在 DDR3 中的起始地址，xlsx 要求 1KB 对齐。 */
  PA_PU_IMG_WR_STR_ADDR_REG = 0x0608,
  /* 0x07a0, pa->pu, 1bit：图像写出模块状态，高电平表示 busy。 */
  PA_PU_IMG_WR_STATE_REG = 0x07a0,
  /* 0x07a8, pa->pu, 1bit：图像写出完成标志，高有效。 */
  PA_PU_IMG_WR_END_REG = 0x07a8,
  /* 0x07b0, pa->pu, 32bit：dynamic 模式本轮最终输出图 DDR 地址。 */
  PA_PU_IMG_WR_FINAL_IMG_ADDR_REG = 0x07b0,
  /* 0x07c0, pa->pu, 8bit：图像写出调试/保留状态。 */
  PA_PU_IMG_WR_DFX_REG = 0x07c0,
  /* 0x07c8, pu->pa, 32bit：图像写出调试输入寄存器，仅调试使用。 */
  PA_PU_IMG_WR_DEBUG_IN_REG = 0x07c8,
  /* 0x07d0, pa->pu, 32bit：图像写出调试输出寄存器，仅调试使用。 */
  PA_PU_IMG_WR_DEBUG_OUT_REG = 0x07d0,

  /* img_correct：图像校正参数、模板地址、状态和调试寄存器。 */
  /* 0x0800, pu->pa, 1bit：写 1 启动图像校正，高有效，RTL 自动清零。 */
  PA_PU_IMG_CORR_STR_REG = 0x0800,
  /* 0x0808, pu->pa, 16/32bit：校正图像分包数量，按 1KB 包计数。 */
  PA_PU_IMG_PKG_NUM_REG = 0x0808,
  /* 0x0810, pu->pa, 16/32bit：校正图像行数。 */
  PA_PU_IMG_ROW_NUM_REG = 0x0810,
  /* 0x0818, pu->pa, 16/32bit：校正图像列数，xlsx 要求 4 对齐。 */
  PA_PU_IMG_COL_NUM_REG = 0x0818,
  /* 0x0820, pu->pa, 1bit：offset 校正使能，高有效。 */
  PA_PU_IMG_CORR_OFFSET_EN_REG = 0x0820,
  /* 0x0828, pu->pa, 32bit：offset 模板在 DDR3 中的起始地址，xlsx 要求 1KB 对齐。 */
  PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG = 0x0828,
  /* 0x0830, pu->pa, 16/32bit：offset 校正附加值，具体公式由 FPGA IMG_CORR 模块定义。 */
  PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG = 0x0830,
  /* 0x0838, pu->pa, 1bit：gain 校正使能，高有效。 */
  PA_PU_IMG_CORR_GAIN_EN_REG = 0x0838,
  /* 0x0840, pu->pa, 32bit：gain 模板在 DDR3 中的起始地址，xlsx 要求 1KB 对齐。 */
  PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG = 0x0840,
  /* 0x0848, pu->pa, 16/32bit：gain 校正限幅值，避免校正结果超过硬件允许范围。 */
  PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG = 0x0848,
  /* 0x0850, pu->pa, 1bit：坏点校正使能，高有效。 */
  PA_PU_IMG_CORR_DEFECT_EN_REG = 0x0850,
  /* 0x0858, pu->pa, 1bit：offset 校正模式，0=静态 offset，1=动态 offset。 */
  PA_PU_IMG_OFFSET_CORR_MODE_REG = 0x0858,
  /* 0x09a0, pa->pu, 1bit：图像校正模块状态，高电平表示 busy。 */
  PA_PU_IMG_CORR_STATE_REG = 0x09a0,
  /* 0x09a8, pa->pu, 1bit：图像校正完成标志，高有效。 */
  PA_PU_IMG_CORR_END_REG = 0x09a8,
  /* 0x09c0, pa->pu, 8bit：图像校正调试/保留状态。 */
  PA_PU_IMG_CORR_DFX_REG = 0x09c0,
  /* 0x09c8, pu->pa, 32bit：图像校正调试输入寄存器。 */
  PA_PU_IMG_CORR_DEBUG_IN_REG = 0x09c8,
  /* 0x09d0, pa->pu, 32bit：图像校正调试输出寄存器，仅调试使用。 */
  PA_PU_IMG_CORR_DEBUG_OUT_REG = 0x09d0,

  /* dynamic_ctrl：动态工作模式步骤表、图像环形地址范围和状态寄存器。 */
  /* 0x0a00, pu->pa, 1bit：写 1 启动动态流程，高有效，RTL 自动清零。 */
  PA_PU_DYNC_STR_REG = 0x0a00,
  /* 0x0a08, pu->pa, 1bit：写 1 停止动态流程，主要用于 xao scan 等等待同步输入的场景。 */
  PA_PU_DYNC_STOP_REG = 0x0a08,
  /* 0x0a10, pu->pa, 32bit：动态循环次数，0 表示直到收到 dynamic stop。 */
  PA_PU_DYNC_CYCLE_NUM_REG = 0x0a10,
  /* 0x0a18, pu->pa, 32bit：动态模式图像环形缓冲起始地址。 */
  PA_PU_DYNC_IMG_STR_ADDR_REG = 0x0a18,
  /* 0x0a20, pu->pa, 32bit：动态模式图像环形缓冲结束地址。 */
  PA_PU_DYNC_IMG_END_ADDR_REG = 0x0a20,
  /* 0x0a28~0x0ac0, pu->pa：10 个动态步骤配置，每步 high/low 两个 32bit 配置字。 */
  PA_PU_DYNC_STEP_0_CFG_H_REG = 0x0a28,
  PA_PU_DYNC_STEP_0_CFG_L_REG = 0x0a30,
  PA_PU_DYNC_STEP_1_CFG_H_REG = 0x0a38,
  PA_PU_DYNC_STEP_1_CFG_L_REG = 0x0a40,
  PA_PU_DYNC_STEP_2_CFG_H_REG = 0x0a48,
  PA_PU_DYNC_STEP_2_CFG_L_REG = 0x0a50,
  PA_PU_DYNC_STEP_3_CFG_H_REG = 0x0a58,
  PA_PU_DYNC_STEP_3_CFG_L_REG = 0x0a60,
  PA_PU_DYNC_STEP_4_CFG_H_REG = 0x0a68,
  PA_PU_DYNC_STEP_4_CFG_L_REG = 0x0a70,
  PA_PU_DYNC_STEP_5_CFG_H_REG = 0x0a78,
  PA_PU_DYNC_STEP_5_CFG_L_REG = 0x0a80,
  PA_PU_DYNC_STEP_6_CFG_H_REG = 0x0a88,
  PA_PU_DYNC_STEP_6_CFG_L_REG = 0x0a90,
  PA_PU_DYNC_STEP_7_CFG_H_REG = 0x0a98,
  PA_PU_DYNC_STEP_7_CFG_L_REG = 0x0aa0,
  PA_PU_DYNC_STEP_8_CFG_H_REG = 0x0aa8,
  PA_PU_DYNC_STEP_8_CFG_L_REG = 0x0ab0,
  PA_PU_DYNC_STEP_9_CFG_H_REG = 0x0ab8,
  PA_PU_DYNC_STEP_9_CFG_L_REG = 0x0ac0,
  /*
   * 已跟fpga核实，按照下面配置来。
   */
  PA_PU_DYNC_STATE_REG = 0x0ba0,
  PA_PU_DYNC_END_REG = 0x0ba8,
  /* 0x0bc0, pu->pa, 32bit：动态模块调试输入寄存器。 */
  PA_PU_DYNC_DEBUG_IN_REG = 0x0bc0,
  /* 0x0bc8, pa->pu, 32bit：动态模块调试输出寄存器。 */
  PA_PU_DYNC_DEBUG_OUT_REG = 0x0bc8,

  /* img_upload：从指定 DDR 地址读取 offset/gain 模板并通过独立上传链路发送。 */
  /* 0x0c00, pu->pa, 1bit：写 1 启动一次图片上传，高有效，RTL 自动清零。 */
  PA_PU_IMG_UPLOAD_STR_REG = 0x0c00,
  /* 0x0c08, pu->pa, 32bit：待上传图片在 DDR 中的起始物理地址。 */
  PA_PU_IMG_UPLOAD_STR_ADDR_REG = 0x0c08,
  /* 0x0c10, pu->pa, 16/32bit：上传分包数量，按 row * col * 2 / 1024 计算。 */
  PA_PU_IMG_UPLOAD_PKG_NUM_REG = 0x0c10,
  /* 0x0c18, pu->pa, 16/32bit：上传图片行数。 */
  PA_PU_IMG_UPLOAD_ROW_NUM_REG = 0x0c18,
  /* 0x0c20, pu->pa, 16/32bit：上传图片列数。 */
  PA_PU_IMG_UPLOAD_COL_NUM_REG = 0x0c20,
  /* 0x0da0, pa->pu, 1bit：图片上传模块状态，高电平表示 busy。 */
  PA_PU_IMG_UPLOAD_STATE_REG = 0x0da0,
  /* 0x0da8, pa->pu, 1bit：图片上传完成标志，高有效。 */
  PA_PU_IMG_UPLOAD_END_REG = 0x0da8,
  /* 0x0dc0, pa->pu, 32bit：图片上传调试/错误状态。 */
  PA_PU_IMG_UPLOAD_DFX_REG = 0x0dc0,
};

/*
 * int_vector bit 定义，按 pa_pu_com_definition.xlsx 协议表执行：
 * bit31~7 reserved
 * bit6 image upload interrupt
 * bit5 dynamic interrupt
 * bit4 image correct interrupt
 * bit3 image write interrupt
 * bit2 roic interrupt
 * bit1 gic interrupt
 * bit0 pa reset init done interrupt
 *
 * 当前 RTL 若返回 {img_corr_end, img_wr_end, roic_end, gic_end}，会和表格协议不一致；
 * 软件侧仍以表格协议为准，现场调试通过日志观察非 0 int_vector。
 */
enum {
  /* bit0：PA reset 初始化完成中断。 */
  PA_PU_IRQ_RST_INIT_DONE = 1u << 0,
  /* bit1：GIC 操作完成中断。 */
  PA_PU_IRQ_GIC_END = 1u << 1,
  /* bit2：ROIC 操作完成中断。 */
  PA_PU_IRQ_ROIC_END = 1u << 2,
  /* bit3：原始图像写出/光口传图完成中断。 */
  PA_PU_IRQ_IMG_WR_END = 1u << 3,
  /* bit4：图像校正完成中断。 */
  PA_PU_IRQ_IMG_CORR_END = 1u << 4,
  /* bit5：动态模块完成中断。 */
  PA_PU_IRQ_DYNC_END = 1u << 5,
  /* bit6：图片上传模块完成中断。 */
  PA_PU_IRQ_IMG_UPLOAD_END = 1u << 6,
};

enum {
  /* GIC 串行扫描模式。 */
  PA_PU_GIC_REQ_SERIAL_SCAN = 0u,
  /* GIC 并行扫描模式。 */
  PA_PU_GIC_REQ_PARALLEL_SCAN = 1u,
  /* GIC xao 扫描模式。 */
  PA_PU_GIC_REQ_XAO_SCAN = 2u,
};

enum {
  /* ROIC 请求码：配置一次 ROIC。 */
  PA_PU_ROIC_REQ_CONFIG_ONCE = 1u,
};

enum {
  /*
   * dynamic step high 配置字：
   * bit31    current step enable
   * bit30~8  reserved
   * bit7~0   current step req code
   */
  PA_PU_DYNC_STEP_ENABLE_MASK = 0x80000000u,
  PA_PU_DYNC_STEP_REQ_MASK = 0x000000ffu,
};

enum {
  /* req_code=0：空闲等待。 */
  PA_PU_DYNC_REQ_IDLE = 0u,
  /* req_code=1：serial clear。 */
  PA_PU_DYNC_REQ_SERIAL_CLEAR = 1u,
  /* req_code=2：parallel clear。 */
  PA_PU_DYNC_REQ_PARALLEL_CLEAR = 2u,
  /* req_code=3：xao clear。 */
  PA_PU_DYNC_REQ_XAO_CLEAR = 3u,
  /* req_code=4：采集一张图。 */
  PA_PU_DYNC_REQ_CAPTURE_ONE_IMAGE = 4u,
  /* req_code=5：等待 sync in 信号。 */
  PA_PU_DYNC_REQ_WAIT_SYNC_IN = 5u,
  /* req_code=6：等待 sync out。 */
  PA_PU_DYNC_REQ_WAIT_SYNC_OUT = 6u,
};

/* 按协议字段拼 dynamic step high 配置字。 */
#define PA_PU_DYNC_STEP_CFG_H(enable, req_code) \
  (((enable) ? PA_PU_DYNC_STEP_ENABLE_MASK : 0u) | ((uint32_t)(req_code) & PA_PU_DYNC_STEP_REQ_MASK))

enum {
  /* 新版 GIC/ROIC 协议使用连续编码 0~7；其它值由 FPGA 按 1x1 处理。 */
  /* 不合并像素，1x1 输出。 */
  PA_PU_BINNING_1X1 = 0u,
  /* 2x2 binning。 */
  PA_PU_BINNING_2X2 = 1u,
  /* 3x3 binning。 */
  PA_PU_BINNING_3X3 = 2u,
  /* 4x4 binning。 */
  PA_PU_BINNING_4X4 = 3u,
  /* 5x5 binning。 */
  PA_PU_BINNING_5X5 = 4u,
  /* 6x6 binning。 */
  PA_PU_BINNING_6X6 = 5u,
  /* 7x7 binning。 */
  PA_PU_BINNING_7X7 = 6u,
  /* 8x8 binning。 */
  PA_PU_BINNING_8X8 = 7u,
};

/* IMG_CORR 写地址按当前协议表直接写入；保留 WR_* 名称，避免上层调用点反复改动。 */
#define PA_PU_WR_IMG_CORR_DEFECT_EN_REG PA_PU_IMG_CORR_DEFECT_EN_REG
#define PA_PU_WR_IMG_CORR_DEBUG_IN_REG PA_PU_IMG_CORR_DEBUG_IN_REG
