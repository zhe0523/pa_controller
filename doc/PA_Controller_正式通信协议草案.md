# PA Controller 正式通信协议草案

版本：草案 V0.2  
日期：2026-09-04  
适用对象：Qt 上位机、ARM `pa_controller`、PA/FPGA 控制链路  
当前状态：帧编解码基础层已在 ARM 工程实现，业务命令迁移尚未完成

## 1. 设计目标

当前 `pa_controller` 使用 ASCII 行协议，适合串口助手、寄存器 bring-up 和研发调试，但不适合作为正式产品协议：

- 字符串命令依赖空格、大小写和 `key=value` 解析，字段多时容易截断或误解析。
- 没有统一帧头、长度和 CRC，串口干扰后无法可靠重同步。
- 没有事务号，迟到响应可能被下一条命令误认为有效响应。
- 长任务、动态模式、模板制作、主动状态事件都很难表达清楚。
- 裸 `READ_REG` / `WRITE_REG` 调试命令和正式业务流程混在一起，后续权限和安全边界不好管理。

正式协议目标：

```text
控制链路可靠，有帧同步、长度、CRC 和事务号
短命令、长任务、主动事件有明确生命周期
静态模式、动态模式、模板制作能作为业务命令表达
调试寄存器读写保留，但与正式工作流区分
图像像素不走 RS422，PCIe 图像链路保持独立
协议字段可扩展，后续增加参数不破坏旧解析器
```

## 2. 总体通信边界

系统通信分为控制面和数据面：

```text
控制面：Qt 上位机 <-> RS422 <-> ARM pa_controller <-> PA/PU 寄存器
数据面：FPGA/PCIe -> idma 驱动 -> Qt 上位机图像接收线程
```

RS422 只承载控制命令、状态、任务进度和故障事件，不传输图像像素。  
图像仍由 PCIe 接收，上位机程序启动后常驻监听图像中断和 `/dev/idma0_c2h_0`。

正式协议中不再把 FPGA 原始中断直接暴露成通用 `WAIT_IRQ` 命令。ARM 内部可以等待 `INT_VECTOR` 或 `/dev/pa_irq`，但对上位机应转换成明确业务结果，例如：

```text
STATIC_CAPTURE_DONE
DYNAMIC_STATUS
DYNAMIC_STOPPED
CAL_GAIN_PROGRESS
DEVICE_FAULT
```

## 3. 物理层约定

初始建议：

```text
接口        RS422 串口
默认端口    由上位机配置，Kylin 当前默认 /dev/ttyWCH0
默认波特率  115200，联调稳定后可评估 921600 或更高
数据位      8
校验位      none
停止位      1
流控        none
字节序      小端 little-endian
```

串口配置示例：

```sh
stty -F /dev/ttyWCH0 115200 raw -echo -echoe -echok -echoctl -echoke -crtscts -ixon -ixoff
```

## 4. 协议帧格式

正式协议采用无包尾的二进制帧。接收端依靠 `magic + payload_len + crc16` 完成同步和校验。

```text
offset  size  field        type    description
0x00    2     magic        u16     固定 0x55AA，小端发送为 AA 55
0x02    1     version      u8      当前 0x01
0x03    1     header_len   u8      当前 0x10
0x04    1     msg_type     u8      REQ/ACK/DONE/ERR/EVT
0x05    1     flags        u8      当前填 0
0x06    2     cmd          u16     命令 ID
0x08    4     seq          u32     上位机事务号，EVT 可为 0
0x0C    2     payload_len  u16     payload 长度
0x0E    2     header_crc   u16     当前可填 0，预留；CRC16 覆盖整帧时可不启用
0x10    N     payload      bytes   TLV 参数或固定二进制结构
0x10+N  2     crc16        u16     CRC16-CCITT-FALSE，覆盖 crc16 前全部字节
```

说明：

- `header_len` 当前固定为 16，后续协议头增加字段时可以变大。
- `payload_len` 初始最大建议为 2048 字节。
- 没有包尾，避免 payload 中出现尾字节后还要转义。
- 接收端遇到 `magic` 不匹配时按字节滑动查找下一个 `0x55AA`。
- 收到未知 `version` 主版本时返回 `ERR UNSUPPORTED_VERSION` 或直接丢弃。
- 帧内所有多字节整数均为小端。

## 5. CRC16

