# PA Controller 正式通信协议草案

版本：草案 V0.1  
日期：2026-08-13  
适用对象：Qt 上位机、ARM `pa_controller`、PA/FPGA 控制链路

## 1. 目标

当前 ASCII 行协议适合串口助手和板上 bring-up 调试，但不适合作为正式产品协议：

- 命令过长时容易被终端、串口缓冲或日志截断。
- 参数依赖字符串解析，字段名、空格、大小写都会引入额外错误面。
- 缺少统一序号、长度、CRC 和错误码，不利于上位机做可靠重试。
- 后续需要增加结构化状态、模板任务、异步事件时，ASCII 协议会越来越难维护。

正式协议建议使用自定义二进制帧协议。ASCII 行协议保留为 debug shell，只用于研发、生产调试和现场排障。

## 2. 通信模型

物理层当前按 RS422 串口设计：

- 默认波特率：115200，可按现场需要提升。
- 数据位：8
- 校验：无
- 停止位：1
- 流控：无
- 字节序：小端。

通信模型采用主从请求应答：

- 上位机为主站，ARM 为从站。
- 上位机发起请求，ARM 必须返回响应。
- 同一链路同一时刻建议只保留一个未完成请求。
- 带耗时硬件动作的命令可同步等待完成后响应；后续如需要，也可扩展为“立即响应 + 事件上报”。

## 3. 帧格式

所有正式协议帧使用统一格式：

```text
SOF       2 bytes  固定 0x55 0xAA
VER       1 byte   协议版本，当前 0x01
TYPE      1 byte   帧类型
SEQ       2 bytes  请求序号，小端
CMD       2 bytes  命令字，小端
LEN       2 bytes  Payload 长度，小端
PAYLOAD   N bytes  命令参数或响应数据
CRC16     2 bytes  CRC16-IBM，小端，覆盖 VER 到 PAYLOAD
EOF       2 bytes  固定 0xAA 0x55
```

说明：

- `LEN` 最大建议先限制为 1024 字节。
- 解析器以 `SOF + LEN + CRC16` 为主要同步依据，`EOF` 作为辅助校验。
- Payload 可以包含任意字节，不需要转义。
- 收到 CRC 错误、长度错误、未知命令时，ARM 返回错误响应；如果帧头都无法同步，可以直接丢弃。

## 4. 帧类型

```text
0x01  REQUEST   上位机请求
0x02  RESPONSE  ARM 对请求的响应
0x03  EVENT     ARM 主动事件，预留
```

响应帧 `SEQ` 必须等于请求帧 `SEQ`。  
事件帧 `SEQ` 可以为 0，也可以使用独立递增序号，后续再定。

## 5. 通用响应 Payload

所有响应 Payload 建议以统一头部开始：

```text
status      u16  结果码
data_len    u16  后续响应数据长度
data        bytes
```

如果 `status != 0`，`data` 可为空，也可包含错误现场信息。

通用结果码：

```text
0x0000  OK
0x0001  UNKNOWN_CMD
0x0002  BAD_LEN
0x0003  BAD_PARAM
0x0004  BUSY
0x0005  TIMEOUT
0x0006  HW_ERROR
0x0007  NO_TEMPLATE
0x0008  UNSUPPORTED
0x0009  CRC_ERROR
0x000A  INTERNAL_ERROR
```

## 6. 命令字规划

```text
0x0001  PING
0x0002  STATUS
0x0003  VERSION
0x0004  SET_TIME
0x0005  GET_TIME

0x0101  CONFIG_GIC
0x0102  START_GIC
0x0103  STOP_GIC

0x0201  CONFIG_ROIC
0x0202  START_ROIC

0x0301  CONFIG_CORR
0x0302  START_CORR
0x0303  START_CORR_GIC

0x0401  SEND_SINGLE
0x0402  START_CONTINUOUS
0x0403  STOP_TRANSFER

0x0501  READ_REG
0x0502  WRITE_REG

0x0601  LOAD_TEMPLATE
0x0602  MAKE_OFFSET
0x0603  MAKE_GAIN
```

命令字按业务模块分段，后续新增命令只追加，不复用旧命令字。

## 7. 数据类型

```text
u8      1 字节无符号整数
u16     2 字节无符号整数，小端
u32     4 字节无符号整数，小端
u64     8 字节无符号整数，小端
bool8   1 字节，0=false，非 0=true
```

所有保留字段发送方填 0，接收方忽略。

## 8. 命令 Payload 定义

### 8.1 PING 0x0001

请求 Payload：空

响应 data：

```text
uptime_ms   u64
```

### 8.2 STATUS 0x0002

