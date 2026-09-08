#include "command_handler.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "calibration_builder.h"
#include "log.h"
#include "pa_pu.h"
#include "template_builder.h"

static bool cmd_is(const char* command, const char* expected) {
  return strcasecmp(command, expected) == 0;
}

static bool cmd_has_name(const char* command, const char* expected) {
  size_t expected_len = strlen(expected);
  return strncasecmp(command, expected, expected_len) == 0 &&
         (command[expected_len] == '\0' || command[expected_len] == ' ');
}

static const char* cmd_args(const char* command) {
  const char* p = strchr(command, ' ');
  if (p == NULL) {
    return "";
  }
  while (*p == ' ') {
    ++p;
  }
  return p;
}

static bool parse_u32_value(const char* text, uint32_t* value) {
  if (text == NULL || *text == '\0' || value == NULL) {
    return false;
  }

  errno = 0;
  char* end = NULL;
  unsigned long parsed = strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) {
    return false;
  }

  *value = (uint32_t)parsed;
  return true;
}

static bool only_spaces_left(const char* text) {
  if (text == NULL) {
    return true;
  }
  while (*text != '\0') {
    if (*text != ' ') {
      return false;
    }
    ++text;
  }
  return true;
}

static bool parse_u64_field(const char* text, uint64_t* value) {
  if (text == NULL || *text == '\0' || value == NULL) {
    return false;
  }

  errno = 0;
  char* end = NULL;
  unsigned long long parsed = strtoull(text, &end, 0);
  if (errno != 0 || end == text || !only_spaces_left(end)) {
    return false;
  }

  *value = (uint64_t)parsed;
  return true;
}

static bool format_local_time(time_t seconds, char* text, size_t text_size) {
  struct tm local_time;
  if (text == NULL || text_size == 0 || localtime_r(&seconds, &local_time) == NULL) {
    return false;
  }

  return strftime(text, text_size, "%Y-%m-%dT%H:%M:%S", &local_time) > 0;
}

static bool parse_datetime_value(const char* text, time_t* seconds) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  int used = 0;

  if (text == NULL || seconds == NULL) {
    return false;
  }

  if (sscanf(text, " %d-%d-%dT%d:%d:%d %n", &year, &month, &day, &hour, &minute, &second, &used) != 6 &&
      sscanf(text, " %d-%d-%d %d:%d:%d %n", &year, &month, &day, &hour, &minute, &second, &used) != 6) {
    return false;
  }

  if (!only_spaces_left(text + used) ||
      year < 1970 ||
      month < 1 || month > 12 ||
      day < 1 || day > 31 ||
      hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 ||
      second < 0 || second > 60) {
    return false;
  }

  struct tm broken_down = {
    .tm_year = year - 1900,
    .tm_mon = month - 1,
    .tm_mday = day,
    .tm_hour = hour,
    .tm_min = minute,
    .tm_sec = second,
    .tm_isdst = -1,
  };

  time_t parsed = mktime(&broken_down);
  if (parsed == (time_t)-1) {
    return false;
  }

  struct tm verified;
  if (localtime_r(&parsed, &verified) == NULL ||
      verified.tm_year != year - 1900 ||
      verified.tm_mon != month - 1 ||
      verified.tm_mday != day ||
      verified.tm_hour != hour ||
      verified.tm_min != minute ||
      verified.tm_sec != second) {
    return false;
  }

  *seconds = parsed;
  return true;
}

static bool parse_set_time_args(const char* args, time_t* seconds, long* nanoseconds) {
  uint64_t value = 0;
  if (args == NULL || seconds == NULL || nanoseconds == NULL) {
    return false;
  }

  while (*args == ' ') {
    ++args;
  }
  if (*args == '\0') {
    return false;
  }

  if (strncasecmp(args, "epoch_ms=", 9) == 0) {
    if (!parse_u64_field(args + 9, &value) || value / 1000u > (uint64_t)LLONG_MAX) {
      return false;
    }
    *seconds = (time_t)(value / 1000u);
    *nanoseconds = (long)((value % 1000u) * 1000000u);
    return true;
  }

  if (strncasecmp(args, "epoch=", 6) == 0) {
    if (!parse_u64_field(args + 6, &value) || value > (uint64_t)LLONG_MAX) {
      return false;
    }
    *seconds = (time_t)value;
    *nanoseconds = 0;
    return true;
  }

  if (parse_u64_field(args, &value)) {
    if (value > (uint64_t)LLONG_MAX) {
      return false;
    }
    *seconds = (time_t)value;
    *nanoseconds = 0;
    return true;
  }

  if (parse_datetime_value(args, seconds)) {
    *nanoseconds = 0;
    return true;
  }

  return false;
}

static bool sleep_ms_for_loop(uint32_t interval_ms) {
  uint32_t remain_ms = interval_ms;
  while (remain_ms > 0) {
    uint32_t chunk_ms = remain_ms > 1000u ? 1000u : remain_ms;
    if (usleep(chunk_ms * 1000u) != 0 && errno == EINTR) {
      return false;
    }
    remain_ms -= chunk_ms;
  }
  return true;
}

typedef struct {
  uint32_t count;
  uint32_t interval_ms;
  bool stop_on_error;
  bool trace;
} static_idle_loop_config_t;

static pa_pu_gic_config_t default_gic_config(void);
static pa_pu_corr_config_t default_corr_config(const fpga_mem_t* fpga_mem);
static pa_pu_img_upload_config_t default_img_upload_config(const fpga_mem_t* fpga_mem);
static bool parse_float_value(const char* text, float* value);

static int apply_runtime_config(command_context_t* ctx) {
  if (ctx == NULL || ctx->runtime_config == NULL) return -1;
  if (!work_mode_allows_hardware_action(ctx->work_mode)) return -2;
  pa_runtime_config_t* config = ctx->runtime_config;
  pa_pu_configure_gic(&config->gic);
  pa_pu_configure_roic(&config->roic);
  pa_pu_configure_correction(&config->corr);
  config->static_idle.bright_corr = config->corr;
  config->static_idle.bright_corr.offset_enable = false;
  config->static_idle.bright_corr.gain_enable = false;
  config->static_idle.bright_corr.defect_enable = false;
  config->static_idle.dark_corr = config->corr;
  config->static_idle.clean_gic = config->gic;
  config->static_idle.clean_gic.dout_enable = false;
  config->static_idle.bright_gic = config->gic;
  config->static_idle.bright_gic.dout_enable = true;
  config->static_idle.dark_gic = config->static_idle.bright_gic;
  dynamic_mode_config_t dynamic_config;
  if (dynamic_mode_default_config(ctx->fpga_mem, &dynamic_config) != 0) return -1;
  dynamic_config.dync = config->dync;
  dynamic_config.start_timeout_ms = config->dynamic_start_timeout_ms;
  dynamic_config.state_poll_interval_ms = config->dynamic_state_poll_interval_ms;
  dynamic_config.stop_timeout_ms = config->dynamic_stop_timeout_ms;
  if (work_mode_update_static_idle_config(ctx->work_mode, &config->static_idle) != 0 ||
      work_mode_update_dynamic_settings(ctx->work_mode, &dynamic_config) != 0) return -1;
  return 0;
}

static int reset_runtime_config_group(command_context_t* ctx, const char* group) {
  if (ctx == NULL || ctx->runtime_config == NULL || group == NULL) return -1;
  pa_runtime_config_t defaults;
  if (config_store_defaults(ctx->fpga_mem, &defaults) != 0) return -1;
  if (strcasecmp(group, "gic") == 0) ctx->runtime_config->gic = defaults.gic;
  else if (strcasecmp(group, "roic") == 0) ctx->runtime_config->roic = defaults.roic;
  else if (strcasecmp(group, "corr") == 0) ctx->runtime_config->corr = defaults.corr;
  else if (strcasecmp(group, "static") == 0 || strcasecmp(group, "static_idle") == 0) ctx->runtime_config->static_idle = defaults.static_idle;
  else if (strcasecmp(group, "dynamic") == 0) {
    ctx->runtime_config->dync = defaults.dync;
    ctx->runtime_config->dynamic_start_timeout_ms = defaults.dynamic_start_timeout_ms;
    ctx->runtime_config->dynamic_state_poll_interval_ms = defaults.dynamic_state_poll_interval_ms;
    ctx->runtime_config->dynamic_stop_timeout_ms = defaults.dynamic_stop_timeout_ms;
  } else if (strcasecmp(group, "template") == 0) {
    snprintf(ctx->runtime_config->offset_file, sizeof(ctx->runtime_config->offset_file), "%s", defaults.offset_file);
    snprintf(ctx->runtime_config->gain_file, sizeof(ctx->runtime_config->gain_file), "%s", defaults.gain_file);
    snprintf(ctx->runtime_config->cal_gain_dir, sizeof(ctx->runtime_config->cal_gain_dir), "%s", defaults.cal_gain_dir);
  } else return -1;
  return 0;
}

/*
 * 解析短格式配置组命令，例如：
 *   SET_CONFIG_GROUP gic req_code=0 line_time_ns=25600
 *   SET_CONFIG_GROUP dynamic cycle=10 step0_h=0x80000004 step0_l=50
 *
 * 正式二进制协议将来可以复用同一组配置项语义；当前 ASCII 入口只负责
 * 把多个短字段合并应用一次，避免上位机逐个寄存器写入。
 */
static int set_runtime_config_group(command_context_t* ctx, const char* args) {
  if (ctx == NULL || ctx->runtime_config == NULL || args == NULL ||
      !work_mode_allows_hardware_action(ctx->work_mode)) {
    return -2;
  }

  char buffer[1024];
  snprintf(buffer, sizeof(buffer), "%s", args);
  char* save = NULL;
  char* token = strtok_r(buffer, " \t", &save);
  if (token == NULL) {
    return -1;
  }

  char group[32];
  if (strncasecmp(token, "group=", 6) == 0) {
    snprintf(group, sizeof(group), "%s", token + 6);
    token = strtok_r(NULL, " \t", &save);
  } else {
    snprintf(group, sizeof(group), "%s", token);
    token = strtok_r(NULL, " \t", &save);
  }
  if (group[0] == '\0' || token == NULL) {
    return -1;
  }
  if (strcasecmp(group, "static_idle") == 0) {
    snprintf(group, sizeof(group), "static");
  }

  pa_runtime_config_t backup = *ctx->runtime_config;
  if (strcasecmp(group, "dynamic") == 0) {
    /* Dynamic 配置组提交的是完整 step 表；未出现在命令中的 step 必须关闭。 */
    memset(ctx->runtime_config->dync.step_cfg_h, 0,
           sizeof(ctx->runtime_config->dync.step_cfg_h));
    memset(ctx->runtime_config->dync.step_cfg_l, 0,
           sizeof(ctx->runtime_config->dync.step_cfg_l));
  }
  unsigned changed = 0u;
  do {
    char* equals = strchr(token, '=');
    if (equals == NULL || equals == token || equals[1] == '\0') {
      *ctx->runtime_config = backup;
      return -1;
    }
    *equals = '\0';
    char item[96];
    snprintf(item, sizeof(item), "%s.%s", group, token);
    if (config_store_set_item(ctx->runtime_config, item, equals + 1) != 0) {
      *ctx->runtime_config = backup;
      return -1;
    }
    ++changed;
    token = strtok_r(NULL, " \t", &save);
  } while (token != NULL);

  if (changed == 0u ||
      (strcasecmp(group, "dynamic") == 0 && ctx->runtime_config->dync.cycle_num == 0u) ||
      apply_runtime_config(ctx) != 0 ||
      config_store_save(ctx->config_file, ctx->runtime_config) != 0) {
    *ctx->runtime_config = backup;
    (void)apply_runtime_config(ctx);
    return -1;
  }
  return 0;
}

typedef struct {
  /* 固定地址裸压测配置：绕过 Static Idle 业务流程，只验证一次三模块联合采图。 */
  uint32_t image_addr;
  bool has_image_addr;
  uint32_t count;
  uint32_t interval_ms;
  uint32_t timeout_ms;
  uint32_t wait_mask;
  bool stop_on_error;
  bool trace;
  /* 是否启动 IMG_WR 向 DDR 写图。false 时不写 IMG_WR_STR_ADDR/IMG_WR_STR，也不等待 IMG_WR_END。 */
  bool write_ddr;
  pa_pu_gic_config_t gic;
  pa_pu_corr_config_t corr;
} capture_addr_loop_config_t;

typedef struct {
  uint32_t levels[CAL_GAIN_MAX_LEVELS];
  uint32_t level_count;
  uint32_t frames;
  float threshold;
} cal_gain_begin_args_t;

typedef struct {
  /* dynamic 模块调试配置；默认使用当前 uio2 图像池作为环形输出范围。 */
  pa_pu_dync_config_t config;
  bool has_config_write;
} dync_command_config_t;

typedef struct {
  uint32_t frames;
  uint32_t valid_frames;
  dync_command_config_t dync;
} dynamic_offset_args_t;

typedef struct {
  pa_pu_img_upload_config_t config;
  bool wait_done;
  const char* source_name;
} img_upload_command_config_t;

static bool parse_static_idle_loop_args(const char* args, static_idle_loop_config_t* config) {
  char buffer[256];
  if (config == NULL) {
    return false;
  }

  config->count = 0;
  config->interval_ms = 5000u;
  config->stop_on_error = true;
  config->trace = false;

  if (args == NULL) {
    return true;
  }
  while (*args == ' ') {
    ++args;
  }
  if (*args == '\0') {
    return true;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "count") == 0 || strcmp(token, "times") == 0) {
      config->count = value;
    } else if (strcmp(token, "interval_ms") == 0 || strcmp(token, "period_ms") == 0) {
      config->interval_ms = value;
    } else if (strcmp(token, "interval_s") == 0 || strcmp(token, "period_s") == 0) {
      if (value > UINT32_MAX / 1000u) {
        return false;
      }
      config->interval_ms = value * 1000u;
    } else if (strcmp(token, "stop_on_error") == 0) {
      config->stop_on_error = value != 0;
    } else if (strcmp(token, "trace") == 0 || strcmp(token, "verbose") == 0) {
      config->trace = value != 0;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }
  return true;
}

static bool parse_level_list(char* text, uint32_t* levels, uint32_t* level_count) {
  if (text == NULL || levels == NULL || level_count == NULL) {
    return false;
  }

  uint32_t count = 0;
  char* item = strtok(text, ",");
  while (item != NULL) {
    if (count >= CAL_GAIN_MAX_LEVELS || !parse_u32_value(item, &levels[count])) {
      return false;
    }
    ++count;
    item = strtok(NULL, ",");
  }

  if (count == 0) {
    return false;
  }
  *level_count = count;
  return true;
}

static bool parse_cal_gain_begin_args(const char* args, cal_gain_begin_args_t* config) {
  char buffer[512];
  if (config == NULL) {
    return false;
  }

  memset(config, 0, sizeof(*config));
  config->frames = 1u;
  config->threshold = 0.3f;

  if (args == NULL || strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* saveptr = NULL;
  char* token = strtok_r(buffer, " ", &saveptr);
  while (token != NULL) {
    char* equals = strchr(token, '=');
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    char* value_text = equals + 1;
    uint32_t value = 0;
    if (strcmp(token, "levels") == 0) {
      if (!parse_level_list(value_text, config->levels, &config->level_count)) {
        return false;
      }
    } else if (strcmp(token, "frames") == 0 || strcmp(token, "frames_per_level") == 0) {
      if (!parse_u32_value(value_text, &value)) {
        return false;
      }
      config->frames = value;
    } else if (strcmp(token, "threshold") == 0 || strcmp(token, "defect_threshold") == 0) {
      if (!parse_float_value(value_text, &config->threshold)) {
        return false;
      }
    } else {
      return false;
    }
    token = strtok_r(NULL, " ", &saveptr);
  }

  return config->level_count >= 1u && config->frames > 0u;
}

static bool parse_float_value(const char* text, float* value) {
  if (text == NULL || *text == '\0' || value == NULL) {
    return false;
  }

  errno = 0;
  char* end = NULL;
  double parsed = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }

  *value = (float)parsed;
  return true;
}

static void default_dync_command_config(const fpga_mem_t* fpga_mem, dync_command_config_t* command_config) {
  if (command_config == NULL) {
    return;
  }

  memset(command_config, 0, sizeof(*command_config));
  dynamic_mode_config_t defaults;
  if (fpga_mem != NULL && dynamic_mode_default_config(fpga_mem, &defaults) == 0) {
    command_config->config = defaults.dync;
    return;
  }

  /* 无有效 UIO 上下文时仍保留 Makefile 默认 cycle/step，供参数解析和错误回包使用。 */
  command_config->config.cycle_num = DYNAMIC_CYCLE_NUM;
  command_config->config.image_start_addr = DYNAMIC_IMG_START_ADDR;
  command_config->config.image_end_addr = DYNAMIC_IMG_END_ADDR;
  const uint32_t default_step_h[PA_PU_DYNC_STEP_COUNT] = {
    DYNAMIC_STEP_0_CFG_H, DYNAMIC_STEP_1_CFG_H, DYNAMIC_STEP_2_CFG_H, DYNAMIC_STEP_3_CFG_H,
    DYNAMIC_STEP_4_CFG_H, DYNAMIC_STEP_5_CFG_H, DYNAMIC_STEP_6_CFG_H, DYNAMIC_STEP_7_CFG_H,
    DYNAMIC_STEP_8_CFG_H, DYNAMIC_STEP_9_CFG_H,
  };
  const uint32_t default_step_l[PA_PU_DYNC_STEP_COUNT] = {
    DYNAMIC_STEP_0_CFG_L, DYNAMIC_STEP_1_CFG_L, DYNAMIC_STEP_2_CFG_L, DYNAMIC_STEP_3_CFG_L,
    DYNAMIC_STEP_4_CFG_L, DYNAMIC_STEP_5_CFG_L, DYNAMIC_STEP_6_CFG_L, DYNAMIC_STEP_7_CFG_L,
    DYNAMIC_STEP_8_CFG_L, DYNAMIC_STEP_9_CFG_L,
  };
  memcpy(command_config->config.step_cfg_h, default_step_h, sizeof(default_step_h));
  memcpy(command_config->config.step_cfg_l, default_step_l, sizeof(default_step_l));
}

static bool parse_dync_step_token(const char* name, const char** suffix_out, unsigned* index_out) {
  const char* text = name;
  unsigned index = 0;

  if (name == NULL || suffix_out == NULL || index_out == NULL) {
    return false;
  }

  if (strncmp(text, "step", 4) == 0) {
    text += 4;
  } else if (strncmp(text, "dync_step_", 10) == 0) {
    text += 10;
  } else {
    return false;
  }

  if (*text < '0' || *text > '9') {
    return false;
  }
  index = (unsigned)(*text - '0');
  ++text;
  if (index >= PA_PU_DYNC_STEP_COUNT || *text != '_') {
    return false;
  }

  *suffix_out = text + 1;
  *index_out = index;
  return true;
}

