#include "config_store.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app_config.h"
#include "log.h"

static void trim(char* text) {
  if (text == NULL) return;
  char* begin = text;
  while (isspace((unsigned char)*begin)) ++begin;
  if (begin != text) memmove(text, begin, strlen(begin) + 1u);
  size_t length = strlen(text);
  while (length > 0u && isspace((unsigned char)text[length - 1u])) text[--length] = '\0';
}

static bool parse_u32(const char* text, uint32_t* value) {
  if (text == NULL || value == NULL || *text == '\0') return false;
  errno = 0;
  char* end = NULL;
  unsigned long parsed = strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX) return false;
  *value = (uint32_t)parsed;
  return true;
}

static bool parse_bool(const char* text, bool* value) {
  uint32_t number = 0;
  if (parse_u32(text, &number)) {
    *value = number != 0u;
    return true;
  }
  if (strcasecmp(text, "true") == 0 || strcasecmp(text, "yes") == 0) {
    *value = true;
    return true;
  }
  if (strcasecmp(text, "false") == 0 || strcasecmp(text, "no") == 0) {
    *value = false;
    return true;
  }
  return false;
}

static void copy_string(char* destination, size_t size, const char* source) {
  if (destination == NULL || size == 0u) return;
  snprintf(destination, size, "%s", source != NULL ? source : "");
}

static int ensure_parent_dir(const char* path) {
  const char* slash = strrchr(path, '/');
  if (slash == NULL) return 0;
  size_t length = (size_t)(slash - path);
  if (length == 0u || length >= 512u) return -1;
  char parent[512];
  memcpy(parent, path, length);
  parent[length] = '\0';
  if (mkdir(parent, 0755) != 0 && errno != EEXIST) return -1;
  return 0;
}

static void set_gic_common(pa_pu_gic_config_t* config, const pa_pu_gic_config_t* source) {
  config->line_time_ns = source->line_time_ns;
  config->oe_raising_edge_ns = source->oe_raising_edge_ns;
  config->oe_falling_edge_ns = source->oe_falling_edge_ns;
  config->start_row = source->start_row;
  config->end_row = source->end_row;
  config->binning_mode = source->binning_mode;
}