建议使用 `CRC16-CCITT-FALSE`：

```text
poly   = 0x1021
init   = 0xFFFF
xorout = 0x0000
refin  = false
refout = false
```

CRC 覆盖范围：

```text
magic, version, header_len, msg_type, flags, cmd, seq, payload_len, header_crc, payload
```

不包含最后的 `crc16` 字段本身。

固定测试向量：`123456789` 的 ASCII 字节计算结果为 `0x29B1`。双方必须用同一组字节流验证 CRC 结果，不能只按文字描述各自实现。

ARM 工程中的 `include/pa_protocol.h` 和 `src/pa_protocol.c` 已提供：

```text
pa_protocol_crc16()       CRC16-CCITT-FALSE
pa_protocol_encode()      编码完整请求/响应/事件帧
pa_protocol_decode()      校验并解析完整帧
pa_protocol_parser_feed() 处理拆包、粘包、CRC 错误和 magic 重同步
```

当前实现仍保持 `header_crc=0`，不会提前启用未定稿的帧头 CRC；业务命令和 ASCII/二进制入口切换将在后续阶段接入。

空 payload 固定帧示例：`REQ cmd=0x0001 seq=1`。

```text
AA 55 01 10 01 00 01 00 01 00 00 00 00 00 00 00 78 62
```

其中最后两个字节按小端表示 CRC16 `0x6278`。这个向量可用于 ARM、Qt 和 SDK 三端最小联调。

## 6. 消息类型

```text
0x01  REQ   上位机请求
0x02  ACK   ARM 已接收请求，通常用于长任务
0x03  DONE  请求已完成
0x04  ERR   请求失败
0x05  EVT   ARM 主动事件
```

生命周期建议：

```text
短命令：REQ -> DONE/ERR
长任务：REQ -> ACK(job_id) -> EVT_PROGRESS/EVT_DONE/EVT_FAILED
持续运行：REQ START -> DONE(state=running)，之后用 EVT 周期上报状态
停止命令：REQ STOP -> DONE(state=stopped) 或 ERR
```

`ACK` 只表示 ARM 接受请求，不表示硬件动作完成。  
`DONE` 表示本次请求已经结束。  
`EVT` 不完成某个 `seq`，事件本身使用 `event_seq` 或 `job_id` 标识。

## 7. 事务号 seq

```text
seq 由上位机生成，uint32，建议从随机非零值开始
同一个 REQ 的 ACK/DONE/ERR 必须回显相同 seq 和 cmd
seq 回绕后跳过 0
串口重新握手后旧 seq 全部失效
收到未知、重复、过期 seq 只记录日志，不完成当前命令
```

初期上位机仍可以限制同一时刻只有一条普通命令在途。  
长任务已经 `ACK` 后，不应一直阻塞只读 `STATUS` 或 `PING`，但 ARM 端可以先按简单模型实现，后续再放开并发。

## 8. Payload 编码

正式 payload 推荐使用 TLV，避免命令参数变化时重做所有结构体。

```text
tlv_type  u16
tlv_len   u16
tlv_value bytes
```

规则：

- 同一 payload 内 TLV 按 `tlv_type` 升序排列，便于调试和测试。
- 必选 TLV 缺失时返回 `ERR BAD_PARAM`。
- 未知 TLV 默认跳过；如果命令不允许扩展参数，返回 `ERR UNSUPPORTED_FIELD`。
- `tlv_len` 必须与字段类型匹配，例如 `u32` 长度必须为 4。
- 字符串使用 UTF-8，不带结尾 `\0`，最大长度由字段定义。

常用基础类型：

```text
u8      1 字节无符号整数
u16     2 字节无符号整数
u32     4 字节无符号整数
u64     8 字节无符号整数
i32     4 字节有符号整数
bool8   1 字节，0=false，1=true
string  UTF-8 字节串，不含 NUL
bytes   原始字节
```

## 9. 通用 TLV 字段

```text
0x0001  status_code       u16     DONE/ERR 状态码
0x0002  error_code        u16     ERR 错误码
0x0003  job_id            u32     长任务 ID
0x0004  event_seq         u32     主动事件序号
0x0005  progress          u8      0~100
0x0006  detail_code       u32     额外错误现场，不使用自由文本判断逻辑
0x0007  uptime_ms         u64     ARM 程序运行时间
0x0008  boot_id           u32     ARM 程序启动 ID
0x0009  acquisition_id    u32     采集会话 ID
```

