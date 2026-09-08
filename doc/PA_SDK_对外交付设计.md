# PA SDK 对外交付设计

版本：V0.1
日期：2026-09-07
状态：接口设计草案；ARM 端已先落地共享二进制协议基础层，SDK 动态库和业务实现待下一阶段开发。

## 1. 定位

实际交付给客户的不是我们的 Qt 上位机源码，而是一套稳定 SDK。我们的 Qt 上位机主要用于研发、演示、产线联调和内部验证；客户会基于 SDK 开发自己的上位机软件。

SDK 的目标是把设备能力包装成稳定业务 API：

```text
连接设备
-> 查询能力和版本
-> 读取/修改配置
-> 静态采图
-> 动态采图开始/停止
-> 模板制作和模板上传
-> PCIe 图像接收
-> 状态、错误和日志回调
```

客户不应该直接拼 RS422 命令字符串，也不应该理解 GIC、ROIC、CORR、IMG_WR 等 FPGA 寄存器细节。协议升级、CRC、重试、超时、配置项分片和状态解析都应由 SDK 内部处理。

## 2. 交付包结构

建议 SDK 交付包结构如下：

```text
pa_sdk/
  include/
    pa_sdk.h              对外主头文件，C ABI

  lib/
    linux-x86_64/
      libpa_sdk.so        Kylin/Ubuntu x86_64 上位机动态库
    windows-x64/
      pa_sdk.dll          Windows 客户上位机动态库，可选
      pa_sdk.lib

  examples/
    c/
      static_capture_demo.c
      dynamic_capture_demo.c
      pcie_receive_demo.c
      template_demo.c
    cpp/
      static_capture_demo.cpp
    python/
      pa_sdk.py           ctypes 封装，可选

  tools/
    pa_device_probe       设备探测和版本查询工具
    pa_capture_cli        命令行采图/动态模式/模板工具

  doc/
    SDK使用说明.md
    通信协议说明.md
    配置项说明.md
    错误码说明.md
    图像数据链路说明.md
    版本兼容说明.md
```

第一阶段可以只交付 Linux/Kylin x86_64 动态库和 C 示例。Windows 动态库、C#、Python 封装可以在客户明确需要后再补。

## 3. SDK 分层

SDK 内部建议分成三层：

```text
业务 API 层
  pa_open / pa_start_static_capture / pa_start_dynamic / pa_stop_dynamic / pa_set_config_item

协议层
  二进制帧组包/拆包、CRC、seq、ACK/DONE/ERR、配置项分片

平台层
  串口 RS422、PCIe idma event/c2h、线程、锁、日志、文件保存
```

对客户公开业务 API 和必要结构体；协议层和平台层尽量隐藏。这样后续 RS422 ASCII 过渡到二进制协议时，客户代码不需要大改。

当前 ARM 工程中的 `include/pa_protocol.h` / `src/pa_protocol.c` 已实现协议层的第一步：

- CRC16-CCITT-FALSE；
- 固定帧头、长度和 CRC 校验；
- 拆包、粘包、错误字节后的 magic 重同步；
- `REQ/ACK/DONE/ERR/EVT` 消息类型字段。

它暂时不改变现有 ASCII 处理，也不是客户 SDK 的最终 ABI；后续 Qt 上位机和 SDK 应分别实现同样的字节级测试向量。

## 4. 对外 API 原则

### 4.1 使用 C ABI

第一版 SDK 使用 C ABI：

- ABI 稳定，跨编译器和语言更可靠。
- C++/Qt 可以直接调用。
- C#、Python、LabVIEW 等环境也容易通过 FFI/ctypes/PInvoke 封装。

对外头文件只暴露不透明句柄：

```c
typedef struct pa_device pa_device_t;
typedef struct pa_pcie_receiver pa_pcie_receiver_t;
```

客户不能直接访问内部结构，避免后续内部状态机调整破坏 ABI。