int config_store_defaults(const fpga_mem_t* fpga_mem, pa_runtime_config_t* config) {
  if (fpga_mem == NULL || config == NULL || !fpga_mem_is_open(fpga_mem)) return -1;
  memset(config, 0, sizeof(*config));

  config->gic = (pa_pu_gic_config_t){
    .req_code = GIC_DEFAULT_REQ_CODE,
    .dout_enable = GIC_DEFAULT_DOUT_EN != 0u,
    .line_time_ns = GIC_DEFAULT_LINE_TIME_NS,
    .oe_raising_edge_ns = GIC_DEFAULT_OE_RISE_NS,
    .oe_falling_edge_ns = GIC_DEFAULT_OE_FALL_NS,
    .start_row = GIC_DEFAULT_START_ROW,
    .end_row = GIC_DEFAULT_END_ROW,
    .binning_mode = GIC_DEFAULT_BINNING,
  };
  config->roic = (pa_pu_roic_config_t){
    .start_col = ROIC_DEFAULT_START_COL, .end_col = ROIC_DEFAULT_END_COL,
    .binning_mode = ROIC_DEFAULT_BINNING,
    .reg_00 = ROIC_DEFAULT_REG_00, .reg_02 = ROIC_DEFAULT_REG_02,
    .reg_05 = ROIC_DEFAULT_REG_05, .reg_06 = ROIC_DEFAULT_REG_06,
    .reg_07 = ROIC_DEFAULT_REG_07, .reg_09 = ROIC_DEFAULT_REG_09,
    .reg_0a = ROIC_DEFAULT_REG_0A, .reg_0b = ROIC_DEFAULT_REG_0B,
    .reg_0c = ROIC_DEFAULT_REG_0C, .reg_0d = ROIC_DEFAULT_REG_0D,
    .reg_0e = ROIC_DEFAULT_REG_0E, .reg_0f = ROIC_DEFAULT_REG_0F,
    .reg_10 = ROIC_DEFAULT_REG_10, .reg_11 = ROIC_DEFAULT_REG_11,
    .reg_17 = ROIC_DEFAULT_REG_17, .reg_24 = ROIC_DEFAULT_REG_24,
    .reg_28 = ROIC_DEFAULT_REG_28, .reg_2d = ROIC_DEFAULT_REG_2D,
    .reg_3b = ROIC_DEFAULT_REG_3B,
  };
  config->corr = (pa_pu_corr_config_t){
    .pkg_num = CORR_DEFAULT_PKG_NUM, .row_num = CORR_DEFAULT_ROW_NUM,
    .col_num = CORR_DEFAULT_COL_NUM,
    .offset_enable = CORR_DEFAULT_OFFSET_EN != 0u,
    .offset_template_addr = fpga_mem->offset_phys_base,
    .offset_adder_value = CORR_DEFAULT_OFFSET_ADDER_VALUE,
    .offset_corr_mode = CORR_DEFAULT_OFFSET_CORR_MODE,
    .gain_enable = CORR_DEFAULT_GAIN_EN != 0u,
    .gain_template_addr = fpga_mem->gain_phys_base,
    .gain_clipping_value = CORR_DEFAULT_GAIN_CLIPPING_VALUE,
    .defect_enable = CORR_DEFAULT_DEFECT_EN != 0u,
  };
  work_mode_default_static_idle_config(&config->static_idle);
  config->static_idle.bright_corr = config->corr;
  config->static_idle.bright_corr.offset_enable = false;
  config->static_idle.bright_corr.gain_enable = false;
  config->static_idle.bright_corr.defect_enable = false;
  config->static_idle.dark_corr = config->corr;
  set_gic_common(&config->static_idle.clean_gic, &config->gic);
  set_gic_common(&config->static_idle.bright_gic, &config->gic);
  set_gic_common(&config->static_idle.dark_gic, &config->gic);

  dynamic_mode_config_t dynamic;
  if (dynamic_mode_default_config(fpga_mem, &dynamic) != 0) return -1;
  config->dync = dynamic.dync;
  config->dynamic_start_timeout_ms = dynamic.start_timeout_ms;
  config->dynamic_state_poll_interval_ms = dynamic.state_poll_interval_ms;
  config->dynamic_stop_timeout_ms = dynamic.stop_timeout_ms;
  copy_string(config->offset_file, sizeof(config->offset_file), TEMPLATE_OFFSET_FILE);
  copy_string(config->gain_file, sizeof(config->gain_file), TEMPLATE_GAIN_FILE);
  copy_string(config->cal_gain_dir, sizeof(config->cal_gain_dir), CAL_GAIN_DIR);
  return 0;
}

static void write_roic(FILE* file, const pa_pu_roic_config_t* r) {
  fprintf(file, "start_col=%u\nend_col=%u\nbinning=%u\n", r->start_col, r->end_col, r->binning_mode);
  fprintf(file, "reg_00=0x%04x\nreg_02=0x%04x\nreg_05=0x%04x\nreg_06=0x%04x\nreg_07=0x%04x\n", r->reg_00, r->reg_02, r->reg_05, r->reg_06, r->reg_07);
  fprintf(file, "reg_09=0x%04x\nreg_0a=0x%04x\nreg_0b=0x%04x\nreg_0c=0x%04x\nreg_0d=0x%04x\nreg_0e=0x%04x\nreg_0f=0x%04x\nreg_10=0x%04x\nreg_11=0x%04x\nreg_17=0x%04x\nreg_24=0x%04x\nreg_28=0x%04x\nreg_2d=0x%04x\nreg_3b=0x%04x\n", r->reg_09, r->reg_0a, r->reg_0b, r->reg_0c, r->reg_0d, r->reg_0e, r->reg_0f, r->reg_10, r->reg_11, r->reg_17, r->reg_24, r->reg_28, r->reg_2d, r->reg_3b);
}

