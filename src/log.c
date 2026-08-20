#include "log.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  /* 日志队列容量固定，避免采图过程中动态分配内存。 */
  LOG_QUEUE_CAPACITY = 256,
  /* 单行日志最大长度；超长日志会被截断，但不会阻塞业务线程。 */
  LOG_LINE_MAX = 1024,
};

typedef struct {
  char line[LOG_LINE_MAX];
} log_entry_t;

static log_level_t g_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_started = false;
static bool g_stopping = false;
static bool g_atexit_registered = false;
static log_entry_t g_queue[LOG_QUEUE_CAPACITY];
static size_t g_head = 0;
static size_t g_tail = 0;
static size_t g_count = 0;
static unsigned g_dropped = 0;

static const char* level_name(log_level_t level) {
  static const char* names[] = { "DEBUG", "INFO", "WARN", "ERROR" };
  if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_ERROR) {
    return "UNKNOWN";
  }
  return names[level];
}

static void write_line_direct(const char* line) {
  if (line == NULL) {
    return;
  }
  fputs(line, stderr);
  fputc('\n', stderr);
}

static bool pop_entry_locked(log_entry_t* entry) {
  if (g_count == 0 || entry == NULL) {
    return false;
  }

  *entry = g_queue[g_head];
  g_head = (g_head + 1u) % LOG_QUEUE_CAPACITY;
  --g_count;
  return true;
}

static void push_entry_locked(const char* line, log_level_t level) {
  /*
   * 队列满时不等待日志线程，避免终端输出反压拖住采图。
   * INFO/DEBUG 直接丢弃；WARN/ERROR 覆盖最旧日志，尽量保留故障现场。
   */
  if (g_count == LOG_QUEUE_CAPACITY) {
    ++g_dropped;
    if (level <= LOG_LEVEL_INFO) {
      return;
    }
    g_head = (g_head + 1u) % LOG_QUEUE_CAPACITY;
    --g_count;
  }

  snprintf(g_queue[g_tail].line, sizeof(g_queue[g_tail].line), "%s", line);
  g_tail = (g_tail + 1u) % LOG_QUEUE_CAPACITY;
  ++g_count;
  pthread_cond_signal(&g_cond);
}

static void* log_thread_main(void* arg) {
  (void)arg;

  for (;;) {
    log_entry_t entry;
    bool has_entry = false;
    unsigned dropped = 0;

    pthread_mutex_lock(&g_mutex);
    while (g_count == 0 && !g_stopping) {
      pthread_cond_wait(&g_cond, &g_mutex);
    }

    if (g_count == 0 && g_stopping) {
      pthread_mutex_unlock(&g_mutex);
      break;
    }

    dropped = g_dropped;
    g_dropped = 0;
    has_entry = pop_entry_locked(&entry);
    pthread_mutex_unlock(&g_mutex);

    if (dropped != 0) {
      fprintf(stderr, "log dropped %u message(s)\n", dropped);
    }
    if (has_entry) {
      write_line_direct(entry.line);
    }
  }

  fflush(stderr);
  return NULL;
}

static bool ensure_log_thread_locked(void) {
  if (g_thread_started) {
    return true;
  }

  if (!g_atexit_registered) {
    atexit(log_shutdown);
    g_atexit_registered = true;
  }

  if (pthread_create(&g_thread, NULL, log_thread_main, NULL) != 0) {
    return false;
  }

  g_thread_started = true;
  return true;
}

void log_set_level(log_level_t level) {
  pthread_mutex_lock(&g_mutex);
  g_level = level;
  pthread_mutex_unlock(&g_mutex);
}

void log_shutdown(void) {
  pthread_mutex_lock(&g_mutex);
  if (!g_thread_started) {
    pthread_mutex_unlock(&g_mutex);
    return;
  }

  g_stopping = true;
  pthread_cond_signal(&g_cond);
  pthread_mutex_unlock(&g_mutex);

  pthread_join(g_thread, NULL);

  pthread_mutex_lock(&g_mutex);
  g_thread_started = false;
  g_stopping = false;
  g_head = 0;
  g_tail = 0;
  g_count = 0;
  g_dropped = 0;
  pthread_mutex_unlock(&g_mutex);
}

void log_write(log_level_t level, const char* file, int line, const char* fmt, ...) {
  pthread_mutex_lock(&g_mutex);
  log_level_t current_level = g_level;
  pthread_mutex_unlock(&g_mutex);

  if (level < current_level) {
    return;
  }

  time_t now = time(NULL);
  struct tm tm_now;
  localtime_r(&now, &tm_now);

  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

  char line_buf[LOG_LINE_MAX];
  int prefix_len = snprintf(line_buf,
                            sizeof(line_buf),
                            "%s [%s] %s:%d ",
                            ts,
                            level_name(level),
                            file,
                            line);
  if (prefix_len < 0) {
    return;
  }
  if ((size_t)prefix_len >= sizeof(line_buf)) {
    prefix_len = (int)sizeof(line_buf) - 1;
  }

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line_buf + prefix_len, sizeof(line_buf) - (size_t)prefix_len, fmt, ap);
  va_end(ap);

  pthread_mutex_lock(&g_mutex);
  if (!ensure_log_thread_locked() || g_stopping) {
    pthread_mutex_unlock(&g_mutex);
    write_line_direct(line_buf);
    return;
  }

  push_entry_locked(line_buf, level);
  pthread_mutex_unlock(&g_mutex);
}
