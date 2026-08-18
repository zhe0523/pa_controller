# Static Idle IMG_WR 问题测试汇总

## 1. 背景

当前 `pa_controller` 正在实现 Static Idle 静态采图流程。该流程由 ARM 侧按照时序配置并启动 FPGA 模块：

1. 第一帧 light：关闭 offset/gain/defect 校正，用于生成 offset 模板。
2. 第二帧输出图：按 `CONFIG_STATIC_IDLE` 配置执行实际输出。
3. 每一帧均同时启动 `IMG_CORR_STR`、`IMG_WR_STR`、`GIC_STR`。
4. 每一帧均等待三个完成中断 bit：

```text
IMG_CORR_END | IMG_WR_END | GIC_END = 0x0000001a
```

目前现场稳定性测试中出现偶发失败或卡死。失败时通常只能收到 `GIC_END = 0x00000002`，`IMG_WR_END` 没有出现，`IMG_WR` 模块保持 busy。

## 2. 当前关键配置

当前测试使用的共享 DDR / UIO 布局如下：

```text
/dev/uio0: offset 模板区，物理地址 0x16000000，大小 0x04000000
/dev/uio1: gain   模板区，物理地址 0x1a000000，大小 0x04000000
/dev/uio2: 输出图图像池，物理地址 0x21000000，大小 0x1f000000
```

图像参数：

```text
IMAGE_WIDTH  = 3072
IMAGE_HEIGHT = 7680
像素格式      = uint16_t
单帧有效图像  = 3072 * 7680 * 2 = 0x02d00000 字节
```

当前 Makefile 中需要注意的参数：

```make
FPGA_IMAGE_PTR ?= 0x21000000
FPGA_OFFSET_PTR ?= 0x16000000
FPGA_GAIN_PTR ?= 0x1A000000

FPGA_IMAGE_UIO_SIZE ?= 0x1F000000
FPGA_OFFSET_UIO_SIZE ?= 0x04000000
FPGA_GAIN_UIO_SIZE ?= 0x04000000

DDR_IMAGE_POOL_BASE ?= $(FPGA_IMAGE_PTR)
DDR_IMAGE_POOL_UIO_SIZE ?= $(FPGA_IMAGE_UIO_SIZE)
DDR_IMAGE_POOL_FRAME_COUNT ?= 2

STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ?= 1
GAIN_TEMPLATE_REPEAT_COUNT ?= 1
```

说明：

- `DDR_IMAGE_POOL_FRAME_COUNT` 只限制 uio2 输出图环形池使用多少帧。
- `GAIN_TEMPLATE_REPEAT_COUNT` 只表示 gain 模板在 uio1 中重复写入的份数。
- 当前 uio1 只有 64MB，只能安全保存 1 份约 45MB 的 gain 模板，因此 `GAIN_TEMPLATE_REPEAT_COUNT` 必须保持为 1。
- `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1` 表示第一帧 light 先写入 uio2，再由 ARM 拷贝到 uio0 作为 offset 模板；这是为了避开 FPGA 直接写 uio0 时出现的 IMG_WR 卡住风险。

## 3. 已观察到的典型现象

### 3.1 FPGA 直接写 uio0 时，bright 阶段失败

当 `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=0`，第一帧 light 直接写 `0x16000000` 时，出现过如下失败：

```text
phase=bright addr=0x16000000 offset_en=0 gain_en=0 defect_en=0
accumulated INT_VECTOR=0x00000002 wait_mask=0x0000001a
img_wr_str_addr = 0x16000000
img_wr_state    = 0x00000001
img_wr_end      = 0x00000000
img_corr_state  = 0x00000000
```

结论：

- 该失败发生在 bright 阶段。
- bright 阶段 offset/gain/defect 全部关闭，因此不是校正模板导致。
- 只收到 `GIC_END=0x02`。
- `IMG_WR` 没有完成，状态保持 busy。
- FPGA 直接向 uio0 写整帧图存在风险，当前不建议作为正式路径。

