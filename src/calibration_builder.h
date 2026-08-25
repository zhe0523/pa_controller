#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fpga_mem.h"
#include "pa_pu.h"

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

typedef struct {
  uint32_t frames;
  uint32_t valid_frames;
  uint32_t offset_addr;
  uint32_t last_img_addr;
  uint32_t last_int_vector;
} cal_dynamic_offset_result_t;

/* 模板制作统一后台任务，避免采集/计算期间阻塞串口命令线程。 */
typedef enum {
  CAL_TASK_NONE = 0,
  CAL_TASK_MAKE_OFFSET,
  CAL_TASK_MAKE_GAIN,
  CAL_TASK_DYNAMIC_OFFSET,
  CAL_TASK_GAIN_CAPTURE,
  CAL_TASK_GAIN_BUILD,
} cal_task_kind_t;

typedef enum {
  CAL_TASK_IDLE = 0,
  CAL_TASK_RUNNING,
  CAL_TASK_STOPPING,
  CAL_TASK_SUCCEEDED,
  CAL_TASK_FAILED,
  CAL_TASK_CANCELED,
} cal_task_state_t;

typedef struct {
  uint32_t task_id;
  cal_task_kind_t kind;
  cal_task_state_t state;
  bool stop_requested;
  int last_error;
  uint32_t progress_current;
  uint32_t progress_total;
  uint32_t gain_level;
  cal_dynamic_offset_result_t dynamic_offset;
} cal_task_status_t;

/*
 * 动态模式 offset 模板制作。
 *
 * 调用者可选择先下发 dynamic 配置，然后按 frames 次重复启动 dynamic。
 * 前面的 frames - valid_frames 帧只用于曝光/链路稳定，最后 valid_frames 帧
 * 才参与逐像素均值。均值结果会同时写入 TEMPLATE_OFFSET_FILE 和 FPGA offset 模板 DDR。
 */
int calibration_dynamic_offset_make(fpga_mem_t* mem,
                                    const pa_pu_dync_config_t* dync_config,
                                    bool write_dync_config,
                                    uint32_t frames,
                                    uint32_t valid_frames,
                                    cal_dynamic_offset_result_t* result);

/* start 成功只表示后台任务已经创建，不表示模板已经制作完成。 */
int calibration_task_start_make_offset(fpga_mem_t* mem);
int calibration_task_start_make_gain(fpga_mem_t* mem);
int calibration_task_start_dynamic_offset(fpga_mem_t* mem,
                                          const pa_pu_dync_config_t* dync_config,
                                          bool write_dync_config,
                                          uint32_t frames,
                                          uint32_t valid_frames);
int calibration_task_start_gain_capture(fpga_mem_t* mem, uint32_t level);
int calibration_task_start_gain_build(fpga_mem_t* mem);
bool calibration_task_request_stop(void);
bool calibration_task_is_active(void);
void calibration_task_get_status(cal_task_status_t* status);
void calibration_task_shutdown(void);
const char* calibration_task_kind_name(cal_task_kind_t kind);
const char* calibration_task_state_name(cal_task_state_t state);