错误说明尽量使用 `error_code/detail_code`，中文文案由上位机本地映射。  
正式协议不要让上位机根据 ARM 返回的自由文本判断业务逻辑。

## 10. 通用错误码

```text
0x0000  OK
0x0001  UNKNOWN_CMD
0x0002  BAD_FRAME
0x0003  BAD_LEN
0x0004  BAD_PARAM
0x0005  CRC_ERROR
0x0006  UNSUPPORTED_VERSION
0x0007  UNSUPPORTED_FIELD
0x0008  INVALID_STATE
0x0009  BUSY
0x000A  NOT_READY
0x000B  TIMEOUT
0x000C  HW_ERROR
0x000D  NO_TEMPLATE
0x000E  INTERNAL_ERROR
0x000F  PERMISSION_DENIED
```

## 11. 命令 ID 规划

命令 ID 按模块分段，只追加不复用。

```text
0x0001  HELLO
0x0002  PING
0x0003  STATUS
0x0004  VERSION
0x0005  SET_TIME
0x0006  GET_TIME
0x0007  GET_CAPABILITY

0x0100  GET_CONFIG_SUMMARY
0x0101  GET_CONFIG_ITEM
0x0102  SET_CONFIG_ITEM
0x0103  GET_CONFIG_GROUP
0x0104  SET_CONFIG_GROUP
0x0105  RESET_CONFIG_GROUP

0x0200  START_STATIC_CAPTURE
0x0201  QUERY_STATIC

0x0210  START_DYNAMIC
0x0211  STOP_DYNAMIC
0x0212  QUERY_DYNAMIC

0x0300  CAL_OFFSET_BEGIN
0x0301  CAL_OFFSET_CAPTURE
0x0302  CAL_OFFSET_BUILD
0x0303  CAL_OFFSET_CANCEL
0x0304  CAL_GAIN_BEGIN
0x0305  CAL_GAIN_CAPTURE
0x0306  CAL_GAIN_BUILD
0x0307  CAL_GAIN_CANCEL
0x0308  CAL_STATUS

0x0400  CONFIG_GIC_DEBUG
0x0401  CONFIG_ROIC_DEBUG
0x0402  CONFIG_CORR_DEBUG
0x0403  START_TRIPLET_DEBUG
0x0404  READ_REG_DEBUG
0x0405  WRITE_REG_DEBUG
0x0406  DUMP_REGS_DEBUG

0x0500  IMG_UPLOAD_CONFIG
0x0501  IMG_UPLOAD_START
0x0502  IMG_UPLOAD_QUERY
```

说明：

- `*_DEBUG` 命令只用于研发/生产调试，正式用户界面默认不直接暴露。
- 现有 ASCII 命令可以继续保留为 debug shell，但正式上位机工作流应逐步切换到这些业务命令。
- 动态模式不再使用 `DYNC` 缩写，正式协议统一使用 `DYNAMIC`。
- 正式协议不再设计 `CONFIG_STATIC_IDLE`、`CONFIG_DYNAMIC` 这种携带大量参数的命令；配置通过 `*_CONFIG_ITEM` 或 `*_CONFIG_GROUP` 分多帧完成。
- 面向最终用户的可写配置项设置成功后，ARM 应立即保存配置文件；`SAVE_CONFIG` 只作为研发/服务模式保留，不进入普通业务命令规划。

## 12. 配置项和配置组

RS422 控制链路不适合一次传输大量 `key=value` 文本。正式协议中，上位机修改参数应使用配置项 ID 或配置组分片。

推荐配置对象：

```text
ConfigItem   单个参数，例如 static.exposure_window_ms
ConfigGroup  参数组，例如 static、dynamic、corr、roic
Profile      一套完整工作流参数，可选，用于后续多产品/多模式复用
```

推荐命令语义：

```text
GET_CONFIG_SUMMARY
  读取配置版本、配置文件状态、可写权限摘要和当前 active profile。

GET_CONFIG_ITEM item_id
  读取一个配置项。

SET_CONFIG_ITEM item_id value
  修改一个配置项。正式用户模式下修改成功即自动落盘。

GET_CONFIG_GROUP group_id offset length
  分片读取一个配置组。

SET_CONFIG_GROUP group_id offset chunk
  分片写入一个配置组。最后一片校验通过后更新内存配置并自动落盘。

RESET_CONFIG_GROUP group_id
  恢复某个配置组到编译默认值或出厂默认值。
```

