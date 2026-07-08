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

## 当前 RS422 调试命令

当前先使用 ASCII 行协议，命令以 `\r\n` 或 `\n` 结束，便于串口助手联调。正式上位机协议确定后，替换 `src/command_handler.c` 即可。

```text
PING              -> 心跳
STATUS            -> 读取 PA 状态
LOAD_TEMPLATE     -> 从 /usr/local/offset.raw 和 /usr/local/gain.raw 加载模板
MAKE_OFFSET       -> 用当前 FPGA 图像生成 offset 模板
MAKE_GAIN         -> 用当前 FPGA 图像和 offset 模板生成 gain 模板
CONFIG_TEMPLATE   -> 将 offset/gain 物理地址配置给 PA
START_CORR        -> 启动 PA 图像校正
SEND_IMAGE        -> 通知 PA 从 FPGA 图像地址启动光口传图
WAIT_IRQ          -> 通过 UIO 等待一次 PA/FPGA 中断，超时 5 秒
QUIT              -> 退出程序
```

## 与项目要求的对应关系

已在 ARM 应用中落地：

```text
RS422 上位机通讯入口
PA/PU 通过 /dev/uio0 访问寄存器
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

当前目标板上的 `f2p_irq_test.ko` 是中断测试驱动，可以验证 hwirq/virq 和 INT_VECTOR；应用层如果要阻塞等待中断，推荐让正式驱动暴露 `read/poll`，或者把同一个中断接入 UIO。`WAIT_IRQ` 命令只在 `/dev/uio0` 具备中断事件时有效。
