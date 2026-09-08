# PA Controller 业务逻辑与工作流设计

版本：V0.2  
日期：2026-09-07  
状态：设计梳理稿，作为后续配置文件、正式协议和代码收敛依据。

## 1. 目标

当前 `pa_controller` 已经包含静态采图、动态采图、模板制作、模板上传、寄存器调试和压力测试等命令。研发阶段这样方便定位问题，但如果直接把这些命令都暴露给上位机，后续协议会变得混乱，也容易出现上位机在错误时机改寄存器的问题。

本设计的目标是把下位机业务逻辑分成三层：

1. 正式业务工作流：上位机长期稳定使用的接口。
2. 配置管理：把 GIC、ROIC、CORR、Dynamic、模板路径、时间参数统一收敛到配置文件。
3. 调试入口：保留研发和现场 bring-up 使用，但不作为正式上位机业务协议的一部分。

后续二进制协议应优先围绕正式业务工作流设计，而不是围绕现有 ASCII 调试命令一比一搬运。

## 2. RS422 通信收敛原则

RS422 带宽有限，而且现场串口链路需要优先保证可靠、可恢复、易定位。因此正式协议不能采用当前调试命令这种“一个命令后面跟很多 `key=value` 参数”的方式。

正式产品通信建议遵循下面原则：

- 上位机不在每次采图命令中携带完整 GIC/ROIC/CORR/Dynamic 参数。
- 下位机维护当前配置和配置文件，上位机只修改少量配置项或配置组。
- 业务命令只表达动作，例如开始静态采图、开始动态采图、停止动态采图、开始模板制作。
- 复杂配置通过“参数 ID + 参数值”或“配置组分包”写入，下位机收到后立即校验并保存，不要求用户再发一次 `SAVE_CONFIG`。
- 研发调试可以保留 ASCII 长命令，但正式协议不依赖这些命令。
- 上位机 UI 可以显示完整参数表，但底层发送时应拆成多个短帧，而不是一次发送很长字符串。

推荐把协议对象抽象为下面几类，方便后续产品复用：

```text
设备状态       DeviceStatus
工作模式       WorkMode
配置项         ConfigItem
配置组         ConfigGroup
业务动作       Action
后台任务       Job
调试寄存器     DebugRegister
```

这样后续产品即使 FPGA 寄存器变化，也可以继续沿用同一套上位机到下位机的工作流：

```text
连接设备 -> 读取能力 -> 读取配置摘要 -> 修改配置项 -> 执行业务动作 -> 查询状态/接收事件
```

## 3. 总体运行模型

### 3.1 启动流程

程序启动后建议按下面流程执行：

```text
读取编译默认参数
-> 检查配置文件是否存在
-> 配置文件不存在：按编译默认参数生成一份配置文件
-> 配置文件存在：读取配置文件并做范围校验
-> 打开 PA/PU 寄存器映射
-> 打开 UIO 共享 DDR
-> 尝试打开 /dev/pa_irq，失败则回退 INT_VECTOR 轮询
-> 加载 offset/gain 模板文件到对应 DDR
-> 按配置文件下发一次 GIC/ROIC/CORR 基础配置
-> 根据配置决定是否自动进入默认工作模式
-> 进入 RS422/stdio 命令循环
```

启动时必须保证“配置来源”清晰：

- 编译默认参数只作为出厂默认值和配置文件缺失时的初始值。
- 实际运行参数以配置文件为准。
- 配置文件需要区分参数权限：哪些允许上位机修改，哪些只允许出厂/研发修改，哪些完全由设备树或硬件自动发现。
- 面向最终用户的正式上位机修改可写参数后，下位机应立即更新内存配置并保存配置文件，不要求用户再发 `SAVE_CONFIG`。
- 研发调试模式可以支持只改内存不保存，用于临时验证。
- 业务采图开始前，ARM 应从当前内存配置重新下发关键硬件配置，避免 FPGA 寄存器被调试命令或异常流程污染。

### 3.2 配置文件建议位置

建议增加 Makefile 可配置项：

```makefile
APP_CONFIG_FILE ?= /usr/local/pa_controller/config.ini
```