int config_store_save(const char* path, const pa_runtime_config_t* c) {
  if (path == NULL || c == NULL) return -1;
  if (ensure_parent_dir(path) != 0) return -1;
  FILE* file = fopen(path, "w");
  if (file == NULL) return -1;
  fprintf(file, "# pa_controller runtime configuration\nversion=1\n\n[image]\nwidth=%u\nheight=%u\n\n[gic]\nreq_code=%u\ndout_en=%u\nline_time_ns=%u\noe_rise_ns=%u\noe_fall_ns=%u\nstart_row=%u\nend_row=%u\nbinning=%u\n", IMAGE_WIDTH, IMAGE_HEIGHT, c->gic.req_code, c->gic.dout_enable ? 1u : 0u, c->gic.line_time_ns, c->gic.oe_raising_edge_ns, c->gic.oe_falling_edge_ns, c->gic.start_row, c->gic.end_row, c->gic.binning_mode);
  fputs("\n[roic]\n", file); write_roic(file, &c->roic);
  fprintf(file, "\n[corr]\npkg_num=%u\nrow_num=%u\ncol_num=%u\noffset_en=%u\noffset_adder_value=%u\noffset_corr_mode=%u\ngain_en=%u\ngain_clipping_value=%u\ndefect_en=%u\n", c->corr.pkg_num, c->corr.row_num, c->corr.col_num, c->corr.offset_enable ? 1u : 0u, c->corr.offset_adder_value, c->corr.offset_corr_mode, c->corr.gain_enable ? 1u : 0u, c->corr.gain_clipping_value, c->corr.defect_enable ? 1u : 0u);
  fprintf(file, "\n[static_idle]\nidle_clean_interval_ms=%u\nexposure_window_ms=%u\ndark_window_ms=%u\n\n[dynamic]\ncycle=%u\nimage_start_addr=0x%08x\nimage_end_addr=0x%08x\nstart_timeout_ms=%u\nstate_poll_interval_ms=%u\nstop_timeout_ms=%u\n", c->static_idle.idle_clean_interval_ms, c->static_idle.exposure_window_ms, c->static_idle.dark_window_ms, c->dync.cycle_num, c->dync.image_start_addr, c->dync.image_end_addr, c->dynamic_start_timeout_ms, c->dynamic_state_poll_interval_ms, c->dynamic_stop_timeout_ms);
  for (unsigned i = 0; i < PA_PU_DYNC_STEP_COUNT; ++i) fprintf(file, "step%u_h=0x%08x\nstep%u_l=%u\n", i, c->dync.step_cfg_h[i], i, c->dync.step_cfg_l[i]);
  fprintf(file, "\n[template]\noffset_file=%s\ngain_file=%s\ncal_gain_dir=%s\n", c->offset_file, c->gain_file, c->cal_gain_dir);
  int write_error = ferror(file);
  int close_error = fclose(file);
  int result = write_error || close_error ? -1 : 0;
  return result;
}

