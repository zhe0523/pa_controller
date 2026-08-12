#include "command_handler.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
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
  {"img_corr_state", PA_PU_IMG_CORR_STATE_REG},
  {"img_corr_end", PA_PU_IMG_CORR_END_REG},
  {"img_corr_dfx", PA_PU_IMG_CORR_DFX_REG},
  {"img_corr_debug_in", PA_PU_IMG_CORR_DEBUG_IN_REG},
  {"img_corr_debug_out", PA_PU_IMG_CORR_DEBUG_OUT_REG},
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

static void write_status_response(char* response, size_t response_size) {
  pa_pu_status_t status;
  pa_pu_read_status(&status);

  /* STATUS 响应保持短格式，方便串口助手查看，也方便上位机按字段解析。 */
  snprintf(response, response_size,
           "OK STATUS pa_version=0x%08x pa_build_information=0x%08x adapted_main_board_version=0x%08x adapted_gic_board_version=0x%08x adapted_roic_board_version=0x%08x adapted_reserved_board_0_version=0x%08x adapted_reserved_board_1_version=0x%08x adapted_reserved_board_2_version=0x%08x pa_pu_com_version=0x%08x pa_rst_init_state=0x%08x wr_state=0x%08x wr_end=0x%08x corr_state=0x%08x corr_end=0x%08x gic_state=0x%08x gic_end=0x%08x gic_dfx=0x%08x roic_state=0x%08x roic_end=0x%08x roic_dfx=0x%08x\r\n",
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
           status.img_corr_state,
           status.img_corr_end,
           status.gic_state,
           status.gic_end,
           status.gic_dfx,
           status.roic_state,
           status.roic_end,
           status.roic_dfx);
}