也应支持配置为软件运行目录下的相对路径，例如：

```makefile
APP_CONFIG_FILE ?= ./config.ini
```

如果后续根文件系统有标准持久化分区，也可以改为：

```makefile
APP_CONFIG_FILE ?= /etc/pa_controller/config.ini
```

当前 Makefile 默认使用 `./config.ini`，方便研发阶段直接查看和修改；正式部署时可以通过 `APP_CONFIG_FILE` 指向 `/etc/pa_controller/config.ini` 或设备指定目录。模板文件路径仍可通过配置文件中的 `[template]` 配置项独立设置。

## 4. 配置文件内容

配置文件建议使用 INI 或简单 key-value 格式。项目参数不复杂，INI 易读、易手改、易从 C 代码实现。

### 4.1 配置权限分类

配置项建议分成四类：

```text
factory       出厂/硬件配置，正式上位机不可修改
service       维护/工程配置，服务模式或工程工具可修改
user          用户业务配置，正式上位机可修改并自动保存
runtime       运行态派生参数，不落盘，ARM 根据模式和配置自动生成
```

典型分类：

```text
factory:
  image.width / image.height / uio.* / pa_pu.base_addr

service:
  roic.reg_* / irq.timeout_ms / write_verify_attempts / dynamic step 表

user:
  static.exposure_window_ms / static.dark_window_ms / corr.offset_en / corr.gain_en / corr.defect_en

runtime:
  gic.req_code / gic.dout_en / corr.offset_corr_mode / img_wr_addr / dynamic 当前输出地址
```

其中 `runtime` 类型尤其重要：它们不应该被当成普通配置项让上位机每次写。比如 `gic_req_code`、`gic_dout_en`、`offset_corr_mode` 都会随静态采图、动态采图、模板制作自动变化，配置文件最多保存默认策略，真正写寄存器的值由当前业务流程决定。

建议结构如下：

```ini
[app]
auto_start=0
default_mode=0

[image]
width=3072
height=7680
row_offset=0
col_offset=0
frame_count=9
frame_align=4096

[uio]
offset_device=/dev/uio0
gain_device=/dev/uio1
image_device=/dev/uio2

[gic]
default_req_code=0
default_dout_en=1
line_time_ns=25600
oe_rise_ns=0
oe_fall_ns=0
start_row=0
end_row=7679
binning=0

[roic]
start_col=0
end_col=3071
binning=0
reg_00=0x0000
reg_02=0x0000
reg_05=0x0000
reg_06=0x0000
reg_07=0x0000
reg_09=0x0000
reg_0a=0x0000
reg_0b=0x0000
reg_0c=0x0000
reg_0d=0x0000
reg_0e=0x0000
reg_0f=0x0000
reg_10=0x0000
reg_11=0x0000
reg_17=0x0000
reg_24=0x0000
reg_28=0x0000
reg_2d=0x0000
reg_3b=0x0000

[corr]
row_num=7680
col_num=3072
pkg_num=46080
offset_en=1
offset_adder_value=100
default_offset_corr_mode=0
gain_en=1
gain_clipping_value=55000
defect_en=0

[static_idle]
idle_clean_interval_ms=50
idle_clean_timeout_ms=1000
exposure_window_ms=50
dark_window_ms=50
capture_timeout_ms=1000

[dynamic]
cycle=10
image_start_addr=auto
image_end_addr=auto
start_timeout_ms=1000
state_poll_interval_ms=10
stop_timeout_ms=2000
step0_en=1
step0_req=0
step0_time_ms=50
step1_en=1
step1_req=4
step1_time_ms=50
step2_en=0
step2_req=0
step2_time_ms=0
# step3~step9 同样保留，未启用时 en=0

[template]
offset_file=/usr/local/offset.raw
gain_file=/usr/local/gain.raw
cal_gain_dir=/usr/local/calib
dynamic_offset_total_frames=12
dynamic_offset_valid_frames=8
```

说明：