static void apply_key(pa_runtime_config_t* c, const char* section, const char* key, const char* value) {
  uint32_t n = 0; bool b = false;
  if (strcmp(section, "gic") == 0 && parse_u32(value, &n)) {
    if (strcmp(key, "req_code") == 0) c->gic.req_code = (uint8_t)n;
    else if (strcmp(key, "dout_en") == 0) c->gic.dout_enable = n != 0u;
    else if (strcmp(key, "line_time_ns") == 0) c->gic.line_time_ns = n;
    else if (strcmp(key, "oe_rise_ns") == 0) c->gic.oe_raising_edge_ns = n;
    else if (strcmp(key, "oe_fall_ns") == 0) c->gic.oe_falling_edge_ns = n;
    else if (strcmp(key, "start_row") == 0) c->gic.start_row = (uint16_t)n;
    else if (strcmp(key, "end_row") == 0) c->gic.end_row = (uint16_t)n;
    else if (strcmp(key, "binning") == 0) c->gic.binning_mode = (uint8_t)n;
    return;
  }
  if (strcmp(section, "roic") == 0 && parse_u32(value, &n)) {
    if (strcmp(key, "start_col") == 0) c->roic.start_col = (uint16_t)n;
    else if (strcmp(key, "end_col") == 0) c->roic.end_col = (uint16_t)n;
    else if (strcmp(key, "binning") == 0) c->roic.binning_mode = (uint8_t)n;
    else if (strncmp(key, "reg_", 4) == 0) {
      uint32_t reg = strtoul(key + 4, NULL, 16);
      uint16_t* fields[] = { &c->roic.reg_00, &c->roic.reg_02, &c->roic.reg_05, &c->roic.reg_06, &c->roic.reg_07, &c->roic.reg_09, &c->roic.reg_0a, &c->roic.reg_0b, &c->roic.reg_0c, &c->roic.reg_0d, &c->roic.reg_0e, &c->roic.reg_0f, &c->roic.reg_10, &c->roic.reg_11, &c->roic.reg_17, &c->roic.reg_24, &c->roic.reg_28, &c->roic.reg_2d, &c->roic.reg_3b };
      const unsigned regs[] = {0x00,0x02,0x05,0x06,0x07,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x17,0x24,0x28,0x2d,0x3b};
      for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); ++i) if (reg == regs[i]) *fields[i] = (uint16_t)n;
    }
    return;
  }
  if (strcmp(section, "corr") == 0 && parse_u32(value, &n)) {
    if (strcmp(key, "pkg_num") == 0) c->corr.pkg_num = (uint16_t)n; else if (strcmp(key, "row_num") == 0) c->corr.row_num = (uint16_t)n; else if (strcmp(key, "col_num") == 0) c->corr.col_num = (uint16_t)n; else if (strcmp(key, "offset_en") == 0) c->corr.offset_enable = n != 0u; else if (strcmp(key, "offset_adder_value") == 0) c->corr.offset_adder_value = (uint16_t)n; else if (strcmp(key, "offset_corr_mode") == 0) c->corr.offset_corr_mode = (uint8_t)n; else if (strcmp(key, "gain_en") == 0) c->corr.gain_enable = n != 0u; else if (strcmp(key, "gain_clipping_value") == 0) c->corr.gain_clipping_value = (uint16_t)n; else if (strcmp(key, "defect_en") == 0) c->corr.defect_enable = n != 0u;
    return;
  }
  if (strcmp(section, "static_idle") == 0 && parse_u32(value, &n)) { if (strcmp(key, "idle_clean_interval_ms") == 0) c->static_idle.idle_clean_interval_ms = n; else if (strcmp(key, "exposure_window_ms") == 0) c->static_idle.exposure_window_ms = n; else if (strcmp(key, "dark_window_ms") == 0) c->static_idle.dark_window_ms = n; return; }
  if (strcmp(section, "dynamic") == 0 && parse_u32(value, &n)) {
    if (strcmp(key, "cycle") == 0) c->dync.cycle_num = n;
    else if (strcmp(key, "image_start_addr") == 0) c->dync.image_start_addr = n;
    else if (strcmp(key, "image_end_addr") == 0) c->dync.image_end_addr = n;
    else if (strcmp(key, "start_timeout_ms") == 0) c->dynamic_start_timeout_ms = n;
    else if (strcmp(key, "state_poll_interval_ms") == 0) c->dynamic_state_poll_interval_ms = n;
    else if (strcmp(key, "stop_timeout_ms") == 0) c->dynamic_stop_timeout_ms = n;
    else if (strncmp(key, "step", 4) == 0) {
      char* end = NULL;
      unsigned index = (unsigned)strtoul(key + 4, &end, 10);
      if (index < PA_PU_DYNC_STEP_COUNT && end != NULL && strcmp(end, "_h") == 0) c->dync.step_cfg_h[index] = n;
      else if (index < PA_PU_DYNC_STEP_COUNT && end != NULL && strcmp(end, "_l") == 0) c->dync.step_cfg_l[index] = n;
    }
    return;
  }
  if (strcmp(section, "template") == 0) { if (strcmp(key, "offset_file") == 0) copy_string(c->offset_file, sizeof(c->offset_file), value); else if (strcmp(key, "gain_file") == 0) copy_string(c->gain_file, sizeof(c->gain_file), value); else if (strcmp(key, "cal_gain_dir") == 0) copy_string(c->cal_gain_dir, sizeof(c->cal_gain_dir), value); }
  (void)b;
}