### 4.2 不暴露调试命令为主入口

正式 SDK 不直接暴露 `READ_REG/WRITE_REG/CONFIG_GIC/CONFIG_CORR/CONFIG_DYNC` 作为常规 API。调试能力可以放在工程模式：

```c
pa_debug_read_reg(...)
pa_debug_write_reg(...)
```

并在文档中明确：

- 仅用于研发和现场服务。
- 动态运行、静态采图、模板任务期间不允许随意写寄存器。
- 读取 `INT_VECTOR` 可能清中断，不建议客户软件使用。

### 4.3 配置项用 ID 管理

正式配置 API 不传大字符串，而是使用配置项 ID：

```c
pa_set_config_u32(dev, PA_CFG_STATIC_EXPOSURE_MS, 50);
pa_set_config_bool(dev, PA_CFG_CORR_GAIN_ENABLE, true);
pa_get_config_u32(dev, PA_CFG_IMAGE_WIDTH, &width);
```

对于 Dynamic step 表、ROIC 寄存器表这类较大配置，可以提供配置组 API：

```c
pa_get_config_group(dev, PA_CFG_GROUP_DYNAMIC, buffer, buffer_size, &actual_size);
pa_set_config_group(dev, PA_CFG_GROUP_DYNAMIC, buffer, buffer_size);
```

SDK 内部负责把配置组拆成多个 RS422 短帧发送。

## 5. 典型客户流程

### 5.1 初始化和状态查询

```c
pa_device_t* dev = NULL;
pa_open_options_t options = {
    .serial_port = "/dev/ttyWCH0",
    .baudrate = 115200,
    .timeout_ms = 1000,
};

pa_open(&options, &dev);
pa_get_version(dev, &version);
pa_get_status(dev, &status);
```

### 5.2 静态采图

静态采图前，客户只需要配置业务参数，不需要拼底层寄存器。

```c
pa_set_config_u32(dev, PA_CFG_STATIC_EXPOSURE_MS, 50);
pa_set_config_u32(dev, PA_CFG_STATIC_DARK_WINDOW_MS, 50);
pa_set_config_bool(dev, PA_CFG_CORR_OFFSET_ENABLE, true);
pa_set_config_bool(dev, PA_CFG_CORR_GAIN_ENABLE, true);

pa_static_capture_result_t result;
pa_start_static_capture(dev, &result);
```

返回结果中包含：

```text
capture_id
offset_addr
output_addr
image_width
image_height
```

图像数据仍通过 PCIe 接收链路获得，不通过 RS422 返回。

### 5.3 动态采图

```c
pa_set_config_u32(dev, PA_CFG_DYNAMIC_CYCLE_COUNT, 0);
pa_start_dynamic(dev, NULL);

/* 图像由 PCIe receiver 回调或 wait_frame 接收。 */

pa_stop_dynamic(dev, &dynamic_result);
```

如果客户使用固定 profile，常规流程甚至可以只调用：

```c
pa_start_dynamic(dev, NULL);
pa_stop_dynamic(dev, NULL);
```

### 5.4 PCIe 图像接收

```c
pa_pcie_receiver_t* rx = NULL;
pa_pcie_open_options_t rx_options = {
    .event_device = "/dev/idma0_event_0",
    .c2h_device = "/dev/idma0_c2h_0",
    .bar0_resource = NULL,
};

pa_pcie_open(&rx_options, &rx);

pa_image_frame_t frame;
pa_pcie_wait_frame(rx, 1000, &frame);

/* 使用 frame.data / frame.width / frame.height。 */
pa_image_frame_release(&frame);
pa_pcie_close(rx);
```

PCIe 接收应从 BAR0 读取实际尺寸：

```text
BAR0[0x00c]  row_num
BAR0[0x010]  col_num
```

读取字节数：

```text
image_bytes = row_num * col_num * 2
```

### 5.5 模板制作和上传

