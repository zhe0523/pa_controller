#pragma once

#include <stddef.h>

/* RS422 串口句柄。RS422 本质上仍通过 Linux tty 设备访问。 */
typedef struct {
  /* 已打开的 /dev/ttySx 文件描述符，-1 表示未打开。 */
  int fd;
} rs422_t;

/* 打开并配置 422 串口，当前使用 8N1、raw 模式、无流控。 */
int rs422_open(rs422_t* port, const char* device, int baud);

/* 关闭 422 串口。 */
void rs422_close(rs422_t* port);

/* 按行读取命令，去掉 \r/\n；返回读取到的字符数。 */
int rs422_read_line(rs422_t* port, char* buffer, size_t size);

/* 写出协议响应，调用者负责提供 \r\n。 */
int rs422_write_text(rs422_t* port, const char* text);
