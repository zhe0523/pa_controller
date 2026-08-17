# PA Controller Static Idle 工作模式设计

版本：草案 V0.1
日期：2026-08-13
适用对象：ARM `pa_controller`、Qt 上位机、PA/FPGA 控制链路

## 1. 背景

旧产品中，完整采集时序主要由 FPGA 主状态机完成。ARM/PS 侧通常只需要设置工作模式、校正模式、曝光参数、自清空间隔等产品参数，然后通知 FPGA 开始出图。

当前项目中，FPGA 侧被拆成若干可独立触发的功能块：

```text
GIC
ROIC
IMG_CORR
IMG_WR / SFP
DDR
INT_VECTOR / F2P IRQ
```

因此 ARM 侧需要承担一部分原先 FPGA 主状态机的职责：按照产品工作模式组织寄存器配置、触发顺序、等待窗口和完成中断。

现有 ASCII 命令如 `CONFIG_GIC`、`START_GIC`、`CONFIG_CORR`、`START_CORR_GIC`、`READ_REG`、`WRITE_REG` 应继续保留为调试命令，但正式采图流程不应由上位机逐条拼接这些调试命令完成，而应由 ARM 内部工作模式状态机统一执行。

## 2. 第一阶段目标

第一阶段只实现静态模式下的 `Idle` 工作流程。

暂不实现：

```text
AED
SyncOut
SyncIn
Prep
Continuous
Inner
FreeSync
DDR
```

但代码结构需要为后续工作模式预留扩展位置。

旧产品工作模式枚举可作为产品层参考：

```c
enum WorkMode {
  Idle,
  AED = 2,
  SyncOut,
  SyncIn = 5,
  Prep,
  Continuous,
  Inner,
  FreeSync,
  DDR,
};
```

当前第一版建议只落地：

```c
WORK_MODE_IDLE = 0
```

## 3. 总体职责分层

建议新增 `work_mode.c/.h`，把正式业务流程从 `command_handler.c` 中分离出来。

```text
command_handler.c
  负责解析上位机命令
  负责保留调试命令
  负责把正式业务请求投递给 work_mode

work_mode.c
  负责 WorkMode / WorkState 状态机
  负责 Idle 自清空
  负责静态采图两阶段流程
  负责配置快照和线程同步

pa_pu.c
  负责底层 PA/PU 寄存器读写
  负责配置 GIC/ROIC/IMG_CORR
  负责启动 GIC/ROIC/IMG_CORR/IMG_WR
  负责等待 INT_VECTOR / pa_irq
```

原则：

```text
产品流程放在 work_mode
寄存器动作放在 pa_pu
协议解析放在 command_handler
```

## 4. Static Idle 流程

设备处于空闲状态时，ARM 周期性执行 GIC 自清空。

### 4.1 Idle 自清空

自清空动作：

```text
配置 GIC:
  gic_req_code = 0
  gic_dout_en = 0
  其它 GIC 时序参数使用 static idle 配置

写 GIC_STR = 1
等待 GIC 完成中断 bit
```

说明：

- `gic_req_code = 0` 表示 serial scan。
- `gic_dout_en = 0` 表示不输出数据，只用于清空 GIC。
- 自清空间隔由 `idle_clean_interval_ms` 配置。
- 如果一次自清空未完成，上位机采图请求需要等待这次自清空结束。

### 4.2 收到静态采图请求

上位机下发静态采图命令后，ARM 不应立即打断正在执行的自清空。

推荐行为：

```text
如果 Idle 正在执行自清空:
  标记 pending_capture = true
  等待当前 GIC 自清空完成
  进入静态采图流程

如果 Idle 正在等待下一次自清空:
  立即进入静态采图流程
```

## 5. 静态采图两阶段流程

一次静态采图包含两次采集：

```text
亮场阶段
暗场阶段
```

两次图像均需要：

```text
写入 DDR
通过 SFP 光口发送
等待 IMG_CORR / IMG_WR / GIC 三个完成中断
```

这里的“亮场/暗场”是业务阶段名，不等同于模板制作命令 `MAKE_GAIN/MAKE_OFFSET`。

### 5.1 曝光窗口

在自清空结束后，先进入曝光窗口：

```text
等待 exposure_window_ms
```

曝光窗口时间由上位机或默认配置决定。

### 5.2 亮场采图

亮场阶段寄存器配置：

