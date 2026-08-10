# PA Controller

这是面向新项目的轻量 ARM 应用，只保留 PA/FPGA 控制、模板生成/加载和 RS422 上位机通讯。

## 构建

```sh
make
```

常用覆盖参数：

```sh
make PA_PU_BASE_ADDR=0x43c10000 RS422_DEVICE=/dev/ttyS1 RS422_BAUD=115200
```

GIC/ROIC 默认配置也可以在构建时覆盖，例如：

```sh
make GIC_DEFAULT_LINE_TIME_NS=100000 GIC_DEFAULT_START_ROW=0 GIC_DEFAULT_END_ROW=7715 \
     ROIC_DEFAULT_START_COL=0 ROIC_DEFAULT_END_COL=3071
```

PA 寄存器默认优先通过 `/dev/uio0` 访问：

```sh
make PA_PU_UIO_DEVICE=/dev/uio0
```

`PA_PU_BASE_ADDR` 只作为 `/dev/uio0` 打不开时的 `/dev/mem` 回退地址。

输出文件：

```text
build/bin/pa_controller
```

运行时也可以指定实际 422 串口：

```sh
./build/bin/pa_controller -d /dev/ttyS1 -b 115200
```

如果默认串口不存在，程序会自动尝试 `/dev/ttyS1`、`/dev/ttyS2`、`/dev/ttyS3`、`/dev/ttyS0`。

开发板只有调试串口、暂时没有独立 RS422 时，使用标准输入输出测试命令协议：

```sh
./build/bin/pa_controller --stdio
```

此模式下日志输出到 `stderr`，协议响应输出到 `stdout`，命令仍然与 RS422 模式完全一致。

如果只想在普通 Ubuntu 上测试文本协议，不访问 `/dev/uio0`、`/dev/mem` 或 FPGA 共享内存：

```sh
./build/bin/pa_controller --stdio --no-hw
```

`--no-hw` 只能和 `--stdio` 一起使用。该模式下 `PING`、`STATUS`、`SEND_SINGLE`、
`START_CONTINUOUS`、`STOP_TRANSFER` 和 `QUIT` 会返回可供上位机联调的协议响应；
`LOAD_TEMPLATE`、`MAKE_OFFSET`、`MAKE_GAIN`、`CONFIG_TEMPLATE`、`CONFIG_GIC`、`START_GIC`、
`STOP_GIC`、`CONFIG_ROIC`、`START_ROIC` 和 `START_CORR`
会返回 `ERR NO_HW`，避免误以为真实硬件动作已经执行。

## 当前 RS422 调试命令

当前先使用 ASCII 行协议，命令以 `\r\n` 或 `\n` 结束，便于串口助手联调。正式上位机协议确定后，替换 `src/command_handler.c` 即可。

```text
PING              -> 心跳
STATUS            -> 读取 PA 状态
LOAD_TEMPLATE     -> 从 /usr/local/offset.raw 和 /usr/local/gain.raw 加载模板
MAKE_OFFSET       -> 用当前 FPGA 图像生成 offset 模板
MAKE_GAIN         -> 用当前 FPGA 图像和 offset 模板生成 gain 模板
CONFIG_TEMPLATE   -> 将 offset/gain 物理地址配置给 PA
CONFIG_GIC        -> 将默认 GIC 时序、行范围和 binning 配置给 PA
START_GIC         -> 启动一次 GIC 操作
STOP_GIC          -> 停止 GIC 操作，主要用于 xao scan
CONFIG_ROIC       -> 将默认 ROIC 寄存器、列范围和 binning 配置给 PA
START_ROIC        -> 启动一次 ROIC 配置操作
START_CORR        -> 启动 PA 图像校正
SEND_SINGLE       -> 当前 Qt 上位机“手动上图”，通知 PA 从 FPGA 图像地址启动一次写图流程
START_CONTINUOUS  -> 当前 Qt 上位机“开始上图”，现阶段暂按一次写图流程兼容
STOP_TRANSFER     -> 当前 Qt 上位机“停止上图”，现阶段仅确认收到停止请求
SEND_IMAGE        -> 早期调试命令，当前等价于 SEND_SINGLE
QUIT              -> 退出程序
```

`STATUS` 当前返回字段：

```text
int_vector   PA/FPGA 中断向量寄存器快照
pa_version   PA 版本寄存器
com_version  PA/PU 通信模块版本寄存器
rst_state    复位初始化状态寄存器
wr_state     图像写出状态机状态
wr_end       图像写出完成标志
corr_state   图像校正状态机状态
corr_end     图像校正完成标志
gic_state    GIC 状态机状态
gic_end      GIC 操作完成标志
gic_dfx      GIC 调试/错误状态
roic_state   ROIC 状态机状态
roic_end     ROIC 操作完成标志
roic_dfx     ROIC 调试/保留状态
```

GIC/ROIC 推荐的手工 bring-up 顺序：

```text
STATUS
CONFIG_GIC
START_GIC
STATUS
CONFIG_ROIC
START_ROIC
STATUS
CONFIG_TEMPLATE
START_CORR
STATUS
SEND_SINGLE
STATUS
```

`CONFIG_GIC` 和 `CONFIG_ROIC` 只负责下发配置，不会自动启动硬件动作。`START_GIC`
和 `START_ROIC` 单独触发，便于串口助手逐步确认状态位和错误位。