static bool parse_dync_config_args(const char* args, dync_command_config_t* command_config) {
  char buffer[1024];
  if (command_config == NULL) {
    return false;
  }
  if (args == NULL) {
    return true;
  }
  while (*args == ' ') {
    ++args;
  }
  if (*args == '\0') {
    return true;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    const char* step_suffix = NULL;
    unsigned step_index = 0;
    if (strcmp(token, "cycle") == 0 || strcmp(token, "cycle_num") == 0 || strcmp(token, "dync_cycle_num") == 0) {
      command_config->config.cycle_num = value;
      command_config->has_config_write = true;
    } else if (strcmp(token, "img_start") == 0 ||
               strcmp(token, "image_start") == 0 ||
               strcmp(token, "dync_img_str_addr") == 0) {
      command_config->config.image_start_addr = value;
      command_config->has_config_write = true;
    } else if (strcmp(token, "img_end") == 0 ||
               strcmp(token, "image_end") == 0 ||
               strcmp(token, "dync_img_end_addr") == 0) {
      command_config->config.image_end_addr = value;
      command_config->has_config_write = true;
    } else if (strcmp(token, "wait") == 0 || strcmp(token, "wait_done") == 0) {
      /* 兼容旧调试脚本中的字段，但 START_DYNC 永远按非阻塞方式执行。 */
    } else if (parse_dync_step_token(token, &step_suffix, &step_index)) {
      if (strcmp(step_suffix, "h") == 0 || strcmp(step_suffix, "cfg_h") == 0) {
        command_config->config.step_cfg_h[step_index] = value;
        command_config->has_config_write = true;
      } else if (strcmp(step_suffix, "l") == 0 || strcmp(step_suffix, "cfg_l") == 0 || strcmp(step_suffix, "time") == 0) {
        command_config->config.step_cfg_l[step_index] = value;
        command_config->has_config_write = true;
      } else if (strcmp(step_suffix, "req") == 0 || strcmp(step_suffix, "req_code") == 0) {
        command_config->config.step_cfg_h[step_index] =
            (command_config->config.step_cfg_h[step_index] & ~PA_PU_DYNC_STEP_REQ_MASK) |
            (value & PA_PU_DYNC_STEP_REQ_MASK);
        command_config->has_config_write = true;
      } else if (strcmp(step_suffix, "en") == 0 || strcmp(step_suffix, "enable") == 0) {
        if (value != 0) {
          command_config->config.step_cfg_h[step_index] |= PA_PU_DYNC_STEP_ENABLE_MASK;
        } else {
          command_config->config.step_cfg_h[step_index] &= ~PA_PU_DYNC_STEP_ENABLE_MASK;
        }
        command_config->has_config_write = true;
      } else {
        return false;
      }
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  return true;
}

static bool parse_dynamic_offset_args(const char* args, dynamic_offset_args_t* config) {
  char buffer[1024];
  if (config == NULL) {
    return false;
  }
  if (args == NULL) {
    return false;
  }
  while (*args == ' ') {
    ++args;
  }
  if (*args == '\0' || strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    const char* step_suffix = NULL;
    unsigned step_index = 0;
    if (strcmp(token, "frames") == 0 || strcmp(token, "frame_count") == 0 || strcmp(token, "count") == 0 ||
        strcmp(token, "total_frames") == 0 || strcmp(token, "total") == 0) {
      config->frames = value;
    } else if (strcmp(token, "valid_frames") == 0 ||
               strcmp(token, "effective_frames") == 0 ||
               strcmp(token, "valid") == 0 ||
               strcmp(token, "avg_frames") == 0) {
      config->valid_frames = value;
    } else if (strcmp(token, "cycle") == 0 || strcmp(token, "cycle_num") == 0 || strcmp(token, "dync_cycle_num") == 0) {
      config->dync.config.cycle_num = value;
      config->dync.has_config_write = true;
    } else if (strcmp(token, "img_start") == 0 ||
               strcmp(token, "image_start") == 0 ||
               strcmp(token, "dync_img_str_addr") == 0) {
      config->dync.config.image_start_addr = value;
      config->dync.has_config_write = true;
    } else if (strcmp(token, "img_end") == 0 ||
               strcmp(token, "image_end") == 0 ||
               strcmp(token, "dync_img_end_addr") == 0) {
      config->dync.config.image_end_addr = value;
      config->dync.has_config_write = true;
    } else if (strcmp(token, "wait") == 0 || strcmp(token, "wait_done") == 0) {
      /*
       * MAKE_DYNC_OFFSET 保留该参数以兼容旧调试脚本；后台任务现按静态逐帧完成采集。
       * 接受该字段仅用于兼容早期调试参数，命令本身始终立即返回。
       */
    } else if (parse_dync_step_token(token, &step_suffix, &step_index)) {
      if (strcmp(step_suffix, "h") == 0 || strcmp(step_suffix, "cfg_h") == 0) {
        config->dync.config.step_cfg_h[step_index] = value;
        config->dync.has_config_write = true;
      } else if (strcmp(step_suffix, "l") == 0 || strcmp(step_suffix, "cfg_l") == 0 || strcmp(step_suffix, "time") == 0) {
        config->dync.config.step_cfg_l[step_index] = value;
        config->dync.has_config_write = true;
      } else if (strcmp(step_suffix, "req") == 0 || strcmp(step_suffix, "req_code") == 0) {
        config->dync.config.step_cfg_h[step_index] =
            (config->dync.config.step_cfg_h[step_index] & ~PA_PU_DYNC_STEP_REQ_MASK) |
            (value & PA_PU_DYNC_STEP_REQ_MASK);
        config->dync.has_config_write = true;
      } else if (strcmp(step_suffix, "en") == 0 || strcmp(step_suffix, "enable") == 0) {
        if (value != 0) {
          config->dync.config.step_cfg_h[step_index] |= PA_PU_DYNC_STEP_ENABLE_MASK;
        } else {
          config->dync.config.step_cfg_h[step_index] &= ~PA_PU_DYNC_STEP_ENABLE_MASK;
        }
        config->dync.has_config_write = true;
      } else {
        return false;
      }
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  return config->frames > 0u && config->valid_frames > 0u && config->valid_frames <= config->frames;
}

static void default_capture_addr_loop_config(const fpga_mem_t* fpga_mem, capture_addr_loop_config_t* config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->count = 1u;
  config->timeout_ms = PA_PU_IRQ_TIMEOUT_MS;
  config->wait_mask = PA_PU_IRQ_IMG_CORR_END | PA_PU_IRQ_IMG_WR_END | PA_PU_IRQ_GIC_END;
  config->stop_on_error = true;
  config->write_ddr = true;
  config->gic = default_gic_config();
  config->corr = default_corr_config(fpga_mem);

  /*
   * 这个命令用于隔离 DDR/IMG_WR/GIC 联合稳定性，默认打开 GIC 数据输出，
   * 默认关闭所有校正项，避免 offset/gain 模板内容干扰基础压测结论。
   */
  config->gic.dout_enable = true;
  config->corr.offset_enable = false;
  config->corr.gain_enable = false;
  config->corr.defect_enable = false;
}

static bool parse_capture_addr_loop_args(const char* args, capture_addr_loop_config_t* config) {
  char buffer[1024];
  if (config == NULL) {
    return false;
  }
  if (args == NULL) {
    return true;
  }
  while (*args == ' ') {
    ++args;
  }
  if (*args == '\0') {
    return true;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "addr") == 0 ||
        strcmp(token, "image_addr") == 0 ||
        strcmp(token, "img_wr_str_addr") == 0) {
      config->image_addr = value;
      config->has_image_addr = true;
    } else if (strcmp(token, "count") == 0 || strcmp(token, "times") == 0) {
      config->count = value;
    } else if (strcmp(token, "interval_ms") == 0 || strcmp(token, "period_ms") == 0) {
      config->interval_ms = value;
    } else if (strcmp(token, "interval_s") == 0 || strcmp(token, "period_s") == 0) {
      if (value > UINT32_MAX / 1000u) {
        return false;
      }
      config->interval_ms = value * 1000u;
    } else if (strcmp(token, "timeout_ms") == 0) {
      config->timeout_ms = value;
    } else if (strcmp(token, "wait_mask") == 0 || strcmp(token, "irq_mask") == 0) {
      config->wait_mask = value;
    } else if (strcmp(token, "stop_on_error") == 0) {
      config->stop_on_error = value != 0;
    } else if (strcmp(token, "trace") == 0 || strcmp(token, "verbose") == 0) {
      config->trace = value != 0;
    } else if (strcmp(token, "req") == 0 || strcmp(token, "gic_req_code") == 0) {
      config->gic.req_code = (uint8_t)value;
    } else if (strcmp(token, "dout") == 0 || strcmp(token, "gic_dout_en") == 0) {
      config->gic.dout_enable = value != 0;
    } else if (strcmp(token, "line_time") == 0 || strcmp(token, "gic_line_time") == 0) {
      config->gic.line_time_ns = value;
    } else if (strcmp(token, "oe_rise") == 0 || strcmp(token, "gic_oe_raising_edge") == 0) {
      config->gic.oe_raising_edge_ns = value;
    } else if (strcmp(token, "oe_fall") == 0 || strcmp(token, "gic_oe_falling_edge") == 0) {
      config->gic.oe_falling_edge_ns = value;
    } else if (strcmp(token, "start_row") == 0 || strcmp(token, "gic_str_row_num") == 0) {
      config->gic.start_row = (uint16_t)value;
    } else if (strcmp(token, "end_row") == 0 || strcmp(token, "gic_end_row_num") == 0) {
      config->gic.end_row = (uint16_t)value;
    } else if (strcmp(token, "binning") == 0 || strcmp(token, "gic_binning_mode") == 0) {
      config->gic.binning_mode = (uint8_t)value;
    } else if (strcmp(token, "pkg") == 0 || strcmp(token, "pkg_num") == 0 || strcmp(token, "img_pkg_num") == 0) {
      config->corr.pkg_num = (uint16_t)value;
    } else if (strcmp(token, "row") == 0 || strcmp(token, "row_num") == 0 || strcmp(token, "img_row_num") == 0) {
      config->corr.row_num = (uint16_t)value;
    } else if (strcmp(token, "col") == 0 || strcmp(token, "col_num") == 0 || strcmp(token, "img_col_num") == 0) {
      config->corr.col_num = (uint16_t)value;
    } else if (strcmp(token, "offset_en") == 0 || strcmp(token, "offset_enable") == 0 || strcmp(token, "img_corr_offset_en") == 0) {
      config->corr.offset_enable = value != 0;
    } else if (strcmp(token, "offset_addr") == 0 || strcmp(token, "offset_template_addr") == 0 || strcmp(token, "img_corr_offset_temp_str_addr") == 0) {
      config->corr.offset_template_addr = value;
    } else if (strcmp(token, "offset_adder") == 0 || strcmp(token, "offset_adder_value") == 0 || strcmp(token, "img_corr_offset_adder_value") == 0) {
      config->corr.offset_adder_value = (uint16_t)value;
    } else if (strcmp(token, "gain_en") == 0 || strcmp(token, "gain_enable") == 0 || strcmp(token, "img_corr_gain_en") == 0) {
      config->corr.gain_enable = value != 0;
    } else if (strcmp(token, "gain_addr") == 0 || strcmp(token, "gain_template_addr") == 0 || strcmp(token, "img_corr_gain_temp_str_addr") == 0) {
      config->corr.gain_template_addr = value;
    } else if (strcmp(token, "gain_clip") == 0 || strcmp(token, "gain_clipping_value") == 0 || strcmp(token, "img_corr_gain_clipping_value") == 0) {
      config->corr.gain_clipping_value = (uint16_t)value;
    } else if (strcmp(token, "defect_en") == 0 || strcmp(token, "defect_enable") == 0 || strcmp(token, "img_corr_defect_en") == 0) {
      config->corr.defect_enable = value != 0;
    } else if (strcmp(token, "no_write") == 0 || strcmp(token, "skip_wr") == 0) {
      /* no_write=1/skip_wr=1 表示这一轮不启动 IMG_WR，也不等待 IMG_WR_END。 */
      config->write_ddr = value == 0;
    } else if (strcmp(token, "write_ddr") == 0) {
      /* write_ddr=1 显式要求写 DDR（默认行为），write_ddr=0 等价 no_write=1。 */
      config->write_ddr = value != 0;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  /*
   * 参数顺序无关收尾：不写 DDR 时从等待掩码里去掉 IMG_WR_END，
   * 避免等待一个永远不会到来的完成中断。
   */
  if (!config->write_ddr) {
    config->wait_mask &= ~PA_PU_IRQ_IMG_WR_END;
  }

  return true;
}

static void write_time_response(char* response, size_t response_size, const char* command_name) {
  struct timespec now;
  char local_text[32] = "unavailable";
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    snprintf(response, response_size, "ERR %s errno=%d\r\n", command_name, errno);
    return;
  }

  (void)format_local_time(now.tv_sec, local_text, sizeof(local_text));
  uint64_t epoch_ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
  snprintf(response, response_size,
           "OK %s epoch=%lld epoch_ms=%llu local=%s\r\n",
           command_name,
           (long long)now.tv_sec,
           (unsigned long long)epoch_ms,
           local_text);
}

typedef struct {
  const char* name;
  uint16_t reg;
} register_name_t;

static const register_name_t k_register_names[] = {
  {"int_vector", PA_PU_INT_VECTOR_REG},
  {"pa_version", PA_PU_PA_VERSION_REG},
  {"pa_build_information", PA_PU_PA_BUILD_INFORMATION_REG},
  {"adapted_main_board_version", PA_PU_ADAPTED_MAIN_BOARD_VERSION_REG},
  {"adapted_gic_board_version", PA_PU_ADAPTED_GIC_BOARD_VERSION_REG},
  {"adapted_roic_board_version", PA_PU_ADAPTED_ROIC_BOARD_VERSION_REG},
  {"adapted_reserved_board_0_version", PA_PU_ADAPTED_RESERVED_BOARD_0_VERSION_REG},
  {"adapted_reserved_board_1_version", PA_PU_ADAPTED_RESERVED_BOARD_1_VERSION_REG},
  {"adapted_reserved_board_2_version", PA_PU_ADAPTED_RESERVED_BOARD_2_VERSION_REG},
  {"pa_pu_com_version", PA_PU_COM_VERSION_REG},
  {"com_version", PA_PU_COM_VERSION_REG},
  {"pa_rst_init_state", PA_PU_RST_INIT_STATE_REG},
  {"rst_state", PA_PU_RST_INIT_STATE_REG},
  {"gic_str", PA_PU_GIC_STR_REG},
  {"gic_stop", PA_PU_GIC_STOP_REG},
  {"gic_req_code", PA_PU_GIC_REQ_CODE_REG},
  {"gic_dout_en", PA_PU_GIC_DOUT_EN_REG},
  {"gic_line_time", PA_PU_GIC_LINE_TIME_REG},
  {"gic_oe_raising_edge", PA_PU_GIC_OE_RAISING_EDGE_REG},
  {"gic_oe_falling_edge", PA_PU_GIC_OE_FALLING_EDGE_REG},
  {"gic_str_row_num", PA_PU_GIC_STR_ROW_NUM_REG},
  {"gic_end_row_num", PA_PU_GIC_END_ROW_NUM_REG},
  {"gic_binning_mode", PA_PU_GIC_BINNING_MODE_REG},
  {"gic_state", PA_PU_GIC_STATE_REG},
  {"gic_end", PA_PU_GIC_END_REG},
  {"gic_dfx", PA_PU_GIC_DFX_REG},
  {"gic_debug_in", PA_PU_GIC_DEBUG_IN_REG},
  {"gic_debug_out", PA_PU_GIC_DEBUG_OUT_REG},
  {"roic_str", PA_PU_ROIC_STR_REG},
  {"roic_req_code", PA_PU_ROIC_REQ_CODE_REG},
  {"roic_reg_00", PA_PU_ROIC_REG_00_REG},
  {"roic_reg_02", PA_PU_ROIC_REG_02_REG},
  {"roic_reg_05", PA_PU_ROIC_REG_05_REG},
  {"roic_reg_06", PA_PU_ROIC_REG_06_REG},
  {"roic_reg_07", PA_PU_ROIC_REG_07_REG},
  {"roic_reg_09", PA_PU_ROIC_REG_09_REG},
  {"roic_reg_0a", PA_PU_ROIC_REG_0A_REG},
  {"roic_reg_0b", PA_PU_ROIC_REG_0B_REG},
  {"roic_reg_0c", PA_PU_ROIC_REG_0C_REG},
  {"roic_reg_0d", PA_PU_ROIC_REG_0D_REG},
  {"roic_reg_0e", PA_PU_ROIC_REG_0E_REG},
  {"roic_reg_0f", PA_PU_ROIC_REG_0F_REG},
  {"roic_reg_10", PA_PU_ROIC_REG_10_REG},
  {"roic_reg_11", PA_PU_ROIC_REG_11_REG},
  {"roic_reg_17", PA_PU_ROIC_REG_17_REG},
  {"roic_reg_24", PA_PU_ROIC_REG_24_REG},
  {"roic_reg_28", PA_PU_ROIC_REG_28_REG},
  {"roic_reg_2d", PA_PU_ROIC_REG_2D_REG},
  {"roic_reg_3b", PA_PU_ROIC_REG_3B_REG},
  {"roic_str_col_num", PA_PU_ROIC_STR_COL_NUM_REG},
  {"roic_end_col_num", PA_PU_ROIC_END_COL_NUM_REG},
  {"roic_binning_mode", PA_PU_ROIC_BINNING_MODE_REG},
  {"roic_state", PA_PU_ROIC_STATE_REG},
  {"roic_end", PA_PU_ROIC_END_REG},
  {"roic_dfx", PA_PU_ROIC_DFX_REG},
  {"roic_debug_in", PA_PU_ROIC_DEBUG_IN_REG},
  {"roic_debug_out", PA_PU_ROIC_DEBUG_OUT_REG},
  {"img_wr_str", PA_PU_IMG_WR_STR_REG},
  {"img_wr_str_addr", PA_PU_IMG_WR_STR_ADDR_REG},
  {"img_wr_state", PA_PU_IMG_WR_STATE_REG},
  {"img_wr_end", PA_PU_IMG_WR_END_REG},
  {"img_wr_final_img_addr", PA_PU_IMG_WR_FINAL_IMG_ADDR_REG},
  {"final_img_addr", PA_PU_IMG_WR_FINAL_IMG_ADDR_REG},
  {"img_wr_dfx", PA_PU_IMG_WR_DFX_REG},
  {"img_wr_debug_in", PA_PU_IMG_WR_DEBUG_IN_REG},
  {"img_wr_debug_out", PA_PU_IMG_WR_DEBUG_OUT_REG},
  {"img_corr_str", PA_PU_IMG_CORR_STR_REG},
  {"img_pkg_num", PA_PU_IMG_PKG_NUM_REG},
  {"img_row_num", PA_PU_IMG_ROW_NUM_REG},
  {"img_col_num", PA_PU_IMG_COL_NUM_REG},
  {"img_corr_offset_en", PA_PU_IMG_CORR_OFFSET_EN_REG},
  {"img_corr_offset_temp_str_addr", PA_PU_IMG_CORR_OFFSET_TEMP_STR_ADDR_REG},
  {"img_corr_offset_adder_value", PA_PU_IMG_CORR_OFFSET_ADDER_VALUE_REG},
  {"img_corr_gain_en", PA_PU_IMG_CORR_GAIN_EN_REG},
  {"img_corr_gain_temp_str_addr", PA_PU_IMG_CORR_GAIN_TEMP_STR_ADDR_REG},
  {"img_corr_gain_clipping_value", PA_PU_IMG_CORR_GAIN_CLIPPING_VALUE_REG},
  {"img_corr_defect_en", PA_PU_IMG_CORR_DEFECT_EN_REG},
  {"img_offset_corr_mode", PA_PU_IMG_OFFSET_CORR_MODE_REG},
  {"img_corr_state", PA_PU_IMG_CORR_STATE_REG},
  {"img_corr_end", PA_PU_IMG_CORR_END_REG},
  {"img_corr_dfx", PA_PU_IMG_CORR_DFX_REG},
  {"img_corr_debug_in", PA_PU_IMG_CORR_DEBUG_IN_REG},
  {"img_corr_debug_out", PA_PU_IMG_CORR_DEBUG_OUT_REG},
  {"dync_str", PA_PU_DYNC_STR_REG},
  {"dynamic_str", PA_PU_DYNC_STR_REG},
  {"dync_stop", PA_PU_DYNC_STOP_REG},
  {"dynamic_stop", PA_PU_DYNC_STOP_REG},
  {"dync_cycle_num", PA_PU_DYNC_CYCLE_NUM_REG},
  {"dync_img_str_addr", PA_PU_DYNC_IMG_STR_ADDR_REG},
  {"dync_img_end_addr", PA_PU_DYNC_IMG_END_ADDR_REG},
  {"dync_step_0_cfg_h", PA_PU_DYNC_STEP_0_CFG_H_REG},
  {"dync_step_0_cfg_l", PA_PU_DYNC_STEP_0_CFG_L_REG},
  {"dync_step_1_cfg_h", PA_PU_DYNC_STEP_1_CFG_H_REG},
  {"dync_step_1_cfg_l", PA_PU_DYNC_STEP_1_CFG_L_REG},
  {"dync_step_2_cfg_h", PA_PU_DYNC_STEP_2_CFG_H_REG},
  {"dync_step_2_cfg_l", PA_PU_DYNC_STEP_2_CFG_L_REG},
  {"dync_step_3_cfg_h", PA_PU_DYNC_STEP_3_CFG_H_REG},
  {"dync_step_3_cfg_l", PA_PU_DYNC_STEP_3_CFG_L_REG},
  {"dync_step_4_cfg_h", PA_PU_DYNC_STEP_4_CFG_H_REG},
  {"dync_step_4_cfg_l", PA_PU_DYNC_STEP_4_CFG_L_REG},
  {"dync_step_5_cfg_h", PA_PU_DYNC_STEP_5_CFG_H_REG},
  {"dync_step_5_cfg_l", PA_PU_DYNC_STEP_5_CFG_L_REG},
  {"dync_step_6_cfg_h", PA_PU_DYNC_STEP_6_CFG_H_REG},
  {"dync_step_6_cfg_l", PA_PU_DYNC_STEP_6_CFG_L_REG},
  {"dync_step_7_cfg_h", PA_PU_DYNC_STEP_7_CFG_H_REG},
  {"dync_step_7_cfg_l", PA_PU_DYNC_STEP_7_CFG_L_REG},
  {"dync_step_8_cfg_h", PA_PU_DYNC_STEP_8_CFG_H_REG},
  {"dync_step_8_cfg_l", PA_PU_DYNC_STEP_8_CFG_L_REG},
  {"dync_step_9_cfg_h", PA_PU_DYNC_STEP_9_CFG_H_REG},
  {"dync_step_9_cfg_l", PA_PU_DYNC_STEP_9_CFG_L_REG},
  {"dync_end", PA_PU_DYNC_END_REG},
  {"dync_state", PA_PU_DYNC_STATE_REG},
  {"dync_debug_in", PA_PU_DYNC_DEBUG_IN_REG},
  {"dync_debug_out", PA_PU_DYNC_DEBUG_OUT_REG},
  {"img_upload_str", PA_PU_IMG_UPLOAD_STR_REG},
  {"img_upload_str_addr", PA_PU_IMG_UPLOAD_STR_ADDR_REG},
  {"img_upload_addr", PA_PU_IMG_UPLOAD_STR_ADDR_REG},
  {"img_upload_pkg_num", PA_PU_IMG_UPLOAD_PKG_NUM_REG},
  {"img_upload_row_num", PA_PU_IMG_UPLOAD_ROW_NUM_REG},
  {"img_upload_col_num", PA_PU_IMG_UPLOAD_COL_NUM_REG},
  {"img_upload_state", PA_PU_IMG_UPLOAD_STATE_REG},
  {"img_upload_end", PA_PU_IMG_UPLOAD_END_REG},
  {"img_upload_dfx", PA_PU_IMG_UPLOAD_DFX_REG},
};

static bool lookup_register_name(const char* name, uint16_t* reg) {
  for (size_t i = 0; i < sizeof(k_register_names) / sizeof(k_register_names[0]); ++i) {
    if (strcasecmp(name, k_register_names[i].name) == 0) {
      *reg = k_register_names[i].reg;
      return true;
    }
  }
  return false;
}

static bool parse_register_ref(const char* text, uint16_t* reg) {
  uint32_t value = 0;
  if (text == NULL || *text == '\0' || reg == NULL) {
    return false;
  }

  if (lookup_register_name(text, reg)) {
    return true;
  }

  if (!parse_u32_value(text, &value)) {
    return false;
  }

  if (value >= PA_PU_BASE_ADDR && value < PA_PU_BASE_ADDR + PA_PU_MAP_SIZE) {
    value -= PA_PU_BASE_ADDR;
  }

  if (value > UINT16_MAX || value + sizeof(uint32_t) > PA_PU_MAP_SIZE) {
    return false;
  }

  *reg = (uint16_t)value;
  return true;
}

static bool get_arg_token(const char** args, char* token, size_t token_size) {
  const char* p = NULL;
  size_t len = 0;
  if (args == NULL || *args == NULL || token == NULL || token_size == 0) {
    return false;
  }

  p = *args;
  while (*p == ' ') {
    ++p;
  }
  if (*p == '\0') {
    *args = p;
    return false;
  }

  while (p[len] != '\0' && p[len] != ' ') {
    ++len;
  }
  if (len >= token_size) {
    return false;
  }

  memcpy(token, p, len);
  token[len] = '\0';
  *args = p + len;
  return true;
}

static bool is_read_reg_command(const char* command) {
  return cmd_has_name(command, "READ_REG") || cmd_has_name(command, "REG_READ");
}

static bool is_write_reg_command(const char* command) {
  return cmd_has_name(command, "WRITE_REG") || cmd_has_name(command, "REG_WRITE");
}

static bool parse_gic_config_args(const char* args, pa_pu_gic_config_t* config) {
  char buffer[256];
  if (args == NULL || config == NULL) {
    return false;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "req") == 0 || strcmp(token, "gic_req_code") == 0) {
      config->req_code = (uint8_t)value;
    } else if (strcmp(token, "dout") == 0 || strcmp(token, "gic_dout_en") == 0) {
      config->dout_enable = value != 0;
    } else if (strcmp(token, "line_time") == 0 || strcmp(token, "gic_line_time") == 0) {
      config->line_time_ns = value;
    } else if (strcmp(token, "oe_rise") == 0 || strcmp(token, "gic_oe_raising_edge") == 0) {
      config->oe_raising_edge_ns = value;
    } else if (strcmp(token, "oe_fall") == 0 || strcmp(token, "gic_oe_falling_edge") == 0) {
      config->oe_falling_edge_ns = value;
    } else if (strcmp(token, "start_row") == 0 || strcmp(token, "gic_str_row_num") == 0) {
      config->start_row = (uint16_t)value;
    } else if (strcmp(token, "end_row") == 0 || strcmp(token, "gic_end_row_num") == 0) {
      config->end_row = (uint16_t)value;
    } else if (strcmp(token, "binning") == 0 || strcmp(token, "gic_binning_mode") == 0) {
      config->binning_mode = (uint8_t)value;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  return true;
}

static pa_pu_corr_config_t default_corr_config(const fpga_mem_t* fpga_mem) {
  pa_pu_corr_config_t config = {
    .pkg_num = CORR_DEFAULT_PKG_NUM,
    .row_num = CORR_DEFAULT_ROW_NUM,
    .col_num = CORR_DEFAULT_COL_NUM,
    .offset_enable = CORR_DEFAULT_OFFSET_EN != 0,
    .offset_template_addr = fpga_mem != NULL ? fpga_mem->offset_phys_base : CORR_DEFAULT_OFFSET_ADDR,
    .offset_adder_value = CORR_DEFAULT_OFFSET_ADDER_VALUE,
    .offset_corr_mode = CORR_DEFAULT_OFFSET_CORR_MODE,
    .gain_enable = CORR_DEFAULT_GAIN_EN != 0,
    .gain_template_addr = fpga_mem != NULL ? fpga_mem->gain_phys_base : CORR_DEFAULT_GAIN_ADDR,
    .gain_clipping_value = CORR_DEFAULT_GAIN_CLIPPING_VALUE,
    .defect_enable = CORR_DEFAULT_DEFECT_EN != 0,
  };
  return config;
}

static bool parse_corr_config_args(const char* args, pa_pu_corr_config_t* config) {
  char buffer[384];
  if (args == NULL || config == NULL) {
    return false;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "pkg") == 0 || strcmp(token, "pkg_num") == 0 || strcmp(token, "img_pkg_num") == 0) {
      config->pkg_num = (uint16_t)value;
    } else if (strcmp(token, "row") == 0 || strcmp(token, "row_num") == 0 || strcmp(token, "img_row_num") == 0) {
      config->row_num = (uint16_t)value;
    } else if (strcmp(token, "col") == 0 || strcmp(token, "col_num") == 0 || strcmp(token, "img_col_num") == 0) {
      config->col_num = (uint16_t)value;
    } else if (strcmp(token, "offset_en") == 0 || strcmp(token, "offset_enable") == 0 || strcmp(token, "img_corr_offset_en") == 0) {
      config->offset_enable = value != 0;
    } else if (strcmp(token, "offset_addr") == 0 || strcmp(token, "offset_template_addr") == 0 || strcmp(token, "img_corr_offset_temp_str_addr") == 0) {
      config->offset_template_addr = value;
    } else if (strcmp(token, "offset_adder") == 0 || strcmp(token, "offset_adder_value") == 0 || strcmp(token, "img_corr_offset_adder_value") == 0) {
      config->offset_adder_value = (uint16_t)value;
    } else if (strcmp(token, "offset_mode") == 0 ||
               strcmp(token, "offset_corr_mode") == 0 ||
               strcmp(token, "img_offset_corr_mode") == 0) {
      config->offset_corr_mode = (uint8_t)value;
    } else if (strcmp(token, "gain_en") == 0 || strcmp(token, "gain_enable") == 0 || strcmp(token, "img_corr_gain_en") == 0) {
      config->gain_enable = value != 0;
    } else if (strcmp(token, "gain_addr") == 0 || strcmp(token, "gain_template_addr") == 0 || strcmp(token, "img_corr_gain_temp_str_addr") == 0) {
      config->gain_template_addr = value;
    } else if (strcmp(token, "gain_clip") == 0 || strcmp(token, "gain_clipping_value") == 0 || strcmp(token, "img_corr_gain_clipping_value") == 0) {
      config->gain_clipping_value = (uint16_t)value;
    } else if (strcmp(token, "defect_en") == 0 || strcmp(token, "defect_enable") == 0 || strcmp(token, "img_corr_defect_en") == 0) {
      config->defect_enable = value != 0;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  return true;
}

static pa_pu_img_upload_config_t default_img_upload_config(const fpga_mem_t* fpga_mem) {
  uint32_t default_addr = IMG_UPLOAD_DEFAULT_ADDR;
  if (default_addr == 0u && fpga_mem != NULL) {
    /* IMG_UPLOAD 只用于模板回传，默认选择 uio0 中的 offset 模板。 */
    default_addr = fpga_mem->offset_phys_base;
  }

  pa_pu_img_upload_config_t config = {
    .image_addr = default_addr,
    .pkg_num = IMG_UPLOAD_DEFAULT_PKG_NUM,
    .row_num = IMG_UPLOAD_DEFAULT_ROW_NUM,
    .col_num = IMG_UPLOAD_DEFAULT_COL_NUM,
  };
  return config;
}

static bool parse_img_upload_args(const char* args, img_upload_command_config_t* command_config) {
  char buffer[256];
  bool pkg_set = false;
  if (command_config == NULL || args == NULL) {
    return false;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    const char* value_text = equals + 1;
    if (strcmp(token, "template") == 0 || strcmp(token, "source") == 0) {
      if (strcmp(value_text, "offset") == 0) {
        if (command_config->config.image_addr == 0u) {
          return false;
        }
        command_config->source_name = "offset";
      } else if (strcmp(value_text, "gain") == 0) {
        /* gain 地址在调用者初始化默认配置后由外层根据 source_name 替换。 */
        command_config->source_name = "gain";
      } else {
        return false;
      }
      token = strtok(NULL, " ");
      continue;
    }

    uint32_t value = 0;
    if (!parse_u32_value(value_text, &value)) {
      return false;
    }

    if (strcmp(token, "addr") == 0 ||
        strcmp(token, "image_addr") == 0 ||
        strcmp(token, "img_addr") == 0 ||
        strcmp(token, "img_upload_str_addr") == 0 ||
        strcmp(token, "img_upload_addr") == 0) {
      command_config->config.image_addr = value;
      command_config->source_name = "custom";
    } else if (strcmp(token, "pkg") == 0 ||
               strcmp(token, "pkg_num") == 0 ||
               strcmp(token, "img_upload_pkg_num") == 0) {
      command_config->config.pkg_num = (uint16_t)value;
      pkg_set = true;
    } else if (strcmp(token, "row") == 0 ||
               strcmp(token, "row_num") == 0 ||
               strcmp(token, "img_upload_row_num") == 0) {
      command_config->config.row_num = (uint16_t)value;
    } else if (strcmp(token, "col") == 0 ||
               strcmp(token, "col_num") == 0 ||
               strcmp(token, "img_upload_col_num") == 0) {
      command_config->config.col_num = (uint16_t)value;
    } else if (strcmp(token, "wait") == 0 || strcmp(token, "wait_done") == 0) {
      command_config->wait_done = value != 0;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  /*
   * pkg 未显式覆盖时保持默认值。若调用者改了 row/col 且希望重新按尺寸计算，
   * 可以直接不传 pkg；这里按当前 config 的 row/col 刷新一次默认分包数。
   */
  if (!pkg_set) {
    uint32_t pkg_num = ((uint32_t)command_config->config.row_num * command_config->config.col_num * 2u) / 1024u;
    if (pkg_num > UINT16_MAX) {
      return false;
    }
    command_config->config.pkg_num = (uint16_t)pkg_num;
  }

  return command_config->config.image_addr != 0u &&
         command_config->config.pkg_num != 0u &&
         command_config->config.row_num != 0u &&
         command_config->config.col_num != 0u;
}

static bool resolve_img_upload_template(const fpga_mem_t* fpga_mem,
                                        img_upload_command_config_t* command_config) {
  if (command_config == NULL || command_config->source_name == NULL) {
    return false;
  }
  if (strcmp(command_config->source_name, "custom") == 0) {
    return command_config->config.image_addr != 0u;
  }
  if (fpga_mem == NULL) {
    return false;
  }
  if (strcmp(command_config->source_name, "offset") == 0) {
    command_config->config.image_addr = fpga_mem->offset_phys_base;
  } else if (strcmp(command_config->source_name, "gain") == 0) {
    command_config->config.image_addr = fpga_mem->gain_phys_base;
  } else {
    return false;
  }
  return command_config->config.image_addr != 0u;
}

static pa_pu_gic_config_t default_gic_config(void) {
  pa_pu_gic_config_t config = {
    .req_code = GIC_DEFAULT_REQ_CODE,
    .dout_enable = GIC_DEFAULT_DOUT_EN != 0,
    .line_time_ns = GIC_DEFAULT_LINE_TIME_NS,
    .oe_raising_edge_ns = GIC_DEFAULT_OE_RISE_NS,
    .oe_falling_edge_ns = GIC_DEFAULT_OE_FALL_NS,
    .start_row = GIC_DEFAULT_START_ROW,
    .end_row = GIC_DEFAULT_END_ROW,
    .binning_mode = GIC_DEFAULT_BINNING,
  };
  return config;
}

static pa_pu_roic_config_t default_roic_config(void) {
  pa_pu_roic_config_t config = {
    .reg_00 = ROIC_DEFAULT_REG_00,
    .reg_02 = ROIC_DEFAULT_REG_02,
    .reg_05 = ROIC_DEFAULT_REG_05,
    .reg_06 = ROIC_DEFAULT_REG_06,
    .reg_07 = ROIC_DEFAULT_REG_07,
    .reg_09 = ROIC_DEFAULT_REG_09,
    .reg_0a = ROIC_DEFAULT_REG_0A,
    .reg_0b = ROIC_DEFAULT_REG_0B,
    .reg_0c = ROIC_DEFAULT_REG_0C,
    .reg_0d = ROIC_DEFAULT_REG_0D,
    .reg_0e = ROIC_DEFAULT_REG_0E,
    .reg_0f = ROIC_DEFAULT_REG_0F,
    .reg_10 = ROIC_DEFAULT_REG_10,
    .reg_11 = ROIC_DEFAULT_REG_11,
    .reg_17 = ROIC_DEFAULT_REG_17,
    .reg_24 = ROIC_DEFAULT_REG_24,
    .reg_28 = ROIC_DEFAULT_REG_28,
    .reg_2d = ROIC_DEFAULT_REG_2D,
    .reg_3b = ROIC_DEFAULT_REG_3B,
    .start_col = ROIC_DEFAULT_START_COL,
    .end_col = ROIC_DEFAULT_END_COL,
    .binning_mode = ROIC_DEFAULT_BINNING,
  };
  return config;
}

static bool parse_roic_config_args(const char* args, pa_pu_roic_config_t* config) {
  char buffer[512];
  if (args == NULL || config == NULL) {
    return false;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "reg_00") == 0 || strcmp(token, "roic_reg_00") == 0) {
      config->reg_00 = (uint16_t)value;
    } else if (strcmp(token, "reg_02") == 0 || strcmp(token, "roic_reg_02") == 0) {
      config->reg_02 = (uint16_t)value;
    } else if (strcmp(token, "reg_05") == 0 || strcmp(token, "roic_reg_05") == 0) {
      config->reg_05 = (uint16_t)value;
    } else if (strcmp(token, "reg_06") == 0 || strcmp(token, "roic_reg_06") == 0) {
      config->reg_06 = (uint16_t)value;
    } else if (strcmp(token, "reg_07") == 0 || strcmp(token, "roic_reg_07") == 0) {
      config->reg_07 = (uint16_t)value;
    } else if (strcmp(token, "reg_09") == 0 || strcmp(token, "roic_reg_09") == 0) {
      config->reg_09 = (uint16_t)value;
    } else if (strcmp(token, "reg_0a") == 0 || strcmp(token, "roic_reg_0a") == 0) {
      config->reg_0a = (uint16_t)value;
    } else if (strcmp(token, "reg_0b") == 0 || strcmp(token, "roic_reg_0b") == 0) {
      config->reg_0b = (uint16_t)value;
    } else if (strcmp(token, "reg_0c") == 0 || strcmp(token, "roic_reg_0c") == 0) {
      config->reg_0c = (uint16_t)value;
    } else if (strcmp(token, "reg_0d") == 0 || strcmp(token, "roic_reg_0d") == 0) {
      config->reg_0d = (uint16_t)value;
    } else if (strcmp(token, "reg_0e") == 0 || strcmp(token, "roic_reg_0e") == 0) {
      config->reg_0e = (uint16_t)value;
    } else if (strcmp(token, "reg_0f") == 0 || strcmp(token, "roic_reg_0f") == 0) {
      config->reg_0f = (uint16_t)value;
    } else if (strcmp(token, "reg_10") == 0 || strcmp(token, "roic_reg_10") == 0) {
      config->reg_10 = (uint16_t)value;
    } else if (strcmp(token, "reg_11") == 0 || strcmp(token, "roic_reg_11") == 0) {
      config->reg_11 = (uint16_t)value;
    } else if (strcmp(token, "reg_17") == 0 || strcmp(token, "roic_reg_17") == 0) {
      config->reg_17 = (uint16_t)value;
    } else if (strcmp(token, "reg_24") == 0 || strcmp(token, "roic_reg_24") == 0) {
      config->reg_24 = (uint16_t)value;
    } else if (strcmp(token, "reg_28") == 0 || strcmp(token, "roic_reg_28") == 0) {
      config->reg_28 = (uint16_t)value;
    } else if (strcmp(token, "reg_2d") == 0 || strcmp(token, "roic_reg_2d") == 0) {
      config->reg_2d = (uint16_t)value;
    } else if (strcmp(token, "reg_3b") == 0 || strcmp(token, "roic_reg_3b") == 0) {
      config->reg_3b = (uint16_t)value;
    } else if (strcmp(token, "start_col") == 0 || strcmp(token, "roic_str_col_num") == 0) {
      config->start_col = (uint16_t)value;
    } else if (strcmp(token, "end_col") == 0 || strcmp(token, "roic_end_col_num") == 0) {
      config->end_col = (uint16_t)value;
    } else if (strcmp(token, "binning") == 0 || strcmp(token, "roic_binning_mode") == 0) {
      config->binning_mode = (uint8_t)value;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  return true;
}

static bool parse_static_idle_config_args(const char* args, static_idle_config_t* config) {
  /*
   * Static Idle 是正式工作流程配置，不暴露亮/暗场 DDR 地址参数。
   * 当前测试版由 ARM 从 uio2 环形图像池分配实际输出图地址。
   * 第一帧 offset 模板地址固定，不从这里分配。
   */
  char buffer[768];
  if (args == NULL || config == NULL) {
    return false;
  }
  if (strlen(args) >= sizeof(buffer)) {
    return false;
  }

  strcpy(buffer, args);
  char* token = strtok(buffer, " ");
  while (token != NULL) {
    char* equals = strchr(token, '=');
    uint32_t value = 0;
    if (equals == NULL) {
      return false;
    }

    *equals = '\0';
    if (!parse_u32_value(equals + 1, &value)) {
      return false;
    }

    if (strcmp(token, "idle_clean_interval_ms") == 0 || strcmp(token, "clean_ms") == 0) {
      config->idle_clean_interval_ms = value;
    } else if (strcmp(token, "exposure_ms") == 0 || strcmp(token, "exposure_window_ms") == 0) {
      config->exposure_window_ms = value;
    } else if (strcmp(token, "dark_window_ms") == 0 || strcmp(token, "dark_ms") == 0) {
      config->dark_window_ms = value;
    } else if (strcmp(token, "offset_en") == 0 || strcmp(token, "img_corr_offset_en") == 0) {
      config->dark_corr.offset_enable = value != 0;
    } else if (strcmp(token, "gain_en") == 0 || strcmp(token, "img_corr_gain_en") == 0) {
      config->dark_corr.gain_enable = value != 0;
    } else if (strcmp(token, "defect_en") == 0 || strcmp(token, "img_corr_defect_en") == 0) {
      config->dark_corr.defect_enable = value != 0;
    } else if (strcmp(token, "line_time") == 0 || strcmp(token, "gic_line_time") == 0) {
      config->clean_gic.line_time_ns = value;
      config->bright_gic.line_time_ns = value;
      config->dark_gic.line_time_ns = value;
    } else if (strcmp(token, "oe_rise") == 0 || strcmp(token, "gic_oe_raising_edge") == 0) {
      config->clean_gic.oe_raising_edge_ns = value;
      config->bright_gic.oe_raising_edge_ns = value;
      config->dark_gic.oe_raising_edge_ns = value;
    } else if (strcmp(token, "oe_fall") == 0 || strcmp(token, "gic_oe_falling_edge") == 0) {
      config->clean_gic.oe_falling_edge_ns = value;
      config->bright_gic.oe_falling_edge_ns = value;
      config->dark_gic.oe_falling_edge_ns = value;
    } else if (strcmp(token, "start_row") == 0 || strcmp(token, "gic_str_row_num") == 0) {
      config->clean_gic.start_row = (uint16_t)value;
      config->bright_gic.start_row = (uint16_t)value;
      config->dark_gic.start_row = (uint16_t)value;
    } else if (strcmp(token, "end_row") == 0 || strcmp(token, "gic_end_row_num") == 0) {
      config->clean_gic.end_row = (uint16_t)value;
      config->bright_gic.end_row = (uint16_t)value;
      config->dark_gic.end_row = (uint16_t)value;
    } else if (strcmp(token, "binning") == 0 || strcmp(token, "gic_binning_mode") == 0) {
      config->clean_gic.binning_mode = (uint8_t)value;
      config->bright_gic.binning_mode = (uint8_t)value;
      config->dark_gic.binning_mode = (uint8_t)value;
    } else if (strcmp(token, "offset_addr") == 0 || strcmp(token, "img_corr_offset_temp_str_addr") == 0) {
      config->bright_corr.offset_template_addr = value;
      config->dark_corr.offset_template_addr = value;
    } else if (strcmp(token, "offset_adder") == 0 || strcmp(token, "img_corr_offset_adder_value") == 0) {
      config->bright_corr.offset_adder_value = (uint16_t)value;
      config->dark_corr.offset_adder_value = (uint16_t)value;
    } else if (strcmp(token, "gain_addr") == 0 || strcmp(token, "img_corr_gain_temp_str_addr") == 0) {
      config->bright_corr.gain_template_addr = value;
      config->dark_corr.gain_template_addr = value;
    } else if (strcmp(token, "gain_clip") == 0 || strcmp(token, "img_corr_gain_clipping_value") == 0) {
      config->bright_corr.gain_clipping_value = (uint16_t)value;
      config->dark_corr.gain_clipping_value = (uint16_t)value;
    } else {
      return false;
    }

    token = strtok(NULL, " ");
  }

  /* 固化本模式的硬件语义：自清空不出图，亮场/暗场都出图，亮场校正全关。 */
  config->clean_gic.req_code = PA_PU_GIC_REQ_SERIAL_SCAN;
  config->clean_gic.dout_enable = false;
  config->bright_gic.req_code = PA_PU_GIC_REQ_SERIAL_SCAN;
  config->bright_gic.dout_enable = true;
  config->dark_gic.req_code = PA_PU_GIC_REQ_SERIAL_SCAN;
  config->dark_gic.dout_enable = true;

  config->bright_corr.offset_enable = false;
  config->bright_corr.gain_enable = false;
  config->bright_corr.defect_enable = false;
  return true;
}

static const char* work_mode_name(work_mode_t mode) {
  switch (mode) {
    case WORK_MODE_IDLE: return "Idle";
    case WORK_MODE_AED: return "AED";
    case WORK_MODE_SYNC_OUT: return "SyncOut";
    case WORK_MODE_SYNC_IN: return "SyncIn";
    case WORK_MODE_PREP: return "Prep";
    case WORK_MODE_CONTINUOUS: return "Continuous";
    case WORK_MODE_INNER: return "Inner";
    case WORK_MODE_FREE_SYNC: return "FreeSync";
    case WORK_MODE_DDR: return "DDR";
    default: return "Unknown";
  }
}

static void write_work_state_response(const work_mode_status_t* status,
                                      const pa_pu_dync_config_t* dync_config,
                                      char* response,
                                      size_t response_size) {
  if (status->mode == WORK_MODE_CONTINUOUS && dync_config != NULL) {
    /*
     * Continuous 只返回本模式真实有效的配置和状态。
     * Static Idle 的 capture/wr/corr/gic 缓存字段在 Dynamic 中从未更新，不能混在回包里冒充当前值。
     */
    int written = snprintf(response,
                           response_size,
                           "OK WORK_STATE mode=Continuous state=%s stop=%u last_error=%d last_phase=%s cycle=%u img_start=0x%08x img_end=0x%08x gic_dout=%u",
                           work_mode_state_name(status->state),
                           status->stop_requested ? 1u : 0u,
                           status->last_error,
                           work_mode_phase_name(status->last_phase),
                           dync_config->cycle_num,
                           dync_config->image_start_addr,
                           dync_config->image_end_addr,
                           GIC_DEFAULT_DOUT_EN != 0u ? 1u : 0u);
    for (unsigned i = 0; i < PA_PU_DYNC_STEP_COUNT &&
                         written > 0 && (size_t)written < response_size; ++i) {
      if ((dync_config->step_cfg_h[i] & PA_PU_DYNC_STEP_ENABLE_MASK) == 0u) {
        continue;
      }
      written += snprintf(response + written,
                          response_size - (size_t)written,
                          " step%u_h=0x%08x step%u_l=0x%08x",
                          i,
                          dync_config->step_cfg_h[i],
                          i,
                          dync_config->step_cfg_l[i]);
    }
    if (written > 0 && (size_t)written < response_size) {
      uint32_t img_wr_final_addr = pa_pu_read(PA_PU_IMG_WR_FINAL_IMG_ADDR_REG);
      snprintf(response + written,
               response_size - (size_t)written,
               " ring_frame_stride=0x%lx ring_frame_count=%lu img_wr_final_addr=0x%08x dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x\r\n",
               (unsigned long)status->ddr_frame_stride,
               (unsigned long)status->ddr_frame_count,
               img_wr_final_addr,
               status->dynamic_dync_state,
               status->dynamic_dync_end,
               status->dynamic_dync_debug_out);
    } else {
      response[response_size - 1u] = '\0';
    }
    return;
  }

  cal_task_status_t cal;
  calibration_task_get_status(&cal);
  /* Static Idle 保留采图阶段、模块失败现场和模板后台任务状态。 */
  snprintf(response,
           response_size,
           "OK WORK_STATE mode=%s state=%s pending_capture=%u stop=%u last_error=%d last_phase=%s last_int_vector=0x%08x last_wait_mask=0x%08x wr_state=0x%08x wr_end=0x%08x corr_state=0x%08x corr_end=0x%08x gic_state=0x%08x gic_end=0x%08x gic_dfx=0x%08x bright_addr=0x%08x dark_addr=0x%08x capture_id=%u ddr_next_offset=0x%08x frame_stride=0x%lx frame_count=%lu dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x template_task=%s template_state=%s template_id=%u template_stop=%u template_progress=%u/%u template_error=%d\r\n",
           work_mode_name(status->mode),
           work_mode_state_name(status->state),
           status->pending_capture ? 1u : 0u,
           status->stop_requested ? 1u : 0u,
           status->last_error,
           work_mode_phase_name(status->last_phase),
           status->last_int_vector,
           status->last_wait_mask,
           status->last_wr_state,
           status->last_wr_end,
           status->last_corr_state,
           status->last_corr_end,
           status->last_gic_state,
           status->last_gic_end,
           status->last_gic_dfx,
           status->last_bright_addr,
           status->last_dark_addr,
           status->capture_id,
           status->ddr_next_offset,
           (unsigned long)status->ddr_frame_stride,
           (unsigned long)status->ddr_frame_count,
           status->dynamic_dync_state,
           status->dynamic_dync_end,
           status->dynamic_dync_debug_out,
           calibration_task_kind_name(cal.kind),
           calibration_task_state_name(cal.state),
           cal.task_id,
           cal.stop_requested ? 1u : 0u,
           cal.progress_current,
           cal.progress_total,
           cal.last_error);
}

static void write_status_response(const command_context_t* ctx,
                                  char* response,
                                  size_t response_size) {
  pa_pu_status_t status;
  work_mode_status_t work_status;
  cal_task_status_t cal;
  char config_summary[256] = "unavailable";
  memset(&work_status, 0, sizeof(work_status));
  memset(&cal, 0, sizeof(cal));
  pa_pu_read_status(&status);
  if (ctx != NULL && ctx->work_mode != NULL) {
    work_mode_get_status(ctx->work_mode, &work_status);
  }
  calibration_task_get_status(&cal);
  if (ctx != NULL && ctx->runtime_config != NULL) {
    (void)config_store_summary(ctx->runtime_config,
                                config_summary,
                                sizeof(config_summary));
  }

  /*
   * STATUS 只读非清零寄存器，同时附带 ARM 工作状态和配置摘要。
   * INT_VECTOR 等 read-clear 寄存器仍由具体工作流内部读取。
   */
  snprintf(response, response_size,
           "OK STATUS pa_version=0x%08x pa_build_information=0x%08x adapted_main_board_version=0x%08x adapted_gic_board_version=0x%08x adapted_roic_board_version=0x%08x adapted_reserved_board_0_version=0x%08x adapted_reserved_board_1_version=0x%08x adapted_reserved_board_2_version=0x%08x pa_pu_com_version=0x%08x pa_rst_init_state=0x%08x wr_state=0x%08x wr_end=0x%08x wr_final_img_addr=0x%08x corr_state=0x%08x corr_end=0x%08x gic_state=0x%08x gic_end=0x%08x gic_dfx=0x%08x roic_state=0x%08x roic_end=0x%08x roic_dfx=0x%08x dync_state=0x%08x dync_end=0x%08x dync_debug_out=0x%08x img_upload_state=0x%08x img_upload_end=0x%08x img_upload_dfx=0x%08x work_mode=%s work_state=%s work_phase=%s work_error=%d config_file=%s config_summary=%s template_task=%s template_state=%s template_progress=%u/%u template_error=%d ddr_next_offset=0x%08x ddr_frame_stride=0x%lx ddr_frame_count=%lu\r\n",
           status.pa_version,
           status.pa_build_information,
           status.adapted_main_board_version,
           status.adapted_gic_board_version,
           status.adapted_roic_board_version,
           status.adapted_reserved_board_0_version,
           status.adapted_reserved_board_1_version,
           status.adapted_reserved_board_2_version,
           status.pa_pu_com_version,
           status.rst_init_state,
           status.img_wr_state,
           status.img_wr_end,
           status.img_wr_final_img_addr,
           status.img_corr_state,
           status.img_corr_end,
           status.gic_state,
           status.gic_end,
           status.gic_dfx,
           status.roic_state,
           status.roic_end,
           status.roic_dfx,
           status.dync_state,
           status.dync_end,
           status.dync_debug_out,
           status.img_upload_state,
           status.img_upload_end,
           status.img_upload_dfx,
           work_mode_name(work_status.mode),
           work_mode_state_name(work_status.state),
           work_mode_phase_name(work_status.last_phase),
           work_status.last_error,
           ctx != NULL && ctx->config_file != NULL ? ctx->config_file : "",
           config_summary,
           calibration_task_kind_name(cal.kind),
           calibration_task_state_name(cal.state),
           cal.progress_current,
           cal.progress_total,
           cal.last_error,
           work_status.ddr_next_offset,
           (unsigned long)work_status.ddr_frame_stride,
           (unsigned long)work_status.ddr_frame_count);
}

static void format_fpga_version(uint32_t value, char* text, size_t text_size) {
  snprintf(text, text_size,
           "%u.%u.%u",
           (value >> 16) & 0xffu,
           (value >> 8) & 0xffu,
           value & 0xffu);
}

static void format_fpga_build_information(uint32_t value, char* text, size_t text_size) {
  uint32_t year = (value >> 24) & 0xffu;
  uint32_t month = (value >> 16) & 0xffu;
  uint32_t day = (value >> 8) & 0xffu;
  uint32_t sub = value & 0xffu;

  if (year < 100u) {
    year += 2000u;
  }

  snprintf(text, text_size, "%04u-%02u-%02u.%u", year, month, day, sub);
}

static void write_version_response(char* response, size_t response_size) {
  pa_pu_status_t status;
  char pa_version[16];
  char pa_build_information[24];
  char adapted_main_board_version[16];
  char adapted_gic_board_version[16];
  char adapted_roic_board_version[16];
  char adapted_reserved_board_0_version[16];
  char adapted_reserved_board_1_version[16];
  char adapted_reserved_board_2_version[16];
  char pa_pu_com_version[16];
  memset(&status, 0, sizeof(status));

  pa_pu_read_status(&status);

  format_fpga_version(status.pa_version, pa_version, sizeof(pa_version));
  format_fpga_build_information(status.pa_build_information, pa_build_information, sizeof(pa_build_information));
  format_fpga_version(status.adapted_main_board_version, adapted_main_board_version, sizeof(adapted_main_board_version));
  format_fpga_version(status.adapted_gic_board_version, adapted_gic_board_version, sizeof(adapted_gic_board_version));
  format_fpga_version(status.adapted_roic_board_version, adapted_roic_board_version, sizeof(adapted_roic_board_version));
  format_fpga_version(status.adapted_reserved_board_0_version, adapted_reserved_board_0_version, sizeof(adapted_reserved_board_0_version));
  format_fpga_version(status.adapted_reserved_board_1_version, adapted_reserved_board_1_version, sizeof(adapted_reserved_board_1_version));
  format_fpga_version(status.adapted_reserved_board_2_version, adapted_reserved_board_2_version, sizeof(adapted_reserved_board_2_version));
  format_fpga_version(status.pa_pu_com_version, pa_pu_com_version, sizeof(pa_pu_com_version));

  snprintf(response, response_size,
           "OK VERSION app_version=%s app_build_time=\"%s\" pa_version=%s pa_build_information=%s adapted_main_board_version=%s adapted_gic_board_version=%s adapted_roic_board_version=%s adapted_reserved_board_0_version=%s adapted_reserved_board_1_version=%s adapted_reserved_board_2_version=%s pa_pu_com_version=%s\r\n",
           APP_VERSION,
           APP_BUILD_TIME,
           pa_version,
           pa_build_information,
           adapted_main_board_version,
           adapted_gic_board_version,
           adapted_roic_board_version,
           adapted_reserved_board_0_version,
           adapted_reserved_board_1_version,
           adapted_reserved_board_2_version,
           pa_pu_com_version);
}

static void write_start_result(char* response,
                               size_t response_size,
                               const char* command,
                               uint32_t irq_mask) {
  uint32_t int_vector = 0;
  int ret = pa_pu_wait_int_vector(irq_mask, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
  if (ret > 0) {
    snprintf(response, response_size, "OK %s int_vector=0x%08x\r\n", command, int_vector);
  } else if (ret == 0) {
    snprintf(response, response_size, "ERR %s TIMEOUT int_vector=0x%08x expect=0x%08x\r\n", command, int_vector, irq_mask);
  } else {
    snprintf(response, response_size, "ERR %s IRQ_WAIT\r\n", command);
  }
}

static void write_start_combo_result(char* response,
                                     size_t response_size,
                                     const char* command,
                                     uint32_t irq_mask) {
  uint32_t int_vector = 0;
  int ret = pa_pu_wait_int_vector_all(irq_mask, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
  if (ret > 0) {
    snprintf(response, response_size, "OK %s int_vector=0x%08x\r\n", command, int_vector);
  } else if (ret == 0) {
    snprintf(response, response_size, "ERR %s TIMEOUT int_vector=0x%08x wait_mask=0x%08x\r\n", command, int_vector, irq_mask);
  } else {
    snprintf(response, response_size, "ERR %s IRQ_WAIT\r\n", command);
  }
}

static bool prepare_manual_hardware_action(command_context_t* ctx,
                                           const char* command_name,
                                           char* response,
                                           size_t response_size) {
  if (ctx == NULL || ctx->work_mode == NULL) {
    return true;
  }

  if (calibration_task_is_active()) {
    cal_task_status_t cal;
    calibration_task_get_status(&cal);
    snprintf(response,
             response_size,
             "ERR %s BUSY template_task=%s template_state=%s template_id=%u hint=GET_WORK_STATE_or_STOP_WORK\r\n",
             command_name,
             calibration_task_kind_name(cal.kind),
             calibration_task_state_name(cal.state),
             cal.task_id);
    return false;
  }

  work_mode_status_t status;
  work_mode_get_status(ctx->work_mode, &status);
  if (status.state == WORK_STATE_STOPPED || status.state == WORK_STATE_DYNAMIC_COMPLETED) {
    return true;
  }

  if (status.state == WORK_STATE_IDLE_WAIT || status.state == WORK_STATE_IDLE_CLEANING) {
    /*
     * 人工 CONFIG/START/WRITE_REG 调试需要独占 PA/PU 寄存器。
     * 后台 Static Idle 处于等待或自清空时，先停掉工作线程，避免刚检查完空闲又进入下一轮自清空。
     */
    log_info("%s stops Static Idle work thread for manual hardware access state=%s",
             command_name,
             work_mode_state_name(status.state));
    work_mode_stop(ctx->work_mode);
    return true;
  }

  snprintf(response,
           response_size,
           "ERR %s BUSY state=%s hint=retry_after_capture_or_STOP_WORK\r\n",
           command_name,
           work_mode_state_name(status.state));
  return false;
}

static uint64_t command_monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int wait_capture_addr_modules_idle(unsigned timeout_ms) {
  /*
   * INT_VECTOR 完成 bit 表示本轮操作完成事件已经出现，但模块 state 回到空闲
   * 可能还存在很短延迟。裸压测连续启动前也等待三模块空闲，避免 start 脉冲
   * 打在上一轮 IMG_WR/IMG_CORR/GIC 还没完全释放的窗口里。
   */
  uint64_t start_ms = command_monotonic_ms();
  pa_pu_status_t status;

  for (;;) {
    pa_pu_read_status(&status);
    if (status.img_wr_state == 0 && status.img_corr_state == 0 && status.gic_state == 0) {
      return 0;
    }

    uint64_t now_ms = command_monotonic_ms();
    if (now_ms - start_ms >= timeout_ms) {
      log_error("loop capture addr wait idle timeout wr_state=0x%08x corr_state=0x%08x gic_state=0x%08x wr_end=0x%08x corr_end=0x%08x gic_end=0x%08x",
                status.img_wr_state,
                status.img_corr_state,
                status.gic_state,
                status.img_wr_end,
                status.img_corr_end,
                status.gic_end);
      return -1;
    }

    usleep(1000u);
  }
}

static void capture_addr_trace(const capture_addr_loop_config_t* config, uint32_t iteration, const char* step) {
  if (config == NULL || !config->trace) {
    return;
  }
  fprintf(stderr,
          "[TRACE] loop_capture_addr iteration=%u step=%s addr=0x%08x\n",
          iteration,
          step,
          config->image_addr);
  fflush(stderr);
}

static void static_idle_loop_trace(const static_idle_loop_config_t* config,
                                   uint32_t iteration,
                                   const char* step,
                                   const work_mode_status_t* status) {
  if (config == NULL || !config->trace) {
    return;
  }

  if (status != NULL) {
    fprintf(stderr,
            "[TRACE] loop_static_idle iteration=%u step=%s state=%s phase=%s capture_id=%u offset_addr=0x%08x output_addr=0x%08x int_vector=0x%08x wait_mask=0x%08x\n",
            iteration,
            step,
            work_mode_state_name(status->state),
            work_mode_phase_name(status->last_phase),
            status->capture_id,
            status->last_bright_addr,
            status->last_dark_addr,
            status->last_int_vector,
            status->last_wait_mask);
  } else {
    fprintf(stderr,
            "[TRACE] loop_static_idle iteration=%u step=%s\n",
            iteration,
            step);
  }
  fflush(stderr);
}

static int run_capture_addr_once(const capture_addr_loop_config_t* config,
                                 uint32_t iteration,
                                 uint32_t* int_vector_out) {
  if (config == NULL || int_vector_out == NULL) {
    return -1;
  }

  /*
   * 裸压测时每一轮都完整下发 GIC、IMG_CORR（以及可选 IMG_WR）。
   * 这样某一轮失败时，可以确认 FPGA 收到的是同一组参数，而不是依赖上一轮残留配置。
   */
  capture_addr_trace(config, iteration, "wait_idle_begin");
  if (wait_capture_addr_modules_idle(1000u) != 0) {
    pa_pu_dump_all_registers("loop_capture_addr_wait_idle");
    return -2;
  }
  capture_addr_trace(config, iteration, "wait_idle_done");
  pa_pu_configure_gic(&config->gic);
  capture_addr_trace(config, iteration, "config_gic_done");
  if (config->write_ddr) {
    pa_pu_configure_image_write(config->image_addr);
    capture_addr_trace(config, iteration, "config_img_wr_done");
  }
  pa_pu_configure_correction(&config->corr);
  capture_addr_trace(config, iteration, "config_corr_done");
  pa_pu_prepare_irq_wait();
  capture_addr_trace(config, iteration, "prepare_irq_done");
  if (config->write_ddr) {
    pa_pu_start_capture_triplet();
    capture_addr_trace(config, iteration, "start_triplet_done");
  } else {
    /* 不写 DDR：只启动 CORR+GIC，写 STR 顺序与 triplet 一致（先 CORR 后 GIC）。 */
    pa_pu_start_correction();
    pa_pu_start_gic();
    capture_addr_trace(config, iteration, "start_corr_gic_done");
  }
  int ret = pa_pu_wait_int_vector_all(config->wait_mask, config->timeout_ms, int_vector_out);
  capture_addr_trace(config, iteration, "wait_irq_done");
  return ret;
}

static void handle_loop_capture_addr(command_context_t* ctx,
                                     const char* command,
                                     char* response,
                                     size_t response_size) {
  capture_addr_loop_config_t config;
  uint32_t ok_count = 0;
  uint32_t fail_count = 0;
  uint32_t last_iteration = 0;
  uint32_t last_int_vector = 0;
  int last_ret = 0;

  default_capture_addr_loop_config(ctx->fpga_mem, &config);
  if (!parse_capture_addr_loop_args(cmd_args(command), &config) ||
      (config.write_ddr && !config.has_image_addr)) {
    snprintf(response, response_size, "ERR LOOP_CAPTURE_ADDR ARG\r\n");
    return;
  }

  /*
   * 该命令专门用于硬件压测，开始前停止 Static Idle 后台线程，
   * 避免空闲自清空和本命令同时写 GIC/IMG_WR/IMG_CORR 寄存器。
   */
  work_mode_stop(ctx->work_mode);
  log_info("loop capture addr start addr=0x%08x count=%u interval_ms=%u timeout_ms=%u wait_mask=0x%08x write_ddr=%u gic_dout=%u corr=%u/%u/%u trace=%u",
           config.image_addr,
           config.count,
           config.interval_ms,
           config.timeout_ms,
           config.wait_mask,
           config.write_ddr ? 1u : 0u,
           config.gic.dout_enable ? 1u : 0u,
           config.corr.offset_enable ? 1u : 0u,
           config.corr.gain_enable ? 1u : 0u,
           config.corr.defect_enable ? 1u : 0u,
           config.trace ? 1u : 0u);
  pa_pu_set_trace(config.trace);

  for (uint32_t iteration = 1; config.count == 0 || iteration <= config.count; ++iteration) {
    last_iteration = iteration;
    last_int_vector = 0;
    last_ret = run_capture_addr_once(&config, iteration, &last_int_vector);
    if (last_ret > 0) {
      ++ok_count;
      if (iteration == 1 || (iteration % 50u) == 0) {
        log_info("loop capture addr progress iteration=%u count=%u ok=%u fail=%u addr=0x%08x int_vector=0x%08x",
                 iteration,
                 config.count,
                 ok_count,
                 fail_count,
                 config.image_addr,
                 last_int_vector);
      }
    } else {
      ++fail_count;
      log_error("loop capture addr failed iteration=%u ret=%d addr=0x%08x int_vector=0x%08x wait_mask=0x%08x",
                iteration,
                last_ret,
                config.image_addr,
                last_int_vector,
                config.wait_mask);
      pa_pu_dump_all_registers("loop_capture_addr_failed");
      if (config.stop_on_error) {
        break;
      }
    }

    if (config.count != 0 && iteration >= config.count) {
      break;
    }
    if (!sleep_ms_for_loop(config.interval_ms)) {
      snprintf(response,
               response_size,
               "ERR LOOP_CAPTURE_ADDR INTERRUPTED ok=%u fail=%u last_iteration=%u addr=0x%08x\r\n",
               ok_count,
               fail_count,
               last_iteration,
               config.image_addr);
      pa_pu_set_trace(false);
      return;
    }
  }

  pa_pu_set_trace(false);
  snprintf(response,
           response_size,
           "%s LOOP_CAPTURE_ADDR ok=%u fail=%u last_iteration=%u addr=0x%08x last_int_vector=0x%08x wait_mask=0x%08x write_ddr=%u\r\n",
           fail_count == 0 ? "OK" : "ERR",
           ok_count,
           fail_count,
           last_iteration,
           config.image_addr,
           last_int_vector,
           config.wait_mask,
           config.write_ddr ? 1u : 0u);
}

int command_handle(command_context_t* ctx, const char* command, char* response, size_t response_size) {
  if (ctx == NULL || command == NULL || response == NULL || response_size == 0) {
    return -1;
  }

  response[0] = '\0';

  /*
   * 当前使用可读性高的 ASCII 行协议，便于串口助手调试。
   * 后续上位机协议确定后，可以只替换本文件，不影响 PA/模板底层模块。
   */
  if (cmd_is(command, "PING")) {
    /* 心跳命令：只验证通信链路和应用主循环是否正常。 */
    snprintf(response, response_size, "OK PONG\r\n");
    return 0;
  }

  if (cmd_is(command, "STATUS")) {
    /* 读取 PA/FPGA 当前非清除类状态，用于上位机刷新状态栏或调试。 */
    write_status_response(ctx, response, response_size);
    return 0;
  }

  if (cmd_is(command, "VERSION") || cmd_is(command, "GET_VERSION")) {
    /* 返回 ARM 应用版本和 PA/FPGA 版本寄存器，不读取 read-clear 的 INT_VECTOR。 */
    write_version_response(response, response_size);
    return 0;
  }

  if (cmd_has_name(command, "SET_TIME")) {
    /*
     * 同步 Linux 本机时间，供上位机连接后校准开发板日志时间。
     * 推荐上位机发送 epoch/epoch_ms，手工调试时也接受本地时间字符串。
     */
    time_t seconds = 0;
    long nanoseconds = 0;
    if (!parse_set_time_args(cmd_args(command), &seconds, &nanoseconds)) {
      snprintf(response, response_size, "ERR SET_TIME ARG\r\n");
      return 0;
    }

    struct timespec new_time = {
      .tv_sec = seconds,
      .tv_nsec = nanoseconds,
    };
    if (clock_settime(CLOCK_REALTIME, &new_time) != 0) {
      snprintf(response, response_size, "ERR SET_TIME errno=%d\r\n", errno);
      return 0;
    }

    write_time_response(response, response_size, "SET_TIME");
    return 0;
  }

  if (cmd_is(command, "GET_TIME") || cmd_is(command, "TIME")) {
    /* 查询 Linux 本机当前时间，用于确认 SET_TIME 是否生效。 */
    write_time_response(response, response_size, "TIME");
    return 0;
  }

  if (cmd_is(command, "GET_WORK_STATE") || cmd_is(command, "WORK_STATE")) {
    /* 查询 ARM 工作线程状态，不访问 read-clear 类 FPGA 中断寄存器。 */
    work_mode_status_t status;
    pa_pu_dync_config_t dync_config;
    memset(&dync_config, 0, sizeof(dync_config));
    work_mode_get_status(ctx->work_mode, &status);
    work_mode_get_dynamic_config(ctx->work_mode, &dync_config);
    write_work_state_response(&status, &dync_config, response, response_size);
    return 0;
  }

  if (cmd_is(command, "QUERY_DYNAMIC") || cmd_is(command, "GET_DYNAMIC_STATUS")) {
    work_mode_status_t status;
    pa_pu_dync_config_t dync_config;
    memset(&dync_config, 0, sizeof(dync_config));
    work_mode_get_status(ctx->work_mode, &status);
    work_mode_get_dynamic_config(ctx->work_mode, &dync_config);
    snprintf(response,
             response_size,
             "OK DYNAMIC_STATUS state=%s phase=%s stop=%u error=%d cycle=%u img_start=0x%08x img_end=0x%08x dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x final_img_addr=0x%08x\r\n",
             work_mode_state_name(status.state),
             work_mode_phase_name(status.last_phase),
             status.stop_requested ? 1u : 0u,
             status.last_error,
             dync_config.cycle_num,
             dync_config.image_start_addr,
             dync_config.image_end_addr,
             status.dynamic_dync_state,
             status.dynamic_dync_end,
             status.dynamic_dync_debug_out,
             pa_pu_read(PA_PU_IMG_WR_FINAL_IMG_ADDR_REG));
    return 0;
  }

  if (cmd_is(command, "GET_CONFIG_SUMMARY") || cmd_is(command, "GET_CONFIG")) {
    char summary[256];
    if (ctx->runtime_config == NULL || config_store_summary(ctx->runtime_config, summary, sizeof(summary)) != 0) {
      snprintf(response, response_size, "ERR GET_CONFIG_SUMMARY\r\n");
    } else {
      snprintf(response, response_size, "OK CONFIG_SUMMARY file=%s %s\r\n", ctx->config_file, summary);
    }
    return 0;
  }

  if (cmd_has_name(command, "GET_CONFIG_GROUP")) {
    const char* group = cmd_args(command);
    if (ctx->runtime_config == NULL || *group == '\0') {
      snprintf(response, response_size, "ERR GET_CONFIG_GROUP ARG\r\n");
    } else if (strcasecmp(group, "gic") == 0) {
      const pa_pu_gic_config_t* g = &ctx->runtime_config->gic;
      snprintf(response, response_size, "OK CONFIG_GROUP group=gic req_code=%u dout_en=%u line_time_ns=%u oe_rise_ns=%u oe_fall_ns=%u start_row=%u end_row=%u binning=%u\r\n", g->req_code, g->dout_enable, g->line_time_ns, g->oe_raising_edge_ns, g->oe_falling_edge_ns, g->start_row, g->end_row, g->binning_mode);
    } else if (strcasecmp(group, "roic") == 0) {
      const pa_pu_roic_config_t* r = &ctx->runtime_config->roic;
      snprintf(response, response_size, "OK CONFIG_GROUP group=roic start_col=%u end_col=%u binning=%u reg_00=0x%04x reg_02=0x%04x reg_05=0x%04x reg_06=0x%04x reg_07=0x%04x reg_09=0x%04x reg_0a=0x%04x reg_0b=0x%04x reg_0c=0x%04x reg_0d=0x%04x reg_0e=0x%04x reg_0f=0x%04x reg_10=0x%04x reg_11=0x%04x reg_17=0x%04x reg_24=0x%04x reg_28=0x%04x reg_2d=0x%04x reg_3b=0x%04x\r\n", r->start_col, r->end_col, r->binning_mode, r->reg_00, r->reg_02, r->reg_05, r->reg_06, r->reg_07, r->reg_09, r->reg_0a, r->reg_0b, r->reg_0c, r->reg_0d, r->reg_0e, r->reg_0f, r->reg_10, r->reg_11, r->reg_17, r->reg_24, r->reg_28, r->reg_2d, r->reg_3b);
    } else if (strcasecmp(group, "corr") == 0) {
      const pa_pu_corr_config_t* c = &ctx->runtime_config->corr;
      snprintf(response, response_size, "OK CONFIG_GROUP group=corr pkg_num=%u row_num=%u col_num=%u offset_en=%u offset_addr=0x%08x offset_adder_value=%u offset_corr_mode=%u gain_en=%u gain_addr=0x%08x gain_clipping_value=%u defect_en=%u\r\n", c->pkg_num, c->row_num, c->col_num, c->offset_enable, c->offset_template_addr, c->offset_adder_value, c->offset_corr_mode, c->gain_enable, c->gain_template_addr, c->gain_clipping_value, c->defect_enable);
    } else if (strcasecmp(group, "static") == 0 || strcasecmp(group, "static_idle") == 0) {
      const static_idle_config_t* s = &ctx->runtime_config->static_idle;
      snprintf(response, response_size, "OK CONFIG_GROUP group=static_idle idle_clean_interval_ms=%u exposure_window_ms=%u dark_window_ms=%u\r\n", s->idle_clean_interval_ms, s->exposure_window_ms, s->dark_window_ms);
    } else if (strcasecmp(group, "dynamic") == 0) {
      const pa_pu_dync_config_t* d = &ctx->runtime_config->dync;
      int written = snprintf(response, response_size, "OK CONFIG_GROUP group=dynamic cycle=%u image_start_addr=0x%08x image_end_addr=0x%08x", d->cycle_num, d->image_start_addr, d->image_end_addr);
      for (unsigned i = 0; i < PA_PU_DYNC_STEP_COUNT && written > 0 && (size_t)written < response_size; ++i) written += snprintf(response + written, response_size - (size_t)written, " step%u_h=0x%08x step%u_l=%u", i, d->step_cfg_h[i], i, d->step_cfg_l[i]);
      if (written > 0 && (size_t)written < response_size) snprintf(response + written, response_size - (size_t)written, "\r\n");
    } else {
      snprintf(response, response_size, "ERR GET_CONFIG_GROUP UNKNOWN\r\n");
    }
    return 0;
  }

  if (cmd_has_name(command, "SET_CONFIG_GROUP")) {
    const char* args = cmd_args(command);
    int result = set_runtime_config_group(ctx, args);
    if (result == -2) {
      snprintf(response, response_size, "ERR SET_CONFIG_GROUP BUSY\r\n");
    } else if (result != 0) {
      snprintf(response, response_size, "ERR SET_CONFIG_GROUP APPLY\r\n");
    } else {
      char group[32] = {0};
      if (strncasecmp(args, "group=", 6) == 0) {
        sscanf(args + 6, "%31s", group);
      } else {
        sscanf(args, "%31s", group);
      }
      snprintf(response, response_size, "OK SET_CONFIG_GROUP group=%s\r\n", group);
    }
    return 0;
  }

  if (cmd_has_name(command, "GET_CONFIG_ITEM")) {
    char value[64];
    const char* item = cmd_args(command);
    if (config_store_get_item(ctx->runtime_config, item, value, sizeof(value)) != 0) {
      snprintf(response, response_size, "ERR GET_CONFIG_ITEM ARG\r\n");
    } else {
      snprintf(response, response_size, "OK CONFIG_ITEM item=%s value=%s\r\n", item, value);
    }
    return 0;
  }

  if (cmd_has_name(command, "SET_CONFIG_ITEM")) {
    char args[256];
    snprintf(args, sizeof(args), "%s", cmd_args(command));
    char* equals = strchr(args, '=');
    if (equals == NULL) equals = strchr(args, ' ');
    if (equals == NULL) {
      snprintf(response, response_size, "ERR SET_CONFIG_ITEM ARG\r\n");
      return 0;
    }
    *equals = '\0';
    char* value = equals + 1;
    while (*value == ' ') ++value;
    if (ctx->runtime_config == NULL || config_store_set_item(ctx->runtime_config, args, value) != 0) {
      snprintf(response, response_size, "ERR SET_CONFIG_ITEM ARG\r\n");
      return 0;
    }
    int apply_result = apply_runtime_config(ctx);
    if (apply_result == -2) {
      snprintf(response, response_size, "ERR SET_CONFIG_ITEM BUSY\r\n");
      return 0;
    }
    if (apply_result != 0 || config_store_save(ctx->config_file, ctx->runtime_config) != 0) {
      snprintf(response, response_size, "ERR SET_CONFIG_ITEM APPLY\r\n");
      return 0;
    }
    snprintf(response, response_size, "OK SET_CONFIG_ITEM item=%s value=%s\r\n", args, value);
    return 0;
  }

  if (cmd_is(command, "RESET_CONFIG")) {
    if (ctx->runtime_config == NULL || !work_mode_allows_hardware_action(ctx->work_mode)) {
      snprintf(response, response_size, "ERR RESET_CONFIG BUSY\r\n");
      return 0;
    }
    if (config_store_defaults(ctx->fpga_mem, ctx->runtime_config) != 0 ||
        apply_runtime_config(ctx) != 0 ||
        config_store_save(ctx->config_file, ctx->runtime_config) != 0) {
      snprintf(response, response_size, "ERR RESET_CONFIG\r\n");
    } else {
      snprintf(response, response_size, "OK RESET_CONFIG\r\n");
    }
    return 0;
  }

  if (cmd_has_name(command, "RESET_CONFIG_GROUP")) {
    const char* group = cmd_args(command);
    if (ctx->runtime_config == NULL || !work_mode_allows_hardware_action(ctx->work_mode)) {
      snprintf(response, response_size, "ERR RESET_CONFIG_GROUP BUSY\r\n");
    } else if (reset_runtime_config_group(ctx, group) != 0 ||
               apply_runtime_config(ctx) != 0 ||
               config_store_save(ctx->config_file, ctx->runtime_config) != 0) {
      snprintf(response, response_size, "ERR RESET_CONFIG_GROUP ARG\r\n");
    } else {
      snprintf(response, response_size, "OK RESET_CONFIG_GROUP group=%s\r\n", group);
    }
    return 0;
  }

  if (cmd_is(command, "GET_TEMPLATE_STATE") || cmd_is(command, "TEMPLATE_STATE")) {
    cal_task_status_t cal;
    calibration_task_get_status(&cal);
    snprintf(response,
             response_size,
             "OK TEMPLATE_STATE task=%s state=%s id=%u stop=%u progress=%u/%u error=%d frames=%u valid_frames=%u offset_addr=0x%08x last_img_addr=0x%08x int_vector=0x%08x\r\n",
             calibration_task_kind_name(cal.kind), calibration_task_state_name(cal.state),
             cal.task_id, cal.stop_requested ? 1u : 0u,
             cal.progress_current, cal.progress_total, cal.last_error,
             cal.dynamic_offset.frames, cal.dynamic_offset.valid_frames,
             cal.dynamic_offset.offset_addr, cal.dynamic_offset.last_img_addr,
             cal.dynamic_offset.last_int_vector);
    return 0;
  }

  if (cmd_is(command, "DUMP_REGS") || cmd_is(command, "DUMP_REGISTERS") || cmd_is(command, "DUMP_PA_REGS")) {
    /*
     * 调试快照输出到日志，命令响应只返回统计信息。
     * safe dump 会跳过 INT_VECTOR 等 read-clear 寄存器，避免读取时清掉中断现场。
     */
    size_t skipped = 0;
    size_t count = pa_pu_dump_safe_registers("command", &skipped);
    snprintf(response,
             response_size,
             "OK DUMP_REGS count=%lu skipped_read_clear=%lu\r\n",
             (unsigned long)count,
             (unsigned long)skipped);
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_STATIC_IDLE")) {
    /*
     * 更新 Static Idle 的时间窗口、GIC 时序和暗场校正开关。
     * 工作线程处于采图窗口或采图阶段时会返回 BUSY，防止半途改寄存器。
     */
    static_idle_config_t config;
    work_mode_get_static_idle_config(ctx->work_mode, &config);
    if (!parse_static_idle_config_args(cmd_args(command), &config)) {
      snprintf(response, response_size, "ERR CONFIG_STATIC_IDLE ARG\r\n");
      return 0;
    }

    int ret = work_mode_update_static_idle_config(ctx->work_mode, &config);
    if (ret == -2) {
      work_mode_status_t status;
      work_mode_get_status(ctx->work_mode, &status);
      snprintf(response, response_size, "ERR CONFIG_STATIC_IDLE BUSY state=%s\r\n", work_mode_state_name(status.state));
      return 0;
    }
    if (ret != 0) {
      snprintf(response, response_size, "ERR CONFIG_STATIC_IDLE\r\n");
      return 0;
    }

    snprintf(response,
             response_size,
             "OK CONFIG_STATIC_IDLE idle_clean_interval_ms=%u exposure_ms=%u dark_window_ms=%u offset_en=%u gain_en=%u defect_en=%u line_time=%u start_row=%u end_row=%u binning=%u\r\n",
             config.idle_clean_interval_ms,
             config.exposure_window_ms,
             config.dark_window_ms,
             config.dark_corr.offset_enable ? 1u : 0u,
             config.dark_corr.gain_enable ? 1u : 0u,
             config.dark_corr.defect_enable ? 1u : 0u,
             config.dark_gic.line_time_ns,
             config.dark_gic.start_row,
             config.dark_gic.end_row,
             config.dark_gic.binning_mode);
    return 0;
  }

  if (cmd_is(command, "START_STATIC_IDLE_CAPTURE") || cmd_is(command, "START_STATIC_CAPTURE")) {
    /*
     * 发起一次静态 Idle 正式采图：
     * 等待当前空闲清空结束后，按曝光窗口 -> 亮场 -> 暗场窗口 -> 暗场执行。
     */
    work_mode_status_t status;
    int ret = work_mode_start_static_idle_capture(ctx->work_mode, &status);
    if (ret == -2) {
      snprintf(response, response_size, "ERR START_STATIC_IDLE_CAPTURE BUSY state=%s\r\n", work_mode_state_name(status.state));
      return 0;
    }
    if (ret != 0 && status.state == WORK_STATE_STOPPED) {
      snprintf(response,
               response_size,
               "ERR START_STATIC_IDLE_CAPTURE STOPPED hint=START_WORK\r\n");
      return 0;
    }
    if (ret != 0) {
      snprintf(response,
               response_size,
               "ERR START_STATIC_IDLE_CAPTURE phase=%s int_vector=0x%08x wait_mask=0x%08x wr_state=0x%08x wr_end=0x%08x corr_state=0x%08x corr_end=0x%08x gic_state=0x%08x gic_end=0x%08x gic_dfx=0x%08x bright_addr=0x%08x dark_addr=0x%08x\r\n",
               work_mode_phase_name(status.last_phase),
               status.last_int_vector,
               status.last_wait_mask,
               status.last_wr_state,
               status.last_wr_end,
               status.last_corr_state,
               status.last_corr_end,
               status.last_gic_state,
               status.last_gic_end,
               status.last_gic_dfx,
               status.last_bright_addr,
               status.last_dark_addr);
      return 0;
    }
    snprintf(response,
             response_size,
             "OK START_STATIC_IDLE_CAPTURE bright_addr=0x%08x dark_addr=0x%08x int_vector=0x%08x capture_id=%u\r\n",
             status.last_bright_addr,
             status.last_dark_addr,
             status.last_int_vector,
             status.capture_id);
    return 0;
  }

  if (cmd_has_name(command, "LOOP_STATIC_IDLE_CAPTURE") || cmd_has_name(command, "START_STATIC_IDLE_LOOP")) {
    if (calibration_task_is_active()) {
      snprintf(response, response_size, "ERR LOOP_STATIC_IDLE_CAPTURE BUSY template_task_running=1\r\n");
      return 0;
    }
    /*
     * 稳定性测试命令：周期性执行正式 Static Idle 采图流程。
     * count=0 表示一直循环；该命令同步运行，长循环时需要 Ctrl+C 或外部终止程序。
     */
    static_idle_loop_config_t loop_config;
    if (!parse_static_idle_loop_args(cmd_args(command), &loop_config)) {
      snprintf(response, response_size, "ERR LOOP_STATIC_IDLE_CAPTURE ARG\r\n");
      return 0;
    }

    uint32_t ok_count = 0;
    uint32_t fail_count = 0;
    uint32_t last_iteration = 0;
    work_mode_status_t status;
    memset(&status, 0, sizeof(status));

    log_info("static idle loop start count=%u interval_ms=%u stop_on_error=%u trace=%u",
             loop_config.count,
             loop_config.interval_ms,
             loop_config.stop_on_error ? 1u : 0u,
             loop_config.trace ? 1u : 0u);
    pa_pu_set_trace(loop_config.trace);
    work_mode_set_trace(ctx->work_mode, loop_config.trace);

    for (uint32_t iteration = 1; loop_config.count == 0 || iteration <= loop_config.count; ++iteration) {
      last_iteration = iteration;

      static_idle_loop_trace(&loop_config, iteration, "capture_begin", NULL);
      int ret = work_mode_start_static_idle_capture(ctx->work_mode, &status);
      if (ret == 0) {
        ++ok_count;
        static_idle_loop_trace(&loop_config, iteration, "capture_done", &status);
        if (iteration == 1 || (iteration % 50u) == 0) {
          log_info("static idle loop progress iteration=%u count=%u ok=%u fail=%u capture_id=%u offset_addr=0x%08x output_addr=0x%08x int_vector=0x%08x",
                   iteration,
                   loop_config.count,
                   ok_count,
                   fail_count,
                   status.capture_id,
                   status.last_bright_addr,
                   status.last_dark_addr,
                   status.last_int_vector);
        }
      } else {
        ++fail_count;
        static_idle_loop_trace(&loop_config, iteration, "capture_failed", &status);
        log_error("static idle loop capture failed iteration=%u ret=%d phase=%s int_vector=0x%08x wait_mask=0x%08x",
                  iteration,
                  ret,
                  work_mode_phase_name(status.last_phase),
                  status.last_int_vector,
                  status.last_wait_mask);
        if (loop_config.stop_on_error) {
          break;
        }
      }

      if (loop_config.count != 0 && iteration >= loop_config.count) {
        break;
      }
      if (!sleep_ms_for_loop(loop_config.interval_ms)) {
        pa_pu_set_trace(false);
        work_mode_set_trace(ctx->work_mode, false);
        snprintf(response,
                 response_size,
                 "ERR LOOP_STATIC_IDLE_CAPTURE INTERRUPTED ok=%u fail=%u last_iteration=%u\r\n",
                 ok_count,
                 fail_count,
                 last_iteration);
        return 0;
      }
    }

    pa_pu_set_trace(false);
    work_mode_set_trace(ctx->work_mode, false);
    snprintf(response,
             response_size,
             "%s LOOP_STATIC_IDLE_CAPTURE ok=%u fail=%u last_iteration=%u last_capture_id=%u last_output_addr=0x%08x\r\n",
             fail_count == 0 ? "OK" : "ERR",
             ok_count,
             fail_count,
             last_iteration,
             status.capture_id,
             status.last_dark_addr);
    return 0;
  }

  if (cmd_has_name(command, "LOOP_CAPTURE_ADDR") || cmd_has_name(command, "STRESS_CAPTURE_ADDR")) {
    /*
     * 固定地址裸压测命令：
     * 不走 Static Idle 亮/暗场业务流程，只重复执行“配置三模块 -> 同时启动 -> 等待中断”。
     */
    handle_loop_capture_addr(ctx, command, response, response_size);
    return 0;
  }

  if (cmd_has_name(command, "CAL_GAIN_BEGIN")) {
    if (calibration_task_is_active()) {
      snprintf(response, response_size, "ERR CAL_GAIN_BEGIN BUSY hint=GET_WORK_STATE_or_STOP_WORK\r\n");
      return 0;
    }
    cal_gain_begin_args_t args;
    if (!parse_cal_gain_begin_args(cmd_args(command), &args)) {
      snprintf(response, response_size, "ERR CAL_GAIN_BEGIN ARG\r\n");
      return 0;
    }

    if (calibration_gain_begin(args.levels, args.level_count, args.frames, args.threshold) == 0) {
      snprintf(response,
               response_size,
               "OK CAL_GAIN_BEGIN levels=%u frames=%u threshold=%.3f\r\n",
               args.level_count,
               args.frames,
               args.threshold);
    } else {
      snprintf(response, response_size, "ERR CAL_GAIN_BEGIN\r\n");
    }
    return 0;
  }

  if (cmd_has_name(command, "CAL_GAIN_CAPTURE")) {
    uint32_t level = 0;
    const char* args = cmd_args(command);
    if (strncasecmp(args, "level=", 6) == 0) {
      args += 6;
    }
    if (!parse_u32_value(args, &level)) {
      snprintf(response, response_size, "ERR CAL_GAIN_CAPTURE ARG\r\n");
      return 0;
    }

    /*
     * gain 校准是独占硬件动作，开始采集前停止 Static Idle 工作线程，
     * 避免空闲自清空或正式采图流程改写 GIC/IMG_WR/IMG_CORR 寄存器。
     */
    work_mode_stop(ctx->work_mode);
#if GAIN_TASK_BACKGROUND_ENABLE
    int ret = calibration_task_start_gain_capture(ctx->fpga_mem, level);
    if (ret == 0) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response,
               response_size,
               "OK CAL_GAIN_CAPTURE state=RUNNING task_id=%u level=%u\r\n",
               cal.task_id,
               level);
    } else if (ret == -2) {
      snprintf(response, response_size, "ERR CAL_GAIN_CAPTURE BUSY level=%u\r\n", level);
    } else {
      snprintf(response, response_size, "ERR CAL_GAIN_CAPTURE level=%u\r\n", level);
    }
#else
    /*
     * 前台模式下命令返回即代表该灰阶采集已经成功或失败，便于结合命令边界
     * 和寄存器日志定位缺失的完成中断。
     */
    if (calibration_gain_capture_level(ctx->fpga_mem, level) == 0) {
      snprintf(response, response_size, "OK CAL_GAIN_CAPTURE level=%u\r\n", level);
    } else {
      snprintf(response, response_size, "ERR CAL_GAIN_CAPTURE level=%u\r\n", level);
    }
#endif
    return 0;
  }

  if (cmd_is(command, "CAL_GAIN_BUILD")) {
    work_mode_stop(ctx->work_mode);
#if GAIN_TASK_BACKGROUND_ENABLE
    int ret = calibration_task_start_gain_build(ctx->fpga_mem);
    if (ret == 0) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response, response_size, "OK CAL_GAIN_BUILD state=RUNNING task_id=%u\r\n", cal.task_id);
    } else if (ret == -2) {
      snprintf(response, response_size, "ERR CAL_GAIN_BUILD BUSY\r\n");
    } else {
      snprintf(response, response_size, "ERR CAL_GAIN_BUILD\r\n");
    }
#else
    /* 前台模式在命令线程完成构建，返回 OK 后 gain 模板已经加载并重新下发校正配置。 */
    if (calibration_gain_build(ctx->fpga_mem) == 0) {
      pa_pu_corr_config_t config = default_corr_config(ctx->fpga_mem);
      pa_pu_configure_correction(&config);
      snprintf(response, response_size, "OK CAL_GAIN_BUILD\r\n");
    } else {
      snprintf(response, response_size, "ERR CAL_GAIN_BUILD\r\n");
    }
#endif
    return 0;
  }

  if (cmd_is(command, "CAL_GAIN_CANCEL")) {
    bool stopping = calibration_task_request_stop();
    if (!stopping) {
      calibration_gain_cancel();
    }
    snprintf(response, response_size, "OK CAL_GAIN_CANCEL state=%s\r\n", stopping ? "STOPPING" : "CANCELED");
    return 0;
  }

  if (cmd_is(command, "CAL_GAIN_STATUS")) {
    cal_gain_status_t status;
    calibration_gain_get_status(&status);
    int written = snprintf(response,
                           response_size,
                           "OK CAL_GAIN_STATUS active=%u levels=%u ready=%u frames=%u threshold=%.3f bad_pixels=%u",
                           status.active ? 1u : 0u,
                           status.level_count,
                           status.levels_ready,
                           status.frames_per_level,
                           status.defect_threshold,
                           status.bad_pixel_count);
    for (uint32_t i = 0; i < status.level_count && written > 0 && (size_t)written < response_size; ++i) {
      written += snprintf(response + written,
                          response_size - (size_t)written,
                          " level%u=%u:%u:%u",
                          i,
                          status.levels[i].level,
                          status.levels[i].ready ? 1u : 0u,
                          status.levels[i].median);
    }
    if (written > 0 && (size_t)written < response_size) {
      snprintf(response + written, response_size - (size_t)written, "\r\n");
    } else {
      response[response_size - 1u] = '\0';
    }
    return 0;
  }

  if (is_read_reg_command(command)) {
    // 读寄存器命令
    const char* args = cmd_args(command);
    char reg_text[96];
    uint16_t reg = 0;
    if (!get_arg_token(&args, reg_text, sizeof(reg_text)) || !parse_register_ref(reg_text, &reg)) {
      snprintf(response, response_size, "ERR READ_REG ARG\r\n");
      return 0;
    }

    uint32_t value = pa_pu_read(reg);
    snprintf(response, response_size, "OK READ_REG ref=%s offset=0x%04x value=0x%08x\r\n", reg_text, reg, value);
    return 0;
  }

  if (is_write_reg_command(command)) {
    if (!prepare_manual_hardware_action(ctx, "WRITE_REG", response, response_size)) {
      return 0;
    }
    // 写寄存器命令
    const char* args = cmd_args(command);
    char reg_text[96];
    char value_text[96];
    uint16_t reg = 0;
    uint32_t value = 0;
    if (!get_arg_token(&args, reg_text, sizeof(reg_text)) ||
        !get_arg_token(&args, value_text, sizeof(value_text)) ||
        !parse_register_ref(reg_text, &reg) ||
        !parse_u32_value(value_text, &value)) {
      snprintf(response, response_size, "ERR WRITE_REG ARG\r\n");
      return 0;
    }

    pa_pu_write(reg, value);
    snprintf(response, response_size, "OK WRITE_REG ref=%s offset=0x%04x value=0x%08x\r\n", reg_text, reg, value);
    return 0;
  }

  if (cmd_is(command, "LOAD_TEMPLATE")) {
    if (!prepare_manual_hardware_action(ctx, "LOAD_TEMPLATE", response, response_size)) {
      return 0;
    }
    /* 从文件系统加载 offset/gain 模板，适合设备重启后恢复已有校正模板。 */
    if (template_load_files(ctx->fpga_mem) == 0) {
      pa_pu_corr_config_t config = default_corr_config(ctx->fpga_mem);
      pa_pu_configure_correction(&config);
      snprintf(response, response_size, "OK LOAD_TEMPLATE\r\n");
    } else {
      snprintf(response, response_size, "ERR LOAD_TEMPLATE\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_OFFSET")) {
    if (!prepare_manual_hardware_action(ctx, "MAKE_OFFSET", response, response_size)) {
      return 0;
    }
    /* 用当前暗场图像生成 offset 模板；上位机应先确保当前帧是有效暗场。 */
    int ret = calibration_task_start_make_offset(ctx->fpga_mem);
    if (ret == 0) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response, response_size, "OK MAKE_OFFSET state=RUNNING task_id=%u\r\n", cal.task_id);
    } else {
      snprintf(response, response_size, "ERR MAKE_OFFSET\r\n");
    }
    return 0;
  }

  if (cmd_has_name(command, "MAKE_DYNC_OFFSET") || cmd_has_name(command, "MAKE_DYNAMIC_OFFSET")) {
    if (!prepare_manual_hardware_action(ctx, "MAKE_DYNC_OFFSET", response, response_size)) {
      return 0;
    }

    /*
     * 保留 MAKE_DYNC_OFFSET 命令名以兼容既有协议；实际制作采用静态逐帧采集。
     * 每一帧完成 GIC/IMG_WR/IMG_CORR 后才触发下一帧，避免连续 Dynamic 出图丢帧。
     */
    dynamic_offset_args_t args;
    memset(&args, 0, sizeof(args));
    default_dync_command_config(ctx->fpga_mem, &args.dync);
    if (!parse_dynamic_offset_args(cmd_args(command), &args)) {
      snprintf(response, response_size, "ERR MAKE_DYNC_OFFSET ARG\r\n");
      return 0;
    }

    int ret = calibration_task_start_dynamic_offset(ctx->fpga_mem,
                                                    NULL,
                                                    false,
                                                    args.frames,
                                                    args.valid_frames);
    if (ret == 0) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response,
               response_size,
               "OK MAKE_DYNC_OFFSET state=RUNNING task_id=%u frames=%u valid_frames=%u\r\n",
               cal.task_id,
               args.frames,
               args.valid_frames);
    } else {
      snprintf(response,
               response_size,
               "ERR MAKE_DYNC_OFFSET frames=%u valid_frames=%u\r\n",
               args.frames,
               args.valid_frames);
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_GAIN")) {
    if (!prepare_manual_hardware_action(ctx, "MAKE_GAIN", response, response_size)) {
      return 0;
    }
    /* 用当前亮场图像和已有 offset 模板生成 gain 模板；需先执行或加载 offset。 */
#if GAIN_TASK_BACKGROUND_ENABLE
    int ret = calibration_task_start_make_gain(ctx->fpga_mem);
    if (ret == 0) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response, response_size, "OK MAKE_GAIN state=RUNNING task_id=%u\r\n", cal.task_id);
    } else {
      snprintf(response, response_size, "ERR MAKE_GAIN\r\n");
    }
#else
    if (template_make_gain(ctx->fpga_mem) == 0) {
      pa_pu_corr_config_t config = default_corr_config(ctx->fpga_mem);
      pa_pu_configure_correction(&config);
      snprintf(response, response_size, "OK MAKE_GAIN\r\n");
    } else {
      snprintf(response, response_size, "ERR MAKE_GAIN\r\n");
    }
#endif
    return 0;
  }

  if (cmd_is(command, "CONFIG_TEMPLATE")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_TEMPLATE", response, response_size)) {
      return 0;
    }
    /* 只重新下发模板地址/尺寸配置，不重新生成或加载模板内容。 */
    pa_pu_corr_config_t config = default_corr_config(ctx->fpga_mem);
    pa_pu_configure_correction(&config);
    snprintf(response, response_size, "OK CONFIG_TEMPLATE\r\n");
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_CORR") || cmd_has_name(command, "CONFIG_IMG_CORR")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_CORR", response, response_size)) {
      return 0;
    }
    /*
     * 下发图像校正尺寸、offset/gain/defect 使能和模板地址。
     * 不带参数时使用默认图像尺寸和模板地址；带 key=value 时覆盖对应字段。
     */
    pa_pu_corr_config_t config = default_corr_config(ctx->fpga_mem);
    if (!parse_corr_config_args(cmd_args(command), &config)) {
      snprintf(response, response_size, "ERR CONFIG_CORR ARG\r\n");
      return 0;
    }
    pa_pu_configure_correction(&config);
    snprintf(response, response_size,
             "OK CONFIG_CORR pkg=%u row=%u col=%u offset_en=%u offset_addr=0x%08x offset_adder=%u offset_mode=%u gain_en=%u gain_addr=0x%08x gain_clip=%u defect_en=%u\r\n",
             config.pkg_num,
             config.row_num,
             config.col_num,
             config.offset_enable ? 1u : 0u,
             config.offset_template_addr,
             config.offset_adder_value,
             config.offset_corr_mode,
             config.gain_enable ? 1u : 0u,
             config.gain_template_addr,
             config.gain_clipping_value,
             config.defect_enable ? 1u : 0u);
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_GIC")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_GIC", response, response_size)) {
      return 0;
    }
    /*
     * 下发 GIC 时序、行范围和 binning 配置。
     * 不带参数时使用 app_config.h 的默认值；带 key=value 时覆盖对应字段。
     */
    pa_pu_gic_config_t config = default_gic_config();
    if (!parse_gic_config_args(cmd_args(command), &config)) {
      snprintf(response, response_size, "ERR CONFIG_GIC ARG\r\n");
      return 0;
    }
    pa_pu_configure_gic(&config);
    snprintf(response, response_size,
             "OK CONFIG_GIC req=%u dout=%u line_time=%u oe_rise=%u oe_fall=%u start_row=%u end_row=%u binning=%u\r\n",
             config.req_code,
             config.dout_enable ? 1u : 0u,
             config.line_time_ns,
             config.oe_raising_edge_ns,
             config.oe_falling_edge_ns,
             config.start_row,
             config.end_row,
             config.binning_mode);
    return 0;
  }

  if (cmd_is(command, "START_GIC")) {
    if (!prepare_manual_hardware_action(ctx, "START_GIC", response, response_size)) {
      return 0;
    }
    /* 启动一次 GIC 操作，并等待 INT_VECTOR 中的 GIC 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_gic();
    write_start_result(response, response_size, "START_GIC", PA_PU_IRQ_GIC_END);
    return 0;
  }

  if (cmd_is(command, "STOP_GIC")) {
    if (calibration_task_request_stop()) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response,
               response_size,
               "OK STOP_GIC template_task=%s template_state=%s template_id=%u\r\n",
               calibration_task_kind_name(cal.kind),
               calibration_task_state_name(cal.state),
               cal.task_id);
      return 0;
    }
    if (!prepare_manual_hardware_action(ctx, "STOP_GIC", response, response_size)) {
      return 0;
    }
    /* 主要用于 xao scan 模式；其它模式下是否有效由 FPGA 决定。 */
    pa_pu_stop_gic();
    snprintf(response, response_size, "OK STOP_GIC\r\n");
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_ROIC")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_ROIC", response, response_size)) {
      return 0;
    }
    /*
     * 下发 ROIC 芯片寄存器和列范围配置，但不立即启动。
     * 不带参数时使用 app_config.h 的默认值；带 key=value 时覆盖对应字段。
     */
    pa_pu_roic_config_t config = default_roic_config();
    if (!parse_roic_config_args(cmd_args(command), &config)) {
      snprintf(response, response_size, "ERR CONFIG_ROIC ARG\r\n");
      return 0;
    }
    pa_pu_configure_roic(&config);
    snprintf(response, response_size,
             "OK CONFIG_ROIC start_col=%u end_col=%u binning=%u\r\n",
             config.start_col,
             config.end_col,
             config.binning_mode);
    return 0;
  }

  if (cmd_is(command, "START_ROIC")) {
    if (!prepare_manual_hardware_action(ctx, "START_ROIC", response, response_size)) {
      return 0;
    }
    /* 启动一次 ROIC 配置操作，并等待 INT_VECTOR 中的 ROIC 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_roic();
    write_start_result(response, response_size, "START_ROIC", PA_PU_IRQ_ROIC_END);
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_DYNC") || cmd_has_name(command, "CONFIG_DYNAMIC")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_DYNC", response, response_size)) {
      return 0;
    }

    /*
     * START_DYNC 是非阻塞底层调试命令，不受正式工作线程状态约束，因此这里必须再读
     * FPGA DYNC_STATE bit0，保证无论从哪条入口启动，运行中都不会改配置寄存器。
     */
    uint32_t hw_dync_state = pa_pu_read(PA_PU_DYNC_STATE_REG);
    if ((hw_dync_state & 0x1u) != 0u) {
      snprintf(response,
               response_size,
               "ERR CONFIG_DYNC BUSY dync_state=0x%08x hint=STOP_DYNC_or_STOP_TRANSFER\r\n",
               hw_dync_state);
      return 0;
    }

    /*
     * dynamic_ctrl 是 FPGA 侧动态工作模式的步骤表。
     * FPGA 正在运行时严格禁止修改；自然结束或 DYNC_STOP 完成后才更新缓存和寄存器。
     */
    dync_command_config_t dync_config;
    default_dync_command_config(ctx->fpga_mem, &dync_config);
    if (!parse_dync_config_args(cmd_args(command), &dync_config) ||
        dync_config.config.cycle_num == 0u) {
      snprintf(response, response_size, "ERR CONFIG_DYNC ARG\r\n");
      return 0;
    }
    int update_ret = ctx->work_mode != NULL
        ? work_mode_update_dynamic_config(ctx->work_mode, &dync_config.config)
        : 0;
    if (update_ret == -2) {
      snprintf(response, response_size, "ERR CONFIG_DYNC BUSY state=DYNAMIC_RUNNING hint=STOP_TRANSFER\r\n");
      return 0;
    }
    if (update_ret != 0) {
      snprintf(response, response_size, "ERR CONFIG_DYNC\r\n");
      return 0;
    }
    pa_pu_configure_dync(&dync_config.config);
    snprintf(response,
             response_size,
             "OK CONFIG_DYNC cycle=%u img_start=0x%08x img_end=0x%08x step0_h=0x%08x step0_l=0x%08x step1_h=0x%08x step1_l=0x%08x\r\n",
             dync_config.config.cycle_num,
             dync_config.config.image_start_addr,
             dync_config.config.image_end_addr,
             dync_config.config.step_cfg_h[0],
             dync_config.config.step_cfg_l[0],
             dync_config.config.step_cfg_h[1],
             dync_config.config.step_cfg_l[1]);
    return 0;
  }

  if (cmd_has_name(command, "START_DYNC")) {
    if (!prepare_manual_hardware_action(ctx, "START_DYNC", response, response_size)) {
      return 0;
    }

    dync_command_config_t dync_config;
    default_dync_command_config(ctx->fpga_mem, &dync_config);
    if (!parse_dync_config_args(cmd_args(command), &dync_config) ||
        dync_config.config.cycle_num == 0u) {
      snprintf(response, response_size, "ERR START_DYNC ARG\r\n");
      return 0;
    }

    if (dync_config.has_config_write) {
      pa_pu_configure_dync(&dync_config.config);
    }
    pa_pu_prepare_irq_wait();
    pa_pu_start_dync();
    /* 只做非阻塞底层调试启动，不读取 read-clear 中断，也不创建监控线程。 */
    uint32_t dync_state = pa_pu_read(PA_PU_DYNC_STATE_REG);
    uint32_t dync_end = pa_pu_read(PA_PU_DYNC_END_REG);
    uint32_t dync_debug_out = pa_pu_read(PA_PU_DYNC_DEBUG_OUT_REG);
    log_info("dynamic started wait=0 dync_state=0x%08x dync_end=0x%08x dync_debug_out=0x%08x",
             dync_state,
             dync_end,
             dync_debug_out);
    snprintf(response,
             response_size,
             "OK START_DYNC wait=0 dync_state=0x%08x dync_end=0x%08x dync_debug_out=0x%08x\r\n",
             dync_state,
             dync_end,
             dync_debug_out);
    return 0;
  }

  if (cmd_is(command, "STOP_DYNC")) {
    if (calibration_task_request_stop()) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response,
               response_size,
               "OK STOP_DYNC template_task=%s template_state=%s template_id=%u\r\n",
               calibration_task_kind_name(cal.kind),
               calibration_task_state_name(cal.state),
               cal.task_id);
      return 0;
    }
    if (!prepare_manual_hardware_action(ctx, "STOP_DYNC", response, response_size)) {
      return 0;
    }
    pa_pu_stop_dync();
    /*
     * STOP_DYNC 是写 FPGA 的 DYNC_STOP 寄存器。写完后立即读回状态，
     * 让上位机能看到 stop 后 state/end/debug/final_addr 的现场值。
     */
    usleep(10000u);
    uint32_t dync_state = pa_pu_read(PA_PU_DYNC_STATE_REG);
    uint32_t dync_end = pa_pu_read(PA_PU_DYNC_END_REG);
    uint32_t dync_debug_out = pa_pu_read(PA_PU_DYNC_DEBUG_OUT_REG);
    uint32_t final_img_addr = pa_pu_read(PA_PU_IMG_WR_FINAL_IMG_ADDR_REG);
    log_info("dynamic stop requested dync_state=0x%08x dync_end=0x%08x dync_debug_out=0x%08x final_img_addr=0x%08x",
             dync_state,
             dync_end,
             dync_debug_out,
             final_img_addr);
    snprintf(response,
             response_size,
             "OK STOP_DYNC dync_state=0x%08x dync_end=0x%08x dync_debug_out=0x%08x final_img_addr=0x%08x\r\n",
             dync_state,
             dync_end,
             dync_debug_out,
             final_img_addr);
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_IMG_UPLOAD") || cmd_has_name(command, "CONFIG_UPLOAD")) {
    if (!prepare_manual_hardware_action(ctx, "CONFIG_IMG_UPLOAD", response, response_size)) {
      return 0;
    }

    /*
     * 图片上传仅用于模板回传。默认 template=offset 使用 uio0；
     * template=gain 使用 uio1；addr=... 只保留给明确地址的现场调试。
     */
    img_upload_command_config_t upload_config = {
      .config = default_img_upload_config(ctx->fpga_mem),
      .wait_done = true,
      .source_name = "offset",
    };
    if (!parse_img_upload_args(cmd_args(command), &upload_config) ||
        !resolve_img_upload_template(ctx->fpga_mem, &upload_config)) {
      snprintf(response, response_size, "ERR CONFIG_IMG_UPLOAD ARG\r\n");
      return 0;
    }

    pa_pu_configure_img_upload(&upload_config.config);
    snprintf(response,
             response_size,
             "OK CONFIG_IMG_UPLOAD template=%s addr=0x%08x pkg=%u row=%u col=%u\r\n",
             upload_config.source_name,
             upload_config.config.image_addr,
             upload_config.config.pkg_num,
             upload_config.config.row_num,
             upload_config.config.col_num);
    return 0;
  }

  if (cmd_has_name(command, "START_IMG_UPLOAD") ||
      cmd_has_name(command, "IMG_UPLOAD") ||
      cmd_has_name(command, "UPLOAD_IMAGE")) {
    if (!prepare_manual_hardware_action(ctx, "START_IMG_UPLOAD", response, response_size)) {
      return 0;
    }

    /*
     * START_IMG_UPLOAD/IMG_UPLOAD 默认配置后启动并等待 bit6。
     * 需要只触发不等待时传 wait=0，方便排查上传模块是否能独立启动。
     */
    img_upload_command_config_t upload_config = {
      .config = default_img_upload_config(ctx->fpga_mem),
      .wait_done = true,
      .source_name = "offset",
    };
    if (!parse_img_upload_args(cmd_args(command), &upload_config) ||
        !resolve_img_upload_template(ctx->fpga_mem, &upload_config)) {
      snprintf(response, response_size, "ERR START_IMG_UPLOAD ARG\r\n");
      return 0;
    }

    pa_pu_configure_img_upload(&upload_config.config);
    pa_pu_prepare_irq_wait();
    pa_pu_start_img_upload();
    if (!upload_config.wait_done) {
      snprintf(response,
               response_size,
               "OK START_IMG_UPLOAD wait=0 template=%s addr=0x%08x pkg=%u row=%u col=%u\r\n",
               upload_config.source_name,
               upload_config.config.image_addr,
               upload_config.config.pkg_num,
               upload_config.config.row_num,
               upload_config.config.col_num);
      return 0;
    }

    uint32_t int_vector = 0;
    int ret = pa_pu_wait_int_vector(PA_PU_IRQ_IMG_UPLOAD_END, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
    if (ret > 0) {
      snprintf(response,
               response_size,
               "OK START_IMG_UPLOAD template=%s addr=0x%08x pkg=%u row=%u col=%u int_vector=0x%08x\r\n",
               upload_config.source_name,
               upload_config.config.image_addr,
               upload_config.config.pkg_num,
               upload_config.config.row_num,
               upload_config.config.col_num,
               int_vector);
    } else if (ret == 0) {
      uint32_t state = pa_pu_read(PA_PU_IMG_UPLOAD_STATE_REG);
      uint32_t end = pa_pu_read(PA_PU_IMG_UPLOAD_END_REG);
      uint32_t dfx = pa_pu_read(PA_PU_IMG_UPLOAD_DFX_REG);
      snprintf(response,
               response_size,
               "ERR START_IMG_UPLOAD TIMEOUT int_vector=0x%08x expect=0x%08x state=0x%08x end=0x%08x dfx=0x%08x\r\n",
               int_vector,
               PA_PU_IRQ_IMG_UPLOAD_END,
               state,
               end,
               dfx);
    } else {
      snprintf(response, response_size, "ERR START_IMG_UPLOAD IRQ_WAIT\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "START_CORR")) {
    if (!prepare_manual_hardware_action(ctx, "START_CORR", response, response_size)) {
      return 0;
    }
    /* 启动 FPGA 图像校正，并等待 INT_VECTOR 中的 IMG_CORR 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_correction();
    write_start_result(response, response_size, "START_CORR", PA_PU_IRQ_IMG_CORR_END);
    return 0;
  }

  if (cmd_is(command, "START_CORR_GIC") || cmd_is(command, "START_CORR_THEN_GIC")) {
    if (!prepare_manual_hardware_action(ctx, "START_CORR_GIC", response, response_size)) {
      return 0;
    }
    /*
     * 启动图像校正后立即启动 GIC，不等待 IMG_CORR 完成后再启动 GIC。
     * 随后等待 IMG_CORR 和 GIC 两个完成中断 bit 都出现。
     */
    pa_pu_prepare_irq_wait();
    pa_pu_start_correction();
    pa_pu_start_gic();
    write_start_combo_result(response,
                             response_size,
                             "START_CORR_GIC",
                             PA_PU_IRQ_IMG_CORR_END | PA_PU_IRQ_GIC_END);
    return 0;
  }

  if (cmd_is(command, "START_CONTINUOUS") ||
      cmd_is(command, "START_DYNAMIC_WORK") ||
      cmd_is(command, "START_DYNAMIC")) {
    const char* start_name = cmd_is(command, "START_DYNAMIC")
        ? "START_DYNAMIC"
        : "START_CONTINUOUS";
    if (!prepare_manual_hardware_action(ctx, start_name, response, response_size)) {
      return 0;
    }

    /*
     * 正式 Continuous 只启动一次 FPGA dynamic_ctrl。逐帧数据是否输出由
     * GIC_DOUT_EN 决定，ARM 不再读取每帧中断或触发模板 IMG_UPLOAD。
     */
    int ret = work_mode_start_dynamic(ctx->work_mode);
    if (ret == -2) {
      snprintf(response, response_size, "ERR %s BUSY\r\n", start_name);
      return 0;
    }
    if (ret != 0) {
      snprintf(response, response_size, "ERR %s\r\n", start_name);
      return 0;
    }

    work_mode_status_t status;
    work_mode_get_status(ctx->work_mode, &status);
    snprintf(response,
             response_size,
             "OK %s state=%s ring_frame_stride=0x%lx ring_frame_count=%lu\r\n",
             start_name,
             work_mode_state_name(status.state),
             (unsigned long)status.ddr_frame_stride,
             (unsigned long)status.ddr_frame_count);
    return 0;
  }

  if (cmd_is(command, "SEND_IMAGE") || cmd_is(command, "SEND_SINGLE")) {
    if (!prepare_manual_hardware_action(ctx, "SEND_IMAGE", response, response_size)) {
      return 0;
    }
    /*
     * 图像数据不经过 ARM 发送。这里仅通知 PA 端从 FPGA 图像物理地址启动写图流程，
     * 光口传输由 PA/FPGA 逻辑完成。
     *
     * SEND_IMAGE 是早期调试命令；SEND_SINGLE 保留一次 IMG_WR 调试/手动上图语义。
     * START_CONTINUOUS 已转入正式 Dynamic 工作线程，不再经过本分支。
     */
    pa_pu_prepare_irq_wait();
    if (ctx->fpga_mem == NULL || ctx->fpga_mem->image_phys_base == 0) {
      snprintf(response, response_size, "ERR %s NO_IMAGE_UIO\r\n", command);
      return 0;
    }
    uint32_t image_addr = ctx->fpga_mem->image_phys_base;
    pa_pu_start_image_write(image_addr);
    uint32_t int_vector = 0;
    int ret = pa_pu_wait_int_vector(PA_PU_IRQ_IMG_WR_END, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
    if (ret > 0) {
      snprintf(response, response_size, "OK %s addr=0x%08x int_vector=0x%08x\r\n", command, image_addr, int_vector);
    } else if (ret == 0) {
      snprintf(response, response_size, "ERR %s TIMEOUT addr=0x%08x int_vector=0x%08x\r\n", command, image_addr, int_vector);
    } else {
      snprintf(response, response_size, "ERR %s IRQ_WAIT addr=0x%08x\r\n", command, image_addr);
    }
    return 0;
  }

  if (cmd_is(command, "STOP_TRANSFER") ||
      cmd_is(command, "STOP_DYNAMIC_WORK") ||
      cmd_is(command, "STOP_DYNAMIC")) {
    const char* stop_name = cmd_is(command, "STOP_DYNAMIC")
        ? "STOP_DYNAMIC"
        : (cmd_is(command, "STOP_DYNAMIC_WORK") ? "STOP_DYNAMIC_WORK" : "STOP_TRANSFER");
    if (calibration_task_request_stop()) {
      cal_task_status_t cal;
      calibration_task_get_status(&cal);
      snprintf(response,
               response_size,
               "OK %s template_task=%s template_state=%s template_id=%u\r\n",
               stop_name,
               calibration_task_kind_name(cal.kind),
               calibration_task_state_name(cal.state),
               cal.task_id);
      return 0;
    }
    /* Dynamic 停止只写 DYNC_STOP，并等待 FPGA dynamic state 回到 idle。 */
    work_mode_status_t status;
    int ret = work_mode_stop_dynamic(ctx->work_mode, &status);
    if (ret != 0) {
      snprintf(response,
               response_size,
               "ERR %s state=%s phase=%s error=%d dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x\r\n",
               stop_name,
               work_mode_state_name(status.state),
               work_mode_phase_name(status.last_phase),
               status.last_error,
               status.dynamic_dync_state,
               status.dynamic_dync_end,
               status.dynamic_dync_debug_out);
      return 0;
    }
    snprintf(response,
             response_size,
             "OK %s state=%s dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x\r\n",
             stop_name,
             work_mode_state_name(status.state),
             status.dynamic_dync_state,
             status.dynamic_dync_end,
             status.dynamic_dync_debug_out);
    return 0;
  }

  if (cmd_is(command, "STOP_WORK")) {
    /* 停止请求不等待模板任务完成，命令线程保持可响应；退出时再统一 join。 */
    bool template_stopping = calibration_task_request_stop();
    work_mode_stop(ctx->work_mode);
    snprintf(response, response_size, "OK STOP_WORK template_state=%s\r\n", template_stopping ? "STOPPING" : "IDLE");
    return 0;
  }

  if (cmd_is(command, "START_WORK")) {
    if (calibration_task_is_active()) {
      snprintf(response, response_size, "ERR START_WORK BUSY template_task_running=1\r\n");
      return 0;
    }
    /* 调试用：重新启动后台 Static Idle 线程。 */
    if (work_mode_start(ctx->work_mode) != 0) {
      snprintf(response, response_size, "ERR START_WORK\r\n");
      return 0;
    }
    snprintf(response, response_size, "OK START_WORK\r\n");
    return 0;
  }

  if (cmd_is(command, "QUIT")) {
    /* 只退出 ARM 应用，不复位 FPGA/PA。 */
    ctx->should_quit = true;
    snprintf(response, response_size, "OK QUIT\r\n");
    return 0;
  }

  log_warn("unknown command: %s", command);
  snprintf(response, response_size, "ERR UNKNOWN\r\n");
  return 0;
}

/* -------------------- 正式二进制协议基础命令 -------------------- */

enum {
  PA_BINARY_CMD_HELLO = 0x0001,
  PA_BINARY_CMD_PING = 0x0002,
  PA_BINARY_CMD_STATUS = 0x0003,
  PA_BINARY_CMD_VERSION = 0x0004,
  PA_BINARY_CMD_START_STATIC_CAPTURE = 0x0200,
  PA_BINARY_CMD_START_DYNAMIC = 0x0210,
  PA_BINARY_CMD_STOP_DYNAMIC = 0x0211,
  PA_BINARY_CMD_QUERY_DYNAMIC = 0x0212,
  PA_BINARY_CMD_CAL_OFFSET_BEGIN = 0x0300,
  PA_BINARY_CMD_CAL_OFFSET_CAPTURE = 0x0301,
  PA_BINARY_CMD_CAL_OFFSET_BUILD = 0x0302,
  PA_BINARY_CMD_CAL_OFFSET_CANCEL = 0x0303,
  PA_BINARY_CMD_CAL_GAIN_BEGIN = 0x0304,
  PA_BINARY_CMD_CAL_GAIN_CAPTURE = 0x0305,
  PA_BINARY_CMD_CAL_GAIN_BUILD = 0x0306,
  PA_BINARY_CMD_CAL_GAIN_CANCEL = 0x0307,
  PA_BINARY_CMD_CAL_STATUS = 0x0308,
  PA_BINARY_CMD_IMG_UPLOAD_CONFIG = 0x0500,
  PA_BINARY_CMD_IMG_UPLOAD_START = 0x0501,
  PA_BINARY_CMD_IMG_UPLOAD_QUERY = 0x0502,
  PA_BINARY_CMD_GET_CONFIG_GROUP = 0x0103,
  PA_BINARY_CMD_SET_CONFIG_GROUP = 0x0104,
  PA_BINARY_CMD_DUMP_REGS = 0x0105,
  PA_BINARY_CMD_WRITE_REG = 0x0106,
  PA_BINARY_TLV_ERROR_CODE = 0x0002,
  PA_BINARY_TLV_APP_VERSION = 0x0300,
  PA_BINARY_TLV_BUILD_TIME = 0x0301,
};

static int binary_payload_append_tlv(uint8_t* payload,
                                     size_t capacity,
                                     size_t* length,
                                     uint16_t type,
                                     const void* value,
                                     uint16_t value_length);

static const uint8_t* binary_find_tlv(const uint8_t* payload,
                                      size_t payload_length,
                                      uint16_t type,
                                      uint16_t* value_length) {
  size_t pos = 0u;
  while (pos < payload_length) {
    if (payload_length - pos < 4u) return NULL;
    uint16_t item_type = (uint16_t)payload[pos] | ((uint16_t)payload[pos + 1u] << 8u);
    uint16_t item_length = (uint16_t)payload[pos + 2u] | ((uint16_t)payload[pos + 3u] << 8u);
    pos += 4u;
    if (payload_length - pos < item_length) return NULL;
    if (item_type == type) {
      if (value_length != NULL) *value_length = item_length;
      return payload + pos;
    }
    pos += item_length;
  }
  return NULL;
}

static bool binary_read_u32_tlv(const uint8_t* payload,
                                size_t payload_length,
                                uint16_t type,
                                uint32_t* value) {
  uint16_t length = 0u;
  const uint8_t* data = binary_find_tlv(payload, payload_length, type, &length);
  if (data == NULL || value == NULL || (length != 1u && length != 2u && length != 4u)) return false;
  uint32_t result = data[0];
  if (length >= 2u) result |= (uint32_t)data[1] << 8u;
  if (length == 4u) result |= (uint32_t)data[2] << 16u | (uint32_t)data[3] << 24u;
  *value = result;
  return true;
}

/*
 * 正式协议的配置组映射。
 * item_id 固定为协议字段，不把 INI 文本名称带到 RS422 上，便于后续产品复用。
 */
typedef struct {
  uint16_t id;
  const char* name;
} binary_config_item_t;

static const binary_config_item_t binary_config_groups[][32] = {
  {
    {0x1000u, "static.idle_clean_interval_ms"},
    {0x1001u, "static.exposure_window_ms"},
    {0x1002u, "static.dark_window_ms"},
    {0u, NULL}
  },
  {
    {0x0300u, "corr.offset_en"}, {0x0301u, "corr.gain_en"},
    {0x0302u, "corr.defect_en"}, {0x0303u, "corr.offset_adder_value"},
    {0x0304u, "corr.gain_clipping_value"}, {0x0305u, "corr.pkg_num"},
    {0x0306u, "corr.row_num"}, {0x0307u, "corr.col_num"},
    {0x0308u, "corr.offset_template_addr"}, {0x0309u, "corr.offset_corr_mode"},
    {0x030au, "corr.gain_template_addr"}, {0u, NULL}
  },
  {
    {0x0400u, "gic.req_code"}, {0x0401u, "gic.dout_en"},
    {0x0402u, "gic.line_time_ns"}, {0x0403u, "gic.start_row"},
    {0x0404u, "gic.end_row"}, {0x0405u, "gic.binning"},
    {0x0406u, "gic.oe_rise_ns"}, {0x0407u, "gic.oe_fall_ns"}, {0u, NULL}
  },
  {
    {0x0500u, "roic.start_col"}, {0x0501u, "roic.end_col"},
    {0x0502u, "roic.binning"},
    {0x0503u, "roic.reg_00"}, {0x0504u, "roic.reg_02"},
    {0x0505u, "roic.reg_05"}, {0x0506u, "roic.reg_06"},
    {0x0507u, "roic.reg_07"}, {0x0508u, "roic.reg_09"},
    {0x0509u, "roic.reg_0a"}, {0x050au, "roic.reg_0b"},
    {0x050bu, "roic.reg_0c"}, {0x050cu, "roic.reg_0d"},
    {0x050du, "roic.reg_0e"}, {0x050eu, "roic.reg_0f"},
    {0x050fu, "roic.reg_10"}, {0x0510u, "roic.reg_11"},
    {0x0511u, "roic.reg_17"}, {0x0512u, "roic.reg_24"},
    {0x0513u, "roic.reg_28"}, {0x0514u, "roic.reg_2d"},
    {0x0515u, "roic.reg_3b"}, {0u, NULL}
  },
  {
    {0x0600u, "dynamic.cycle"}, {0x0601u, "dynamic.image_start_addr"},
    {0x0602u, "dynamic.image_end_addr"},
    {0x0603u, "dynamic.start_timeout_ms"},
    {0x0604u, "dynamic.state_poll_interval_ms"},
    {0x0605u, "dynamic.stop_timeout_ms"},
    {0x2100u, "dynamic.step0_h"}, {0x2101u, "dynamic.step0_l"},
    {0x2110u, "dynamic.step1_h"}, {0x2111u, "dynamic.step1_l"},
    {0x2120u, "dynamic.step2_h"}, {0x2121u, "dynamic.step2_l"},
    {0x2130u, "dynamic.step3_h"}, {0x2131u, "dynamic.step3_l"},
    {0x2140u, "dynamic.step4_h"}, {0x2141u, "dynamic.step4_l"},
    {0x2150u, "dynamic.step5_h"}, {0x2151u, "dynamic.step5_l"},
    {0x2160u, "dynamic.step6_h"}, {0x2161u, "dynamic.step6_l"},
    {0x2170u, "dynamic.step7_h"}, {0x2171u, "dynamic.step7_l"},
    {0x2180u, "dynamic.step8_h"}, {0x2181u, "dynamic.step8_l"},
    {0x2190u, "dynamic.step9_h"}, {0x2191u, "dynamic.step9_l"},
    {0u, NULL}
  }
};

static const binary_config_item_t* binary_config_group(unsigned group) {
  if (group == 0u || group > sizeof(binary_config_groups) / sizeof(binary_config_groups[0])) {
    return NULL;
  }
  return binary_config_groups[group - 1u];
}

static const char* binary_config_item_name(unsigned group, uint16_t id) {
  const binary_config_item_t* items = binary_config_group(group);
  if (items == NULL) return NULL;
  for (size_t i = 0u; items[i].name != NULL; ++i) {
    if (items[i].id == id) return items[i].name;
  }
  return NULL;
}

static int binary_append_config_group(const pa_runtime_config_t* config,
                                      unsigned group,
                                      uint8_t* payload,
                                      size_t capacity,
                                      size_t* length) {
  const binary_config_item_t* items = binary_config_group(group);
  if (config == NULL || items == NULL) return -1;
  char text[32];
  for (size_t i = 0u; items[i].name != NULL; ++i) {
    if (config_store_get_item(config, items[i].name, text, sizeof(text)) != 0) return -1;
    uint32_t value = (uint32_t)strtoul(text, NULL, 0);
    uint8_t encoded[4] = {
      (uint8_t)(value & 0xffu), (uint8_t)((value >> 8u) & 0xffu),
      (uint8_t)((value >> 16u) & 0xffu), (uint8_t)((value >> 24u) & 0xffu)
    };
    if (binary_payload_append_tlv(payload, capacity, length, items[i].id,
                                  encoded, sizeof(encoded)) != 0) return -1;
  }
  return 0;
}

static int binary_apply_config_group(command_context_t* ctx,
                                     const uint8_t* input,
                                     size_t input_length,
                                     unsigned group) {
  if (ctx == NULL || ctx->runtime_config == NULL || input == NULL ||
      binary_config_group(group) == NULL || input_length < 2u) return -1;
  uint16_t requested_group = (uint16_t)input[0] | ((uint16_t)input[1] << 8u);
  if (requested_group != group) return -1;
  size_t pos = 2u;
  while (pos < input_length) {
    if (input_length - pos < 8u) return -1;
    uint16_t id = (uint16_t)input[pos] | ((uint16_t)input[pos + 1u] << 8u);
    uint16_t len = (uint16_t)input[pos + 2u] | ((uint16_t)input[pos + 3u] << 8u);
    if (len != 4u || input_length - pos < 4u + len) return -1;
    const char* name = binary_config_item_name(group, id);
    if (name == NULL) return -1;
    uint32_t value = (uint32_t)input[pos + 4u]
                   | ((uint32_t)input[pos + 5u] << 8u)
                   | ((uint32_t)input[pos + 6u] << 16u)
                   | ((uint32_t)input[pos + 7u] << 24u);
    char text[16];
    snprintf(text, sizeof(text), "0x%08x", value);
    if (config_store_set_item(ctx->runtime_config, name, text) != 0) return -1;
    pos += 8u;
  }
  if (!work_mode_allows_hardware_action(ctx->work_mode) ||
      apply_runtime_config(ctx) != 0 ||
      config_store_save(ctx->config_file, ctx->runtime_config) != 0) return -1;
  return 0;
}

static int binary_payload_append_tlv(uint8_t* payload,
                                     size_t capacity,
                                     size_t* length,
                                     uint16_t type,
                                     const void* value,
                                     uint16_t value_length) {
  if (payload == NULL || length == NULL || value == NULL ||
      *length + 4u + value_length > capacity) {
    return -1;
  }
  payload[*length + 0u] = (uint8_t)(type & 0xffu);
  payload[*length + 1u] = (uint8_t)((type >> 8u) & 0xffu);
  payload[*length + 2u] = (uint8_t)(value_length & 0xffu);
  payload[*length + 3u] = (uint8_t)((value_length >> 8u) & 0xffu);
  memcpy(payload + *length + 4u, value, value_length);
  *length += 4u + value_length;
  return 0;
}

static int binary_payload_append_u32(uint8_t* payload,
                                     size_t capacity,
                                     size_t* length,
                                     uint16_t type,
                                     uint32_t value) {
  uint8_t encoded[4] = {
    (uint8_t)(value & 0xffu),
    (uint8_t)((value >> 8u) & 0xffu),
    (uint8_t)((value >> 16u) & 0xffu),
    (uint8_t)((value >> 24u) & 0xffu),
  };
  return binary_payload_append_tlv(payload, capacity, length, type, encoded, sizeof(encoded));
}

static int binary_payload_append_string(uint8_t* payload,
                                        size_t capacity,
                                        size_t* length,
                                        uint16_t type,
                                        const char* value) {
  if (value == NULL) {
    value = "";
  }
  size_t value_length = strlen(value);
  if (value_length > UINT16_MAX) {
    return -1;
  }
  return binary_payload_append_tlv(payload,
                                   capacity,
                                   length,
                                   type,
                                   value,
                                   (uint16_t)value_length);
}

static int binary_response(command_context_t* ctx,
                           const pa_protocol_frame_view_t* request,
                           uint8_t message_type,
                           const uint8_t* payload,
                           uint16_t payload_length,
                           uint8_t* response,
                           size_t response_capacity,
                           size_t* response_length) {
  (void)ctx;
  return pa_protocol_encode(message_type,
                             0u,
                             request->cmd,
                             request->seq,
                             payload,
                             payload_length,
                             response,
                             response_capacity,
                             response_length);
}

static int binary_error_response(command_context_t* ctx,
                                 const pa_protocol_frame_view_t* request,
                                 uint16_t error_code,
                                 uint8_t* response,
                                 size_t response_capacity,
                                 size_t* response_length) {
  uint8_t payload[8];
  size_t payload_length = 0u;
  if (binary_payload_append_u32(payload,
                                sizeof(payload),
                                &payload_length,
                                PA_BINARY_TLV_ERROR_CODE,
                                error_code) != 0) {
    return -1;
  }
  return binary_response(ctx,
                          request,
                          PA_PROTOCOL_MSG_ERR,
                          payload,
                          (uint16_t)payload_length,
                          response,
                          response_capacity,
                          response_length);
}

static int binary_append_work_status(const work_mode_status_t* status,
                                     uint8_t* payload,
                                     size_t capacity,
                                     size_t* length) {
  if (status == NULL) {
    return -1;
  }
  const struct {
    uint16_t type;
    uint32_t value;
  } fields[] = {
    {0x0009u, status->capture_id},
    {0x1012u, status->capture_id},
    {0x1013u, status->last_bright_addr},
    {0x1014u, status->last_dark_addr},
    {0x1015u, status->last_int_vector},
    {0x2003u, status->dynamic_dync_state},
    {0x2004u, pa_pu_read(PA_PU_IMG_WR_FINAL_IMG_ADDR_REG)},
    {0x2005u, (uint32_t)status->ddr_frame_count},
    {0x2006u, status->last_int_vector},
    {0x2007u, status->dynamic_dync_end},
    {0x2008u, status->dynamic_dync_debug_out},
  };
  for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    if (binary_payload_append_u32(payload, capacity, length,
                                  fields[i].type, fields[i].value) != 0) {
      return -1;
    }
  }
  return 0;
}

static int binary_append_calibration_status(uint8_t* payload,
                                            size_t capacity,
                                            size_t* length) {
  cal_task_status_t task;
  cal_gain_status_t gain;
  calibration_task_get_status(&task);
  calibration_gain_get_status(&gain);
  const struct {
    uint16_t type;
    uint32_t value;
  } fields[] = {
    {0x3200u, task.task_id},
    {0x3201u, (uint32_t)task.kind},
    {0x3202u, (uint32_t)task.state},
    {0x3203u, (uint32_t)task.last_error},
    {0x3204u, task.progress_current},
    {0x3205u, task.progress_total},
    {0x3206u, task.gain_level},
    {0x3207u, task.stop_requested ? 1u : 0u},
    {0x3210u, gain.active ? 1u : 0u},
    {0x3211u, gain.level_count},
    {0x3212u, gain.levels_ready},
    {0x3213u, gain.frames_per_level},
    {0x3215u, gain.bad_pixel_count},
  };
  for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    if (binary_payload_append_u32(payload, capacity, length,
                                  fields[i].type, fields[i].value) != 0) return -1;
  }
  uint32_t threshold_bits = 0u;
  memcpy(&threshold_bits, &gain.defect_threshold, sizeof(threshold_bits));
  if (binary_payload_append_u32(payload, capacity, length, 0x3214u, threshold_bits) != 0) return -1;
  for (uint32_t i = 0u; i < gain.level_count && i < CAL_GAIN_MAX_LEVELS; ++i) {
    if (binary_payload_append_u32(payload, capacity, length,
                                  (uint16_t)(0x3300u + i), gain.levels[i].level) != 0 ||
        binary_payload_append_u32(payload, capacity, length,
                                  (uint16_t)(0x3340u + i), gain.levels[i].ready ? 1u : 0u) != 0 ||
        binary_payload_append_u32(payload, capacity, length,
                                  (uint16_t)(0x3380u + i), gain.levels[i].median) != 0) return -1;
  }
  return 0;
}

static bool binary_prepare_hardware_action(command_context_t* ctx, const char* name) {
  char response[256];
  response[0] = '\0';
  return prepare_manual_hardware_action(ctx, name, response, sizeof(response));
}

int command_handle_binary(command_context_t* ctx,
                          const pa_protocol_frame_view_t* request,
                          uint8_t* response,
                          size_t response_capacity,
                          size_t* response_length) {
  if (ctx == NULL || request == NULL || response == NULL || response_length == NULL) {
    return -1;
  }
  *response_length = 0u;
  if (request->msg_type != PA_PROTOCOL_MSG_REQ) {
    return binary_error_response(ctx, request, 0x0002u, response, response_capacity, response_length);
  }

  uint8_t payload[PA_PROTOCOL_MAX_PAYLOAD];
  size_t payload_length = 0u;
  pa_pu_status_t pa_status;
  memset(&pa_status, 0, sizeof(pa_status));

  switch (request->cmd) {
    case PA_BINARY_CMD_GET_CONFIG_GROUP: {
      if (request->payload_len != 2u) {
        return binary_error_response(ctx, request, 0x0002u, response, response_capacity, response_length);
      }
      unsigned group = (unsigned)request->payload[0] | ((unsigned)request->payload[1] << 8u);
      if (binary_config_group(group) == NULL || ctx->runtime_config == NULL) {
        return binary_error_response(ctx, request, 0x0001u, response, response_capacity, response_length);
      }
      if (binary_append_config_group(ctx->runtime_config, group, payload, sizeof(payload), &payload_length) != 0) {
        return binary_error_response(ctx, request, 0x000Eu, response, response_capacity, response_length);
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_SET_CONFIG_GROUP: {
      if (request->payload_len < 2u) {
        return binary_error_response(ctx, request, 0x0002u, response, response_capacity, response_length);
      }
      unsigned group = (unsigned)request->payload[0] | ((unsigned)request->payload[1] << 8u);
      if (binary_config_group(group) == NULL ||
          binary_apply_config_group(ctx, request->payload, request->payload_len, group) != 0) {
        return binary_error_response(ctx, request, 0x0008u, response, response_capacity, response_length);
      }
      if (binary_append_config_group(ctx->runtime_config, group, payload, sizeof(payload), &payload_length) != 0) {
        return binary_error_response(ctx, request, 0x000Eu, response, response_capacity, response_length);
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_DUMP_REGS: {
      if (request->payload_len != 0u) {
        return binary_error_response(ctx, request, 0x0002u, response, response_capacity, response_length);
      }
      uint16_t offsets[128];
      uint32_t values[128];
      const size_t count = pa_pu_read_register_snapshot(offsets, values, 128u);
      for (size_t i = 0u; i < count; ++i) {
        if (binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                      offsets[i], values[i]) != 0) {
          return binary_error_response(ctx, request, 0x000Eu, response, response_capacity, response_length);
        }
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_WRITE_REG: {
      if (request->payload_len != 8u) {
        return binary_error_response(ctx, request, 0x0002u, response, response_capacity, response_length);
      }
      const uint16_t offset = (uint16_t)request->payload[0]
                            | ((uint16_t)request->payload[1] << 8u);
      const uint32_t value = (uint32_t)request->payload[4]
                           | ((uint32_t)request->payload[5] << 8u)
                           | ((uint32_t)request->payload[6] << 16u)
                           | ((uint32_t)request->payload[7] << 24u);
      pa_pu_write(offset, value);
      if (binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    offset, pa_pu_read(offset)) != 0) {
        return binary_error_response(ctx, request, 0x000Eu, response, response_capacity, response_length);
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }


    case PA_BINARY_CMD_HELLO:
    case PA_BINARY_CMD_VERSION:
      pa_pu_read_status(&pa_status);
      if (binary_payload_append_string(payload, sizeof(payload), &payload_length,
                                        PA_BINARY_TLV_APP_VERSION, APP_VERSION) != 0 ||
          binary_payload_append_string(payload, sizeof(payload), &payload_length,
                                       PA_BINARY_TLV_BUILD_TIME, APP_BUILD_TIME) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0302u, pa_status.pa_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0303u, pa_status.pa_build_information) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0304u, pa_status.adapted_main_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0305u, pa_status.adapted_gic_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0306u, pa_status.adapted_roic_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x030Au, pa_status.pa_pu_com_version) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);

    case PA_BINARY_CMD_PING:
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             NULL, 0u, response, response_capacity, response_length);

    case PA_BINARY_CMD_STATUS: {
      work_mode_status_t work_status;
      memset(&work_status, 0, sizeof(work_status));
      pa_pu_read_status(&pa_status);
      work_mode_get_status(ctx->work_mode, &work_status);

      const struct {
        uint16_t type;
        uint32_t value;
      } fields[] = {
        {0x0200u, (uint32_t)work_status.mode},
        {0x0201u, (uint32_t)work_status.state},
        {0x0202u, (uint32_t)work_status.last_error},
        {0x0204u, work_status.capture_id},
        {0x0205u, (uint32_t)work_status.ddr_frame_count},
        {0x0206u, work_status.last_dark_addr},
        {0x0207u, work_status.last_bright_addr},
        {0x0209u, pa_status.img_wr_state},
        {0x020Au, pa_status.img_wr_end},
        {0x020Bu, pa_status.img_corr_state},
        {0x020Cu, pa_status.img_corr_end},
        {0x020Du, pa_status.gic_state},
        {0x020Eu, pa_status.gic_end},
        {0x020Fu, pa_status.gic_dfx},
        {0x0213u, pa_status.dync_state},
        {0x0214u, pa_status.img_upload_state},
      };
      for (size_t i = 0u; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                      fields[i].type, fields[i].value) != 0) {
          return -1;
        }
      }
      /* STATUS 同步返回版本信息，帮助窗口和 SDK 无需额外发送 VERSION 请求。 */
      if (binary_payload_append_string(payload, sizeof(payload), &payload_length,
                                       PA_BINARY_TLV_APP_VERSION, APP_VERSION) != 0 ||
          binary_payload_append_string(payload, sizeof(payload), &payload_length,
                                       PA_BINARY_TLV_BUILD_TIME, APP_BUILD_TIME) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0302u, pa_status.pa_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0303u, pa_status.pa_build_information) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0304u, pa_status.adapted_main_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0305u, pa_status.adapted_gic_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0306u, pa_status.adapted_roic_board_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0307u, pa_status.adapted_reserved_board_0_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0308u, pa_status.adapted_reserved_board_1_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x0309u, pa_status.adapted_reserved_board_2_version) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length,
                                    0x030Au, pa_status.pa_pu_com_version) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_START_STATIC_CAPTURE: {
      /* 正式静态采图命令自动确保 Static Idle 工作线程已启动。 */
      if (work_mode_start(ctx->work_mode) != 0) {
        return binary_error_response(ctx, request, 0x0008u,
                                      response, response_capacity, response_length);
      }
      work_mode_status_t work_status;
      memset(&work_status, 0, sizeof(work_status));
      int ret = work_mode_start_static_idle_capture(ctx->work_mode, &work_status);
      if (ret != 0) {
        return binary_error_response(ctx, request,
                                     ret == -2 ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      payload_length = 0u;
      if (binary_append_work_status(&work_status, payload, sizeof(payload), &payload_length) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_START_DYNAMIC: {
      int ret = work_mode_start_dynamic(ctx->work_mode);
      if (ret != 0) {
        return binary_error_response(ctx, request,
                                     ret == -2 ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      work_mode_status_t work_status;
      memset(&work_status, 0, sizeof(work_status));
      work_mode_get_status(ctx->work_mode, &work_status);
      payload_length = 0u;
      if (binary_append_work_status(&work_status, payload, sizeof(payload), &payload_length) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_STOP_DYNAMIC: {
      work_mode_status_t work_status;
      memset(&work_status, 0, sizeof(work_status));
      int ret = work_mode_stop_dynamic(ctx->work_mode, &work_status);
      if (ret != 0) {
        return binary_error_response(ctx, request, 0x000Eu,
                                     response, response_capacity, response_length);
      }
      payload_length = 0u;
      if (binary_append_work_status(&work_status, payload, sizeof(payload), &payload_length) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_QUERY_DYNAMIC: {
      work_mode_status_t work_status;
      memset(&work_status, 0, sizeof(work_status));
      work_mode_get_status(ctx->work_mode, &work_status);
      payload_length = 0u;
      if (binary_append_work_status(&work_status, payload, sizeof(payload), &payload_length) != 0) {
        return -1;
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_OFFSET_BEGIN: {
      uint32_t total_frames = 0u;
      uint32_t valid_frames = 0u;
      uint32_t mode = 0u;
      cal_gain_status_t gain_status;
      calibration_gain_get_status(&gain_status);
      if (calibration_task_is_active() || gain_status.active ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3000u, &total_frames) ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3001u, &valid_frames) ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3002u, &mode) ||
          total_frames == 0u || valid_frames == 0u || valid_frames > total_frames ||
          total_frames > 65535u || mode > 1u ||
          (mode == 0u && (total_frames != 1u || valid_frames != 1u))) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      ctx->offset_calibration_configured = true;
      ctx->offset_total_frames = total_frames;
      ctx->offset_valid_frames = valid_frames;
      ctx->offset_calibration_mode = (uint8_t)mode;
      if (binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x3000u, total_frames) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x3001u, valid_frames) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x3002u, mode) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_OFFSET_CAPTURE: {
      if (request->payload_len != 0u || !ctx->offset_calibration_configured ||
          !binary_prepare_hardware_action(ctx, "CAL_OFFSET_CAPTURE")) {
        return binary_error_response(ctx, request, 0x0008u,
                                     response, response_capacity, response_length);
      }
      int ret = -1;
      if (ctx->offset_calibration_mode == 0u) {
        ret = calibration_task_start_make_offset(ctx->fpga_mem);
      } else {
        /* mode=1 为历史协议值；实现已改为静态逐帧采集，避免 Dynamic 连续出图丢帧。 */
        ret = calibration_task_start_dynamic_offset(ctx->fpga_mem,
                                                    NULL,
                                                    false,
                                                    ctx->offset_total_frames,
                                                    ctx->offset_valid_frames);
      }
      if (ret != 0) {
        return binary_error_response(ctx, request, ret == -2 ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_OFFSET_BUILD: {
      cal_task_status_t task;
      calibration_task_get_status(&task);
      if (request->payload_len != 0u || !ctx->offset_calibration_configured ||
          (task.kind != CAL_TASK_MAKE_OFFSET && task.kind != CAL_TASK_DYNAMIC_OFFSET) ||
          task.state != CAL_TASK_SUCCEEDED) {
        return binary_error_response(ctx, request,
                                     calibration_task_is_active() ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      /* 当前实现由后台采集任务原子生成并加载模板；BUILD 用作完成确认边界。 */
      ctx->offset_calibration_configured = false;
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_OFFSET_CANCEL:
      if (request->payload_len != 0u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      {
        cal_task_status_t task;
        calibration_task_get_status(&task);
        if (calibration_task_is_active() &&
            task.kind != CAL_TASK_MAKE_OFFSET && task.kind != CAL_TASK_DYNAMIC_OFFSET) {
          return binary_error_response(ctx, request, 0x0008u,
                                       response, response_capacity, response_length);
        }
        (void)calibration_task_request_stop();
      }
      ctx->offset_calibration_configured = false;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             NULL, 0u, response, response_capacity, response_length);

    case PA_BINARY_CMD_CAL_GAIN_BEGIN: {
      uint32_t level_count = 0u;
      uint32_t frames_per_level = 0u;
      uint32_t threshold_bits = 0u;
      uint16_t level_bytes_length = 0u;
      const uint8_t* level_bytes = binary_find_tlv(request->payload,
                                                   request->payload_len,
                                                   0x3110u,
                                                   &level_bytes_length);
      if (ctx->offset_calibration_configured || calibration_task_is_active() ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3100u, &level_count) ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3101u, &frames_per_level) ||
          !binary_read_u32_tlv(request->payload, request->payload_len, 0x3102u, &threshold_bits) ||
          level_count < 2u || level_count > CAL_GAIN_MAX_LEVELS ||
          level_bytes == NULL || level_bytes_length != level_count * 4u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      uint32_t levels[CAL_GAIN_MAX_LEVELS];
      for (uint32_t i = 0u; i < level_count; ++i) {
        const uint8_t* item = level_bytes + i * 4u;
        levels[i] = (uint32_t)item[0] | ((uint32_t)item[1] << 8u)
                  | ((uint32_t)item[2] << 16u) | ((uint32_t)item[3] << 24u);
      }
      float threshold = 0.0f;
      memcpy(&threshold, &threshold_bits, sizeof(threshold));
      if (!isfinite(threshold) || threshold <= 0.0f || threshold > 1.0f) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      calibration_gain_cancel();
      if (calibration_gain_begin(levels, level_count, frames_per_level, threshold) != 0) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_GAIN_CAPTURE: {
      uint32_t level = 0u;
      if (!binary_read_u32_tlv(request->payload, request->payload_len, 0x3120u, &level) ||
          !binary_prepare_hardware_action(ctx, "CAL_GAIN_CAPTURE")) {
        return binary_error_response(ctx, request, 0x0008u,
                                     response, response_capacity, response_length);
      }
      int ret = calibration_task_start_gain_capture(ctx->fpga_mem, level);
      if (ret != 0) {
        return binary_error_response(ctx, request, ret == -2 ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_GAIN_BUILD: {
      if (request->payload_len != 0u || !binary_prepare_hardware_action(ctx, "CAL_GAIN_BUILD")) {
        return binary_error_response(ctx, request, 0x0008u,
                                     response, response_capacity, response_length);
      }
      int ret = calibration_task_start_gain_build(ctx->fpga_mem);
      if (ret != 0) {
        return binary_error_response(ctx, request, ret == -2 ? 0x0008u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_CAL_GAIN_CANCEL:
      if (request->payload_len != 0u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      {
        cal_task_status_t task;
        calibration_task_get_status(&task);
        if (calibration_task_is_active() &&
            task.kind != CAL_TASK_GAIN_CAPTURE && task.kind != CAL_TASK_GAIN_BUILD &&
            task.kind != CAL_TASK_MAKE_GAIN) {
          return binary_error_response(ctx, request, 0x0008u,
                                       response, response_capacity, response_length);
        }
        if (!calibration_task_request_stop()) calibration_gain_cancel();
      }
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             NULL, 0u, response, response_capacity, response_length);

    case PA_BINARY_CMD_CAL_STATUS:
      if (request->payload_len != 0u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      if (binary_append_calibration_status(payload, sizeof(payload), &payload_length) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);

    case PA_BINARY_CMD_IMG_UPLOAD_CONFIG: {
      if (!binary_prepare_hardware_action(ctx, "IMG_UPLOAD_CONFIG")) {
        return binary_error_response(ctx, request, 0x0008u,
                                     response, response_capacity, response_length);
      }
      uint32_t template_kind = 0u;
      uint32_t value = 0u;
      img_upload_command_config_t upload = {
        .config = default_img_upload_config(ctx->fpga_mem),
        .wait_done = true,
        .source_name = "offset",
      };
      if (!binary_read_u32_tlv(request->payload, request->payload_len, 0x5000u, &template_kind) ||
          template_kind > 1u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      upload.source_name = template_kind == 0u ? "offset" : "gain";
      if (binary_read_u32_tlv(request->payload, request->payload_len, 0x5001u, &value)) {
        upload.config.image_addr = value;
        upload.source_name = "custom";
      }
      if (binary_read_u32_tlv(request->payload, request->payload_len, 0x5002u, &value)) {
        if (value == 0u || value > UINT16_MAX) {
          return binary_error_response(ctx, request, 0x0002u,
                                       response, response_capacity, response_length);
        }
        upload.config.row_num = (uint16_t)value;
      }
      if (binary_read_u32_tlv(request->payload, request->payload_len, 0x5003u, &value)) {
        if (value == 0u || value > UINT16_MAX) {
          return binary_error_response(ctx, request, 0x0002u,
                                       response, response_capacity, response_length);
        }
        upload.config.col_num = (uint16_t)value;
      }
      if (binary_read_u32_tlv(request->payload, request->payload_len, 0x5004u, &value)) {
        if (value == 0u || value > UINT16_MAX) {
          return binary_error_response(ctx, request, 0x0002u,
                                       response, response_capacity, response_length);
        }
        upload.config.pkg_num = (uint16_t)value;
      }
      if (!resolve_img_upload_template(ctx->fpga_mem, &upload) ||
          upload.config.row_num == 0u || upload.config.col_num == 0u || upload.config.pkg_num == 0u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      pa_pu_configure_img_upload(&upload.config);
      if (binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5000u, template_kind) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5001u, upload.config.image_addr) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5002u, upload.config.row_num) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5003u, upload.config.col_num) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5004u, upload.config.pkg_num) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_IMG_UPLOAD_START: {
      if (request->payload_len != 0u || !binary_prepare_hardware_action(ctx, "IMG_UPLOAD_START")) {
        return binary_error_response(ctx, request, 0x0008u,
                                     response, response_capacity, response_length);
      }
      pa_pu_prepare_irq_wait();
      pa_pu_start_img_upload();
      uint32_t int_vector = 0u;
      int ret = pa_pu_wait_int_vector(PA_PU_IRQ_IMG_UPLOAD_END, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
      if (ret <= 0) {
        return binary_error_response(ctx, request, ret == 0 ? 0x0005u : 0x000Eu,
                                     response, response_capacity, response_length);
      }
      if (binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5005u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_STATE_REG)) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5006u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_END_REG)) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5007u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_DFX_REG)) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5008u,
                                    int_vector) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);
    }

    case PA_BINARY_CMD_IMG_UPLOAD_QUERY:
      if (request->payload_len != 0u) {
        return binary_error_response(ctx, request, 0x0002u,
                                     response, response_capacity, response_length);
      }
      if (binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5005u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_STATE_REG)) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5006u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_END_REG)) != 0 ||
          binary_payload_append_u32(payload, sizeof(payload), &payload_length, 0x5007u,
                                    pa_pu_read(PA_PU_IMG_UPLOAD_DFX_REG)) != 0) return -1;
      return binary_response(ctx, request, PA_PROTOCOL_MSG_DONE,
                             payload, (uint16_t)payload_length,
                             response, response_capacity, response_length);

    default:
      return binary_error_response(ctx, request, 0x0001u,
                                   response, response_capacity, response_length);
  }
}