```text
配置 GIC:
  gic_req_code = 0
  gic_dout_en = 1
  start_row / end_row / binning / line_time 使用 static idle 配置

配置 IMG_CORR:
  img_corr_offset_en = 0
  img_corr_gain_en   = 0
  img_corr_defect_en = 0
  row / col / pkg / template_addr 等尺寸和地址仍按默认或配置写入

配置 IMG_WR:
  img_wr_str_addr = 本次亮场图像 DDR 首地址

触发顺序:
  写 IMG_CORR_STR = 1
  写 IMG_WR_STR = 1
  写 GIC_STR = 1

三个 start 寄存器应在软件上连续写 1，作为同一次采图动作启动。

等待:
  等待 IMG_CORR 完成 bit
  等待 IMG_WR 完成 bit
  等待 GIC 完成 bit
```

说明：

- 亮场阶段强制关闭所有校正模块。
- 这张图仍然走 DDR 和 SFP。
- `img_wr_str_addr` 由 ARM 从 DDR 图像池中分配，用于指定本次图像写入 DDR 的首地址。
- 当前 `START_CORR_GIC` 只能作为早期调试参考；正式流程需要同时触发 `IMG_CORR_STR`、`IMG_WR_STR` 和 `GIC_STR`。

### 5.3 暗场窗口

亮场采图完成后进入暗场窗口：

```text
等待 dark_window_ms
```

暗场窗口时间由上位机或默认配置决定。

### 5.4 暗场采图

暗场阶段寄存器配置：

```text
配置 GIC:
  gic_req_code = 0
  gic_dout_en = 1
  start_row / end_row / binning / line_time 使用 static idle 配置

配置 IMG_CORR:
  img_corr_offset_en = 上位机配置值
  img_corr_gain_en   = 上位机配置值
  img_corr_defect_en = 上位机配置值
  其它尺寸、模板地址、offset_adder、gain_clip 使用 static idle 配置

配置 IMG_WR:
  img_wr_str_addr = 本次暗场图像 DDR 首地址

触发顺序:
  写 IMG_CORR_STR = 1
  写 IMG_WR_STR = 1
  写 GIC_STR = 1

三个 start 寄存器应在软件上连续写 1，作为同一次采图动作启动。

等待:
  等待 IMG_CORR 完成 bit
  等待 IMG_WR 完成 bit
  等待 GIC 完成 bit
```

说明：

- 暗场阶段是否打开 offset/gain/defect 由上位机配置。
- 这张图仍然走 DDR 和 SFP。
- `img_wr_str_addr` 由 ARM 从 DDR 图像池中分配，用于指定本次图像写入 DDR 的首地址。

## 6. 状态机建议

建议产品层状态拆成 `WorkMode` 和 `WorkState`。

### 6.1 WorkMode

第一版只启用 `Idle`：

```c
typedef enum {
  WORK_MODE_IDLE = 0,
  WORK_MODE_AED = 2,
  WORK_MODE_SYNC_OUT = 3,
  WORK_MODE_SYNC_IN = 5,
  WORK_MODE_PREP = 6,
  WORK_MODE_CONTINUOUS = 7,
  WORK_MODE_INNER = 8,
  WORK_MODE_FREE_SYNC = 9,
  WORK_MODE_DDR = 10,
} work_mode_t;
```

### 6.2 WorkState

第一版建议状态：

```c
typedef enum {
  WORK_STATE_STOPPED = 0,
  WORK_STATE_IDLE_WAIT,
  WORK_STATE_IDLE_CLEANING,
  WORK_STATE_EXPOSURE_WINDOW,
  WORK_STATE_BRIGHT_CAPTURE,
  WORK_STATE_DARK_WINDOW,
  WORK_STATE_DARK_CAPTURE,
  WORK_STATE_ERROR,
} work_state_t;
```

状态含义：

| 状态 | 含义 |
|---|---|
| `STOPPED` | 工作线程未运行或已停止 |
| `IDLE_WAIT` | Idle 等待下一次自清空或采图请求 |
| `IDLE_CLEANING` | 正在执行 GIC 自清空 |
| `EXPOSURE_WINDOW` | 自清空完成后等待曝光窗口 |
| `BRIGHT_CAPTURE` | 亮场采图 |
| `DARK_WINDOW` | 亮场完成后等待暗场窗口 |
| `DARK_CAPTURE` | 暗场采图 |
| `ERROR` | 工作流程发生错误，等待查询或复位 |

## 7. 配置结构建议

建议新增正式业务配置结构，不直接暴露零散寄存器字段给上位机。