- `image_start_addr=auto` 和 `image_end_addr=auto` 表示从 UIO map0 的物理地址和 size 计算，不再要求 Makefile 手工维护物理地址。
- `pkg_num` 默认按 `row_num * col_num * 2 / 1024` 生成。配置文件可以保存实际值，方便现场确认。
- `frame_count` 不应超过 UIO 图像池能容纳的完整帧数。启动校验失败时应拒绝进入业务模式。
- `default_req_code/default_dout_en/default_offset_corr_mode` 只是默认策略，不代表每个业务流程都按这个值写寄存器。

## 5. 正式业务命令

正式协议建议只保留面向业务动作的命令，不直接暴露 GIC/ROIC/CORR/IMG_WR 的底层细节。

### 5.1 基础命令

```text
HELLO
VERSION
STATUS
SET_TIME
GET_TIME
GET_CONFIG
SET_CONFIG
RESET_CONFIG
```

职责：

- `HELLO`：建立通信、确认协议版本和设备能力。
- `VERSION`：返回 app 版本、FPGA 版本、适配板卡版本。
- `STATUS`：返回工作状态、错误状态、模板状态、DDR 图像池状态。
- `SET_TIME/GET_TIME`：用于上位机同步板端时间。
- `GET_CONFIG`：读取配置摘要、配置组或单个配置项。
- `SET_CONFIG`：修改单个配置项或配置组分片；正式用户模式下修改成功即自动保存。
- `RESET_CONFIG`：恢复编译默认配置，并重新生成配置文件。

正式协议里不建议把 `SAVE_CONFIG` 作为普通用户命令。原因是用户上位机修改参数后通常期望立即生效且下次开机仍有效，额外保存命令容易遗漏。`SAVE_CONFIG` 可以作为研发/服务模式命令保留，但不应进入普通业务工作流。

### 5.2 配置命令的短帧形式

为适应 RS422，正式配置命令建议不要使用长文本，也不要一次发送完整 INI。推荐两种方式：

```text
GET_CONFIG_SUMMARY
GET_CONFIG_ITEM item_id
SET_CONFIG_ITEM item_id value
GET_CONFIG_GROUP group_id offset length
SET_CONFIG_GROUP group_id offset chunk
RESET_CONFIG_GROUP group_id
```

其中：

- `item_id` 是固定编号，例如 `0x0201 = static.exposure_window_ms`。
- `group_id` 是配置组编号，例如 `0x02 = static`，`0x03 = dynamic`。
- `SET_CONFIG_ITEM` 只改一个参数，帧很短，适合 RS422。
- `SET_CONFIG_GROUP` 用于上位机一次性同步某个参数页面，可分片发送。
- 下位机每收到一个可写参数都做范围校验，失败时返回错误码和参数 ID。
- 正式用户模式下写成功即保存配置文件；研发模式可以带 `no_save` 标志只改内存。

这样上位机 UI 即使有很多配置项，底层也只是多发几条短帧，不会出现一条超长命令。

### 5.3 静态模式命令

```text
START_STATIC_CAPTURE
```

静态模式正式业务入口应尽量只有一次采图请求。其它参数都应在采图前通过配置项维护好，采图命令本身不携带 GIC/ROIC/CORR 参数。

一次静态采图建议流程：

```text
确认当前没有 Dynamic/模板/上传任务
-> 如 Static Idle 自清空正在进行，等待本次自清空结束
-> 按当前配置重新下发 GIC/ROIC/CORR 基础参数
-> 等待 exposure_window_ms
-> 第一帧 light：关闭 offset/gain/defect，写入 offset 模板 DDR
-> 等待 IMG_CORR + IMG_WR + GIC 完成
-> 等待 dark_window_ms
-> 第二帧 output：按配置启用 offset/gain/defect，写入 uio2 图像池
-> 等待 IMG_CORR + IMG_WR + GIC 完成
-> 返回 output 图像地址、capture_id 和状态
```

当前代码中回包字段仍使用 `bright_addr/dark_addr`，后续协议建议改为更清楚的名称：

```text
offset_addr
output_addr
capture_id
```

### 5.4 动态模式命令

```text
START_DYNAMIC
STOP_DYNAMIC
QUERY_DYNAMIC
```

