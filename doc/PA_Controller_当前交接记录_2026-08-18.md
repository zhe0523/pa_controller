# PA Controller 当前交接记录

本文档用于把当前 `pa_controller` 调试现场交接给其他 AI 工具或开发人员继续处理。

记录时间：2026-08-18

## 1. 当前目标

当前主要问题不是普通命令解析问题，而是反复采图时开发板会出现卡死或采图超时。

已知卡死多发生在：

- Static Idle 两阶段采图循环压测。
- `LOOP_CAPTURE_ADDR` 单地址压测。
- 卡死前用户态日志通常已经完成 FPGA 三模块启动，即 `img_corr_str`、`img_wr_str`、`gic_str` 已写入启动。
- 后续进入等待中断流程，有时停在 `/dev/pa_irq` 的 `poll()` 中，没有返回。

因此当前重点是区分：

1. `pa_irq.ko` 中断驱动是否导致阻塞或内核卡死。
2. FPGA 侧 IMG_WR/GIC/IMG_CORR 是否在某次启动后拖死 AXI/DDR/中断链路。
3. DDR 地址布局、回环写入、SFP/IMG_WR 并发访问是否触发硬件侧问题。

## 2. 当前内存布局

当前用户设备树计划/实际使用的三块 UIO 共享 DDR：

```text
uio0: offset 模板区
  phys = 0x1EA00000
  size = 0x04000000

uio1: gain 模板区
  phys = 0x22A00000
  size = 0x04000000

uio2: 图像输出池
  phys = 0x26A00000
  size = 0x19600000
```

当前 `Makefile` 对应宏：

```makefile
FPGA_IMAGE_PTR ?= 0x26A00000
FPGA_OFFSET_PTR ?= 0x1EA00000
FPGA_GAIN_PTR ?= 0x22A00000
FPGA_IMAGE_UIO_SIZE ?= 0x19600000
FPGA_OFFSET_UIO_SIZE ?= 0x04000000
FPGA_GAIN_UIO_SIZE ?= 0x04000000

DDR_IMAGE_POOL_BASE ?= $(FPGA_IMAGE_PTR)
DDR_IMAGE_POOL_UIO_SIZE ?= $(FPGA_IMAGE_UIO_SIZE)
DDR_IMAGE_POOL_FRAME_COUNT ?= 9
STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ?= 0
```

说明：

- 单帧 3072 x 7680 x 16bit，当前帧步长约 `0x02D00000`。
- `0x19600000` 理论最大只能稳定容纳 9 张完整帧，不能按 10 张配置。
- 用户明确表示不再考虑 `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1`，当前应按 `0` 处理，即第一帧 light 直接写 offset 模板地址。

## 3. 当前关键默认参数

当前默认中断等待时间已经从 5s 改为 1s：

```makefile
PA_PU_IRQ_TIMEOUT_MS ?= 1000
PA_PU_IRQ_POLL_INTERVAL_US ?= 1000
```

Static Idle 默认参数：

```makefile
STATIC_IDLE_CLEAN_INTERVAL_MS ?= 50
STATIC_IDLE_EXPOSURE_MS ?= 50
STATIC_IDLE_DARK_WINDOW_MS ?= 300

GIC_DEFAULT_LINE_TIME_NS ?= 25600
GIC_DEFAULT_START_ROW ?= 0
GIC_DEFAULT_END_ROW ?= 7679
GIC_DEFAULT_BINNING ?= 0

ROIC_DEFAULT_START_COL ?= 0
ROIC_DEFAULT_END_COL ?= 3071
ROIC_DEFAULT_BINNING ?= 0
```

IMG_CORR 默认：

```makefile
CORR_DEFAULT_ROW_NUM ?= 7680
CORR_DEFAULT_COL_NUM ?= 3072
CORR_DEFAULT_PKG_NUM ?= 46080
CORR_DEFAULT_OFFSET_EN ?= 1
CORR_DEFAULT_OFFSET_ADDER_VALUE ?= 100
CORR_DEFAULT_GAIN_EN ?= 1
CORR_DEFAULT_GAIN_CLIPPING_VALUE ?= 55000
CORR_DEFAULT_DEFECT_EN ?= 0
```

## 4. 当前 Static Idle 业务流程

Static Idle 当前设计为一次上位机采图命令触发两次 FPGA 采图：

1. bright/light 阶段：
   - 写入 `offset` 模板地址。
   - GIC 数据输出打开。
   - IMG_CORR offset/gain/defect 全关闭。
   - 同时启动 `img_corr_str`、`img_wr_str`、`gic_str`。
   - 等待 `IMG_CORR + IMG_WR + GIC` 三个中断 bit，当前 wait mask 为 `0x0000001a`。

