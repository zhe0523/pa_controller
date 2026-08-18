# PA Controller

这是面向新项目的轻量 ARM 应用，只保留 PA/FPGA 控制、模板生成/加载和 RS422 上位机通讯。

## 构建

```sh
make
```

常用覆盖参数：

```sh
make APP_VERSION=0.1.0 PA_PU_BASE_ADDR=0x40000000 RS422_DEVICE=/dev/ttyS1 RS422_BAUD=115200
```

程序启动时会打印 `app_version` 和 `build_time`。发布时可用 `APP_VERSION=x.y.z`
指定本软件版本；`BUILD_TIME` 默认由 Makefile 在编译时生成。

默认参数集中在 `src/app_config.h`，并且都可以通过 Makefile 覆盖。比如修改默认校正配置：

```sh
make CORR_DEFAULT_ROW_NUM=7680 CORR_DEFAULT_COL_NUM=3072 \
     CORR_DEFAULT_OFFSET_EN=1 CORR_DEFAULT_OFFSET_ADDER_VALUE=100 \
     CORR_DEFAULT_GAIN_EN=1 \
     CORR_DEFAULT_GAIN_CLIPPING_VALUE=55000
```

offset/gain/image 的物理地址和 UIO size 通过 Makefile 配置，必须和设备树中的
UIO 布局保持一致。

可用 `make config` 查看当前构建参数展开后的默认值。

当前共享 DDR 默认使用三个 UIO 节点：

```text
/dev/uio0 -> offset/暗场模板
/dev/uio1 -> gain/亮场模板
/dev/uio2 -> 实际输出图环形图像池
```

当前默认物理地址和窗口大小：

```text
FPGA_OFFSET_PTR=0x16000000 FPGA_OFFSET_UIO_SIZE=0x04000000
FPGA_GAIN_PTR=0x1A000000 FPGA_GAIN_UIO_SIZE=0x04000000
FPGA_IMAGE_PTR=0x21000000 FPGA_IMAGE_UIO_SIZE=0x1F000000
```

对应构建参数：

```sh
make FPGA_OFFSET_UIO_DEVICE=/dev/uio0 FPGA_OFFSET_PTR=0x16000000 FPGA_OFFSET_UIO_SIZE=0x04000000 \
     FPGA_GAIN_UIO_DEVICE=/dev/uio1 FPGA_GAIN_PTR=0x1A000000 FPGA_GAIN_UIO_SIZE=0x04000000 \
     FPGA_IMAGE_UIO_DEVICE=/dev/uio2 FPGA_IMAGE_PTR=0x21000000 FPGA_IMAGE_UIO_SIZE=0x1F000000
```

`GAIN_TEMPLATE_REPEAT_COUNT` 默认是 `1`。当前 3072×7680 的 16bit 图像一帧约 45MB，
64MB 的亮场 UIO 窗口只能稳定放一份完整 gain 模板；如果后续重新扩大亮场窗口，可在
构建时改回多份。

GIC/ROIC 默认配置也可以在构建时覆盖，例如：

```sh
make GIC_DEFAULT_LINE_TIME_NS=100000 GIC_DEFAULT_START_ROW=0 GIC_DEFAULT_END_ROW=7715 \
     ROIC_DEFAULT_START_COL=0 ROIC_DEFAULT_END_COL=3071
```

start 类命令会写启动寄存器后等待完成 bit。程序启动时会优先打开 `/dev/pa_irq`，
如果该节点存在，则由驱动阻塞等待中断；如果不存在，则自动回退到轮询 `INT_VECTOR`。
默认超时 5000ms：

```sh
make PA_PU_IRQ_TIMEOUT_MS=10000 PA_PU_IRQ_POLL_INTERVAL_US=1000 PA_IRQ_DEVICE=/dev/pa_irq
```

PA 寄存器默认通过 `/dev/mem + PA_PU_BASE_ADDR` 访问，当前确认的默认基地址为 `0x40000000`。
`/dev/uio0` 已经用于暗场 DDR，因此暂时不把 PA/PU 寄存器包装为 UIO。
如果后续设备树给 PA 寄存器单独暴露 UIO，可以再覆盖：

```sh
make PA_PU_UIO_DEVICE=/dev/uio4
```

不要把 `PA_PU_UIO_DEVICE` 配成 `/dev/uio0`、`/dev/uio1` 或 `/dev/uio2`，
这些现在都是共享 DDR。

输出文件：

```text
build/bin/pa_controller
```