请求 Payload：空

响应 data：

```text
pa_version                         u32
pa_build_information               u32
adapted_main_board_version         u32
adapted_gic_board_version          u32
adapted_roic_board_version         u32
adapted_reserved_board_0_version   u32
adapted_reserved_board_1_version   u32
adapted_reserved_board_2_version   u32
pa_pu_com_version                  u32
pa_rst_init_state                  u32
wr_state                           u32
wr_end                             u32
corr_state                         u32
corr_end                           u32
gic_state                          u32
gic_end                            u32
gic_dfx                            u32
roic_state                         u32
roic_end                           u32
roic_dfx                           u32
```

注意：`STATUS` 不读取 `INT_VECTOR`，避免清除完成中断。

### 8.3 VERSION 0x0003

请求 Payload：空

响应 data：

```text
app_major                          u16
app_minor                          u16
app_patch                          u16
reserved                           u16
pa_version                         u32
pa_build_information               u32
adapted_main_board_version         u32
adapted_gic_board_version          u32
adapted_roic_board_version         u32
adapted_reserved_board_0_version   u32
adapted_reserved_board_1_version   u32
adapted_reserved_board_2_version   u32
pa_pu_com_version                  u32
```

版本寄存器解析规则：

- bit23~16：一级版本号
- bit15~8：二级版本号
- bit7~0：三级版本号

构建信息解析规则：

- bit31~24：年份低两位或约定年份字段
- bit23~16：月份
- bit15~8：日期
- bit7~0：子编号

### 8.4 SET_TIME 0x0004

请求 Payload：

```text
epoch_ms   u64  Unix 时间，毫秒
```

响应 data：

```text
epoch_ms   u64  设置后的当前时间，毫秒
```

### 8.5 GET_TIME 0x0005

请求 Payload：空

响应 data：

```text
epoch_ms   u64
```

### 8.6 CONFIG_GIC 0x0101

请求 Payload：

```text
req_code              u8
dout_enable           bool8
binning_mode          u8
reserved0             u8
line_time_ns          u32
oe_raising_edge_ns    u32
oe_falling_edge_ns    u32
start_row             u16
end_row               u16
```

响应 data：空

### 8.7 START_GIC 0x0102

请求 Payload：空

响应 data：

```text
int_vector   u32
```

成功条件：收到表格协议定义的 GIC 完成 bit。

### 8.8 STOP_GIC 0x0103

请求 Payload：空

响应 data：空

### 8.9 CONFIG_ROIC 0x0201

请求 Payload：

```text
reg_00        u16
reg_02        u16
reg_05        u16
reg_06        u16
reg_07        u16
reg_09        u16
reg_0a        u16
reg_0b        u16
reg_0c        u16
reg_0d        u16
reg_0e        u16
reg_0f        u16
reg_10        u16
reg_11        u16
reg_17        u16
reg_24        u16
reg_28        u16
reg_2d        u16
reg_3b        u16
start_col     u16
end_col       u16
binning_mode  u8
reserved      u8
```

响应 data：空

说明：正式二进制协议中一次帧即可携带完整 ROIC 配置，不再存在 ASCII 长命令截断问题。

### 8.10 START_ROIC 0x0202

请求 Payload：空

响应 data：

```text
int_vector   u32
```

成功条件：收到表格协议定义的 ROIC 完成 bit。

### 8.11 CONFIG_CORR 0x0301

请求 Payload：

```text
pkg_num              u16
row_num              u16
col_num              u16
offset_enable        bool8
gain_enable          bool8
defect_enable        bool8
reserved0            u8
offset_template_addr u32
offset_adder_value   u16
gain_clipping_value  u16
gain_template_addr   u32
```

默认建议：

```text
row_num = 7680
col_num = 3072
pkg_num = row_num * col_num * 2 / 1024
offset_adder_value = 100
gain_clipping_value = 55000
```

响应 data：空

### 8.12 START_CORR 0x0302

请求 Payload：空

响应 data：

```text
int_vector   u32
```

成功条件：收到表格协议定义的 IMG_CORR 完成 bit。

### 8.13 START_CORR_GIC 0x0303

请求 Payload：空

执行顺序：

```text
1. 清除旧中断事件
2. 写 IMG_CORR_STR=1
3. 立即写 GIC_STR=1
4. 等待 IMG_CORR 和 GIC 两个完成 bit
```

响应 data：

```text
int_vector   u32  累计读到的中断 bit
wait_mask    u32  本命令等待的中断 bit 集合
```

`wait_mask` 当前为：

```text
IMG_CORR bit | GIC bit = 0x00000010 | 0x00000002 = 0x00000012
```