动态模式应由 FPGA `dynamic_ctrl` 自主运行。ARM 的职责是：

```text
按配置下发 GIC/ROIC/CORR 基础参数
-> 下发 dynamic cycle、图像地址范围和 step 表
-> 写 DYNC_STR
-> 查询 dync_state/dync_end/dync_debug_out
-> 收到 STOP_DYNAMIC 后写 DYNC_STOP
-> 等 dync_state 回到 idle
```

动态模式不建议让命令线程长时间阻塞等待全部 cycle 完成。更合理的方式是：

- `START_DYNAMIC`：启动后尽快返回启动结果。
- `QUERY_DYNAMIC`：上位机按需查询运行状态。
- `STOP_DYNAMIC`：请求停止，并返回停止是否完成。
- 动态图像上图由 PCIe 中断和上位机自动接收链路处理，不应依赖 RS422 每帧回包。

动态配置不应通过一条带 10 个 step 参数的长命令完成。建议 step 表放在配置文件或配置组里：

```text
SET_CONFIG_ITEM dynamic.cycle
SET_CONFIG_GROUP dynamic.steps
START_DYNAMIC
```

如果后续不同产品或不同曝光流程很多，可以进一步引入 `profile_id`：

```text
SET_ACTIVE_PROFILE profile_id
START_DYNAMIC
```

`profile` 表示一套完整工作流参数，ARM 根据 profile 下发 FPGA 寄存器。这样上位机只需要选择“模式/方案”，不用理解每个 FPGA 寄存器。

### 5.5 模板制作命令

```text
MAKE_OFFSET
MAKE_DYNAMIC_OFFSET
CAL_GAIN_BEGIN
CAL_GAIN_CAPTURE
CAL_GAIN_BUILD
CAL_GAIN_CANCEL
CAL_STATUS
UPLOAD_TEMPLATE
```

模板制作属于较长任务，建议统一按“启动任务 + 查询状态 + 完成事件/结果”的方式设计。

offset 模板：

```text
MAKE_OFFSET：采一帧或多帧暗场/亮场，生成 offset.raw，并加载到 offset DDR
MAKE_DYNAMIC_OFFSET：动态模式采集 total_frames，只取最后 valid_frames 做逐像素均值
```

gain/defect 模板：

```text
CAL_GAIN_BEGIN：设置灰度级数组、每级帧数、坏点阈值
CAL_GAIN_CAPTURE：上位机切换灰度级后触发该级采集
CAL_GAIN_BUILD：根据各灰度级均值图生成 gain.raw，坏点通过 gain=0 标记
CAL_GAIN_CANCEL：取消当前模板任务
CAL_STATUS：查询任务进度
```

模板上传：

```text
UPLOAD_TEMPLATE template=offset
UPLOAD_TEMPLATE template=gain
```

模板上传只负责把 DDR 中已有模板通过 FPGA 上传模块发出，不应混入正常 Dynamic 上图流程。

## 6. 工作模式与 FPGA 通用抽象

为了让后续产品继续沿用同一套工作流，建议正式业务层不要直接命名为 GIC/ROIC/CORR 寄存器操作，而是抽象成工作模式和动作。

推荐内部模型：

```text
WorkMode:
  Idle
  Static
  Dynamic
  Calibration
  Upload
  Debug

Action:
  Prepare
  CaptureOnce
  StartContinuous
  StopContinuous
  BuildTemplate
  UploadTemplate
  QueryStatus
```

FPGA 模块能力用 capability 描述：

```text
has_gic
has_roic
has_img_corr
has_img_wr
has_dynamic_ctrl
has_img_upload
irq_bits
max_dynamic_steps
image_addr_source
```

正式协议只关心：

```text
设备支持什么能力
当前模式是什么
当前配置是什么
执行哪个动作
动作结果是什么
```

至于某个动作内部需要写哪些 FPGA 寄存器，由 ARM 根据当前硬件版本和配置文件决定。这样如果后续 FPGA 寄存器变了，上位机协议可以尽量不变。

## 7. 调试命令边界