部署到当前开发板：

```sh
make deploy-board
```

默认会上传到：

```text
root@192.168.3.17:/root/pa_controller
```

当前默认使用 `sshpass` 自动输入开发板密码，Ubuntu 编译主机需要安装：

```sh
sudo apt install sshpass
```

默认登录参数：

```text
BOARD_USER=root
BOARD_PASS=root
```

也可以一键上传并运行：

```sh
make run-board
```

默认运行参数是：

```text
-d /dev/ttyS1 -b 115200
```

临时覆盖示例：

```sh
make run-board BOARD_HOST=192.168.3.17 BOARD_USER=root BOARD_DIR=/root BOARD_RUN_ARGS="--stdio"
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

## 当前 RS422 调试命令

当前先使用 ASCII 行协议，命令以 `\r\n` 或 `\n` 结束，便于串口助手联调。正式上位机协议确定后，替换 `src/command_handler.c` 即可。

```text
PING              -> 心跳
STATUS            -> 读取 PA 状态
VERSION           -> 查询本 app 版本和 PA/FPGA 版本寄存器，GET_VERSION 等价
SET_TIME          -> 设置 Linux 本机时间，供上位机连接后同步开发板日志时间
GET_TIME          -> 读取 Linux 本机当前时间，TIME 等价
DUMP_REGS         -> 打印 PA/PU 寄存器快照到日志，跳过 read-clear 中断寄存器
READ_REG          -> 直接读取 PA/PU 寄存器，支持寄存器名、偏移或绝对地址
WRITE_REG         -> 直接写 PA/PU 寄存器，支持寄存器名、偏移或绝对地址
LOAD_TEMPLATE     -> 从 /usr/local/offset.raw 和 /usr/local/gain.raw 加载模板
MAKE_OFFSET       -> 用当前 FPGA 图像生成 offset 模板
MAKE_GAIN         -> 用当前 FPGA 图像和 offset 模板生成 gain 模板
CONFIG_TEMPLATE   -> 将 offset/gain 物理地址配置给 PA
CONFIG_CORR       -> 将图像校正尺寸、模板地址和 offset/gain/defect 使能配置给 PA
CONFIG_STATIC_IDLE -> 配置静态 Idle 工作模式的时间窗口、GIC 时序和暗场校正开关
START_STATIC_IDLE_CAPTURE -> 触发一次静态 Idle 采图，等待 offset 模板帧和实际输出帧完成
LOOP_STATIC_IDLE_CAPTURE -> 循环触发 Static Idle 采图，用于稳定性测试
GET_WORK_STATE    -> 查询当前工作模式状态、最近一次错误和本次 DDR 写图地址
STOP_WORK         -> 停止后台 Static Idle 工作线程
START_WORK        -> 重新启动后台 Static Idle 工作线程
CONFIG_GIC        -> 将 GIC 时序、行范围和 binning 配置给 PA
START_GIC         -> 启动一次 GIC 操作，等待 GIC 完成中断
STOP_GIC          -> 停止 GIC 操作，主要用于 xao scan
CONFIG_ROIC       -> 将默认 ROIC 寄存器、列范围和 binning 配置给 PA
START_ROIC        -> 启动一次 ROIC 配置操作，等待 ROIC 完成中断
START_CORR        -> 启动 PA 图像校正，等待 IMG_CORR 完成中断
START_CORR_GIC    -> 启动 PA 图像校正后立即启动 GIC，等待 IMG_CORR 和 GIC 两个完成中断
SEND_SINGLE       -> 当前 Qt 上位机“手动上图”，通知 PA 从 FPGA 图像地址启动一次写图流程并等待 IMG_WR 完成中断
START_CONTINUOUS  -> 当前 Qt 上位机“开始上图”，现阶段暂按一次写图流程兼容并等待 IMG_WR 完成中断
STOP_TRANSFER     -> 当前 Qt 上位机“停止上图”，现阶段仅确认收到停止请求
SEND_IMAGE        -> 早期调试命令，当前等价于 SEND_SINGLE
QUIT              -> 退出程序
```

`STATUS` 当前返回字段：

所有字段值按 32bit 十六进制输出，便于直接和寄存器表、`READ_REG` 结果对照。

```text
pa_version                         PA 版本寄存器
pa_build_information               PA 构建信息寄存器
adapted_main_board_version         适配主板版本寄存器
adapted_gic_board_version          适配 GIC 板版本寄存器
adapted_roic_board_version         适配 ROIC 板版本寄存器
adapted_reserved_board_0_version   适配预留板卡 0 版本寄存器
adapted_reserved_board_1_version   适配预留板卡 1 版本寄存器
adapted_reserved_board_2_version   适配预留板卡 2 版本寄存器
pa_pu_com_version                  PA/PU 通信模块版本寄存器
pa_rst_init_state                  复位初始化状态寄存器
wr_state                           图像写出状态机状态
wr_end                             图像写出完成标志
corr_state                         图像校正状态机状态
corr_end                           图像校正完成标志
gic_state                          GIC 状态机状态
gic_end                            GIC 操作完成标志
gic_dfx                            GIC 调试/错误状态
roic_state                         ROIC 状态机状态
roic_end                           ROIC 操作完成标志
roic_dfx                           ROIC 调试/保留状态
```

`VERSION` 返回字段：

PA/FPGA 版本寄存器按 `fpga/pa_pu_com_definition.xlsx` 解析：
`bit23~16.bit15~8.bit7~0` 对应 `first.second.third`。
`pa_build_information` 按 `bit31~24` 年、`bit23~16` 月、`bit15~8` 日、`bit7~0` 子版本解析。

```text
app_version                        当前 pa_controller 软件版本
app_build_time                     当前 pa_controller 编译时间
pa_version                         PA 版本寄存器
pa_build_information               PA 构建信息寄存器
adapted_main_board_version         适配主板版本寄存器
adapted_gic_board_version          适配 GIC 板版本寄存器
adapted_roic_board_version         适配 ROIC 板版本寄存器
adapted_reserved_board_0_version   适配预留板卡 0 版本寄存器
adapted_reserved_board_1_version   适配预留板卡 1 版本寄存器
adapted_reserved_board_2_version   适配预留板卡 2 版本寄存器
pa_pu_com_version                  PA/PU 通信模块版本寄存器
```

返回示例：

```text
OK VERSION app_version=0.1.0 app_build_time="2026-08-17 10:30:00 +0800" pa_version=0.0.1 pa_build_information=2026-08-11.1 adapted_main_board_version=0.0.0 adapted_gic_board_version=0.0.0 adapted_roic_board_version=0.0.0 adapted_reserved_board_0_version=0.0.0 adapted_reserved_board_1_version=0.0.0 adapted_reserved_board_2_version=0.0.0 pa_pu_com_version=0.0.0
```

上位机同步时间推荐使用 epoch 秒或 epoch 毫秒，避免时区字符串歧义：

```text
SET_TIME epoch=1786435200
SET_TIME epoch_ms=1786435200123
GET_TIME
```

手工串口调试时也可以发送本地时间字符串，程序会按开发板当前本地时区解释：

```text
SET_TIME 2026-08-11 14:30:00
SET_TIME 2026-08-11T14:30:00
```

成功返回示例：

```text
OK SET_TIME epoch=1786435200 epoch_ms=1786435200123 local=2026-08-11T14:30:00
OK TIME epoch=1786435200 epoch_ms=1786435200123 local=2026-08-11T14:30:00
```

注意：当前硬件的 `INT_VECTOR` 是 read-clear，ARM 或驱动每读一次就会清除已经置位的中断。
因此 `STATUS` 不读取 `INT_VECTOR`，只读取版本、busy/end 和调试状态。`START_GIC`、
`START_ROIC`、`START_CORR` 和 `SEND_SINGLE`/`START_CONTINUOUS` 内部会优先通过 `/dev/pa_irq`
等待对应完成 bit；如果 `/dev/pa_irq` 不存在，则回退到直接轮询 `INT_VECTOR`。

寄存器直接读写调试命令：

```text
READ_REG pa_version
READ_REG gic_req_code
READ_REG 0x0210
READ_REG 0x40000210
WRITE_REG gic_req_code 0
WRITE_REG gic_dout_en 1
WRITE_REG gic_line_time 100000
WRITE_REG 0x0210 0x0
WRITE_REG 0x40000200 1
```

`REG_READ` 等价于 `READ_REG`，`REG_WRITE` 等价于 `WRITE_REG`。寄存器地址既可以写相对
PA/PU 基地址的 offset，例如 `0x0210`，也可以写绝对地址，例如 `0x40000210`。

注意：`READ_REG int_vector` 或 `READ_REG 0x0000` 会读取并清除 `INT_VECTOR`，可能影响
正在等待完成中断的调试流程。
如果只是想看完整寄存器现场，优先使用 `DUMP_REGS`；该命令会把寄存器快照打印到日志，
并跳过 `INT_VECTOR` 这类 read-clear 中断寄存器。

start 类命令完成时返回示例：

```text
OK START_GIC int_vector=0x00000002
OK START_ROIC int_vector=0x00000004
OK START_CORR int_vector=0x00000010
OK SEND_SINGLE addr=0x21000000 int_vector=0x00000008
```

超时则返回：

```text
ERR START_GIC TIMEOUT int_vector=0x00000000
```

GIC/ROIC 推荐的手工 bring-up 顺序：

```text
STATUS
CONFIG_GIC
START_GIC
CONFIG_ROIC
START_ROIC
CONFIG_TEMPLATE
CONFIG_CORR
START_CORR_GIC
SEND_SINGLE
STATUS
```

`CONFIG_GIC` 和 `CONFIG_ROIC` 只负责下发配置，不会自动启动硬件动作。`START_GIC`
和 `START_ROIC` 单独触发并等待完成中断，之后可用 `STATUS` 查看状态位和错误位。

`CONFIG_GIC` 不带参数时使用构建默认值，也可以用 `key=value` 临时覆盖：

```text
CONFIG_GIC req=0 dout=1 line_time=100000 oe_rise=1000 oe_fall=90000 start_row=0 end_row=7679 binning=1
```

也支持完整寄存器名：

```text
CONFIG_GIC gic_req_code=0 gic_dout_en=1 gic_line_time=100000 gic_oe_raising_edge=1000 gic_oe_falling_edge=90000 gic_str_row_num=0 gic_end_row_num=7679 gic_binning_mode=1
```

写完这些配置寄存器后，再发 `START_GIC`，程序会写 `GIC_STR=1` 让配置生效并启动一次 GIC 操作。

`CONFIG_CORR` 不带参数时使用默认图像尺寸，模板地址默认来自 `FPGA_OFFSET_PTR` 和
`FPGA_GAIN_PTR`；也可以用 `key=value` 临时覆盖：

```text
CONFIG_CORR pkg=46080 row=7680 col=3072 offset_en=1 offset_addr=0x16000000 offset_adder=100 gain_en=1 gain_addr=0x1a000000 gain_clip=55000 defect_en=0
```

也支持完整寄存器名：

```text
CONFIG_CORR img_pkg_num=46080 img_row_num=7680 img_col_num=3072 img_corr_offset_en=1 img_corr_offset_temp_str_addr=0x16000000 img_corr_offset_adder_value=100 img_corr_gain_en=1 img_corr_gain_temp_str_addr=0x1a000000 img_corr_gain_clipping_value=55000 img_corr_defect_en=0
```

写完图像校正配置后，再发 `START_CORR`，程序会写 `IMG_CORR_STR=1` 并等待表格协议中的
IMG_CORR 完成中断 bit。

Static Idle 是正式业务流程入口。启动后后台工作线程会按 `idle_clean_interval_ms`
周期执行 GIC 自清空，`gic_req_code=0`、`gic_dout_en=0`。可以用下面命令配置：

```text
CONFIG_STATIC_IDLE idle_clean_interval_ms=50 exposure_ms=50 dark_window_ms=50 offset_en=1 gain_en=1 defect_en=0 line_time=25600 start_row=0 end_row=7679 binning=0
```

触发一次静态采图：

```text
START_STATIC_IDLE_CAPTURE
```

循环稳定性测试：

```text
LOOP_STATIC_IDLE_CAPTURE count=100 interval_ms=5000
```

`count=0` 表示一直循环；不带参数时默认 `count=0 interval_ms=5000 stop_on_error=1`。
该命令同步运行，长时间测试时可用 Ctrl+C 结束程序。

该命令会等待当前自清空结束后执行：

```text
曝光窗口
-> 第一帧 light：关闭 offset/gain/defect，写到 offset 模板区，等待 IMG_CORR+IMG_WR+GIC
-> 暗场窗口
-> 第二帧输出图：按配置打开 offset/gain/defect，写到 uio2 图像池，等待 IMG_CORR+IMG_WR+GIC
```

当前测试版中，第一帧未校正 light 固定写到 `/dev/uio0` 的物理地址，作为 offset 模板。
第二帧实际输出图由 ARM 从 `/dev/uio2` 图像池按 `frame_stride` 环形分配，并写入
`img_wr_str_addr`。如果 uio2 尾部剩余空间不足一帧，下一张输出图会回到池起始地址；
单张图不会跨越 uio2 尾部。
状态可用：

```text
GET_WORK_STATE
```

如果需要图像校正启动后马上启动 GIC，可以发：

```text
START_CORR_GIC
```

该命令会先写 `IMG_CORR_STR=1`，紧接着写 `GIC_STR=1`，然后等待表格协议中的
IMG_CORR 和 GIC 两个完成中断 bit。`START_CORR_THEN_GIC` 是等价别名。

注意：`ROIC_DEFAULT_REG_*` 当前是占位值，真实 ROIC 芯片寄存器值需要由 panel
测试参数或旧工程参数覆盖后再用于真板配置。

`CONFIG_ROIC` 不带参数时使用构建默认值，也可以用 `key=value` 临时覆盖：

```text
CONFIG_ROIC start_col=0 end_col=3071 binning=1 reg_00=0x0000 reg_02=0x0000 reg_05=0x0000 reg_06=0x0000 reg_07=0x0000 reg_09=0x0000 reg_0a=0x0000 reg_0b=0x0000 reg_0c=0x0000 reg_0d=0x0000 reg_0e=0x0000 reg_0f=0x0000 reg_10=0x0000 reg_11=0x0000 reg_17=0x0000 reg_24=0x0000 reg_28=0x0000 reg_2d=0x0000 reg_3b=0x0000
```

也可以只覆盖本次需要改的字段，其它字段继续使用构建默认值：

```text
CONFIG_ROIC start_col=0 end_col=3071 binning=1
```

也支持完整寄存器名：

```text
CONFIG_ROIC roic_str_col_num=0 roic_end_col_num=3071 roic_binning_mode=1 roic_reg_00=0x0000 roic_reg_02=0x0000 roic_reg_05=0x0000 roic_reg_06=0x0000 roic_reg_07=0x0000 roic_reg_09=0x0000 roic_reg_0a=0x0000 roic_reg_0b=0x0000 roic_reg_0c=0x0000 roic_reg_0d=0x0000 roic_reg_0e=0x0000 roic_reg_0f=0x0000 roic_reg_10=0x0000 roic_reg_11=0x0000 roic_reg_17=0x0000 roic_reg_24=0x0000 roic_reg_28=0x0000 roic_reg_2d=0x0000 roic_reg_3b=0x0000
```

写完 ROIC 配置后，再发 `START_ROIC`，程序会写 `ROIC_STR=1` 并等待表格协议中的
ROIC 完成中断 bit。

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
PA/PU 通过 /dev/mem 或独立 PA 寄存器 UIO 访问寄存器
image/offset/gain 通过三个共享 DDR UIO 映射
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
src/fpga_mem.c/h        FPGA image/offset/gain 三个 UIO 内存映射
src/image_frame.h       光口图像头格式
src/template_builder.c  offset/gain 模板生成与加载
src/auto_offset_plan.*  自动暗场模板更新的配置、状态和质量门槛规划
src/rs422.c/h           422 串口配置和行收发
src/command_handler.c   临时调试命令分发
```