注意：`ROIC_DEFAULT_REG_*` 当前是占位值，真实 ROIC 芯片寄存器值需要由 panel
测试参数或旧工程参数覆盖后再用于真板配置。

说明：当前 PA/FPGA 侧尚未提供正式持续上图和停流寄存器，因此 `SEND_SINGLE`、
`START_CONTINUOUS` 和早期 `SEND_IMAGE` 都会触发同一个 `IMG_WR_STR` 写图流程；
`STOP_TRANSFER` 只返回 `OK STOP_TRANSFER`，不额外操作硬件。后续硬件接口明确后，
只需要在 `src/command_handler.c` 中拆分这三条命令的具体实现。

## 自动暗场模板更新规划

当前 `MAKE_OFFSET` 仍保持手动单帧生成 offset 模板，便于现场明确触发和验证。后续“机器
空闲时自动更新暗场图”按以下方式落地，不直接让单帧暗场覆盖正式模板：

```text
机器空闲
-> 等待空闲状态稳定一段时间
-> 连续读取 N 帧暗场
-> 对每个像素做多帧平均
-> 检查均值、最大值、行噪声和相对旧模板的变化
-> 通过后写入 FPGA offset_template
-> 先写 /usr/local/offset.raw.tmp
-> 校验大小成功后 rename 为 /usr/local/offset.raw
-> 重新配置 PA 模板地址
```

初始规划代码在 `src/auto_offset_plan.*`，目前只定义配置、状态和质量门槛，不启动后台
自动任务，也不改变现有 `MAKE_OFFSET` 行为。等以下硬件条件确认后再启用实际更新：

```text
1. wr_state / corr_state 的空闲取值
2. 上图、校正、写图和曝光互斥关系
3. ARM 是否能可靠知道射线源未曝光
4. FPGA 图像内存中的帧完成和帧稳定判据
5. 暗场质量门槛：最大值、均值变化、行噪声阈值
```

默认建议从 `8` 帧平均开始，空闲稳定时间暂按 `3000 ms`，质量门槛需用真实暗场样例校准。

## 与项目要求的对应关系

已在 ARM 应用中落地：

```text
RS422 上位机通讯入口
PA/PU 通过 /dev/uio0 访问寄存器
GIC/ROIC 默认配置、启动和状态查询
offset/gain 模板生成与加载
通知 PA/FPGA 启动图像校正
通知 PA/FPGA 启动光口传图
43×108cm@140um 对应的默认 3072×7716 图像尺寸
光口图像头 5 个 Ushort 字段的数据结构定义
```

需要 PA/FPGA 或驱动继续配合：

```text
图像头插入到光口数据流
角位置 A、Home Z、皮带 B 编码器计数
脉冲合并：1/2/3/4/6/8/9/10/12/15/16 分频触发
ROI 下 1×1、2×2、4×4、8×8 帧率控制
F2P 图像完成中断的正式 read/poll 接口
Trig PTO 输出给射线源/加速器
```

## 关键文件

```text
src/main.c              主流程：初始化 FPGA/PA、RS422 循环
src/pa_pu.c/h           PA 寄存器控制，地址来自 fpga/pa_pu_com.v
src/fpga_mem.c/h        FPGA image/offset/gain 内存映射
src/image_frame.h       光口图像头格式
src/template_builder.c  offset/gain 模板生成与加载
src/auto_offset_plan.*  自动暗场模板更新的配置、状态和质量门槛规划
src/rs422.c/h           422 串口配置和行收发
src/command_handler.c   临时调试命令分发
```

## 注意

`src/pa_pu_regs.h` 中 `IMG_CORR_DEFECT_EN` 和 `IMG_CORR_DEBUG_IN` 的写地址按当前 `fpga/pa_pu_com.v` 的实际写 decode 做了兼容；如果 PA RTL 修正了这两个地址，需要同步调整。

根据 `fpga/pa_pu_com_definition.xlsx` 和 `fpga/pa_pu_com.v` 对照，目前有几个需要和 FPGA 继续确认的点：

```text
1. IMG_CORR 部分存在 0x0801、0x0802、0x0803 等非 4 字节对齐寄存器地址。
   ARM 用户态 mmap 后做 32 位 MMIO 访问时，非对齐地址在真实硬件上有风险。
   建议 FPGA 后续把所有寄存器地址改为 4 字节或 8 字节对齐。

2. xlsx 中 int_vector bit0 定义为 pa reset init done interrupt，
   但当前 RTL 的 int_vector 只拼了 img_corr/img_wr/roic/gic 四类完成信号。
   需要 FPGA 明确是修文档还是修 RTL。

3. xlsx 中 img_corr_defect_en 地址是 0x0810，img_corr_debug_in 地址是 0x09C8；
   当前 RTL 写 decode 里这两个地址互换，软件暂时用 WR_* 宏兼容。
```

当前目标板上的 `f2p_irq_test.ko` 是中断测试驱动，可以验证 hwirq/virq 和 INT_VECTOR；应用层如果要阻塞等待中断，推荐让正式驱动暴露 `read/poll`，或者把同一个中断接入 UIO。RS422 调试协议不再暴露 `WAIT_IRQ`。