2. dark/output 阶段：
   - 写入 uio2 图像池当前帧地址。
   - GIC 数据输出打开。
   - IMG_CORR 按 `CONFIG_STATIC_IDLE` 的 offset/gain/defect 开关。
   - 同时启动 `img_corr_str`、`img_wr_str`、`gic_str`。
   - 同样等待 wait mask `0x0000001a`。

当前用户希望第一帧 light 作为 offset 模板，第二帧作为实际输出图。

## 5. 当前可用测试命令

### 5.1 查询状态

```text
GET_WORK_STATE
STATUS
DUMP_REGS
```

其中 `DUMP_REGS` 用于打印调试寄存器，但不应读会清中断的寄存器。

### 5.2 Static Idle 单次采图

```text
START_STATIC_IDLE_CAPTURE
```

如果之前执行过 `LOOP_CAPTURE_ADDR`、`CAL_GAIN_CAPTURE` 等命令，工作线程可能已停止，需要先：

```text
START_WORK
```

否则可能返回：

```text
ERR START_STATIC_IDLE_CAPTURE STOPPED hint=START_WORK
```

### 5.3 Static Idle 循环测试

```text
LOOP_STATIC_IDLE_CAPTURE count=1000 interval_ms=300 stop_on_error=1
```

### 5.4 单地址硬件压测

这个命令用于绕过 Static Idle 业务逻辑，固定写同一个 DDR 地址，只测试：

```text
配置 GIC -> 配置 IMG_WR -> 配置 IMG_CORR -> 启动三模块 -> 等待中断
```

示例：

```text
LOOP_CAPTURE_ADDR addr=0x2F100000 count=1000 interval_ms=300 offset_en=0 gain_en=0 defect_en=0 trace=1
```

可调参数：

```text
addr=0x...
count=1000
interval_ms=300
timeout_ms=1000
wait_mask=0x1a
offset_en=0/1
gain_en=0/1
defect_en=0/1
gic_dout_en=0/1
trace=0/1
```

## 6. 已观察到的关键现象

### 6.1 `/dev/pa_irq` 驱动路径卡死点

用户最新 trace 显示卡死在：

```text
[TRACE] loop_capture_addr iteration=379 step=start_triplet_done addr=0x2f100000
[TRACE] pa_pu wait_all_enter mask=0x0000001a value=0x00001388 irq_fd=7
[TRACE] pa_pu wait_all_before_wait_one mask=0x0000001a value=0x00000000 irq_fd=7
[TRACE] pa_pu wait_int_vector_enter mask=0x0000001a value=0x00001388 irq_fd=7
[TRACE] pa_pu wait_irq_driver_enter mask=0x0000001a value=0x00001388 irq_fd=7
```

后面没有出现：

```text
wait_irq_driver_poll_return
```

这说明用户态已经进入 `poll(/dev/pa_irq)`，但 `poll()` 没有按超时返回。

如果只是普通 FPGA 没给中断，理论上 1s 或 5s 后应该返回 timeout。因此这个现象更像：

- 内核/中断驱动路径卡住。
- 或 FPGA/AXI/DDR 侧导致系统级卡死，使 Linux 调度和 poll timeout 都不能继续。

### 6.2 不加载 `pa_irq.ko` 时的意义

建议测试：

```bash
rmmod pa_irq
./pa_controller --stdio
LOOP_CAPTURE_ADDR addr=0x2F100000 count=1000 interval_ms=300 offset_en=0 gain_en=0 defect_en=0 trace=1
```

判断逻辑：

- 如果不加载 `pa_irq.ko` 后稳定，优先查 `pa_irq.ko`。
- 如果仍然卡死，优先查 FPGA/AXI/DDR/IMG_WR。
- 如果不再卡死但变成超时，说明硬件完成中断或 INT_VECTOR 链路有问题，但 Linux 没被拖死。

### 6.3 dark_window_ms 测试记录

用户报告：

- `dark_window_ms=500` 曾经稳定 1000 次。
- `dark_window_ms=200` 仍可能在长测后卡死。
- 但单地址 `LOOP_CAPTURE_ADDR` 在关闭 offset/gain/defect 时也会卡死，因此问题不只在 Static Idle 两阶段业务逻辑。

### 6.4 地址/帧数测试记录

用户曾测试：

- 单独测试两个地址 1000 次都成功。
- 环形多地址/长时间循环更容易触发问题。
- 近期 9 帧池、4 帧池均出现过卡死，不能简单归因于帧数配置。

## 7. PDS/JTAG 调试建议

用户 PDS 安装路径：

```text
C:\pango\PDS_2025.1-ads
```

已看到工具：

