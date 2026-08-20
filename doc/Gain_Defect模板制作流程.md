# Gain/Defect 模板制作流程

本文档记录当前 `pa_controller` 中已经实现的 Gain/Defect 模板制作流程。

当前约定：

- 灰度级 `level` 由上位机控制。
- ARM 端只负责在当前灰度级下采图、生成均值图、拟合 gain 模板。
- defect 不生成单独模板，通过 `gain = 0` 表示坏点。
- 旧的 `MAKE_GAIN` 单帧模板命令保留，新算法使用 `CAL_GAIN_*` 命令。

## 1. 相关文件

代码文件：

- `src/calibration_builder.c`
- `src/calibration_builder.h`
- `src/command_handler.c`

输出文件：

- 灰阶均值图目录：`/usr/local/calib`
- 每个灰阶均值图：`/usr/local/calib/gain_mean_<level>.raw`
- 最终 gain 模板文件：`/usr/local/gain.raw`

内存写入：

- 采集均值图时，原始帧固定写入 `uio2` 图像池起始地址。
- `CAL_GAIN_BUILD` 成功后，会把 `/usr/local/gain.raw` 加载到 `uio1` gain 模板区。

## 2. 命令流程

### 2.1 开始一次校准

```text
CAL_GAIN_BEGIN levels=5000,10000,20000 frames=8 threshold=0.3
```

参数说明：

- `levels`：灰度级列表，逗号分隔，最多 `CAL_GAIN_MAX_LEVELS` 个，当前默认最多 16 个。
- `frames`：每个灰度级采集帧数。
- `threshold`：坏点判断阈值，当前默认建议 `0.3`。

单个 level 也允许：

```text
CAL_GAIN_BEGIN levels=5000 frames=4 threshold=0.3
```

但单个 level 只能用于采集均值图和查看 median，不能执行最终 gain 拟合。

### 2.2 上位机切换灰度级

每个灰度级由上位机控制。ARM 端不会改变灰度级。

例如，上位机先把探测器/光源/曝光控制到灰度级 `5000`，稳定后再发送：

```text
CAL_GAIN_CAPTURE level=5000
```

然后上位机切到 `10000`，稳定后发送：

```text
CAL_GAIN_CAPTURE level=10000
```

以此类推。

### 2.3 采集某个灰度级

```text
CAL_GAIN_CAPTURE level=5000
```

执行动作：

1. 停止 Static Idle 工作线程，避免空闲自清空或正式采图流程抢寄存器。
2. 每帧都关闭 offset/gain/defect 校正。
3. 配置 GIC、IMG_WR、IMG_CORR。
4. 图像写入 `uio2` 图像池起始地址。
5. 等待三个中断完成 bit：

```text
IMG_CORR_END | IMG_WR_END | GIC_END
```

也就是当前等待掩码：

```text
0x0000001a
```

6. ARM 从 `uio2` 有效图像区域读取像素并累加。
7. 采满 `frames` 帧后生成该 level 的均值图。
8. 对该均值图计算全图中位值 `median`。

生成文件示例：

```text
/usr/local/calib/gain_mean_5000.raw
```

文件格式：

- 只保存有效图像区域。
- 尺寸为 `IMAGE_WIDTH x IMAGE_HEIGHT`。
- 当前默认 `3072 x 7680`。
- 像素格式为 `uint16_t` raw。

### 2.4 查看状态

```text
CAL_GAIN_STATUS
```

返回内容包含：

- 当前是否有活动校准任务。
- level 总数。
- 已完成 level 数。
- 每个 level 是否 ready。
- 每个 level 的 median。
- defect 阈值。
- 已统计坏点数量。

示例字段：

```text
OK CAL_GAIN_STATUS active=1 levels=3 ready=1 frames=8 threshold=0.300 bad_pixels=0 level0=5000:1:4920 level1=10000:0:0 level2=20000:0:0
```

其中：

```text
level0=5000:1:4920
```

表示：

- level = 5000
- ready = 1
- median = 4920

### 2.5 生成 gain 模板

```text
CAL_GAIN_BUILD
```

前提：

- 至少 2 个 level。
- 所有 level 都已经执行过 `CAL_GAIN_CAPTURE`。

如果只有 1 个 level，会失败并打印日志：

```text
cal gain build requires at least 2 levels for linear fit
```

执行成功后：

- 生成 `/usr/local/gain.raw`
- 把 gain 模板加载到 `uio1`
- 重新配置 IMG_CORR 默认模板地址

### 2.6 取消校准

```text
CAL_GAIN_CANCEL
```

只清空 ARM 侧当前校准状态，不删除已经生成的均值图文件。

## 3. 均值图生成算法

每个灰度级采集 `x` 帧图像。

