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

程序默认只初始化 PA/PU 寄存器映射、UIO 共享 DDR 和命令入口，不自动启动后台工作模式。
这样启动后可以直接做 `CONFIG_DYNC`、`CONFIG_GIC`、`WRITE_REG` 等寄存器级调试，不会被
Static Idle 自清空线程抢寄存器。需要恢复开机自动进入 Static Idle 时：

```sh
make WORK_MODE_AUTO_START=1 WORK_MODE_DEFAULT_MODE=0
```

`WORK_MODE_DEFAULT_MODE` 沿用上一代 WorkMode 编号。当前 ARM 侧只实现
`0=Idle/Static Idle`，其它模式编号先作为后续扩展预留。

默认参数集中在 `src/app_config.h`，并且都可以通过 Makefile 覆盖。比如修改默认校正配置：

```sh
make CORR_DEFAULT_ROW_NUM=7680 CORR_DEFAULT_COL_NUM=3072 \
     CORR_DEFAULT_OFFSET_EN=1 CORR_DEFAULT_OFFSET_ADDER_VALUE=100 \
     CORR_DEFAULT_GAIN_EN=1 \
     CORR_DEFAULT_GAIN_CLIPPING_VALUE=55000
```

offset/gain/image 的物理地址和 UIO size 由设备树中的 UIO map0 描述。程序启动时会从
`/sys/class/uio/uioX/maps/map0/addr` 和 `size` 自动读取，Makefile 只需要配置使用哪个
`/dev/uioX`。

可用 `make config` 查看当前构建参数展开后的默认值。

当前共享 DDR 默认使用三个 UIO 节点：

```text
/dev/uio0 -> offset/暗场模板
/dev/uio1 -> gain/亮场模板
/dev/uio2 -> 实际输出图环形图像池
```

可以在板端用下面的方式确认设备树实际暴露出来的物理地址和窗口大小：

```sh
cat /sys/class/uio/uio0/maps/map0/addr
cat /sys/class/uio/uio0/maps/map0/size
cat /sys/class/uio/uio1/maps/map0/addr
cat /sys/class/uio/uio1/maps/map0/size
cat /sys/class/uio/uio2/maps/map0/addr
cat /sys/class/uio/uio2/maps/map0/size
```

对应构建参数：

```sh
make FPGA_OFFSET_UIO_DEVICE=/dev/uio0 \
     FPGA_GAIN_UIO_DEVICE=/dev/uio1 \
     FPGA_IMAGE_UIO_DEVICE=/dev/uio2
```

gain 区只保存一份完整模板。当前 3072×7680 的 16bit 图像一帧约 45MB，
64MB 的亮场 UIO 窗口可以稳定容纳一份 gain 模板。

GIC/ROIC 默认配置也可以在构建时覆盖，例如：

```sh
make GIC_DEFAULT_LINE_TIME_NS=100000 GIC_DEFAULT_START_ROW=0 GIC_DEFAULT_END_ROW=7715 \
     ROIC_DEFAULT_START_COL=0 ROIC_DEFAULT_END_COL=3071
```

start 类命令会写启动寄存器后等待完成 bit。程序启动时会优先打开 `/dev/pa_irq`，
如果该节点存在，则由驱动阻塞等待中断；如果不存在，则自动回退到轮询 `INT_VECTOR`。
默认超时 1000ms：

```sh
make PA_PU_IRQ_TIMEOUT_MS=500 PA_PU_IRQ_POLL_INTERVAL_US=1000 PA_IRQ_DEVICE=/dev/pa_irq
```

配置寄存器默认写后立即读回校验 2 次，`*_STR`、`*_STOP` 和 `*_en` 这类触发/使能信号只写不校验。
需要临时关闭读回时：

```sh
make PA_PU_WRITE_VERIFY_ATTEMPTS=0
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
root@192.168.3.54:/root/pa_controller
```

当前默认使用 `sshpass` 自动输入开发板密码，Ubuntu 编译主机需要安装：

```sh
sudo apt install sshpass
```

下位机正常运行时使用 systemd 保持 `pa_controller` 常驻，并在开机自动启动。部署程序和服务文件后，在开发板执行：

```sh
chmod +x /root/install_service.sh
/root/install_service.sh /root
```

服务配置为异常退出自动重启，直到设备关机。手动停止服务仅用于维护：

