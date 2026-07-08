#include "log.h"

#include <stdio.h>
#include <time.h>

static log_level_t g_level = LOG_LEVEL_INFO;

void log_set_level(log_level_t level) {
  g_level = level;
}

void log_write(log_level_t level, const char* file, int line, const char* fmt, ...) {
  if (level < g_level) {
    return;
  }

  static const char* names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

  fprintf(stderr, "%s [%s] %s:%d ", ts, names[level], file, line);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);

  fputc('\n', stderr);
}