## 注意

根据 `fpga/pa_pu_com_definition.xlsx` 和 `fpga/pa_pu_com.v` 对照，目前有几个需要和 FPGA 继续确认的点：

```text
1. IMG_CORR 地址按当前协议表使用 0x0800~0x0850 的 8 字节对齐地址。
   defect_en 写 0x0850，debug_in 写 0x09C8，不再做旧 RTL 的 defect/debug 地址互换兼容。

2. 软件协议以 `pa_pu_com_definition.xlsx` 为准：int_vector bit0 是 pa reset init done interrupt，
   bit1 是 gic interrupt，bit2 是 roic interrupt，bit3 是 image write interrupt，
   bit4 是 image correct interrupt。若当前 RTL 返回 `{img_corr_end,img_wr_end,roic_end,gic_end}`，
   则属于 FPGA/表格协议不一致，软件侧仍按表格协议等待，并通过日志打印所有非 0 int_vector
   方便现场定位。
```

当前完成中断优先通过 `/dev/pa_irq` 驱动处理。`gic_str`、`roic_str`、`img_wr_str`
和 `img_corr_str` 触发后，驱动读取 `INT_VECTOR` 清硬件 sticky，并把 int_vector 事件交给
`pa_controller`；如果没有加载驱动或没有 `/dev/pa_irq` 节点，应用会回退到原来的
1ms `INT_VECTOR` 轮询。
RS422 调试协议不再暴露 `WAIT_IRQ`。
