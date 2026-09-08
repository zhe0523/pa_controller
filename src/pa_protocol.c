#include "../include/pa_protocol.h"

#include <stdbool.h>
#include <string.h>

static uint16_t read_u16_le(const uint8_t* data) {
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8u);
}

static uint32_t read_u32_le(const uint8_t* data) {
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) |
         ((uint32_t)data[3] << 24u);
}

static void write_u16_le(uint8_t* data, uint16_t value) {
  data[0] = (uint8_t)(value & 0xffu);
  data[1] = (uint8_t)((value >> 8u) & 0xffu);
}

static void write_u32_le(uint8_t* data, uint32_t value) {
  data[0] = (uint8_t)(value & 0xffu);
  data[1] = (uint8_t)((value >> 8u) & 0xffu);
  data[2] = (uint8_t)((value >> 16u) & 0xffu);
  data[3] = (uint8_t)((value >> 24u) & 0xffu);
}

uint16_t pa_protocol_crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xffffu;
  if (data == NULL && length != 0u) {
    return 0u;
  }

  for (size_t i = 0u; i < length; ++i) {
    crc ^= (uint16_t)data[i] << 8u;
    for (unsigned bit = 0u; bit < 8u; ++bit) {
      if ((crc & 0x8000u) != 0u) {
        crc = (uint16_t)((crc << 1u) ^ 0x1021u);
      } else {
        crc <<= 1u;
      }
    }
  }
  return crc;
}

int pa_protocol_encode(uint8_t msg_type,
                       uint8_t flags,
                       uint16_t cmd,
                       uint32_t seq,
                       const uint8_t* payload,
                       uint16_t payload_len,
                       uint8_t* out,
                       size_t out_capacity,
                       size_t* out_length) {
  const size_t frame_length = PA_PROTOCOL_HEADER_SIZE +
                              (size_t)payload_len + PA_PROTOCOL_CRC_SIZE;
  if (out == NULL || out_length == NULL ||
      (payload == NULL && payload_len != 0u)) {
    return PA_PROTOCOL_ERR_ARGUMENT;
  }
  if (payload_len > PA_PROTOCOL_MAX_PAYLOAD || out_capacity < frame_length) {
    return PA_PROTOCOL_ERR_CAPACITY;
  }

  write_u16_le(out + 0u, PA_PROTOCOL_MAGIC);
  out[2] = PA_PROTOCOL_VERSION;
  out[3] = PA_PROTOCOL_HEADER_SIZE;
  out[4] = msg_type;
  out[5] = flags;
  write_u16_le(out + 6u, cmd);
  write_u32_le(out + 8u, seq);
  write_u16_le(out + 12u, payload_len);
  /* 当前版本不启用 header_crc，保留为 0 以便以后扩展帧头。 */
  write_u16_le(out + 14u, 0u);
  if (payload_len != 0u) {
    memcpy(out + PA_PROTOCOL_HEADER_SIZE, payload, payload_len);
  }

  const uint16_t crc = pa_protocol_crc16(out, frame_length - PA_PROTOCOL_CRC_SIZE);
  write_u16_le(out + frame_length - PA_PROTOCOL_CRC_SIZE, crc);
  *out_length = frame_length;
  return PA_PROTOCOL_OK;
}