```c
pa_make_dynamic_offset(dev, total_frames, valid_frames, &job);
pa_wait_job(dev, job.job_id, 60000, &job_result);

pa_cal_gain_begin(dev, levels, level_count, frames_per_level, threshold, &job);
pa_cal_gain_capture(dev, job.job_id, level);
pa_cal_gain_build(dev, job.job_id);

pa_upload_template(dev, PA_TEMPLATE_OFFSET, &upload_result);
pa_upload_template(dev, PA_TEMPLATE_GAIN, &upload_result);
```

模板制作属于长任务，SDK 应支持：

- 同步等待接口，适合简单客户。
- 状态查询接口，适合 GUI。
- 回调接口，适合异步集成。

## 6. 错误处理

SDK 对外错误码必须稳定，不直接返回中文字符串作为逻辑判断依据。

建议错误码分组：

```text
PA_OK
PA_ERR_BAD_ARGUMENT
PA_ERR_NOT_OPEN
PA_ERR_SERIAL_OPEN
PA_ERR_SERIAL_IO
PA_ERR_PROTOCOL
PA_ERR_CRC
PA_ERR_TIMEOUT
PA_ERR_BUSY
PA_ERR_INVALID_STATE
PA_ERR_DEVICE_FAULT
PA_ERR_NO_TEMPLATE
PA_ERR_PCIE_OPEN
PA_ERR_PCIE_IO
PA_ERR_UNSUPPORTED
PA_ERR_INTERNAL
```

同时提供错误文本函数：

```c
const char* pa_error_string(pa_status_t status);
```

客户 UI 可以显示这个文本，但业务判断应使用错误码。

## 7. 线程模型

建议第一版 SDK 采用简单清晰的线程模型：

- `pa_device_t` 串口控制句柄默认不是多线程并发安全的。
- 同一个 `pa_device_t` 同一时刻只允许一个业务命令在途。
- PCIe 接收可以独立于 RS422 控制句柄运行。
- GUI 程序如需异步调用，可以由客户自己放到工作线程，或使用 SDK 回调版接口。

后续如果需要，可以增加异步 API：

```c
pa_start_static_capture_async(dev, callback, user_data);
pa_start_dynamic_async(dev, callback, user_data);
```

第一版不建议一开始就把异步 API 做得太复杂。

## 8. 版本兼容

SDK 连接设备后必须先执行握手，确认：

```text
SDK ABI 版本
通信协议版本
pa_controller app 版本
FPGA/PA 版本
设备能力 capability
```

如果协议主版本不兼容，应返回 `PA_ERR_UNSUPPORTED`。如果次版本不同，可以根据 capability 降级。

建议 SDK 提供：

```c
pa_get_sdk_version(&version);
pa_get_version(dev, &version);
pa_get_capability(dev, &capability);
```

## 9. 与我们自研上位机的关系

我们的 Qt 上位机后续建议逐步改成调用同一套 SDK：

```text
Qt UI -> PA SDK -> RS422/PCIe -> ARM/FPGA
```

这样可以带来三个好处：

- SDK 会被我们的演示软件持续验证。
- 客户遇到问题时，我们的上位机和客户上位机使用同一套底层逻辑，容易复现。
- 协议升级时，只需要升级 SDK 和我们的上位机底层，不需要复制两套实现。

## 10. 第一阶段建议落地内容

第一阶段建议先完成：

```text
include/pa_sdk.h
doc/SDK使用说明.md
doc/错误码说明.md
examples/c/static_capture_demo.c
examples/c/pcie_receive_demo.c
tools/pa_capture_cli
```

功能范围：

- 串口打开/关闭。
- HELLO/VERSION/STATUS。
- 单配置项读写。
- 静态采图。
- 动态开始/停止/查询。
- PCIe 单帧接收。
- 模板上传。

调试寄存器读写可以放到 `pa_capture_cli`，不要在客户主流程文档里强调。