### 3.2 bright 先写 uio2 再由 ARM 拷贝到 uio0，可以绕开 uio0 直写风险

当 `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1` 时，第一帧 light 写入 uio2 基地址 `0x21000000`，完成后由 ARM 拷贝到 uio0：

```text
static capture phase done phase=bright addr=0x21000000 int_vector=0x0000001a
static bright copied to offset template src=0x21000000 dst=0x16000000 bytes=0x2d00000
```

该方式能够避免 bright 阶段直接写 uio0 的问题。

### 3.3 即使 dark 阶段关闭 offset/gain/defect，仍可能在 IMG_WR 失败

在 `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1` 且 dark 阶段关闭全部校正后，仍出现失败：

```text
phase=dark addr=0x23d00000 offset_en=0 gain_en=0 defect_en=0
accumulated INT_VECTOR=0x00000002 wait_mask=0x0000001a
img_wr_str_addr = 0x23d00000
img_wr_state    = 0x00000001
img_wr_end      = 0x00000000
img_corr_state  = 0x00000000
```

结论：

- 该失败发生在 dark 阶段。
- dark 阶段 offset/gain/defect 全部关闭。
- 只收到 `GIC_END=0x02`。
- `IMG_WR` 保持 busy，`IMG_WR_END` 没来。
- 这说明问题不只和 offset/gain 模板读取有关，`IMG_WR` 写图链路本身也可能偶发卡住。

## 4. 已排除或暂时降低优先级的方向

### 4.1 不是单纯的 gain 模板重复份数问题

曾经将 `GAIN_TEMPLATE_REPEAT_COUNT` 设为 6，导致启动时报错：

```text
gain uio window too small: need=283115520 size=67108864
```

这是配置错误，因为 uio1 只有 64MB，不能存 6 份 gain 模板。当前已改回 1。

该问题会导致启动失败或模板写越界风险，但不是后续 `IMG_WR` busy 的根因。

### 4.2 不是单纯的 offset/gain 校正使能问题

测试中 dark 阶段已经关闭：

```text
offset_en=0 gain_en=0 defect_en=0
```

仍然出现：

```text
IMG_WR busy, IMG_WR_END missing
```

因此不能只从校正模块配置或模板内容方向解释当前失败。

### 4.3 不是 ARM 写寄存器过程卡住

为定位卡死点，代码中已加入分步日志：

```text
config_gic begin/done
config_image_write begin/done
config_correction begin/done
prepare_irq begin/done
start_triplet begin/done
phase wait
```

失败日志显示 `start_triplet done` 和 `phase wait` 已经打印，说明 ARM 已经完成寄存器配置并发出启动，随后是在等待硬件完成过程中只收到 GIC 完成。

### 4.4 `/dev/pa_irq` 与 INT_VECTOR 轮询不是当前主要矛盾

当前失败日志中可以读到：

```text
accumulated INT_VECTOR=0x00000002
```

说明中断读取路径至少能拿到 GIC 完成 bit。问题是 `IMG_WR_END` 未产生，而不是 ARM 完全读不到中断。

## 5. 当前最可能的问题方向

当前现象最集中指向 FPGA 侧 `IMG_WR` 写图链路：

1. GIC 已经完成，说明行扫描或数据源侧至少走到了结束。
2. `IMG_CORR` 在关闭校正时保持 idle，说明失败不依赖校正模块完成状态。
3. `IMG_WR` 收到启动后保持 busy。
4. `IMG_WR_END` 不产生。
5. 失败地址曾出现于：

```text
0x16000000  # uio0 offset 区，FPGA 直写 light 时
0x23d00000  # uio2 输出图池，dark 输出时
```

需要 FPGA 侧重点确认：

