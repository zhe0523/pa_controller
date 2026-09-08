#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config_store.h"
#include "fpga_mem.h"
#include "pa_protocol.h"
#include "work_mode.h"

/*
 * 命令处理上下文。
 *
 * 这个结构把通信层和业务层隔开：无论命令来自 RS422 还是 --stdio，
 * 最后都会进入 command_handle()，并通过这里的上下文访问 FPGA 内存或控制退出。
 */
typedef struct {
  /* FPGA/PA 共享内存，用于模板加载、offset/gain 生成。 */
  fpga_mem_t* fpga_mem;
  /* 正式工作模式状态机。 */
  work_mode_context_t* work_mode;
  /* 当前运行时配置；正式配置命令修改后会立即应用并保存。 */
  pa_runtime_config_t* runtime_config;
  const char* config_file;
  /* CAL_OFFSET_BEGIN 保存本轮参数，后续 CAPTURE/BUILD 复用，避免长任务阻塞串口。 */
  bool offset_calibration_configured;
  uint32_t offset_total_frames;
  uint32_t offset_valid_frames;
  uint8_t offset_calibration_mode;
  /* QUIT 命令置位后，main.c 的通信循环会退出。 */
  bool should_quit;
} command_context_t;

/*
 * 处理一条 ASCII 命令。
 *
 * command 传入时不包含结尾的 \r 或 \n。
 * response 会写入带 \r\n 的协议响应；如果不需要回复，则保持为空字符串。
 */
int command_handle(command_context_t* ctx, const char* command, char* response, size_t response_size);

/* 处理一帧正式二进制请求，并编码一帧 ACK/DONE/ERR 响应。 */
int command_handle_binary(command_context_t* ctx,
                          const pa_protocol_frame_view_t* request,
                          uint8_t* response,
                          size_t response_capacity,
                          size_t* response_length);