配置项权限建议分为：

```text
factory   出厂/硬件配置，普通上位机不可写
service   维护配置，工程模式可写
user      用户业务配置，普通上位机可写并自动保存
runtime   运行态派生参数，不落盘，由 ARM 根据当前工作流生成
```

示例：

```text
factory:
  image.width / image.height / uio device / PA 基地址

service:
  roic.reg_* / irq timeout / dynamic step 表

user:
  static exposure / dark window / corr offset_en / gain_en / defect_en

runtime:
  gic_req_code / gic_dout_en / img_wr_addr / corr offset_corr_mode
```

`runtime` 参数不应由正式上位机直接配置。例如静态自清空、静态 offset 帧、静态 output 帧、动态模式和模板制作都会使用不同的 `gic_dout_en` 或校正模式，这些值应由 ARM 工作流自动派生。

## 13. 握手 HELLO

上位机打开串口后不能直接认为设备在线，必须先握手。

REQ `HELLO` payload：

```text
0x0100  host_name          string  上位机名称，例如 PA_HOST
0x0101  proto_major        u16     主版本，当前 1
0x0102  proto_minor        u16     次版本，当前 0
0x0103  host_session_id    u64     上位机本次连接随机 ID
```

DONE `HELLO` payload：

```text
0x0101  proto_major        u16
0x0102  proto_minor        u16
0x0104  app_version        string  pa_controller 软件版本
0x0105  build_time         string  编译时间
0x0106  model              string  设备型号，可为空
0x0107  serial             string  设备序列号，可为空
0x0108  boot_id            u32
0x0109  capability_flags   u32
```

握手规则：

- `proto_major` 不一致时，上位机提示协议不兼容并禁止业务命令。
- `proto_minor` 不一致时，根据 `capability_flags` 降级功能。
- `boot_id` 改变表示 ARM 程序重启，上位机必须清空旧任务和旧状态。

## 14. STATUS / VERSION

`STATUS` 用于读取运行状态，不读取会清零的 `INT_VECTOR`。

DONE `STATUS` payload 建议包含：

```text
0x0200  work_mode          u16     当前工作模式
0x0201  work_state         u16     ARM 工作线程状态
0x0202  last_error         u16
0x0203  last_phase         u16
0x0204  capture_id         u32
0x0205  frame_count        u32
0x0206  output_addr        u32
0x0207  offset_addr        u32
0x0208  gain_addr          u32
0x0209  wr_state           u32
0x020A  wr_end             u32
0x020B  corr_state         u32
0x020C  corr_end           u32
0x020D  gic_state          u32
0x020E  gic_end            u32
0x020F  gic_dfx            u32
0x0210  roic_state         u32
0x0211  roic_end           u32
0x0212  roic_dfx           u32
0x0213  dync_state         u32
0x0214  img_upload_state   u32
```

`VERSION` 用于读取软件和 FPGA 版本，DONE payload 建议包含：

```text
0x0300  app_version        string
0x0301  build_time         string
0x0302  pa_version                         u32
0x0303  pa_build_information               u32
0x0304  adapted_main_board_version         u32
0x0305  adapted_gic_board_version          u32
0x0306  adapted_roic_board_version         u32
0x0307  adapted_reserved_board_0_version   u32
0x0308  adapted_reserved_board_1_version   u32
0x0309  adapted_reserved_board_2_version   u32
0x030A  pa_pu_com_version                  u32
```

版本寄存器解析规则：

```text
bit23~16  first version num
bit15~8   second version num
bit7~0    third version num
```

上位机显示时应解析为 `x.y.z`，不要只显示原始十六进制。

## 15. 静态 Idle 工作流

静态模式是正式业务命令，不应由上位机逐条发送 `CONFIG_GIC`、`CONFIG_CORR`、`WRITE_REG` 拼出来。

静态参数通过配置项或配置组维护，不设计单独的长参数 `CONFIG_STATIC_IDLE` 命令。常用配置项示例：