```sh
systemctl disable --now pa_controller.service
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
make run-board BOARD_HOST=192.168.3.54 BOARD_USER=root BOARD_DIR=/root BOARD_RUN_ARGS="--stdio"
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

## 命令协议

当前先使用 ASCII 行协议，命令以 `\r\n` 或 `\n` 结束，便于串口助手联调。
命令名大小写不敏感，参数通常使用 `key=value`，十进制和 `0x` 十六进制都支持。
正式业务命令通过 `src/command_handler.c` 的二进制入口接入。当前 ARM 工程已经加入独立的二进制帧基础层：`include/pa_protocol.h`、`src/pa_protocol.c`，并支持 `--binary` RS422 运行模式。二进制模式当前已实现 `HELLO(0x0001)`、`PING(0x0002)`、`STATUS(0x0003)`、`VERSION(0x0004)`、`REBOOT(0x0005)`、`GET_CONFIG_GROUP(0x0103)`、`SET_CONFIG_GROUP(0x0104)`、`START_STATIC_CAPTURE(0x0200)`、`START_DYNAMIC(0x0210)`、`STOP_DYNAMIC(0x0211)`、`QUERY_DYNAMIC(0x0212)`、`CAL_OFFSET_*(0x0300~0x0303)`、`CAL_GAIN_*(0x0304~0x0307)`、`CAL_STATUS(0x0308)` 和 `IMG_UPLOAD_*(0x0500~0x0502)`。配置组 payload 为 `u16 group_id` 加连续的 `{u16 item_id, u16 len=4, u32 value}` 小端 TLV；下位机收到 SET 后会校验、应用并保存 `config.ini`。`REBOOT` 返回确认帧后由下位机延迟执行系统重启。

协议基础层的固定联调帧如下，表示 `REQ cmd=0x0001 seq=1` 的空 payload 请求：

```text
AA 55 01 10 01 00 01 00 01 00 00 00 00 00 00 00 78 62
```

静态采图命令会自动确保 Static Idle 工作线程启动；动态启停和查询复用当前 `config.ini` 中的 cycle、地址范围和 step 表。Offset/Gain 制作使用后台任务，命令线程通过 `CAL_STATUS` 返回进度；模板查看先配置 `IMG_UPLOAD_CONFIG`，再由 `IMG_UPLOAD_START` 触发 FPGA 上传。ASCII 入口继续作为研发调试入口保留。

RS422 二进制联调示例：

```sh
./pa_controller -d /dev/ttyS1 -b 115200
```

上位机默认直接使用二进制协议启动，无需附加参数。二进制帧为小端字段，帧头 `AA 55`，协议版本为 1，
固定头长度 16 字节，末尾为 CRC16-CCITT-FALSE。空 payload 的 PING 请求固定帧为：

```text
AA 55 01 10 01 00 02 00 01 00 00 00 00 00 00 00 B7 D3
```

`--stdio` 仍然只用于 ASCII 命令研发测试，不能用于验证正式二进制通信链路。

协议帧编解码、固定 PING 向量、拆包、粘包和 CRC 错误可在 Ubuntu 主机直接测试，不需要目标板：

```sh
make test-protocol
```

注意：`INT_VECTOR` 是 read-clear，读一次会清除已经置位的中断。因此 `STATUS`、
`GET_WORK_STATE` 和 `DUMP_REGS` 默认不读 `INT_VECTOR`；只有明确等待中断的 start 类命令、
或手工执行 `READ_REG int_vector` 时才会读取。

工作流命令和调试命令应分开使用：工作流命令由 ARM 保证时序和互斥，适合作为上位机正式入口；
调试命令直接操作单个模块或寄存器，适合 bring-up、定位 FPGA 状态和临时验证。

## 工作流命令

### 静态模式

Static Idle 是当前静态业务流程入口。默认启动后不自动运行；需要上位机或串口手动发送
`START_WORK`，或者编译时设置：

```sh
make WORK_MODE_AUTO_START=1 WORK_MODE_DEFAULT_MODE=0
```

### 运行时配置文件

程序启动时会读取 `APP_CONFIG_FILE` 指定的 INI 文件，当前 Makefile 默认位置为
`./config.ini`，方便研发阶段直接查看和修改。文件不存在时，程序会按当前 Makefile
和 `src/app_config.h` 中的编译默认值自动生成；文件存在时，启动参数以文件为准。
构建时可以修改位置：

```bash
make APP_CONFIG_FILE=/etc/pa_controller/config.ini
```

配置文件主要包含 `[gic]`、`[roic]`、`[corr]`、`[static_idle]`、`[dynamic]`
和 `[template]`。启动时 ARM 会先校验并下发 GIC、ROIC、CORR 基础配置，再初始化
静态/动态工作流。RS422/stdio 当前提供短配置命令：

`STATUS` 除了返回 PA/FPGA 非清零状态寄存器，还会返回 ARM 工作模式、配置文件摘要、
模板任务进度和 DDR 图像池状态；不会读取会清零的 `INT_VECTOR`。

```text
GET_CONFIG_SUMMARY
GET_CONFIG_ITEM static.exposure_window_ms
SET_CONFIG_ITEM static.exposure_window_ms=50
SET_CONFIG_ITEM corr.gain_en=1
RESET_CONFIG
GET_CONFIG_GROUP gic
GET_CONFIG_GROUP dynamic
SET_CONFIG_GROUP gic req_code=0 dout_en=1 line_time_ns=25600 start_row=0 end_row=7679 binning=0
SET_CONFIG_GROUP dynamic cycle=0 step0_h=0x80000004 step0_l=50
QUERY_DYNAMIC
RESET_CONFIG_GROUP corr
```

`SET_CONFIG_ITEM` 成功后会立即校验、应用并保存配置文件；采图、动态运行或模板任务
占用硬件时返回 `BUSY`。复杂的 GIC、ROIC、Dynamic step 表先通过配置文件维护，
不要求正式上位机在一条 RS422 命令中携带大量 `key=value` 参数。

配置组命令用于研发和设备维护：`GET_CONFIG_GROUP` 返回指定组的完整当前值，支持
`gic`、`roic`、`corr`、`static`、`dynamic`；`SET_CONFIG_GROUP` 在一条短命令中提交
同一组的多个字段，成功后一次性应用并保存；`RESET_CONFIG_GROUP` 将指定组恢复为
编译默认值并立即应用、保存，支持上述组以及 `template`。Dynamic 的 `step0_h/l`
到 `step9_h/l` 仍直接保存在配置文件中，便于不同产品复用相同工作流而只替换参数。
每次 `SET_CONFIG_GROUP dynamic` 都会重新提交完整 step 表；未写出的 `stepN_h/l`
自动清零并关闭，不会沿用旧配置。

Static Idle 后台线程运行后，会按 `idle_clean_interval_ms` 周期执行一次 GIC 自清空：

```text
gic_req_code=0
gic_dout_en=0
```

配置静态模式参数：

```text
CONFIG_STATIC_IDLE idle_clean_interval_ms=50 exposure_ms=50 dark_window_ms=50 offset_en=1 gain_en=1 defect_en=0 line_time=25600 start_row=0 end_row=7679 binning=0
```

参数含义：

```text
idle_clean_interval_ms  空闲自清空间隔，单位 ms
exposure_ms             收到采图请求后，第一帧 light 采集前的曝光窗口，单位 ms
dark_window_ms          第一帧完成后，第二帧实际输出图采集前的窗口，单位 ms
offset_en               第二帧实际输出图是否启用 offset 校正
gain_en                 第二帧实际输出图是否启用 gain 校正
defect_en               第二帧实际输出图是否启用 defect 校正
line_time               GIC 行时间
start_row/end_row       GIC 起止行
binning                 GIC binning，0 表示 1x1
```

启动和停止 Static Idle 后台线程：

```text
START_WORK
STOP_WORK
```

触发一次静态采图：

```text
START_STATIC_IDLE_CAPTURE
```

一次静态采图流程：

```text
等待当前自清空结束
-> exposure_ms 曝光窗口
-> 第一帧 light：关闭 offset/gain/defect，写入 /dev/uio0 对应 offset 模板区
-> dark_window_ms 暗场窗口
-> 第二帧输出图：按 CONFIG_STATIC_IDLE 的 offset/gain/defect 配置校正，写入 /dev/uio2 图像池
-> 两帧均等待 IMG_CORR + IMG_WR + GIC 完成
```

成功回包示例：

```text
OK START_STATIC_IDLE_CAPTURE bright_addr=0x1ea00000 dark_addr=0x26a00000 int_vector=0x0000001a capture_id=1
```

失败回包会带出失败阶段和模块状态：

```text
ERR START_STATIC_IDLE_CAPTURE phase=dark_capture int_vector=0x00000018 wait_mask=0x0000001a wr_state=0x00000000 wr_end=0x00000001 corr_state=0x00000000 corr_end=0x00000001 gic_state=0x00000001 gic_end=0x00000000 gic_dfx=0x00000001 bright_addr=0x1ea00000 dark_addr=0x26a00000
```

静态稳定性测试：

```text
LOOP_STATIC_IDLE_CAPTURE count=100 interval_ms=5000
LOOP_STATIC_IDLE_CAPTURE count=0 interval_ms=1000 stop_on_error=1
LOOP_STATIC_IDLE_CAPTURE count=0 interval_ms=10 stop_on_error=1 trace=1
```

参数含义：

```text
count           循环次数；0 表示一直循环
interval_ms     两次采图之间的等待时间，单位 ms
interval_s      秒级等待时间，和 interval_ms 二选一
stop_on_error   出错后是否停止循环
trace           是否打开 Static Idle 和 PA/PU 等待过程 trace
```

循环命令是同步命令，运行期间命令线程被占用；长时间测试可用 Ctrl+C 结束程序。

查询静态工作状态：

```text
GET_WORK_STATE
```

Static Idle 回包字段主要包括：

```text
mode/state/pending_capture/stop/last_error/last_phase
wr_state/wr_end/corr_state/corr_end/gic_state/gic_end/gic_dfx
bright_addr/dark_addr/capture_id/ddr_next_offset/frame_stride/frame_count
```

### 动态模式

动态模式的正式入口是 `START_CONTINUOUS`。ARM 只负责配置并启动 FPGA `dynamic_ctrl`，
后续逐帧动作由 FPGA 按 cycle 和 step 表自主运行。命令线程不等待每帧完成，状态通过
`GET_WORK_STATE` 查询，停止通过 `STOP_TRANSFER`。

配置 Dynamic：

```text
CONFIG_DYNC cycle=10 img_start=0x26A00000 img_end=0x3FFFFFFF step0_en=1 step0_req=0 step0_time=50 step1_en=1 step1_req=4 step1_time=50
```

也可以直接写 high/low 配置字：

```text
CONFIG_DYNC cycle=10 img_start=0x26A00000 img_end=0x3FFFFFFF step0_h=0x80000000 step0_l=50 step1_h=0x80000004 step1_l=50
```

参数含义：

```text
cycle           FPGA dynamic 循环次数；0 表示持续运行，直到 STOP_TRANSFER/STOP_DYNC
img_start       dynamic 输出图环形 DDR 起始物理地址
img_end         dynamic 输出图环形 DDR 结束物理地址
stepN_en        第 N 个 step 是否启用
stepN_req       第 N 个 step 的 req_code
stepN_time      第 N 个 step 的等待/定时参数，单位 ms
stepN_h         第 N 个 step high 配置字，bit31=enable，bit7~0=req_code
stepN_l         第 N 个 step low 配置字，当前按 ms 参数使用
```

当前 req_code 定义：

```text
0 -> idle                  PA_PU_DYNC_REQ_IDLE
1 -> serial clear          PA_PU_DYNC_REQ_SERIAL_CLEAR
2 -> parallel clear        PA_PU_DYNC_REQ_PARALLEL_CLEAR
3 -> xao clear             PA_PU_DYNC_REQ_XAO_CLEAR
4 -> capture one image     PA_PU_DYNC_REQ_CAPTURE_ONE_IMAGE
5 -> wait sync in signals  PA_PU_DYNC_REQ_WAIT_SYNC_IN
6 -> wait sync out         PA_PU_DYNC_REQ_WAIT_SYNC_OUT
```

代码中使用 `PA_PU_DYNC_STEP_CFG_H(enable, req_code)` 拼 `stepN_h`：
`bit31` 为 enable，`bit[7:0]` 为 req_code，`bit[30:8]` 保留为 0。

启动、查询、停止正式 Dynamic：

```text
START_CONTINUOUS
GET_WORK_STATE
STOP_TRANSFER
GET_WORK_STATE
```

启动成功示例：

```text
OK START_CONTINUOUS state=DYNAMIC_RUNNING ring_frame_stride=0x2d00000 ring_frame_count=9
```

运行中查询示例：

```text
OK WORK_STATE mode=Continuous state=DYNAMIC_RUNNING phase=dynamic_running stop=0 error=0 dync_state=0x00000001 dync_end=0x00000000 dync_debug=0x00000000 cycle=10 img_start=0x26a00000 img_end=0x3fffffff step0_h=0x80000000 step0_l=0x00000032 step1_h=0x80000004 step1_l=0x00000032 ring_frame_stride=0x2d00000 ring_frame_count=9
```

有限 cycle 自然结束后，`GET_WORK_STATE` 返回 `state=DYNAMIC_COMPLETED stop=0`；
人工停止后返回 `state=STOPPED stop=1`。Dynamic 正在运行时禁止修改 cycle、地址和 step，
`CONFIG_DYNC` 会返回 BUSY。需要先等待自然结束，或发送 `STOP_TRANSFER`。

停止成功示例：

```text
OK STOP_TRANSFER state=STOPPED dync_state=0x00000000 dync_end=0x00000001 dync_debug=0x00000000
```

### 模板制作

模板文件路径由 Makefile 配置：

```text
TEMPLATE_OFFSET_FILE ?= /usr/local/offset.raw
TEMPLATE_GAIN_FILE   ?= /usr/local/gain.raw
CAL_GAIN_DIR         ?= /usr/local/calib
```

启动时会尝试加载 `TEMPLATE_OFFSET_FILE` 和 `TEMPLATE_GAIN_FILE`；文件不存在不阻断启动。
模板写文件时先写 `.tmp` 临时文件，成功后再 `rename`，避免中途失败破坏旧模板。

加载已有模板：

```text
LOAD_TEMPLATE
```

#### 单帧 offset 模板

`MAKE_OFFSET` 用当前图像池中的一帧生成 offset 模板，同时写入 `/dev/uio0` 和
`TEMPLATE_OFFSET_FILE`。命令立即返回后台任务号。

```text
MAKE_OFFSET
GET_TEMPLATE_STATE
```

回包示例：

```text
OK MAKE_OFFSET state=RUNNING task_id=1
OK TEMPLATE_STATE task=MAKE_OFFSET state=SUCCEEDED id=1 stop=0 progress=7680/7680 error=0 frames=0 valid_frames=0 offset_addr=0x00000000 last_img_addr=0x00000000 int_vector=0x00000000
```

#### 动态 offset 模板

`MAKE_DYNC_OFFSET` 使用 dynamic 模式采集多帧，只取最后 `valid_frames` 帧做逐像素均值。
结果写入 `/dev/uio0` 和 `TEMPLATE_OFFSET_FILE`，成功后重新下发默认校正配置。

复用当前 Dynamic 配置：

```text
MAKE_DYNC_OFFSET frames=12 valid_frames=8
```

命令内同时覆盖 Dynamic 配置：

```text
MAKE_DYNC_OFFSET frames=12 valid_frames=8 cycle=1 img_start=0x26A00000 img_end=0x3FFFFFFF step0_en=1 step0_req=4 step0_time=50
```

流程：

```text
停止 Static Idle
-> 如命令带 dynamic 参数，则下发 CONFIG_DYNC
-> 连续采集 frames 帧
-> 丢弃前 frames-valid_frames 帧
-> 对最后 valid_frames 帧做逐像素均值
-> 写入 offset 模板 DDR 和 TEMPLATE_OFFSET_FILE
-> 重新配置 PA 校正模块
```

查询和停止：

```text
GET_TEMPLATE_STATE
STOP_WORK
STOP_TRANSFER
CAL_GAIN_CANCEL
```

`STOP_WORK`、`STOP_TRANSFER`、`CAL_GAIN_CANCEL` 都会请求停止当前模板后台任务，状态先变为
`STOPPING`，后台线程到达安全检查点后变为 `CANCELED`。

#### 单帧 gain 模板

`MAKE_GAIN` 用当前图像和 offset 模板生成 gain 模板。是否后台运行由
`GAIN_TASK_BACKGROUND_ENABLE` 控制。

```text
MAKE_GAIN
GET_TEMPLATE_STATE
```

#### 多灰阶 gain/defect 模板

多灰阶 gain 制作由上位机控制光源/剂量灰阶，ARM 负责每个灰阶采集、均值文件保存和最终模板构建。
defect 当前通过 gain 模板像素写 0 表示。

开始一次 gain 校准：

```text
CAL_GAIN_BEGIN levels=5000,10000,20000 frames=4 threshold=0.3
```

参数含义：

```text
levels      灰阶列表，逗号分隔；也兼容单个灰阶
frames      每个灰阶采集帧数
threshold   defect 判定阈值，默认算法中 abs(filtered-original)/original 大于该值则标坏点
```

采集每个灰阶：

```text
CAL_GAIN_CAPTURE level=5000
CAL_GAIN_CAPTURE level=10000
CAL_GAIN_CAPTURE level=20000
```

查询灰阶准备情况：

```text
CAL_GAIN_STATUS
```

构建 gain/defect 模板：

```text
CAL_GAIN_BUILD
GET_TEMPLATE_STATE
```

取消当前 gain 或模板后台任务：

```text
CAL_GAIN_CANCEL
```

### 工作流命令速查

```text
START_WORK / STOP_WORK
CONFIG_STATIC_IDLE / START_STATIC_IDLE_CAPTURE / LOOP_STATIC_IDLE_CAPTURE
CONFIG_DYNC / START_CONTINUOUS / STOP_TRANSFER
LOAD_TEMPLATE / MAKE_OFFSET / MAKE_DYNC_OFFSET / MAKE_GAIN
CAL_GAIN_BEGIN / CAL_GAIN_CAPTURE / CAL_GAIN_BUILD / CAL_GAIN_STATUS / CAL_GAIN_CANCEL
GET_WORK_STATE / GET_TEMPLATE_STATE
```

## 调试命令

调试命令直接访问底层模块或寄存器。使用前应确认没有正式工作流正在采图或 Dynamic 正在运行；
命令层会尽量做互斥保护，Busy 时会返回 `ERR ... BUSY`。

### 基础状态和时间

心跳：

```text
PING
OK PONG
```

读取 PA/FPGA 状态：

```text
STATUS
```

`STATUS` 不读取 `INT_VECTOR`。主要字段：

```text
pa_version/pa_build_information/pa_pu_com_version
pa_rst_init_state
wr_state/wr_end/wr_final_img_addr
corr_state/corr_end
gic_state/gic_end/gic_dfx
roic_state/roic_end/roic_dfx
dync_state/dync_end/dync_debug_out
img_upload_state/img_upload_end/img_upload_dfx
```

读取软件和 FPGA 版本：

```text
VERSION
GET_VERSION
```

时间同步：

```text
SET_TIME epoch=1786435200
SET_TIME epoch_ms=1786435200123
SET_TIME 2026-08-11 14:30:00
GET_TIME
TIME
```

退出程序：

```text
QUIT
```

### 寄存器读写

读取寄存器：

```text
READ_REG pa_version
READ_REG gic_req_code
READ_REG 0x0210
READ_REG 0x40000210
REG_READ img_upload_state
```

写寄存器：

```text
WRITE_REG gic_req_code 0
WRITE_REG gic_dout_en 1
WRITE_REG gic_line_time 100000
WRITE_REG 0x0210 0x0
WRITE_REG 0x40000200 1
REG_WRITE dync_stop 1
```

寄存器引用可以是：

```text
寄存器名                  gic_req_code
PA/PU 相对 offset          0x0210
PA/PU 绝对地址             0x40000210
```

注意：`READ_REG int_vector` 或 `READ_REG 0x0000` 会读取并清除 `INT_VECTOR`，可能影响正在等待中断的流程。

安全寄存器快照：

```text
DUMP_REGS
DUMP_REGISTERS
DUMP_PA_REGS
```

该命令把快照打印到日志，并跳过 `INT_VECTOR` 等 read-clear 寄存器。

### GIC 调试

配置 GIC：

```text
CONFIG_GIC req=0 dout=1 line_time=25600 oe_rise=0 oe_fall=0 start_row=0 end_row=7679 binning=0
```

完整寄存器名也支持：

```text
CONFIG_GIC gic_req_code=0 gic_dout_en=1 gic_line_time=25600 gic_oe_raising_edge=0 gic_oe_falling_edge=0 gic_str_row_num=0 gic_end_row_num=7679 gic_binning_mode=0
```

参数含义：

```text
req / gic_req_code                  GIC 请求码
dout / gic_dout_en                  是否输出数据
line_time / gic_line_time           行时间
oe_rise / gic_oe_raising_edge       OE 上升沿时间
oe_fall / gic_oe_falling_edge       OE 下降沿时间
start_row / gic_str_row_num         起始行
end_row / gic_end_row_num           结束行
binning / gic_binning_mode          binning 模式
```

启动和停止：

```text
START_GIC
STOP_GIC
```

`START_GIC` 会写 `GIC_STR=1` 并等待 `INT_VECTOR bit1`。成功示例：

```text
OK START_GIC int_vector=0x00000002
```

### ROIC 调试

配置 ROIC：

```text
CONFIG_ROIC start_col=0 end_col=3071 binning=0
CONFIG_ROIC start_col=0 end_col=3071 binning=0 reg_00=0x0000 reg_02=0x0000 reg_05=0x0000 reg_06=0x0000 reg_07=0x0000 reg_09=0x0000 reg_0a=0x0000 reg_0b=0x0000 reg_0c=0x0000 reg_0d=0x0000 reg_0e=0x0000 reg_0f=0x0000 reg_10=0x0000 reg_11=0x0000 reg_17=0x0000 reg_24=0x0000 reg_28=0x0000 reg_2d=0x0000 reg_3b=0x0000
```

完整寄存器名也支持：

```text
CONFIG_ROIC roic_str_col_num=0 roic_end_col_num=3071 roic_binning_mode=0 roic_reg_00=0x0000 roic_reg_02=0x0000 roic_reg_05=0x0000 roic_reg_06=0x0000 roic_reg_07=0x0000 roic_reg_09=0x0000 roic_reg_0a=0x0000 roic_reg_0b=0x0000 roic_reg_0c=0x0000 roic_reg_0d=0x0000 roic_reg_0e=0x0000 roic_reg_0f=0x0000 roic_reg_10=0x0000 roic_reg_11=0x0000 roic_reg_17=0x0000 roic_reg_24=0x0000 roic_reg_28=0x0000 roic_reg_2d=0x0000 roic_reg_3b=0x0000
```

启动：

```text
START_ROIC
```

`START_ROIC` 会写 `ROIC_STR=1` 并等待 `INT_VECTOR bit2`。成功示例：

```text
OK START_ROIC int_vector=0x00000004
```

### 图像校正调试

配置校正模块：

```text
CONFIG_CORR pkg=46080 row=7680 col=3072 offset_en=1 offset_addr=0x1EA00000 offset_adder=100 offset_mode=0 gain_en=1 gain_addr=0x22A00000 gain_clip=55000 defect_en=0
```

完整寄存器名也支持：

```text
CONFIG_CORR img_pkg_num=46080 img_row_num=7680 img_col_num=3072 img_corr_offset_en=1 img_corr_offset_temp_str_addr=0x1EA00000 img_corr_offset_adder_value=100 img_offset_corr_mode=0 img_corr_gain_en=1 img_corr_gain_temp_str_addr=0x22A00000 img_corr_gain_clipping_value=55000 img_corr_defect_en=0
```

参数含义：

```text
pkg / img_pkg_num                       row * col * 2 / 1024
row / img_row_num                       图像行数
col / img_col_num                       图像列数
offset_en / img_corr_offset_en          offset 使能
offset_addr / img_corr_offset_temp_str_addr
offset_adder / img_corr_offset_adder_value
offset_mode / img_offset_corr_mode      0=static offset，1=dynamic offset
gain_en / img_corr_gain_en              gain 使能
gain_addr / img_corr_gain_temp_str_addr
gain_clip / img_corr_gain_clipping_value
defect_en / img_corr_defect_en          defect 使能
```

启动：

```text
START_CORR
START_CORR_GIC
START_CORR_THEN_GIC
```

`START_CORR` 等待 `INT_VECTOR bit4`；`START_CORR_GIC` 会先写 `IMG_CORR_STR=1`，
紧接着写 `GIC_STR=1`，等待 `IMG_CORR + GIC` 两个完成 bit。

成功示例：

```text
OK START_CORR int_vector=0x00000010
OK START_CORR_GIC int_vector=0x00000012
```

### Dynamic 底层调试

底层调试命令不创建正式 Dynamic 工作线程，只用于直接验证 `DYNC_STR/DYNC_STOP`。
正式动态流程请使用 `START_DYNAMIC -> QUERY_DYNAMIC/GET_WORK_STATE -> STOP_DYNAMIC`。
兼容旧研发入口 `CONFIG_DYNC`、`START_CONTINUOUS`、`STOP_TRANSFER` 仍然保留。

配置：

```text
CONFIG_DYNC cycle=1 img_start=0x26A00000 img_end=0x3FFFFFFF step0_en=1 step0_req=4 step0_time=50
CONFIG_DYNC cycle=1 img_start=0x26A00000 img_end=0x3FFFFFFF step0_h=0x80000004 step0_l=50
```

非阻塞启动：

```text
START_DYNC
START_DYNC cycle=1 img_start=0x26A00000 img_end=0x3FFFFFFF step0_en=1 step0_req=4 step0_time=50
```

成功示例：

```text
OK START_DYNC wait=0 dync_state=0x00000001 dync_end=0x00000000 dync_debug_out=0x00000000
```

手动停止：

```text
STOP_DYNC
```

正式动态状态查询：

```text
QUERY_DYNAMIC
```

该命令只读取 Dynamic 状态寄存器和最终图像地址，不读取会清零的 `INT_VECTOR`。

成功示例：

```text
OK STOP_DYNC dync_state=0x00000000 dync_end=0x00000001 dync_debug_out=0x00000000 final_img_addr=0x26A00000
```

`START_DYNC_WAIT` / `START_DYNAMIC_WAIT` 当前已删除，避免命令线程长时间阻塞后无法再处理停止命令。

`STOP_DYNAMIC` 属于正式 Dynamic 工作流命令，会通过工作模式状态机停止 Dynamic；
底层调试停止请使用 `STOP_DYNC`。

### 图片上传调试

图片上传模块从指定 DDR 地址读取一张图并通过上传链路发送，完成中断是 `INT_VECTOR bit6`。
默认 `template=offset`，也支持 `template=gain` 或明确 `addr=...`。

配置但不启动：

```text
CONFIG_IMG_UPLOAD template=offset
CONFIG_IMG_UPLOAD template=gain
CONFIG_IMG_UPLOAD addr=0x26A00000 row=7680 col=3072
```

启动上传：

```text
START_IMG_UPLOAD template=offset
START_IMG_UPLOAD template=gain
START_IMG_UPLOAD addr=0x26A00000 row=7680 col=3072
IMG_UPLOAD template=offset
UPLOAD_IMAGE template=gain wait=0
```

参数含义：

```text
template       offset 使用 /dev/uio0，gain 使用 /dev/uio1
addr           明确指定 DDR 物理地址，优先级高于 template
row/col        上传图像尺寸
pkg            上传分包数；不传时按 row * col * 2 / 1024 自动计算
wait           1=等待 bit6 完成；0=只触发不等待
```

成功示例：

```text
OK START_IMG_UPLOAD template=offset addr=0x1ea00000 pkg=46080 row=7680 col=3072 int_vector=0x00000040
```

常用读回：

```text
READ_REG img_upload_str_addr
READ_REG img_upload_pkg_num
READ_REG img_upload_row_num
READ_REG img_upload_col_num
READ_REG img_upload_state
READ_REG img_upload_end
READ_REG img_upload_dfx
```

### 图像写出兼容调试

单帧写图：

```text
SEND_SINGLE
SEND_IMAGE
```

这两个命令会从 `/dev/uio2` 图像池物理基地址启动一次 `IMG_WR_STR`，并等待
`INT_VECTOR bit3`。成功示例：

```text
OK SEND_SINGLE addr=0x26a00000 int_vector=0x00000008
```

`START_CONTINUOUS` 已经用于正式 Dynamic 工作流，不再等价于单帧写图。

### Binning 说明

GIC/ROIC 的 binning 当前按下列方式解释：

```text
0 -> 1x1
1 -> 2x2
2 -> 3x3
3 -> 4x4
4 -> 5x5
5 -> 6x6
6 -> 7x7
7 -> 8x8
其它 -> 1x1
```

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
-> 先写 TEMPLATE_OFFSET_FILE 对应的临时文件
-> 校验大小成功后 rename 为 TEMPLATE_OFFSET_FILE
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

### 动态模式 Offset 模板补充

当前已经提供 `MAKE_DYNC_OFFSET` / `MAKE_DYNAMIC_OFFSET` 做动态 offset 模板，详细命令、
参数和示例见上面的“工作流命令 / 模板制作 / 动态 offset 模板”。后续自动暗场更新启用时，
也应复用这套“总采集张数 + 最后有效张数均值 + 临时文件 rename”的模板写入流程。

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
src/pa_pu.c/h           PA 寄存器控制，地址来自 fpga/pa_pu_com_definition_add.xlsx 和 fpga/dynamic.txt
src/dynamic_mode.c/h    正式 Dynamic 工作流状态和启动/停止封装
src/fpga_mem.c/h        FPGA image/offset/gain 三个 UIO 内存映射
src/image_frame.h       光口图像头格式
src/template_builder.c  offset/gain 模板生成与加载
src/auto_offset_plan.*  自动暗场模板更新的配置、状态和质量门槛规划
src/rs422.c/h           422 串口配置和行收发
src/command_handler.c   临时调试命令分发
fpga/IMG_UPLOAD.txt     图片上传模块寄存器补充定义
doc/PA_Controller_业务逻辑与工作流设计.md  正式业务流程、配置文件和命令收敛设计
doc/PA_Controller_正式通信协议草案.md      后续二进制通信协议草案
doc/PA_SDK_对外交付设计.md                客户 SDK 交付包、API 分层和使用流程设计
include/pa_sdk.h                            客户 SDK 第一版 C ABI 接口草案
include/pa_protocol.h                       ARM/上位机/SDK 共用的二进制帧定义
```