int config_store_load_or_create(const char* path, const fpga_mem_t* fpga_mem, pa_runtime_config_t* config) {
  if (path == NULL || config_store_defaults(fpga_mem, config) != 0) return -1;
  FILE* file = fopen(path, "r");
  if (file == NULL && errno == ENOENT) { if (config_store_save(path, config) != 0) { log_warn("config file create failed path=%s errno=%d", path, errno); return -1; } log_info("config file created path=%s", path); return 1; }
  if (file == NULL) return -1;
  char line[512], section[64] = "";
  while (fgets(line, sizeof(line), file) != NULL) { trim(line); if (line[0] == '\0' || line[0] == '#') continue; if (line[0] == '[') { char* end = strchr(line, ']'); if (end != NULL) { *end = '\0'; copy_string(section, sizeof(section), line + 1); } continue; } char* equals = strchr(line, '='); if (equals == NULL) continue; *equals = '\0'; trim(line); trim(equals + 1); apply_key(config, section, line, equals + 1); }
  fclose(file);
  set_gic_common(&config->static_idle.clean_gic, &config->gic);
  set_gic_common(&config->static_idle.bright_gic, &config->gic);
  set_gic_common(&config->static_idle.dark_gic, &config->gic);
  config->static_idle.clean_gic.dout_enable = false;
  config->static_idle.bright_gic.dout_enable = true;
  config->static_idle.dark_gic.dout_enable = true;
  config->static_idle.bright_corr = config->corr;
  config->static_idle.bright_corr.offset_enable = false;
  config->static_idle.bright_corr.gain_enable = false;
  config->static_idle.bright_corr.defect_enable = false;
  config->static_idle.dark_corr = config->corr;
  if (config_store_validate(config, fpga_mem) != 0) return -1;
  log_info("config file loaded path=%s", path);
  return 0;
}

int config_store_validate(const pa_runtime_config_t* c, const fpga_mem_t* fpga_mem) {
  if (c == NULL || fpga_mem == NULL || c->gic.start_row > c->gic.end_row || c->roic.start_col > c->roic.end_col || c->corr.row_num == 0u || c->corr.col_num == 0u || c->dync.image_start_addr > c->dync.image_end_addr || c->corr.offset_template_addr != fpga_mem->offset_phys_base || c->corr.gain_template_addr != fpga_mem->gain_phys_base) return -1;
  return 0;
}

int config_store_summary(const pa_runtime_config_t* c, char* out, size_t size) { if (c == NULL || out == NULL || size == 0u) return -1; int n = snprintf(out, size, "gic=%u/%u/%u roic=%u-%u corr=%u/%u/%u static=%u/%u/%u dynamic_cycle=%u", c->gic.req_code, c->gic.dout_enable ? 1u : 0u, c->gic.line_time_ns, c->roic.start_col, c->roic.end_col, c->corr.offset_enable ? 1u : 0u, c->corr.gain_enable ? 1u : 0u, c->corr.defect_enable ? 1u : 0u, c->static_idle.idle_clean_interval_ms, c->static_idle.exposure_window_ms, c->static_idle.dark_window_ms, c->dync.cycle_num); return n >= 0 && (size_t)n < size ? 0 : -1; }

static bool is_bool_item(const char* item) {
  return strcmp(item, "gic.dout_en") == 0 ||
         strcmp(item, "corr.offset_en") == 0 ||
         strcmp(item, "corr.gain_en") == 0 ||
         strcmp(item, "corr.defect_en") == 0;
}

