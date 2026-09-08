#pragma once

/*
 * PA Controller 正式二进制协议基础层。
 *
 * 这一层只负责帧格式、CRC 和串口字节流重同步，不负责具体业务命令。
 * ASCII 调试协议可以继续独立使用；后续 ARM、上位机和 SDK 共用本定义。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PA_PROTOCOL_MAGIC = 0x55AAu,
  PA_PROTOCOL_VERSION = 1u,
  PA_PROTOCOL_HEADER_SIZE = 16u,
  PA_PROTOCOL_CRC_SIZE = 2u,
  PA_PROTOCOL_MAX_PAYLOAD = 2048u,
  PA_PROTOCOL_MAX_FRAME = PA_PROTOCOL_HEADER_SIZE +
                          PA_PROTOCOL_MAX_PAYLOAD + PA_PROTOCOL_CRC_SIZE,
};

typedef enum {
  PA_PROTOCOL_MSG_REQ = 0x01,
  PA_PROTOCOL_MSG_ACK = 0x02,
  PA_PROTOCOL_MSG_DONE = 0x03,
  PA_PROTOCOL_MSG_ERR = 0x04,
  PA_PROTOCOL_MSG_EVT = 0x05,
} pa_protocol_msg_type_t;

typedef enum {
  PA_PROTOCOL_OK = 0,
  PA_PROTOCOL_ERR_ARGUMENT = -1,
  PA_PROTOCOL_ERR_CAPACITY = -2,
  PA_PROTOCOL_ERR_MAGIC = -3,
  PA_PROTOCOL_ERR_VERSION = -4,
  PA_PROTOCOL_ERR_HEADER = -5,
  PA_PROTOCOL_ERR_LENGTH = -6,
  PA_PROTOCOL_ERR_HEADER_CRC = -7,
  PA_PROTOCOL_ERR_CRC = -8,
  PA_PROTOCOL_ERR_CALLBACK = -9,
} pa_protocol_result_t;

/* 解码后的帧视图；payload 指向输入缓冲区，不由本结构体管理生命周期。 */
typedef struct {
  uint8_t version;
  uint8_t header_len;
  uint8_t msg_type;
  uint8_t flags;
  uint16_t cmd;
  uint32_t seq;
  uint16_t payload_len;
  uint16_t header_crc;
  const uint8_t* payload;
} pa_protocol_frame_view_t;

/*
 * 计算 CRC16-CCITT-FALSE。
 * 参数：poly=0x1021、init=0xFFFF、xorout=0、无 bit reverse。
 */
uint16_t pa_protocol_crc16(const uint8_t* data, size_t length);

/* 将一帧编码到 out；成功后 out_length 为完整帧长度。 */
int pa_protocol_encode(uint8_t msg_type,
                       uint8_t flags,
                       uint16_t cmd,
                       uint32_t seq,
                       const uint8_t* payload,
                       uint16_t payload_len,
                       uint8_t* out,
                       size_t out_capacity,
                       size_t* out_length);

/* 校验并解析一帧；frame_length 必须等于完整帧长度。 */
int pa_protocol_decode(const uint8_t* frame,
                       size_t frame_length,
                       pa_protocol_frame_view_t* view);

/* 串口流解析回调；view 仅在回调执行期间有效。 */
typedef int (*pa_protocol_frame_callback_t)(const pa_protocol_frame_view_t* view,
                                             void* user);

typedef struct {
  uint8_t buffer[PA_PROTOCOL_MAX_FRAME];
  size_t length;
} pa_protocol_parser_t;

void pa_protocol_parser_init(pa_protocol_parser_t* parser);

/*
 * 输入任意长度的串口字节流，自动处理拆包、粘包和错误字节。
 * 成功返回 0；回调返回非零时返回 PA_PROTOCOL_ERR_CALLBACK。
 * 回调结束后 view 失效，业务如需保存 payload 必须自行复制。
 */
int pa_protocol_parser_feed(pa_protocol_parser_t* parser,
                            const uint8_t* data,
                            size_t length,
                            pa_protocol_frame_callback_t callback,
                            void* user);

#ifdef __cplusplus
}
#endif
