#include "pa_protocol.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  unsigned count;
  uint16_t last_cmd;
  uint32_t last_seq;
} callback_state_t;

static int receive_frame(const pa_protocol_frame_view_t* frame, void* user) {
  callback_state_t* state = (callback_state_t*)user;
  state->count++;
  state->last_cmd = frame->cmd;
  state->last_seq = frame->seq;
  return 0;
}

static int check(int condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "FAILED: %s\n", message);
    return 0;
  }
  return 1;
}

int main(void) {
  static const uint8_t expected_ping[] = {
    0xAA, 0x55, 0x01, 0x10, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xB7, 0xD3,
  };
  uint8_t frame[PA_PROTOCOL_MAX_FRAME];
  size_t frame_length = 0u;
  if (!check(pa_protocol_encode(PA_PROTOCOL_MSG_REQ, 0u, 0x0002u, 1u,
                                NULL, 0u, frame, sizeof(frame), &frame_length) == PA_PROTOCOL_OK,
             "encode ping") ||
      !check(frame_length == sizeof(expected_ping), "ping length") ||
      !check(memcmp(frame, expected_ping, sizeof(expected_ping)) == 0, "fixed ping vector")) {
    return 1;
  }

  pa_protocol_frame_view_t decoded;
  if (!check(pa_protocol_decode(frame, frame_length, &decoded) == PA_PROTOCOL_OK, "decode ping") ||
      !check(decoded.cmd == 0x0002u && decoded.seq == 1u, "decoded identity")) {
    return 1;
  }

  pa_protocol_parser_t parser;
  pa_protocol_parser_init(&parser);
  callback_state_t state = {0};
  const uint8_t noise[] = {0x11, 0x22, 0x33};
  if (!check(pa_protocol_parser_feed(&parser, noise, sizeof(noise), receive_frame, &state) == PA_PROTOCOL_OK,
             "feed noise") ||
      !check(pa_protocol_parser_feed(&parser, frame, 7u, receive_frame, &state) == PA_PROTOCOL_OK,
             "feed split head") ||
      !check(state.count == 0u, "split frame not delivered early") ||
      !check(pa_protocol_parser_feed(&parser, frame + 7u, frame_length - 7u,
                                     receive_frame, &state) == PA_PROTOCOL_OK,
             "feed split tail") ||
      !check(state.count == 1u && state.last_cmd == 0x0002u, "split frame delivered")) {
    return 1;
  }

  uint8_t joined[sizeof(expected_ping) * 2u];
  memcpy(joined, frame, frame_length);
  memcpy(joined + frame_length, frame, frame_length);
  if (!check(pa_protocol_parser_feed(&parser, joined, sizeof(joined), receive_frame, &state) == PA_PROTOCOL_OK,
             "feed joined frames") ||
      !check(state.count == 3u && state.last_seq == 1u, "joined frames delivered")) {
    return 1;
  }

  frame[frame_length - 1u] ^= 0x01u;
  if (!check(pa_protocol_decode(frame, frame_length, &decoded) == PA_PROTOCOL_ERR_CRC, "reject bad crc")) {
    return 1;
  }

  puts("pa_protocol_tests passed");
  return 0;
}