/* 将 dynamic.stepN_h/l 映射到配置数组，避免 RS422 命令层携带完整步骤表。 */
static bool dynamic_item_index(const char* item, unsigned* index, bool* high) {
  const char* p = NULL;
  if (strncmp(item, "dynamic.step", 12) != 0) return false;
  p = item + 12;
  if (!isdigit((unsigned char)*p)) return false;
  char* end = NULL;
  unsigned n = (unsigned)strtoul(p, &end, 10);
  if (n >= PA_PU_DYNC_STEP_COUNT || end == NULL || (end[0] != '_' || (end[1] != 'h' && end[1] != 'l') || end[2] != '\0')) return false;
  *index = n;
  *high = end[1] == 'h';
  return true;
}

int config_store_get_item(const pa_runtime_config_t* c,
                          const char* item,
                          char* value,
                          size_t size) {
  if (c == NULL || item == NULL || value == NULL || size == 0u) return -1;
  uint32_t n = 0;
  if (strcmp(item, "gic.req_code") == 0) n = c->gic.req_code;
  else if (strcmp(item, "gic.dout_en") == 0) n = c->gic.dout_enable;
  else if (strcmp(item, "gic.line_time_ns") == 0) n = c->gic.line_time_ns;
  else if (strcmp(item, "gic.oe_rise_ns") == 0) n = c->gic.oe_raising_edge_ns;
  else if (strcmp(item, "gic.oe_fall_ns") == 0) n = c->gic.oe_falling_edge_ns;
  else if (strcmp(item, "gic.start_row") == 0) n = c->gic.start_row;
  else if (strcmp(item, "gic.end_row") == 0) n = c->gic.end_row;
  else if (strcmp(item, "gic.binning") == 0) n = c->gic.binning_mode;
  else if (strcmp(item, "roic.start_col") == 0) n = c->roic.start_col;
  else if (strcmp(item, "roic.end_col") == 0) n = c->roic.end_col;
  else if (strcmp(item, "roic.binning") == 0) n = c->roic.binning_mode;
  else if (strcmp(item, "roic.reg_00") == 0) n = c->roic.reg_00;
  else if (strcmp(item, "roic.reg_02") == 0) n = c->roic.reg_02;
  else if (strcmp(item, "roic.reg_05") == 0) n = c->roic.reg_05;
  else if (strcmp(item, "roic.reg_06") == 0) n = c->roic.reg_06;
  else if (strcmp(item, "roic.reg_07") == 0) n = c->roic.reg_07;
  else if (strcmp(item, "roic.reg_09") == 0) n = c->roic.reg_09;
  else if (strcmp(item, "roic.reg_0a") == 0) n = c->roic.reg_0a;
  else if (strcmp(item, "roic.reg_0b") == 0) n = c->roic.reg_0b;
  else if (strcmp(item, "roic.reg_0c") == 0) n = c->roic.reg_0c;
  else if (strcmp(item, "roic.reg_0d") == 0) n = c->roic.reg_0d;
  else if (strcmp(item, "roic.reg_0e") == 0) n = c->roic.reg_0e;
  else if (strcmp(item, "roic.reg_0f") == 0) n = c->roic.reg_0f;
  else if (strcmp(item, "roic.reg_10") == 0) n = c->roic.reg_10;
  else if (strcmp(item, "roic.reg_11") == 0) n = c->roic.reg_11;
  else if (strcmp(item, "roic.reg_17") == 0) n = c->roic.reg_17;
  else if (strcmp(item, "roic.reg_24") == 0) n = c->roic.reg_24;
  else if (strcmp(item, "roic.reg_28") == 0) n = c->roic.reg_28;
  else if (strcmp(item, "roic.reg_2d") == 0) n = c->roic.reg_2d;
  else if (strcmp(item, "roic.reg_3b") == 0) n = c->roic.reg_3b;
  else if (strcmp(item, "corr.pkg_num") == 0) n = c->corr.pkg_num;
  else if (strcmp(item, "corr.row_num") == 0) n = c->corr.row_num;
  else if (strcmp(item, "corr.col_num") == 0) n = c->corr.col_num;
  else if (strcmp(item, "corr.offset_en") == 0) n = c->corr.offset_enable;
  else if (strcmp(item, "corr.offset_adder_value") == 0) n = c->corr.offset_adder_value;
  else if (strcmp(item, "corr.offset_template_addr") == 0) n = c->corr.offset_template_addr;
  else if (strcmp(item, "corr.offset_corr_mode") == 0) n = c->corr.offset_corr_mode;
  else if (strcmp(item, "corr.gain_en") == 0) n = c->corr.gain_enable;
  else if (strcmp(item, "corr.gain_clipping_value") == 0) n = c->corr.gain_clipping_value;
  else if (strcmp(item, "corr.gain_template_addr") == 0) n = c->corr.gain_template_addr;
  else if (strcmp(item, "corr.defect_en") == 0) n = c->corr.defect_enable;
  else if (strcmp(item, "static.exposure_window_ms") == 0) n = c->static_idle.exposure_window_ms;
  else if (strcmp(item, "static.dark_window_ms") == 0) n = c->static_idle.dark_window_ms;
  else if (strcmp(item, "static.idle_clean_interval_ms") == 0) n = c->static_idle.idle_clean_interval_ms;
  else if (strcmp(item, "dynamic.cycle") == 0) n = c->dync.cycle_num;
  else if (strcmp(item, "dynamic.image_start_addr") == 0) n = c->dync.image_start_addr;
  else if (strcmp(item, "dynamic.image_end_addr") == 0) n = c->dync.image_end_addr;
  else if (strcmp(item, "dynamic.start_timeout_ms") == 0) n = c->dynamic_start_timeout_ms;
  else if (strcmp(item, "dynamic.state_poll_interval_ms") == 0) n = c->dynamic_state_poll_interval_ms;
  else if (strcmp(item, "dynamic.stop_timeout_ms") == 0) n = c->dynamic_stop_timeout_ms;
  else {
    unsigned index = 0; bool high = false;
    if (!dynamic_item_index(item, &index, &high)) return -1;
    n = high ? c->dync.step_cfg_h[index] : c->dync.step_cfg_l[index];
  }
  if (is_bool_item(item)) return snprintf(value, size, "%u", n != 0u ? 1u : 0u) < 0 ? -1 : 0;
  return snprintf(value, size, "0x%08x", n) < 0 ? -1 : 0;
}