以下命令建议保留为 ASCII 调试命令，后续正式二进制协议中默认不暴露给普通上位机业务流程：

```text
CONFIG_GIC
START_GIC
CONFIG_ROIC
START_ROIC
CONFIG_CORR
START_CORR
START_CORR_GIC
CONFIG_DYNC
START_DYNC
READ_REG
WRITE_REG
DUMP_REGS
LOOP_STATIC_IDLE_CAPTURE
LOOP_CAPTURE_ADDR
CONFIG_IMG_UPLOAD
START_IMG_UPLOAD
```

调试命令的使用原则：

- 只用于研发、现场 bring-up、FPGA 联调和压力测试。
- 调试写寄存器必须避开正式采图、动态模式和模板任务。
- `READ_REG int_vector` 会清中断，不能和正式等待中断流程同时使用。
- `LOOP_*` 压测命令不进入正式协议，只作为开发阶段定位稳定性问题的工具。

## 8. GIC、ROIC、CORR 配置策略

### 8.1 开机基础配置

开机后应根据配置文件先下发一次：

```text
GIC 默认时序
ROIC 默认寄存器表
CORR 默认尺寸、模板地址和校正参数
Dynamic 默认 step 表和图像地址范围
```

这样做的好处：

- 上位机打开后可以直接查询状态，知道当前硬件基础配置。
- 首次采图前硬件不是未知状态。
- 后续 `START_STATIC_CAPTURE` 和 `START_DYNAMIC` 可以重新下发关键参数，保证业务动作自洽。

### 8.2 业务动作前重新下发

虽然开机已经配置过，正式业务动作前仍建议重新下发关键寄存器：

- 静态采图前：GIC、ROIC、CORR、IMG_WR 地址。
- 动态启动前：GIC、ROIC、CORR、DYNAMIC step 和地址范围。
- 模板上传前：IMG_UPLOAD 地址、行列、pkg_num。

原因是研发阶段仍存在调试命令，可能修改 FPGA 寄存器。业务动作前重新下发可以把流程拉回当前配置文件描述的状态。

注意：这里的“当前配置文件描述的状态”不是简单把 `[gic] req_code/dout_en` 原样写入所有流程，而是 ARM 根据业务动作派生最终寄存器值。例如：

```text
空闲自清空：gic_req_code=serial scan，gic_dout_en=0
静态第一帧 offset：gic_dout_en=1，corr offset/gain/defect 全关
静态第二帧 output：gic_dout_en=1，corr 开关按配置
动态模式：dynamic_ctrl 根据 step/profile 运行，corr mode 按模式自动选择
```

### 8.3 写寄存器校验策略

寄存器写入建议继续分两类：

- 配置寄存器：写后立即读回校验，校验次数由 Makefile/配置项控制，可为 0。
- 触发/使能寄存器：只写不校验，例如 `*_str`、`*_stop`、部分 enable 信号。

后续可以把校验次数也放入配置文件：

```ini
[debug]
write_verify_attempts=2
```

## 9. Static Idle 是否用 dynamic_ctrl 实现

静态模式理论上可以用 FPGA `dynamic_ctrl` 组合步骤实现，但要满足几个前提。

### 9.1 当前 ARM 静态流程

当前 ARM 静态流程更明确：

```text
ARM 控制 exposure_window_ms
-> ARM 配置第一帧写 offset DDR，校正全关
-> ARM 同时启动 IMG_CORR/IMG_WR/GIC
-> ARM 等三个完成中断
-> ARM 控制 dark_window_ms
-> ARM 配置第二帧写 output DDR，按配置开校正
-> ARM 同时启动 IMG_CORR/IMG_WR/GIC
-> ARM 等三个完成中断
```

优点：

- 每一步日志清楚。
- 失败阶段容易定位。
- 适合当前 FPGA/DDR 稳定性问题还在排查的阶段。

缺点：

- ARM 参与逐帧调度，时间精度受 Linux 调度影响。
- RS422 命令线程和工作线程需要做互斥。

### 9.2 dynamic_ctrl 承接静态流程的条件

如果希望后续静态模式也由 `dynamic_ctrl` 实现，FPGA step 至少需要支持：

