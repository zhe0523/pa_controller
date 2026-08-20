#pragma once

#include <stdarg.h>

/* 简单日志级别，数值越大表示越严重。 */
typedef enum {
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR,
} log_level_t;

/* 设置最低输出日志级别。 */
void log_set_level(log_level_t level);

/*
 * 底层日志输出函数；业务代码优先使用下面的 log_* 宏以自动带上文件和行号。
 * 实现侧使用异步队列写 stderr，避免稳定性测试时被终端/串口输出反压阻塞。
 */
void log_write(log_level_t level, const char* file, int line, const char* fmt, ...);

/* 程序退出前主动刷新日志队列；未调用时也会由 atexit 尽量刷新。 */
void log_shutdown(void);

/* 日志最终统一输出到 stderr，避免污染 --stdio 模式下 stdout 的协议响应。 */
#define log_debug(...) log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, __VA_ARGS__)