int pa_protocol_decode(const uint8_t* frame,
                       size_t frame_length,
                       pa_protocol_frame_view_t* view) {
  if (frame == NULL || view == NULL) {
    return PA_PROTOCOL_ERR_ARGUMENT;
  }
  if (frame_length < PA_PROTOCOL_HEADER_SIZE + PA_PROTOCOL_CRC_SIZE) {
    return PA_PROTOCOL_ERR_LENGTH;
  }
  if (read_u16_le(frame) != PA_PROTOCOL_MAGIC) {
    return PA_PROTOCOL_ERR_MAGIC;
  }
  if (frame[2] != PA_PROTOCOL_VERSION) {
    return PA_PROTOCOL_ERR_VERSION;
  }
  if (frame[3] != PA_PROTOCOL_HEADER_SIZE) {
    return PA_PROTOCOL_ERR_HEADER;
  }

  const uint16_t payload_len = read_u16_le(frame + 12u);
  const size_t expected_length = PA_PROTOCOL_HEADER_SIZE +
                                 (size_t)payload_len + PA_PROTOCOL_CRC_SIZE;
  if (payload_len > PA_PROTOCOL_MAX_PAYLOAD || frame_length != expected_length) {
    return PA_PROTOCOL_ERR_LENGTH;
  }

  const uint16_t header_crc = read_u16_le(frame + 14u);
  if (header_crc != 0u) {
    /* header_crc 当前保留，非零值不能被本版本误当作已验证字段。 */
    return PA_PROTOCOL_ERR_HEADER_CRC;
  }

  const uint16_t expected_crc = pa_protocol_crc16(frame,
                                                  frame_length - PA_PROTOCOL_CRC_SIZE);
  if (read_u16_le(frame + frame_length - PA_PROTOCOL_CRC_SIZE) != expected_crc) {
    return PA_PROTOCOL_ERR_CRC;
  }

  view->version = frame[2];
  view->header_len = frame[3];
  view->msg_type = frame[4];
  view->flags = frame[5];
  view->cmd = read_u16_le(frame + 6u);
  view->seq = read_u32_le(frame + 8u);
  view->payload_len = payload_len;
  view->header_crc = header_crc;
  view->payload = frame + PA_PROTOCOL_HEADER_SIZE;
  return PA_PROTOCOL_OK;
}

void pa_protocol_parser_init(pa_protocol_parser_t* parser) {
  if (parser != NULL) {
    memset(parser, 0, sizeof(*parser));
  }
}

static void parser_drop_prefix(pa_protocol_parser_t* parser, size_t count) {
  if (count >= parser->length) {
    parser->length = 0u;
    return;
  }
  memmove(parser->buffer, parser->buffer + count, parser->length - count);
  parser->length -= count;
}

int pa_protocol_parser_feed(pa_protocol_parser_t* parser,
                            const uint8_t* data,
                            size_t length,
                            pa_protocol_frame_callback_t callback,
                            void* user) {
  if (parser == NULL || (data == NULL && length != 0u) || callback == NULL) {
    return PA_PROTOCOL_ERR_ARGUMENT;
  }

  for (size_t i = 0u; i < length; ++i) {
    if (parser->length == sizeof(parser->buffer)) {
      /* 缓冲区满而仍未形成有效帧时，丢弃一个字节并继续找 magic。 */
      parser_drop_prefix(parser, 1u);
    }
    parser->buffer[parser->length++] = data[i];

    for (;;) {
      if (parser->length < 2u) {
        break;
      }
      if (parser->buffer[0] != 0xAAu || parser->buffer[1] != 0x55u) {
        parser_drop_prefix(parser, 1u);
        continue;
      }
      if (parser->length < PA_PROTOCOL_HEADER_SIZE) {
        break;
      }

      const uint16_t payload_len = read_u16_le(parser->buffer + 12u);
      const size_t frame_length = PA_PROTOCOL_HEADER_SIZE +
                                  (size_t)payload_len + PA_PROTOCOL_CRC_SIZE;
      if (payload_len > PA_PROTOCOL_MAX_PAYLOAD ||
          frame_length > sizeof(parser->buffer)) {
        parser_drop_prefix(parser, 1u);
        continue;
      }
      if (parser->length < frame_length) {
        break;
      }

      pa_protocol_frame_view_t view;
      const int result = pa_protocol_decode(parser->buffer, frame_length, &view);
      if (result != PA_PROTOCOL_OK) {
        /* CRC/版本/头字段错误时只滑动一个字节，避免丢掉后续合法 magic。 */
        parser_drop_prefix(parser, 1u);
        continue;
      }

      if (callback(&view, user) != 0) {
        return PA_PROTOCOL_ERR_CALLBACK;
      }
      parser_drop_prefix(parser, frame_length);
    }
  }
  return PA_PROTOCOL_OK;
}