对每个像素坐标 `(row, col)`：

```text
mean(row, col) = round(sum(frame_i(row, col)) / x)
```

生成均值图 `Mn`。

当前实现使用 `uint32_t` 累加每个像素，输出 `uint16_t` 均值图。

## 4. Median 数组生成

对每个均值图 `Mn` 计算全图中位值。

当前实现使用 16bit 直方图统计：

```text
histogram[0..65535]
```

然后根据有效像素数量找到中位值。

最终得到：

```text
Medians[n]
```

其中 `n` 的顺序和 `CAL_GAIN_BEGIN levels=...` 中的顺序一致。

## 5. Gain 拟合算法

对每个像素坐标 `(row, col)`，按 level 顺序取出：

```text
Pixels[n] = Mn(row, col)
```

然后对：

```text
x = Medians[n]
y = Pixels[n]
```

做最小二乘线性拟合，并取斜率 `slope`。

当前实现使用带截距的一元线性拟合：

```text
slope = sum((x_i - mean_x) * (y_i - mean_y)) / sum((x_i - mean_x)^2)
```

规则：

```text
如果 slope < 0 或 slope >= 3.99，则 gain = 0
否则 gain = round(slope * 16384)
```

代码中保留了负数编码分支：

```text
如果 scaled < 0，则 scaled = -scaled + 0x8000
```

但按当前 `slope < 0` 直接置 0 的规则，正常不会走到该分支。

## 6. Defect 判断算法

Defect 判断在 `CAL_GAIN_BUILD` 阶段执行。

对每个均值图 `Mn`：

1. 执行半径 5 的中值滤波，也就是 `11 x 11` 窗口。
2. 对中值滤波输出再执行半径 5 的均值滤波，也就是 `11 x 11` 窗口。
3. 对每个像素比较滤波结果 `pixel1` 和原始均值图像素 `pixel2`。
4. 如果 `pixel2 == 0`，直接标记坏点。
5. 如果满足：

```text
abs(pixel1 - pixel2) * 1.0 / pixel2 > threshold
```

则标记坏点。

当前策略：

```text
任意一个 level 判断为坏点，该坐标最终 gain 写 0。
```

## 7. 滤波实现方式

中值滤波：

- 窗口半径 5。
- 精确 `11 x 11` 中值。
- 当前使用 16bit 滑动直方图，避免每个像素重新排序窗口。

均值滤波：

- 窗口半径 5。
- 当前使用列方向滑动和加行方向滑动和。
- 不使用大积分图，减少内存占用。

## 8. 内存和性能注意事项

当前实现偏向算法正确和可调试。

主要内存占用：

- `CAL_GAIN_CAPTURE` 每个 level 需要一张 `uint32_t` 累加图。
- 默认 `3072 x 7680` 时，累加图约 90 MB。
- `CAL_GAIN_BUILD` defect 阶段会加载一张均值图、一个中值滤波图、一个均值滤波图，以及坏点图。

板端内存紧张时，需要注意：

- 不要同时运行其它大内存任务。
- 尽量不要在校准过程中并发运行上位机大图传输压力测试。
- 如果后续速度或内存不满足要求，可以进一步改成分块处理。

## 9. 当前边界

当前实现已经支持：

- 单 level 采均值图。
- 多 level 采均值图。
- median 统计。
- 多 level 最小二乘拟合 gain。
- defect 通过 gain=0 表示。
- 生成 `/usr/local/gain.raw`。
- 写入 `uio1` gain 模板内存。

当前还没有实现：

- 上位机灰度级自动切换，灰度级仍由上位机控制。
- 断电后从已有均值图恢复校准状态。
- 独立 defect 模板。
- 分块式低内存版本。
- 校准过程中的图像质量检查，例如曝光饱和、灰阶顺序异常、median 单调性检查。

## 10. 推荐调试流程

单 level 调试：

```text
CAL_GAIN_BEGIN levels=5000 frames=4 threshold=0.3
CAL_GAIN_CAPTURE level=5000
CAL_GAIN_STATUS
```

完整 gain/defect 调试：

```text
CAL_GAIN_BEGIN levels=5000,10000,20000 frames=8 threshold=0.3
```

上位机切到 `5000` 后：

```text
CAL_GAIN_CAPTURE level=5000
```

上位机切到 `10000` 后：

```text
CAL_GAIN_CAPTURE level=10000
```

上位机切到 `20000` 后：

```text
CAL_GAIN_CAPTURE level=20000
```

确认状态：

```text
CAL_GAIN_STATUS
```

生成模板：

```text
CAL_GAIN_BUILD
```

生成后可以用：

```text
LOAD_TEMPLATE
CONFIG_TEMPLATE
```

重新加载/下发模板配置。
