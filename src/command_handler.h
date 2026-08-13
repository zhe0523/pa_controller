#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "fpga_mem.h"

/*
 * 命令处理上下文。
 *
 * 这个结构把通信层和业务层隔开：无论命令来自 RS422 还是 --stdio，
 * 最后都会进入 command_handle()，并通过这里的上下文访问 FPGA 内存或控制退出。
 */
typedef struct {
  /* FPGA/PA 共享内存，用于模板加载、offset/gain 生成。 */
  fpga_mem_t* fpga_mem;
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