static void write_no_hw_status_response(char* response, size_t response_size) {
  snprintf(response, response_size,
           "OK STATUS pa_version=0x00000000 pa_build_information=0x00000000 adapted_main_board_version=0x00000000 adapted_gic_board_version=0x00000000 adapted_roic_board_version=0x00000000 adapted_reserved_board_0_version=0x00000000 adapted_reserved_board_1_version=0x00000000 adapted_reserved_board_2_version=0x00000000 pa_pu_com_version=0x00000000 pa_rst_init_state=0x00000000 wr_state=0x00000000 wr_end=0x00000000 corr_state=0x00000000 corr_end=0x00000000 gic_state=0x00000000 gic_end=0x00000000 gic_dfx=0x00000000 roic_state=0x00000000 roic_end=0x00000000 roic_dfx=0x00000000\r\n");
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

static void write_version_response(bool hardware_enabled, char* response, size_t response_size) {
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

  if (hardware_enabled) {
    pa_pu_read_status(&status);
  }

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
           "OK VERSION app_version=%s pa_version=%s pa_build_information=%s adapted_main_board_version=%s adapted_gic_board_version=%s adapted_roic_board_version=%s adapted_reserved_board_0_version=%s adapted_reserved_board_1_version=%s adapted_reserved_board_2_version=%s pa_pu_com_version=%s\r\n",
           APP_VERSION,
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

static bool reject_no_hw(const command_context_t* ctx, char* response, size_t response_size) {
  if (ctx != NULL && !ctx->hardware_enabled) {
    snprintf(response, response_size, "ERR NO_HW\r\n");
    return true;
  }
  return false;
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
    snprintf(response, response_size, "ERR %s TIMEOUT int_vector=0x%08x\r\n", command, int_vector);
  } else {
    snprintf(response, response_size, "ERR %s IRQ_WAIT\r\n", command);
  }
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
    if (ctx->hardware_enabled) {
      write_status_response(response, response_size);
    } else {
      write_no_hw_status_response(response, response_size);
    }
    return 0;
  }

  if (cmd_is(command, "VERSION") || cmd_is(command, "GET_VERSION")) {
    /* 返回 ARM 应用版本和 PA/FPGA 版本寄存器，不读取 read-clear 的 INT_VECTOR。 */
    write_version_response(ctx->hardware_enabled, response, response_size);
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

  if (is_read_reg_command(command)) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }

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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }

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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 从文件系统加载 offset/gain 模板，适合设备重启后恢复已有校正模板。 */
    if (template_load_files(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK LOAD_TEMPLATE\r\n");
    } else {
      snprintf(response, response_size, "ERR LOAD_TEMPLATE\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_OFFSET")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 用当前暗场图像生成 offset 模板；上位机应先确保当前帧是有效暗场。 */
    if (template_make_offset(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK MAKE_OFFSET\r\n");
    } else {
      snprintf(response, response_size, "ERR MAKE_OFFSET\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "MAKE_GAIN")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 用当前亮场图像和已有 offset 模板生成 gain 模板；需先执行或加载 offset。 */
    if (template_make_gain(ctx->fpga_mem) == 0) {
      pa_pu_configure_templates();
      snprintf(response, response_size, "OK MAKE_GAIN\r\n");
    } else {
      snprintf(response, response_size, "ERR MAKE_GAIN\r\n");
    }
    return 0;
  }

  if (cmd_is(command, "CONFIG_TEMPLATE")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 只重新下发模板地址/尺寸配置，不重新生成或加载模板内容。 */
    pa_pu_configure_templates();
    snprintf(response, response_size, "OK CONFIG_TEMPLATE\r\n");
    return 0;
  }

  if (cmd_has_name(command, "CONFIG_GIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
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
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动一次 GIC 操作，并等待 INT_VECTOR 中的 GIC 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_gic();
    write_start_result(response, response_size, "START_GIC", PA_PU_IRQ_GIC_END);
    return 0;
  }

  if (cmd_is(command, "STOP_GIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 主要用于 xao scan 模式；其它模式下是否有效由 FPGA 决定。 */
    pa_pu_stop_gic();
    snprintf(response, response_size, "OK STOP_GIC\r\n");
    return 0;
  }

  if (cmd_is(command, "CONFIG_ROIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /*
     * 下发 ROIC 芯片寄存器默认值和列范围配置，但不立即启动。
     * ROIC_DEFAULT_REG_* 现在是占位值，真板联调前应按 panel 参数覆盖。
     */
    pa_pu_configure_roic_defaults();
    snprintf(response, response_size, "OK CONFIG_ROIC\r\n");
    return 0;
  }

  if (cmd_is(command, "START_ROIC")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动一次 ROIC 配置操作，并等待 INT_VECTOR 中的 ROIC 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_roic();
    write_start_result(response, response_size, "START_ROIC", PA_PU_IRQ_ROIC_END);
    return 0;
  }

  if (cmd_is(command, "START_CORR")) {
    if (reject_no_hw(ctx, response, response_size)) {
      return 0;
    }
    /* 启动 FPGA 图像校正，并等待 INT_VECTOR 中的 IMG_CORR 完成 bit。 */
    pa_pu_prepare_irq_wait();
    pa_pu_start_correction();
    write_start_result(response, response_size, "START_CORR", PA_PU_IRQ_IMG_CORR_END);
    return 0;
  }

  if (cmd_is(command, "SEND_IMAGE") || cmd_is(command, "SEND_SINGLE") || cmd_is(command, "START_CONTINUOUS")) {
    /*
     * 图像数据不经过 ARM 发送。这里仅通知 PA 端从 FPGA 图像物理地址启动写图流程，
     * 光口传输由 PA/FPGA 逻辑完成。
     *
     * SEND_IMAGE 是早期调试命令；SEND_SINGLE / START_CONTINUOUS 是当前 Qt 上位机
     * 的客户入口命令。当前 FPGA 侧尚未区分单帧和持续上图，因此两者暂时都触发
     * 同一次写图流程，后续硬件支持持续模式后只需要在这里拆分实现。
     */
    if (ctx->hardware_enabled) {
      pa_pu_prepare_irq_wait();
      pa_pu_start_image_write(FPGA_IMAGE_PTR);
      uint32_t int_vector = 0;
      int ret = pa_pu_wait_int_vector(PA_PU_IRQ_IMG_WR_END, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
      if (ret > 0) {
        snprintf(response, response_size, "OK %s addr=0x%08x int_vector=0x%08x\r\n", command, FPGA_IMAGE_PTR, int_vector);
      } else if (ret == 0) {
        snprintf(response, response_size, "ERR %s TIMEOUT addr=0x%08x int_vector=0x%08x\r\n", command, FPGA_IMAGE_PTR, int_vector);
      } else {
        snprintf(response, response_size, "ERR %s IRQ_WAIT addr=0x%08x\r\n", command, FPGA_IMAGE_PTR);
      }
    } else {
      snprintf(response, response_size, "OK %s addr=0x%08x\r\n", command, FPGA_IMAGE_PTR);
    }
    return 0;
  }

  if (cmd_is(command, "STOP_TRANSFER")) {
    /*
     * 当前 PA/FPGA 暂未提供明确的停流寄存器。先兼容上位机按钮流程：
     * ARM 确认收到停止请求，但不额外操作硬件。
     */
    snprintf(response, response_size, "OK STOP_TRANSFER\r\n");
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
