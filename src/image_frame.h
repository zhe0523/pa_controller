#pragma once

#include <stdint.h>

/*
 * 光口图像数据头，来自项目要求：
 * - 角位置编码 A+/A-：0~1439
 * - 外触发脉冲计数：实际采集帧数计数
 * - Home pulse 计数 Z+/Z-：每圈归零后递增
 * - 皮带位置编码 B+/B-：0~65535
 * - 预留字段
 *
 * 该结构只描述传输头格式；实际插入图像流的位置由 PA/FPGA 传图逻辑实现。
 */
typedef struct __attribute__((packed)) {
  uint16_t angle_position;
  uint16_t trigger_frame_count;
  uint16_t home_pulse_count;
  uint16_t belt_position;
  uint16_t reserved;
} detector_image_header_t;

enum {
  DETECTOR_IMAGE_HEADER_BYTES = sizeof(detector_image_header_t),
};

_Static_assert(sizeof(detector_image_header_t) == 10, "detector image header must be 10 bytes");