int config_store_set_item(pa_runtime_config_t* c, const char* item, const char* value) {
  if (c == NULL || item == NULL || value == NULL) return -1;
  uint32_t n = 0;
  bool b = false;
  if (is_bool_item(item)) {
    if (!parse_bool(value, &b)) return -1;
    n = b ? 1u : 0u;
  } else if (!parse_u32(value, &n)) {
    return -1;
  }
  if (strcmp(item, "gic.req_code") == 0) c->gic.req_code = (uint8_t)n;
  else if (strcmp(item, "gic.dout_en") == 0) c->gic.dout_enable = n != 0u;
  else if (strcmp(item, "gic.line_time_ns") == 0) c->gic.line_time_ns = n;
  else if (strcmp(item, "gic.oe_rise_ns") == 0) c->gic.oe_raising_edge_ns = n;
  else if (strcmp(item, "gic.oe_fall_ns") == 0) c->gic.oe_falling_edge_ns = n;
  else if (strcmp(item, "gic.start_row") == 0) c->gic.start_row = (uint16_t)n;
  else if (strcmp(item, "gic.end_row") == 0) c->gic.end_row = (uint16_t)n;
  else if (strcmp(item, "gic.binning") == 0) c->gic.binning_mode = (uint8_t)n;
  else if (strcmp(item, "roic.start_col") == 0) c->roic.start_col = (uint16_t)n;
  else if (strcmp(item, "roic.end_col") == 0) c->roic.end_col = (uint16_t)n;
  else if (strcmp(item, "roic.binning") == 0) c->roic.binning_mode = (uint8_t)n;
  else if (strcmp(item, "roic.reg_00") == 0) c->roic.reg_00 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_02") == 0) c->roic.reg_02 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_05") == 0) c->roic.reg_05 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_06") == 0) c->roic.reg_06 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_07") == 0) c->roic.reg_07 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_09") == 0) c->roic.reg_09 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0a") == 0) c->roic.reg_0a = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0b") == 0) c->roic.reg_0b = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0c") == 0) c->roic.reg_0c = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0d") == 0) c->roic.reg_0d = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0e") == 0) c->roic.reg_0e = (uint16_t)n;
  else if (strcmp(item, "roic.reg_0f") == 0) c->roic.reg_0f = (uint16_t)n;
  else if (strcmp(item, "roic.reg_10") == 0) c->roic.reg_10 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_11") == 0) c->roic.reg_11 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_17") == 0) c->roic.reg_17 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_24") == 0) c->roic.reg_24 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_28") == 0) c->roic.reg_28 = (uint16_t)n;
  else if (strcmp(item, "roic.reg_2d") == 0) c->roic.reg_2d = (uint16_t)n;
  else if (strcmp(item, "roic.reg_3b") == 0) c->roic.reg_3b = (uint16_t)n;
  else if (strcmp(item, "corr.pkg_num") == 0) c->corr.pkg_num = (uint16_t)n;
  else if (strcmp(item, "corr.row_num") == 0) c->corr.row_num = (uint16_t)n;
  else if (strcmp(item, "corr.col_num") == 0) c->corr.col_num = (uint16_t)n;
  else if (strcmp(item, "corr.offset_en") == 0) c->corr.offset_enable = n != 0u;
  else if (strcmp(item, "corr.offset_adder_value") == 0) c->corr.offset_adder_value = (uint16_t)n;
  else if (strcmp(item, "corr.offset_template_addr") == 0) c->corr.offset_template_addr = n;
  else if (strcmp(item, "corr.offset_corr_mode") == 0) c->corr.offset_corr_mode = (uint8_t)n;
  else if (strcmp(item, "corr.gain_en") == 0) c->corr.gain_enable = n != 0u;
  else if (strcmp(item, "corr.gain_clipping_value") == 0) c->corr.gain_clipping_value = (uint16_t)n;
  else if (strcmp(item, "corr.gain_template_addr") == 0) c->corr.gain_template_addr = n;
  else if (strcmp(item, "corr.defect_en") == 0) c->corr.defect_enable = n != 0u;
  else if (strcmp(item, "static.exposure_window_ms") == 0) c->static_idle.exposure_window_ms = n;
  else if (strcmp(item, "static.dark_window_ms") == 0) c->static_idle.dark_window_ms = n;
  else if (strcmp(item, "static.idle_clean_interval_ms") == 0) c->static_idle.idle_clean_interval_ms = n;
  else if (strcmp(item, "dynamic.cycle") == 0) c->dync.cycle_num = n;
  else if (strcmp(item, "dynamic.image_start_addr") == 0) c->dync.image_start_addr = n;
  else if (strcmp(item, "dynamic.image_end_addr") == 0) c->dync.image_end_addr = n;
  else if (strcmp(item, "dynamic.start_timeout_ms") == 0) c->dynamic_start_timeout_ms = n;
  else if (strcmp(item, "dynamic.state_poll_interval_ms") == 0) c->dynamic_state_poll_interval_ms = n;
  else if (strcmp(item, "dynamic.stop_timeout_ms") == 0) c->dynamic_stop_timeout_ms = n;
  else {
    unsigned index = 0; bool high = false;
    if (!dynamic_item_index(item, &index, &high)) return -1;
    if (high) c->dync.step_cfg_h[index] = n;
    else c->dync.step_cfg_l[index] = n;
  }
  return 0;
}