```text
0x1000  static.idle_clean_interval_ms  u32   user/service
0x1001  static.exposure_ms             u32   user
0x1002  static.dark_window_ms          u32   user
0x1003  corr.offset_enable             bool8 user
0x1004  corr.gain_enable               bool8 user
0x1005  corr.defect_enable             bool8 user
0x1006  gic.line_time_ns               u32   service
0x1007  gic.start_row                  u16   service
0x1008  gic.end_row                    u16   service
0x1009  gic.binning                    u8    service
0x100A  roic.start_col                 u16   service
0x100B  roic.end_col                   u16   service
0x100C  roic.binning                   u8    service
```

默认值建议继续来自 `src/app_config.h`：

```text
idle_clean_interval_ms = 50
exposure_ms            = 50
dark_window_ms         = 50
offset_enable          = 1
gain_enable            = 1
defect_enable          = 0
line_time_ns           = 25600
start_row              = 0
end_row                = 7679
gic_binning            = 0
roic_start_col         = 0
roic_end_col           = 3071
roic_binning           = 0
```

`START_STATIC_CAPTURE` payload：

```text
空，或仅带 acquisition_id。
```

执行语义：

```text
1. 如果 Static Idle 自清空正在执行，等待本次自清空结束
2. 进入曝光窗口 exposure_ms
3. bright 阶段：关闭 offset/gain/defect，写 offset 模板区
4. 进入 dark_window_ms
5. dark 阶段：按当前配置中的校正开关采图，写 output_addr
6. 每个采图阶段由 ARM 同时启动 IMG_CORR_STR、IMG_WR_STR、GIC_STR
7. 每个采图阶段等待 IMG_CORR、IMG_WR、GIC 三个完成条件
8. DONE 返回 offset_addr、output_addr、capture_id、acquisition_id
```

DONE payload：

```text
0x0009  acquisition_id          u32
0x1012  capture_id              u32
0x1013  offset_addr             u32
0x1014  output_addr             u32
0x1015  int_vector              u32
```

失败时返回 `ERR`，`detail_code` 可标识阶段：

```text
1  idle_clean
2  exposure_window
3  bright_capture
4  dark_window
5  dark_capture
```

一旦正式采图失败，ARM 应进入错误状态并停止继续操作 FPGA，等待上位机查询状态或人工恢复。

## 16. 动态模式工作流

动态模式由 ARM/FPGA 自行循环，上位机只负责维护配置、启动、查询和停止。

动态参数通过 `SET_CONFIG_ITEM` 或 `SET_CONFIG_GROUP dynamic.steps` 维护，不设计单独的长参数 `CONFIG_DYNAMIC` 命令。常用配置项示例：

```text
0x2000  dynamic.cycle_count       u32   0 表示无限循环
0x2001  dynamic.img_start_addr    u32   可为 auto，由 ARM 根据 UIO 推导
0x2002  dynamic.img_end_addr      u32   可为 auto，由 ARM 根据 UIO 推导
0x2010  dynamic.step_count        u8
0x2100  dynamic.step0.enable      bool8
0x2101  dynamic.step0.req_code    u8
0x2102  dynamic.step0.time_ms     u32
0x2110  dynamic.step1.enable      bool8
0x2111  dynamic.step1.req_code    u8
0x2112  dynamic.step1.time_ms     u32
...     dynamic.stepN             ...
```

动态 step high word 拼接规则来自 FPGA 定义：

```text
bit31     current step enable
bit30~8   reserved，写 0
bit7~0    current step req_code
```

`req_code` 定义：

```text
0  idle
1  serial clear
2  parallel clear
3  xao clear
4  capture one image
5  wait sync in signals
6  wait sync out
```

`START_DYNAMIC` payload：

```text
空，或仅带 acquisition_id / profile_id。
```

语义：

- `START_DYNAMIC` 只负责启动动态模式。
- 如果 `cycle_count=0`，该命令不能一直等待模式结束。
- 成功启动后返回 `DONE state=running`。
- 运行状态通过 `QUERY_DYNAMIC` 或 `EVT DYNAMIC_STATUS` 获取。

DONE `START_DYNAMIC` payload：

```text
0x0009  acquisition_id    u32
0x2003  dynamic_state     u32
0x2004  final_img_addr    u32     当前可读 IMG_WR_FINAL_IMG_ADDR
```

`STOP_DYNAMIC` payload：空  
DONE `STOP_DYNAMIC` payload：

```text
0x2003  dynamic_state     u32
0x2004  final_img_addr    u32
0x2005  frame_count       u32
```

`QUERY_DYNAMIC` payload：空  
DONE `QUERY_DYNAMIC` payload：

