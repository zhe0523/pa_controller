# PA Controller 正式通信协议冻结版

版本：V1.0（冻结）  
适用对象：PA Host、ARM `pa_controller`、后续 PA SDK  
冻结内容：帧格式、命令号、序号语义、错误码、可靠性策略

## 1. 通信边界

- RS422 只传输控制命令、状态、任务进度和错误信息。
- 图像像素通过 PCIe 接收，不与 RS422 响应做采集 ID 关联。
- 当前正式链路使用二进制协议；上位机不使用 ASCII 协议。
- 默认串口参数：115200、8N1、无流控。

## 2. 二进制帧

所有多字节整数均为小端字节序。

| 偏移 | 长度 | 字段 | 说明 |
|---|---:|---|---|
| 0x00 | 2 | magic | 固定 `0x55AA`，线上字节为 `AA 55` |
| 0x02 | 1 | version | 当前为 `0x01` |
| 0x03 | 1 | header_len | 当前为 `0x10` |
| 0x04 | 1 | msg_type | REQ=0x01、ACK=0x02、DONE=0x03、ERR=0x04、EVT=0x05 |
| 0x05 | 1 | flags | 当前为 0 |
| 0x06 | 2 | cmd | 命令号 |
| 0x08 | 4 | seq | 请求序号；同一请求重试必须保持不变 |
| 0x0C | 2 | payload_len | payload 长度，最大 2048 |
| 0x0E | 2 | header_crc | 当前保留，填 0 |
| 0x10 | N | payload | 固定结构或 TLV |
| 0x10+N | 2 | crc16 | CRC16-CCITT-FALSE，覆盖前面全部字段 |

## 3. 命令号

| 命令 | ID | 说明 |
|---|---:|---|
| HELLO | 0x0001 | 基础握手 |
| PING | 0x0002 | 链路测试 |
| STATUS | 0x0003 | 读取设备状态 |
| VERSION | 0x0004 | 读取版本 |
| REBOOT | 0x0005 | 重启下位机 |
| GET_CONFIG_GROUP | 0x0103 | 读取配置组 |
| SET_CONFIG_GROUP | 0x0104 | 下发并保存配置组 |
| DUMP_REGS | 0x0105 | 读取调试寄存器 |
| WRITE_REG | 0x0106 | 写调试寄存器 |
| START_STATIC_CAPTURE | 0x0200 | 静态手动上图 |
| START_DYNAMIC | 0x0210 | 启动 Dynamic |
| STOP_DYNAMIC | 0x0211 | 停止 Dynamic |
| QUERY_DYNAMIC | 0x0212 | 查询 Dynamic |
| CAL_OFFSET_BEGIN/CAPTURE/BUILD/CANCEL | 0x0300~0x0303 | 暗场模板流程 |
| CAL_GAIN_BEGIN/CAPTURE/BUILD/CANCEL | 0x0304~0x0307 | 亮场模板流程 |
| CAL_STATUS | 0x0308 | 查询模板任务 |
| IMG_UPLOAD_CONFIG/START/QUERY | 0x0500~0x0502 | 模板上传查看流程 |

配置组和配置 item ID 以 ARM `command_handler.c` 的正式配置表为准，新增 item 不得复用已有 ID。

## 4. 序号与可靠性

- 上位机为每个新请求分配递增 `seq`，0 不使用；序号回绕后从 1 继续。
- 超时重试使用完全相同的 `seq/cmd/payload`，最多重试次数由上位机运行参数控制。
- 默认响应超时为 500 ms，默认最大重试次数为 2 次。
- 模板采集和生成等长命令使用更长的内部超时，不改变协议格式。
- 下位机缓存最近 8 条已完成响应，缓存有效期 30 秒。
- 收到相同 `seq + cmd + payload CRC` 的请求时直接重放响应，不重复执行硬件动作。
- 相同 `seq` 但命令或 payload 不同，返回序号冲突错误。
- RS422 为单请求单响应链路，不使用滑动窗口、多帧并发或额外 ACK 事务。

## 5. 工作模式约束

- Dynamic 持续运行、静态曝光或采集阶段禁止修改硬件配置。
- Static Idle 等待或自清空阶段允许配置；配置前暂停静态线程，配置完成后自动恢复。
- PCIe 图像接收独立运行，不要求与 RS422 响应一一对应。

## 6. 错误码

错误响应 payload 使用 TLV `type=0x0002`、`length=4`、`value=u32`。

当前约定：

- `0x0001`：未知命令或资源不存在
- `0x0002`：参数、帧格式或序号冲突
- `0x0005`：硬件操作超时
- `0x0008`：设备忙或当前工作模式禁止
- `0x000E`：内部处理或保存失败

后续 SDK 必须遵守本文件定义；协议扩展使用新命令号或新 TLV，不修改既有字段语义。