注意：`wait_mask=0x00000012` 表示需要累计收到 IMG_CORR 和 GIC 两个 bit，
不要求某一次硬件读取的 `int_vector` 必须等于 `0x00000012`。

### 8.14 SEND_SINGLE 0x0401

请求 Payload：

```text
image_addr   u32  0 表示使用 /dev/uio2 的 sysfs 物理地址
```

响应 data：

```text
image_addr   u32
int_vector   u32
```

### 8.15 START_CONTINUOUS 0x0402

请求 Payload：

```text
image_addr   u32  0 表示使用 /dev/uio2 的 sysfs 物理地址
```

响应 data：

```text
image_addr   u32
int_vector   u32
```

说明：当前硬件尚未区分连续传图和单帧传图，现阶段可等价于 `SEND_SINGLE`。后续硬件支持后保持命令字不变，扩展 payload。

### 8.16 STOP_TRANSFER 0x0403

请求 Payload：空

响应 data：空

说明：当前硬件尚未提供停流寄存器，现阶段只确认收到。

### 8.17 READ_REG 0x0501

请求 Payload：

```text
offset   u16  PA/PU 寄存器 offset
reserved u16
```

响应 data：

```text
offset   u16
reserved u16
value    u32
```

注意：读取 `INT_VECTOR` 会清除中断；读取未实现或只写寄存器可能触发硬件总线异常，正式上位机默认不应暴露给普通用户。

### 8.18 WRITE_REG 0x0502

请求 Payload：

```text
offset   u16
reserved u16
value    u32
```

响应 data：

```text
offset   u16
reserved u16
value    u32
```

### 8.19 LOAD_TEMPLATE 0x0601

请求 Payload：空

响应 data：空

### 8.20 MAKE_OFFSET 0x0602

请求 Payload：空

响应 data：空

### 8.21 MAKE_GAIN 0x0603

请求 Payload：空

响应 data：空

## 9. 中断 bit 约定

正式协议软件侧按 `pa_pu_com_definition.xlsx` 表格协议判断完成 bit：

```text
bit0  pa reset init done interrupt
bit1  gic interrupt
bit2  roic interrupt
bit3  image write interrupt
bit4  image correct interrupt
```

如果当前 RTL 返回 `{img_corr_end,img_wr_end,roic_end,gic_end}`，属于 FPGA 与表格协议不一致，需要 FPGA 或协议表统一；ARM 正式协议不应默默切换 bit 定义。

## 10. CRC16

建议使用 CRC16-IBM：

```text
poly   = 0xA001
init   = 0xFFFF
xorout = 0x0000
refin  = true
refout = true
```

CRC 覆盖范围：

```text
VER, TYPE, SEQ, CMD, LEN, PAYLOAD
```

不包含 `SOF`、`CRC16`、`EOF`。

## 11. 解析状态机建议

ARM 接收状态机建议：

```text
WAIT_SOF_0
WAIT_SOF_1
READ_HEADER
READ_PAYLOAD
READ_CRC
READ_EOF
CHECK_CRC
DISPATCH
```

异常处理：

- SOF 不匹配：继续滑动查找下一个 `0x55 0xAA`。
- LEN 超限：丢弃当前帧并重新找 SOF。
- CRC 错误：丢弃当前帧，可选返回 `CRC_ERROR`。
- CMD 未知：返回 `UNKNOWN_CMD`。
- 参数长度不符合：返回 `BAD_LEN`。

## 12. 兼容与迁移策略

建议分三阶段迁移：

```text
阶段 1：保留 ASCII，新增二进制协议解析入口，二者共存。
阶段 2：Qt 上位机默认使用二进制协议，ASCII 只保留隐藏调试入口。
阶段 3：正式发布版本默认关闭危险调试命令，例如裸 READ_REG/WRITE_REG，仅工程模式打开。
```

ASCII 调试协议继续保留以下用途：

- 串口助手手工调试。
- 生产或研发现场快速验证。
- 二进制协议 bring-up 前的兜底通道。

## 13. 待确认事项

```text
1. 是否需要 ARM 主动 EVENT 上报，而不是所有动作都同步等待完成。
2. 正式波特率是否保持 115200，还是提升到 921600。
3. READ_REG/WRITE_REG 是否只允许工程模式使用。
4. int_vector bit 定义最终以表格还是 RTL 当前实现为准。
5. START_CONTINUOUS/STOP_TRANSFER 的硬件正式寄存器定义。
6. 模板生成命令是否需要进度查询和取消命令。
7. 是否需要设备编号、会话握手或权限口令。
```