```c
typedef struct {
  uint32_t idle_clean_interval_ms;
  uint32_t exposure_window_ms;
  uint32_t dark_window_ms;

  pa_pu_gic_config_t clean_gic;
  pa_pu_gic_config_t bright_gic;
  pa_pu_gic_config_t dark_gic;

  pa_pu_corr_config_t bright_corr;
  pa_pu_corr_config_t dark_corr;
} static_idle_config_t;
```

配置默认值建议放到 `app_config.h`，并支持 Makefile 覆盖。

### 7.1 默认配置建议

```text
STATIC_IDLE_CLEAN_INTERVAL_MS  默认 50 ms
STATIC_IDLE_EXPOSURE_MS        默认 50 ms
STATIC_IDLE_DARK_WINDOW_MS     默认 50 ms
```

两次采图的 DDR 写图地址由 ARM 在每次采图前写入 `img_wr_str_addr` 寄存器。
当前测试实现中，第一帧未校正 light 固定写到 `/dev/uio0` 的物理地址，作为 offset 模板；第二帧实际输出图
从 `/dev/uio2` 图像池按 `frame_stride` 环形分配。尾部剩余空间不足一帧时回到池起始地址。

DDR 图像池第一版按以下口径设计：

```text
后续使用 uio2
总大小由设备树/uio2 的 sysfs size 决定
FPGA 每次只接收一个图像首地址
ARM 负责选择本次写图首地址
```

具体边界策略见“DDR 图像池管理”章节。

GIC 默认：

```text
clean_gic.req_code = 0
clean_gic.dout_enable = false

bright_gic.req_code = 0
bright_gic.dout_enable = true

dark_gic.req_code = 0
dark_gic.dout_enable = true
```

CORR 默认：

```text
bright_corr.offset_enable = false
bright_corr.gain_enable = false
bright_corr.defect_enable = false

dark_corr.offset_enable = 上位机配置或默认配置
dark_corr.gain_enable = 上位机配置或默认配置
dark_corr.defect_enable = 上位机配置或默认配置
```

## 8. DDR 图像池管理

Static Idle 的两次采图 DDR 地址由 ARM 管理。FPGA 不感知图像池结构；FPGA 每次只接收 `img_wr_str_addr` 中的一个图像首地址。

### 8.1 图像池范围

当前第一帧 offset 模板固定到 uio0，第二帧输出图使用 `uio2` 作为环形图像池：

```text
FPGA_OFFSET_UIO_DEVICE = /dev/uio0
FPGA_GAIN_UIO_DEVICE   = /dev/uio1
FPGA_IMAGE_UIO_DEVICE  = /dev/uio2
addr / size            = 由 /sys/class/uio/uioN/maps/map0 读取
```

ARM 需要同时保存：

```text
pool_phys_base   // 写给 FPGA 的物理地址基准
pool_virt_base   // ARM mmap 后访问用的虚拟地址
pool_size        // 由 uio2 sysfs size 决定
write_offset     // 下一张图候选偏移
frame_stride     // 每张图占用空间，建议按 ACTIVE_IMAGE_BYTES 向上对齐
```

### 8.2 分配规则

每次需要采实际输出图时，ARM 从图像池分配一个完整帧槽位：

```text
if write_offset + frame_stride > pool_size:
  write_offset = 0

frame_phys_addr = pool_phys_base + write_offset
write_offset += frame_stride
```

边界要求：

```text
单张图不能跨越 uio2 尾部。
如果尾部剩余空间不足一张图，下一张图回绕到池起始地址。
frame_stride 必须大于等于本次图像实际写入字节数。
frame_phys_addr 建议按 FPGA/FDMA 要求对齐。
```

### 8.3 覆盖策略

当前测试版不做覆盖，日志必须打印每次分配结果。

```text
capture_id
phase = output
frame_phys_addr
write_offset
frame_stride
pool_size
```

后续如果上位机或 SFP 发送存在异步延迟，需要增加槽位引用状态：

```text
FREE
FPGA_WRITING
READY
SENDING
DONE
```

当前阶段如果 FPGA 写图完成中断已经表示 DDR 写入和 SFP 本次操作完成，则可以先在完成后释放/允许覆盖该槽位。

### 8.4 Static Idle 中的地址使用

一次静态采图当前使用两个地址：

```text
bright_addr = uio0 map0 addr
dark_addr   = ddr_image_pool_alloc()
```

第一帧 light 阶段：