```text
0x2003  dynamic_state     u32
0x2004  final_img_addr    u32
0x2005  frame_count       u32
0x2006  last_int_vector   u32     调试字段，可选
0x2007  dync_end          u32
0x2008  dync_debug_out    u32
```

动态完成事件：

```text
EVT DYNAMIC_STATUS    每 1s 或状态变化时上报
EVT DYNAMIC_STOPPED   STOP_DYNAMIC 生效后上报
EVT DYNAMIC_FAILED    FPGA/ARM 检测到异常
```

动态模式正式等待逻辑：

```text
需要判断动态状态时，以 dync_state 为事实来源。
dync_state=1 表示动态模块仍在运行。
dync_state=0 表示动态模块停止或本轮结束。
dynamic interrupt bit 只作为完成通知，不作为唯一事实来源。
```

## 17. 模板制作工作流

### 17.1 Offset 模板

Offset 模板支持静态或动态采集方式。动态方式需要采集总张数和有效张数，取最后 `valid_frames` 张做点对点均值。

`CAL_OFFSET_BEGIN` payload：

```text
0x3000  total_frames      u32
0x3001  valid_frames      u32
0x3002  mode              u8      0=static, 1=dynamic
0x3003  output_path       string  可选，默认 /usr/local/offset.raw
```

规则：

```text
total_frames >= valid_frames
valid_frames > 0
采集过程中前 total_frames-valid_frames 张只用于预热/稳定，不参与均值
后 valid_frames 张逐像素累加后除以 valid_frames，写入 offset 模板文件和 offset UIO
```

### 17.2 Gain/Defect 模板

Gain 模板由多个灰度级均值图拟合生成，defect 不生成单独模板，通过 `gain=0` 标记坏点。

`CAL_GAIN_BEGIN` payload：

```text
0x3100  level_count       u16
0x3101  frames_per_level  u32
0x3102  defect_threshold  u32     float32 原始字节，默认 0.3
0x3110  level_values      bytes   u32 数组，长度 = level_count * 4
```

`CAL_GAIN_CAPTURE` payload：

```text
0x3120  level             u32
```

`CAL_GAIN_BUILD` payload：空

算法规则：

```text
1. 每个灰度级由上位机控制光源或剂量，ARM 只采集 frames_per_level 张
2. 对同一灰度级的多帧图像逐像素均值，得到均值图 Mn
3. 对每个 Mn 计算中位值，得到 Medians[n]
4. 每个像素位置取 Pixels[n]，与 Medians[n] 做最小二乘线性拟合，得到 slope
5. 如果 slope < 0 或 slope >= 3.99，则 slope = 0
6. gain_pixel = round(slope * 16384)
7. 缺陷检测命中的像素 gain 写 0
8. 输出 /usr/local/gain.raw 并加载到 gain UIO
```

`CAL_STATUS` 用于查询当前模板制作进度，避免长时间无日志导致上位机无法判断状态。

当前实现的 `CAL_STATUS` DONE payload：

```text
0x3200  task_id           u32
0x3201  task_kind         u32
0x3202  task_state        u32   0=idle,1=running,2=stopping,3=succeeded,4=failed,5=canceled
0x3203  last_error        u32
0x3204  progress_current  u32
0x3205  progress_total    u32
0x3206  gain_level        u32
0x3211  level_count       u32
0x3212  levels_ready      u32
0x3213  frames_per_level  u32
0x3214  defect_threshold  u32   float32 原始字节
0x3215  bad_pixel_count   u32
```

Offset 当前由 `CAL_OFFSET_CAPTURE` 启动后台采集，后台任务完成时已经原子生成并加载模板；
`CAL_OFFSET_BUILD` 是上位机确认完成的协议边界。Static 模式只接受 `1/1` 单帧，Dynamic
模式支持总帧数/有效帧数均值。

### 17.3 模板上传查看

`IMG_UPLOAD_CONFIG` payload：

```text
0x5000  template_kind  u32   0=offset, 1=gain
0x5001  image_addr     u32   可选，研发自定义地址
0x5002  row_num        u32
0x5003  col_num        u32
0x5004  pkg_num        u32
```

`IMG_UPLOAD_START` payload 为空，触发 FPGA 上传并等待 `IMG_UPLOAD_END` 中断；
`IMG_UPLOAD_QUERY` 返回 `0x5005 state`、`0x5006 end`、`0x5007 dfx`。