- `IMG_WR` 是否等不到输入数据流结束。
- `IMG_WR` 是否等待某个 AXI 写响应或突发写完成。
- `IMG_WR` 对 `img_wr_str_addr` 的地址范围是否有限制。
- `IMG_WR` 在 `IMG_CORR` 关闭时的数据输入路径是否正确。
- `GIC_END` 出现后，是否保证给 `IMG_WR` 的图像数据也完整结束。
- SFP 输出和 DDR 写图是否共享 backpressure，SFP 链路异常是否会反向卡住 `IMG_WR`。

## 6. 建议下一步测试

### 6.1 先保持最小功能测试 IMG_WR 稳定性

目的：排除 offset/gain/defect 干扰，只验证 GIC + IMG_WR 基本链路。

建议配置：

```text
CONFIG_STATIC_IDLE idle_clean_interval_ms=50 exposure_ms=50 dark_window_ms=50 offset_en=0 gain_en=0 defect_en=0 line_time=25600 start_row=0 end_row=7679 binning=0
```

建议循环：

```text
LOOP_STATIC_IDLE_CAPTURE count=1000 interval_ms=1000 stop_on_error=1
```

如果 1000ms 稳定，再逐步缩短：

```text
LOOP_STATIC_IDLE_CAPTURE count=1000 interval_ms=500 stop_on_error=1
LOOP_STATIC_IDLE_CAPTURE count=1000 interval_ms=200 stop_on_error=1
```

### 6.2 确认输出图地址对失败概率的影响

当前 `DDR_IMAGE_POOL_FRAME_COUNT=2` 时，输出图主要在有限地址之间切换。建议分别测试：

```bash
make clean
make STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1 DDR_IMAGE_POOL_FRAME_COUNT=1
```

```bash
make clean
make STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1 DDR_IMAGE_POOL_FRAME_COUNT=2
```

```bash
make clean
make STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1 DDR_IMAGE_POOL_FRAME_COUNT=6
```

观察失败是否和固定 `img_wr_str_addr` 强相关。

### 6.3 再逐步恢复校正

只有在 offset/gain/defect 全关稳定后，再逐步打开：

```text
offset_en=1 gain_en=0 defect_en=0
offset_en=1 gain_en=1 defect_en=0
```

如果全关都不稳定，暂时不应该继续分析校正效果。

## 7. 当前代码侧处理建议

ARM 侧目前建议保持：

1. `STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1`。
2. `GAIN_TEMPLATE_REPEAT_COUNT=1`。
3. 保留 `run_capture_phase()` 的分步日志，直到 FPGA 侧问题定位完成。
4. 失败时继续 dump 非 read-clear 寄存器，重点观察：

```text
img_wr_str_addr
img_wr_state
img_wr_end
img_wr_dfx
img_wr_debug_in
img_wr_debug_out
gic_state
gic_end
gic_dfx
img_corr_state
img_corr_end
```

ARM 侧不建议在当前阶段做：

1. 通过反复读写 `INT_VECTOR` 尝试“救回”失败现场。
2. 在 `IMG_WR` busy 时继续发下一次 STR。
3. 把 `GAIN_TEMPLATE_REPEAT_COUNT` 增大到超过 uio1 容量。
4. 将 FPGA 直接写 uio0 作为正式模板生成路径。

## 8. 临时结论

截至当前测试，问题已经从“是否为 offset/gain 校正导致”收敛为：

```text
Static Idle 连续采图过程中，IMG_WR 写图链路偶发不完成。
```

最有价值的失败特征是：

```text
INT_VECTOR      = 0x00000002
wait_mask       = 0x0000001a
GIC_END         = 已收到
IMG_WR_END      = 未收到
IMG_WR_STATE    = 1
IMG_CORR_STATE  = 0
```

这说明 GIC 完成但 IMG_WR 未完成。下一步应由 FPGA 侧优先检查 `IMG_WR` 数据流结束条件、AXI 写响应、DDR 地址窗口和 SFP/DDR backpressure 关系。