- 每个 step 独立配置 `req_code` 和时间。
- 每个 capture step 能独立选择 `gic_dout_en`。
- 每个 capture step 能独立选择 IMG_WR 目标地址。
- 每个 capture step 能独立选择 offset/gain/defect 使能。
- 能明确返回本次静态流程完成状态和最终 output DDR 地址。
- 能区分第一帧 offset 模板和第二帧 output 图。

如果 `dynamic_ctrl` 目前只有 `req_code + time`，则还不足以完整替代当前 ARM 静态流程。短期建议继续保持 ARM 静态流程，等 FPGA dynamic step 能描述完整采图动作后，再统一迁移。

## 10. 时间参数梳理

时间参数建议按用途分组，不要混在一个 timeout 里。

### 10.1 静态模式时间

```text
idle_clean_interval_ms  空闲自清空间隔
idle_clean_timeout_ms   等 GIC 自清空完成的超时
exposure_window_ms      上位机请求采图后，第一帧前曝光窗口
dark_window_ms          第一帧完成后，第二帧前窗口
capture_timeout_ms      单帧等待 IMG_CORR + IMG_WR + GIC 完成的超时
```

当前研发默认值可先保持：

```text
idle_clean_interval_ms = 50
exposure_window_ms = 50
dark_window_ms = 50 或现场验证值
capture_timeout_ms = 1000
```

注意：如果现场发现 `dark_window_ms=50` 不稳定而 `300/500` 稳定，应把默认值写入配置文件，并在文档里标注对应 FPGA bitstream 版本和测试结论。

### 10.2 动态模式时间

```text
dynamic_start_timeout_ms      写 DYNC_STR 后等待 dync_state 置位
dynamic_state_poll_interval_ms 运行期间查询 dync_state 的间隔
dynamic_stop_timeout_ms       写 DYNC_STOP 后等待 dync_state 清零
dynamic_log_interval_ms       运行期间状态日志打印间隔
```

动态模式有限 cycle 是否需要总超时，要结合 FPGA 真实循环次数和曝光时间决定。当前阶段可以不设置总超时，只通过 `QUERY_DYNAMIC` 和 `STOP_DYNAMIC` 管理。

### 10.3 模板时间

```text
template_capture_interval_ms   模板多帧采集间隔
template_capture_timeout_ms    单帧模板采集等待超时
dynamic_offset_total_frames    动态 offset 总采集张数
dynamic_offset_valid_frames    动态 offset 参与均值的最后几张
```

模板制作通常是长任务，命令层不应长时间占住通信线程。建议后台任务执行，上位机通过 `CAL_STATUS` 或事件获取进度。

## 11. 状态机与互斥

建议正式业务状态统一如下：

```text
BOOTING
IDLE
STATIC_CLEANING
STATIC_WAIT_EXPOSURE
STATIC_CAPTURE_OFFSET
STATIC_WAIT_DARK
STATIC_CAPTURE_OUTPUT
DYNAMIC_STARTING
DYNAMIC_RUNNING
DYNAMIC_STOPPING
TEMPLATE_RUNNING
UPLOAD_RUNNING
ERROR
```

互斥规则：

- `STATIC_CAPTURE_*` 期间禁止修改 GIC/ROIC/CORR/IMG_WR 配置。
- `DYNAMIC_RUNNING` 期间只允许 `STOP_DYNAMIC`、`QUERY_DYNAMIC`、`STATUS`。
- `TEMPLATE_RUNNING` 期间禁止普通静态/动态采图。
- `UPLOAD_RUNNING` 期间禁止再次启动上传，是否允许采图需结合 FPGA 上传模块和 SFP 通路确认。
- 进入 `ERROR` 后不再自动自清空，也不再继续动 FPGA，等待上位机查询状态或人工复位。

## 12. 上位机视角的推荐工作流

### 12.1 首次连接

```text
HELLO
VERSION
GET_CONFIG_SUMMARY
STATUS
```

如果配置文件不存在，下位机应已经自动生成默认配置。上位机可以读取后展示当前参数。

### 12.2 修改参数