```text
写 img_wr_str_addr = bright_addr
触发 IMG_CORR_STR / IMG_WR_STR / GIC_STR
等待 IMG_CORR / IMG_WR / GIC 完成
```

第二帧输出图阶段：

```text
写 img_wr_str_addr = dark_addr
触发 IMG_CORR_STR / IMG_WR_STR / GIC_STR
等待 IMG_CORR / IMG_WR / GIC 完成
```

如果任意阶段失败，日志必须包含已经分配的地址，便于现场 dump DDR 定位。

### 8.5 配置默认值建议

当前 Makefile 只保留 UIO 节点和帧对齐策略：

```text
FPGA_OFFSET_UIO_DEVICE = "/dev/uio0"
FPGA_GAIN_UIO_DEVICE   = "/dev/uio1"
FPGA_IMAGE_UIO_DEVICE  = "/dev/uio2"
DDR_IMAGE_FRAME_ALIGN  = 4096
```

如果设备树中 uio2 的物理基地址可从 sysfs 读取，优先从 `/sys/class/uio/uio2/maps/map0/addr` 获取；否则通过 Makefile/app_config 显式配置物理基地址。

## 9. 命令接口建议

当前仍使用 ASCII 调试协议时，建议新增以下正式业务命令。

### 9.1 CONFIG_STATIC_IDLE

配置静态 Idle 工作流程。

示例：

```text
CONFIG_STATIC_IDLE idle_clean_interval_ms=50 exposure_ms=50 dark_window_ms=50 offset_en=1 gain_en=1 defect_en=0
```

可选字段：

```text
idle_clean_interval_ms
exposure_ms
dark_window_ms
offset_en
gain_en
defect_en
line_time
oe_rise
oe_fall
start_row
end_row
binning
offset_addr
offset_adder
gain_addr
gain_clip
```

响应：

```text
OK CONFIG_STATIC_IDLE ...
ERR CONFIG_STATIC_IDLE ARG
```

### 9.2 START_STATIC_IDLE_CAPTURE

触发一次静态 Idle 采图。

```text
START_STATIC_IDLE_CAPTURE
```

行为：

```text
如果当前处于 Idle 自清空:
  等待自清空完成后执行采图

如果当前处于 Idle 等待:
  立即执行采图

如果当前已经在采图:
  返回 BUSY
```

响应建议：

```text
OK START_STATIC_IDLE_CAPTURE bright_int_vector=0x0000001a dark_int_vector=0x0000001a
ERR START_STATIC_IDLE_CAPTURE BUSY state=...
ERR START_STATIC_IDLE_CAPTURE TIMEOUT phase=...
ERR START_STATIC_IDLE_CAPTURE HARDWARE phase=...
```

一次采图完成的中断等待条件为：

```text
IMG_CORR_END | IMG_WR_END | GIC_END
```

只有三个完成 bit 全部收到，才认为本阶段采图完成。任一阶段超时都需要日志打印：

```text
phase
wait_mask
int_vector
bright_addr / dark_addr
```

### 9.3 GET_WORK_STATE

查询当前工作状态。

```text
GET_WORK_STATE
```

响应建议：

```text
OK WORK_STATE mode=Idle state=IDLE_WAIT pending_capture=0 last_error=0 last_phase=none capture_id=12
```

### 9.4 STOP_WORK

停止工作线程或退出当前工作模式。

```text
STOP_WORK
```

第一版可先实现为：

```text
停止后续 Idle 自清空
如果正在等待窗口，可尽快退出
如果正在硬件等待中，等待当前硬件动作返回后退出
```

不建议在第一版中强行中断正在执行的 GIC/IMG_CORR 硬件动作。

## 10. 线程模型建议

推荐引入工作线程。

```text
通信线程 / main loop:
  接收 RS422 或 stdio 命令
  解析命令
  更新配置
  投递采图请求
  查询状态

工作线程:
  周期性执行 Idle 自清空
  接收 pending_capture
  执行曝光窗口、亮场、暗场流程
  记录状态和错误
```

线程同步：

```text
pthread_mutex_t
pthread_cond_t
```

共享数据：

```text
current_mode
current_state
static_idle_config
pending_capture
stop_requested
last_error
last_phase
last_int_vector
capture_id
```

锁使用原则：

```text
更新状态和配置时加锁
执行硬件寄存器操作时不要长期持锁
等待硬件中断时不要长期持锁
```

这样可以避免命令查询状态被硬件等待阻塞。

## 11. 错误处理

第一版建议记录但不过度自动恢复。

