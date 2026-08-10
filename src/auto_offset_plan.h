#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 自动暗场 offset 更新规划。
 *
 * 本文件先定义后续落地自动更新需要的配置、状态和质量门槛，不在当前阶段启动后台线程，
 * 也不改变 MAKE_OFFSET 的现有单帧行为。等硬件空闲判据、曝光状态和多帧采集接口确认后，
 * 再把这些结构接入实际任务调度。
 */

typedef enum {
  AUTO_OFFSET_DISABLED = 0,
  AUTO_OFFSET_WAIT_IDLE,
  AUTO_OFFSET_COLLECTING,
  AUTO_OFFSET_VALIDATING,
  AUTO_OFFSET_COMMITTING,
  AUTO_OFFSET_FAILED,
} auto_offset_state_t;

typedef struct {
  bool enabled;
  unsigned sample_frames;
  unsigned idle_stable_ms;
  uint16_t max_allowed_pixel;
  uint32_t max_mean_delta;
  uint32_t max_row_noise;
} auto_offset_config_t;

typedef struct {
  auto_offset_state_t state;
  uint64_t update_count;
  uint64_t reject_count;
  uint32_t last_mean;
  uint32_t last_max;
  uint32_t last_row_noise;
  char last_error[96];
} auto_offset_status_t;

/* 当前推荐默认值：保守启用条件，默认不自动运行。 */
auto_offset_config_t auto_offset_default_config(void);

/* 生成给 STATUS/诊断使用的人类可读状态名。 */
const char* auto_offset_state_name(auto_offset_state_t state);