## 18. 调试命令边界

调试命令包括：

```text
CONFIG_GIC_DEBUG
CONFIG_ROIC_DEBUG
CONFIG_CORR_DEBUG
START_TRIPLET_DEBUG
READ_REG_DEBUG
WRITE_REG_DEBUG
DUMP_REGS_DEBUG
```

约束：

- 只在工程模式或研发版本暴露。
- 工作模式采图阶段不允许裸写寄存器。
- `READ_REG_DEBUG` 默认不应读取 `INT_VECTOR`，除非明确指定调试选项。
- `WRITE_REG_DEBUG` 可选择只写或写后校验，校验次数由构建或配置决定。
- 触发类寄存器，例如 `*_STR`、`*_STOP`，只写不校验。

## 19. 事件定义

事件 payload 统一包含：

```text
0x0004  event_seq         u32
0x0003  job_id            u32     如果事件属于长任务
0x0009  acquisition_id    u32     如果事件属于采集
```

建议事件 ID：

```text
0x8001  STATUS_CHANGED
0x8002  JOB_PROGRESS
0x8003  JOB_DONE
0x8004  JOB_FAILED
0x8005  STATIC_CAPTURE_DONE
0x8006  STATIC_CAPTURE_FAILED
0x8007  DYNAMIC_STATUS
0x8008  DYNAMIC_STOPPED
0x8009  DYNAMIC_FAILED
0x800A  TEMPLATE_READY
0x800B  DEVICE_FAULT
```

主动事件只是通知；如果事件丢失，上位机应能通过 `STATUS`、`QUERY_DYNAMIC` 或 `CAL_STATUS` 查询事实状态。

## 20. 上位机状态机建议

RS422 链路状态：

```text
Disconnected  串口未打开
Connecting    正在打开串口
Verifying     已打开串口，正在 HELLO
Online        协议握手完成
Degraded      短暂超时或错误，但未判定断线
Recovering    正在重连或重新握手
```

命令状态：

```text
Idle          无在途命令
WaitingReply  等待短命令 DONE/ERR
RunningJob    长任务已 ACK，等待事件或查询结果
Failed        最近命令失败
```

上位机不要用单个 `Error` 同时表示串口断线、协议错误、业务失败和设备硬件故障。

## 21. 迁移计划

当前实现和后续迁移顺序：

```text
阶段 1：已完成；ASCII 调试协议保留，二进制帧收发、CRC、拆包/粘包已接入
阶段 2：已完成基础版；HELLO、PING、STATUS、VERSION 已由 ARM 二进制入口支持
阶段 3：已完成；Qt 上位机使用 `--binary` 后可发送和解析正式二进制帧
阶段 4：已完成；START_STATIC_CAPTURE、START_DYNAMIC、STOP_DYNAMIC、QUERY_DYNAMIC 已接入 ARM 和 Qt 上位机
阶段 5：已完成配置组；GET/SET_CONFIG_GROUP 已接入，单配置项命令按需要继续补充
阶段 6：已完成模板业务；Offset/Gain 制作、CAL_STATUS 和模板上传已接入，调试命令仍独立保留
阶段 7：正式版本隐藏危险调试命令，ASCII 仅保留工程入口
```

每个阶段都需要同时提供：

```text
ARM 解析器测试
上位机协议编码/解码测试
固定二进制测试向量
串口丢字节/错字节/粘包/拆包测试
```

## 22. 上位机和 SDK 联调前必须确认

帧格式、CRC 算法、空请求固定向量以及 ARM 端基础编解码接口已经确定并落地。下面只保留业务联调前仍需要双方确认的事项：

```text
1. 是否启用 header_crc，还是继续保持 0
2. 最大 payload 长度是否继续使用 2048
3. 正式波特率，以及是否评估 921600
4. HELLO 中 model/serial/capability_flags 的来源
5. dynamic_state、frame_count、final_img_addr 的事实寄存器
6. acquisition_id 是否需要进入 FPGA/PCIe 图像元数据
7. READ_REG/WRITE_REG 在正式版本中的权限策略
8. Offset/Gain 模板制作是否必须支持取消和进度事件
9. ASCII 调试协议在正式发布时是否默认关闭
10. ARM、Qt、SDK 三端的 CRC/拆包单元测试框架
```