## 注意

当前寄存器定义以 `fpga/pa_pu_com_definition_add.xlsx` 为主，其中 dynamic 模块地址以
`fpga/dynamic.txt` 为准。

```text
1. IMG_CORR 地址按当前协议表使用 0x0800~0x0858 的 8 字节对齐地址。
   defect_en 写 0x0850，debug_in 写 0x09C8，不再做旧 RTL 的 defect/debug 地址互换兼容。
   img_offset_corr_mode 写 0x0858，0 表示 static offset，1 表示 dynamic offset。

2. 软件协议以 `pa_pu_com_definition_add.xlsx` 为准：int_vector bit0 是 pa reset init done interrupt，
   bit1 是 gic interrupt，bit2 是 roic interrupt，bit3 是 image write interrupt，
   bit4 是 image correct interrupt，bit5 是 dynamic interrupt，bit6 是 image upload interrupt。

3. dynamic 模块中 dync_end/dync_state 地址按 fpga/dynamic.txt：
   dync_end=0x0BA0，dync_state=0x0BA8。
```

当前完成中断优先通过 `/dev/pa_irq` 驱动处理。`gic_str`、`roic_str`、`img_wr_str`、
`img_corr_str` 和 `dync_str` 触发后，驱动读取 `INT_VECTOR` 清硬件 sticky，并把 int_vector 事件交给
`pa_controller`；如果没有加载驱动或没有 `/dev/pa_irq` 节点，应用会回退到原来的
`INT_VECTOR` 轮询，轮询间隔由 `PA_PU_IRQ_POLL_INTERVAL_US` 配置。
RS422 调试协议不再暴露 `WAIT_IRQ`。