错误来源：

```text
GIC 自清空超时
亮场 IMG_CORR/IMG_WR/GIC 等待超时
暗场 IMG_CORR/IMG_WR/GIC 等待超时
pa_irq/read INT_VECTOR 失败
非法配置参数
工作状态忙
```

状态机行为：

```text
Idle 自清空失败:
  进入 ERROR 或记录错误后继续下一周期，需要现场确认

亮场失败:
  本次采图失败，不继续暗场
  返回 Idle 或 ERROR，需要现场确认

暗场失败:
  本次采图失败
  返回 Idle 或 ERROR，需要现场确认
```

建议第一版采用日志优先策略：

```text
采图失败时日志必须说明失败阶段、等待 mask、实际 int_vector 和关键配置地址
GET_WORK_STATE 返回 last_error / last_phase / last_int_vector
状态机是否进入 ERROR 可先按具体失败类型决定，第一版重点保证错误可定位
```

如果后续现场希望严格停机或自动恢复，再增加策略配置。

## 12. 与现有调试命令的关系

继续保留：

```text
READ_REG
WRITE_REG
STATUS
VERSION
CONFIG_GIC
START_GIC
CONFIG_ROIC
START_ROIC
CONFIG_CORR
START_CORR
START_CORR_GIC
LOAD_TEMPLATE
MAKE_OFFSET
MAKE_GAIN
CONFIG_TEMPLATE
```

但文档中应标明：

```text
这些命令用于研发、生产和现场调试。
正式采图流程使用 WorkMode 命令。
```

如果工作线程正在执行正式流程，调试命令策略如下：

```text
工作线程处于 IDLE_WAIT 时允许 WRITE_REG 直接改寄存器。
工作线程处于采图阶段时禁止 WRITE_REG，返回 BUSY。
工作线程处于 IDLE_CLEANING 时是否允许 WRITE_REG 需要谨慎，第一版建议禁止会影响 GIC 的写操作。
READ_REG / STATUS / VERSION 可继续允许。
会触发硬件动作的调试命令在采图阶段返回 BUSY。
```

ROIC 策略：

```text
Static Idle 启动时配置一次 ROIC。
后续如果收到上位机 CONFIG_ROIC 或正式 ROIC 配置命令，允许再次配置。
采图阶段不允许重配 ROIC，返回 BUSY。
```

## 13. 推荐实现步骤

第一阶段按以下顺序实现：

```text
1. 新增 work_mode.h/.c
2. 定义 work_mode_t、work_state_t、static_idle_config_t
3. 在 app_config.h 增加 Static Idle 默认参数
4. main.c 启动工作线程，退出时停止工作线程
5. command_handler.c 增加 CONFIG_STATIC_IDLE
6. command_handler.c 增加 START_STATIC_IDLE_CAPTURE
7. command_handler.c 增加 GET_WORK_STATE
8. command_handler.c 增加 STOP_WORK
9. 将现有 pa_pu_configure_gic / pa_pu_configure_correction / pa_pu_start_* 复用到 work_mode
10. 在 README 中标明正式命令和调试命令边界
```

第一版不建议同时改正式二进制协议。等 ASCII 调试协议下的 Static Idle 流程跑通后，再把这些命令映射到正式二进制协议草案。

## 14. 已确认设计口径

以下问题已按当前讨论确认：

```text
1. idle_clean_interval_ms 默认 50 ms。
2. exposure_window_ms 默认 50 ms。
3. dark_window_ms 默认 50 ms。
4. 第一帧 offset 模板帧和第二帧实际输出帧都等待 IMG_CORR + IMG_WR + GIC 三个完成中断 bit。
5. 当前测试实现中，第一帧未校正 light 固定写到 uio0/offset 模板区，第二帧实际输出图从 uio2 图像池中环形分配；单张图不跨越 uio2 尾部。
6. 每次采图前先配置 GIC、IMG_WR、IMG_CORR，再连续写 IMG_CORR_STR、IMG_WR_STR、GIC_STR。
7. 采图失败后先通过日志和 GET_WORK_STATE 表明失败阶段与现场值。
8. 工作线程 Idle 空闲阶段允许 WRITE_REG，采图阶段不允许 WRITE_REG。
9. ROIC 在 Static Idle 启动时配置一次；收到上位机命令时允许再次配置。
```

后续如果恢复环形池实现，仍需确认 uio2 的设备树物理基地址、frame_stride 的最终对齐要求，以及覆盖安全条件。