```text
C:\pango\PDS_2025.1-ads\bin\cdt_dbg.exe
C:\pango\PDS_2025.1-ads\bin\cdt_dbg_shell.exe
C:\pango\PDS_2025.1-ads\bin\cdt_ins.exe
C:\pango\PDS_2025.1-ads\edk\bin\cdt_edk_shell.exe
C:\pango\PDS_2025.1-ads\edk\bin\cdt_js.exe
C:\pango\PDS_2025.1-ads\edk\bin\booter.exe
```

相关文档：

```text
C:\pango\PDS_2025.1-ads\doc\UG990402_Fabric_Debugger_User_Guide.pdf
C:\pango\PDS_2025.1-ads\edk\doc\UG991401_EDK_User_Guide.pdf
```

建议优先使用 Fabric Debugger 观察 FPGA 侧信号，而不是一开始就尝试 halt Linux CPU。

建议抓取信号：

```text
gic_str
img_wr_str
img_corr_str
gic_state / gic_end
img_wr_state / img_wr_end
img_corr_state / img_corr_end
int_vector
f2p_irq
img_wr_str_addr
AXI 写通道 awvalid/awready/wvalid/wready/bvalid/bready/bresp
DDR 写突发相关计数或状态
```

重点确认卡死那一拍：

1. FPGA 是否真的发出了 `int_vector=0x1a`。
2. `f2p_irq` 是否拉起但 Linux 没响应。
3. IMG_WR 是否卡在 AXI 写通道。
4. 写地址是否越过 uio2 预留范围。
5. SFP 输出和 DDR 写是否存在未处理的背压。

## 8. Gain/Defect 模板功能现状

当前已新增 gain/defect 模板构建逻辑。

新增文件：

```text
src/calibration_builder.c
src/calibration_builder.h
doc/Gain_Defect模板制作流程.md
```

命令：

```text
CAL_GAIN_BEGIN levels=5000,10000,20000 frames=8 threshold=0.3
CAL_GAIN_CAPTURE level=5000
CAL_GAIN_STATUS
CAL_GAIN_BUILD
CAL_GAIN_CANCEL
```

说明：

- 灰度 level 由上位机控制。
- 单个 level 可以 begin/capture/status，但 `CAL_GAIN_BUILD` 至少需要 2 个 level。
- defect 不单独输出模板，只通过 gain 模板像素写 0 表示。
- 中间均值图保存到 `/usr/local/calib/gain_mean_<level>.raw`。
- 最终 gain 模板保存到 `/usr/local/gain.raw`，并加载到 uio1。

## 9. 当前工作区文件状态

当前存在较多未提交改动，继续处理前建议先查看：

```bash
git status --short
git diff --stat
```

当前已知修改/新增包括：

```text
M  Makefile
M  doc/PA_Controller_Static_Idle工作模式设计.md
M  src/app_config.h
M  src/command_handler.c
M  src/log.c
M  src/log.h
M  src/main.c
M  src/pa_pu.c
M  src/pa_pu.h
M  src/work_mode.c
?? doc/Gain_Defect模板制作流程.md
?? src/calibration_builder.c
?? src/calibration_builder.h
```

注意：

- 不要随意回滚用户本地改动。
- 当前代码中有一些 trace 级调试输出，是为了定位卡死位置临时加入的。
- 用户已经明确不想引入额外复杂工作模式；调试命令可以保留，但不要继续扩展业务状态机。

## 10. 建议下一步

建议按下面顺序继续：

1. 等用户完成 `rmmod pa_irq` 后的压测结果。
2. 如果不加载驱动后稳定，重点审查 `pa_irq.ko` 的 poll/waitqueue/中断清除时序。
3. 如果不加载驱动仍卡死，停止在 ARM 侧堆日志，转 FPGA Fabric Debugger 抓 IMG_WR/AXI/INT_VECTOR/F2P IRQ。
4. 对单地址压测分别测试：

```text
LOOP_CAPTURE_ADDR addr=0x2F100000 count=1000 interval_ms=300 offset_en=0 gain_en=0 defect_en=0 trace=1
LOOP_CAPTURE_ADDR addr=0x2F100000 count=1000 interval_ms=300 offset_en=1 gain_en=0 defect_en=0 trace=1
LOOP_CAPTURE_ADDR addr=0x2F100000 count=1000 interval_ms=300 offset_en=1 gain_en=1 defect_en=0 trace=1
```

5. 如果固定地址稳定、多地址不稳定，重点查 DDR 地址边界、SFP/IMG_WR 地址递增、FPGA 内部缓存释放。
6. 如果固定地址也不稳定，重点查 IMG_WR/GIC/IMG_CORR 连续启动复位时序、AXI 写响应、F2P 中断驱动。