正式上位机修改参数时，不建议一次性发送一整串参数。推荐按配置项或配置组分片发送：

```text
GET_CONFIG_ITEM static.exposure_window_ms
SET_CONFIG_ITEM static.exposure_window_ms 50
SET_CONFIG_ITEM corr.offset_en 1
SET_CONFIG_ITEM corr.gain_en 1
GET_CONFIG_SUMMARY
STATUS
```

给用户使用的正式模式下，`SET_CONFIG_ITEM` 成功后立即更新内存配置并保存配置文件。调试阶段可以有工程界面直接修改寄存器或只改内存配置，但这不作为普通业务流程。

### 12.3 静态采图

```text
START_STATIC_CAPTURE
```

静态采图命令不携带参数。是否需要后台自清空线程、是否等待当前自清空结束、第一帧写哪里、第二帧校正开关如何配置，都由 ARM 根据当前配置自动完成。上位机只关心返回的 `capture_id`、`output_addr` 和结果状态。

### 12.4 动态采图

```text
SET_CONFIG_ITEM dynamic.cycle 0
SET_CONFIG_GROUP dynamic.steps ...
START_DYNAMIC
QUERY_DYNAMIC
STOP_DYNAMIC
```

动态图像通过 PCIe 自动上图，上位机不需要通过 RS422 等待每帧。

如果配置已经提前保存好，常规用户流程只需要：

```text
START_DYNAMIC
STOP_DYNAMIC
```

### 12.5 模板制作

```text
MAKE_DYNAMIC_OFFSET
CAL_STATUS
UPLOAD_TEMPLATE template=offset

CAL_GAIN_BEGIN
CAL_GAIN_CAPTURE
CAL_GAIN_BUILD
CAL_STATUS
UPLOAD_TEMPLATE template=gain
```

模板制作和上传完成后，下位机应更新模板状态，`STATUS` 能返回模板是否有效、文件路径、生成时间和最后错误。

## 13. 与当前代码的差距

当前代码已经具备的能力：

- GIC/ROIC/CORR/IMG_WR/DYNAMIC/IMG_UPLOAD 寄存器封装。
- Static Idle 后台自清空和两阶段静态采图。
- Dynamic 独立线程启动、状态查询和停止。
- offset/gain 模板加载、制作和上传命令。
- UIO 地址和 size 读取。
- `/dev/pa_irq` 和 `INT_VECTOR` 轮询两种中断等待方式。

已落地的配置管理能力：

- 启动时读取 `APP_CONFIG_FILE`；文件不存在时按 Makefile 默认值生成 `./config.ini`。
- 支持配置摘要、配置项读写、配置组读取、配置组写入和恢复默认配置。
- 配置项或配置组写入成功后立即应用并保存，采图/动态运行期间返回 BUSY。
- 支持 `QUERY_DYNAMIC` 查询 Dynamic 状态和最终图像地址。
- 正式动作名 `START_STATIC_CAPTURE`、`START_DYNAMIC`、`STOP_DYNAMIC` 已兼容到现有工作流；旧研发命令继续保留。

仍建议补充或收敛的部分：

- 把正式业务命令和调试命令在代码结构上分开。
- 增加配置项 ID、配置组 ID、权限分类和统一错误码，避免正式 RS422 协议传超长参数。
- `STATUS` 增加配置文件状态、模板状态、PCIe/DDR 图像池状态。
- 静态模式回包字段从 `bright_addr/dark_addr` 迁移到 `offset_addr/output_addr`。
- 增加正式二进制协议帧、CRC、序号、ACK/DONE/ERR 和主动事件。

## 14. 下一步建议

建议按下面顺序推进：

1. 定义正式协议的配置项 ID、配置组 ID、权限和错误码。
2. 把正式业务命令和调试命令在代码结构上分开。
3. `STATUS` 增加配置文件、模板、DDR 图像池和 Dynamic 任务摘要。
4. 二进制协议先实现基础命令、短配置命令和状态查询，再迁移静态/动态/模板业务。
5. 增加固定测试向量、拆包/粘包/丢字节/CRC 错误测试。
