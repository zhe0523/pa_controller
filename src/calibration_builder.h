#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fpga_mem.h"

/*
 * 多灰阶 gain/坏点模板制作接口。
 *
 * 上位机负责切换灰度级 level；ARM 在每个 level 下采集多帧均值图，
 * 最后按 Medians/Pixels 最小二乘拟合生成 gain 模板。坏点不单独输出模板，
 * 而是在 gain 模板对应坐标写 0。
 */

#ifndef CAL_GAIN_MAX_LEVELS
/* 一次 gain 校准允许的最大灰阶数量，避免命令误传导致资源不可控。 */
#define CAL_GAIN_MAX_LEVELS 16u
#endif

typedef struct {
  uint32_t level;
  uint32_t frames_captured;
  uint32_t median;
  int ready;
} cal_gain_level_status_t;

typedef struct {
  int active;
  uint32_t level_count;
  uint32_t frames_per_level;
  float defect_threshold;
  uint32_t levels_ready;
  uint32_t bad_pixel_count;
  cal_gain_level_status_t levels[CAL_GAIN_MAX_LEVELS];
} cal_gain_status_t;

/* 初始化一次新的多灰阶 gain 校准任务。 */
int calibration_gain_begin(const uint32_t* levels,
                           uint32_t level_count,
                           uint32_t frames_per_level,
                           float defect_threshold);

/* 在当前上位机设置的灰度级下采集 frames_per_level 帧，并生成该 level 的均值图。 */
int calibration_gain_capture_level(fpga_mem_t* mem, uint32_t level);

/* 根据所有已采集灰阶均值图生成 gain.raw，并写入 FPGA gain 模板内存。 */
int calibration_gain_build(fpga_mem_t* mem);

/* 取消当前校准任务，仅清空 ARM 侧状态，不删除已经落盘的均值图。 */
void calibration_gain_cancel(void);

/* 获取当前校准任务状态。 */
void calibration_gain_get_status(cal_gain_status_t* status);
