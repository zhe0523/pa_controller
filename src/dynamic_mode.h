#pragma once

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "fpga_mem.h"
#include "pa_pu.h"

/* ARM 只管理 FPGA dynamic_ctrl 的启动、运行和停止生命周期。 */
typedef enum {
  DYNAMIC_STATE_STOPPED = 0,
  DYNAMIC_STATE_STARTING,
  DYNAMIC_STATE_RUNNING,
  DYNAMIC_STATE_STOPPING,
  /* 有限 cycle 自然执行完毕，未收到 ARM 停止请求。 */
  DYNAMIC_STATE_COMPLETED,
  DYNAMIC_STATE_ERROR,
} dynamic_state_t;

/* 最近执行到的硬件阶段，用于定位启动或停止超时。 */
typedef enum {
  DYNAMIC_PHASE_NONE = 0,
  DYNAMIC_PHASE_CONFIGURE,
  DYNAMIC_PHASE_START_WAIT,
  DYNAMIC_PHASE_RUNNING,
  DYNAMIC_PHASE_STOP_WAIT,
} dynamic_phase_t;

/*
 * Continuous/Dynamic 默认配置。
 * FPGA 根据 cycle 和 step 表自主循环；ARM 不参与逐帧调度，也不触发 IMG_UPLOAD。
 */
typedef struct {
  pa_pu_dync_config_t dync;
  size_t frame_stride;
  size_t frame_count;
  unsigned start_timeout_ms;
  unsigned state_poll_interval_ms;
  unsigned stop_timeout_ms;
} dynamic_mode_config_t;

/* Dynamic 运行快照；状态寄存器由生命周期线程轮询，但不会读取 read-clear 中断。 */
typedef struct {
  dynamic_state_t state;
  dynamic_phase_t phase;
  bool stop_requested;
  int last_error;
  uint32_t last_dync_state;
  uint32_t last_dync_end;
  uint32_t last_dync_debug_out;
} dynamic_mode_status_t;

typedef struct dynamic_mode_context {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  bool initialized;
  bool thread_running;
  bool thread_joinable;
  bool stop_requested;
  fpga_mem_t* fpga_mem;
  dynamic_mode_config_t config;
  dynamic_mode_status_t status;
} dynamic_mode_context_t;

/* 根据 Makefile 默认值和 uio2 图像池生成 FPGA 自主 Continuous 配置。 */
int dynamic_mode_default_config(const fpga_mem_t* fpga_mem, dynamic_mode_config_t* config);
/* 初始化 Dynamic 上下文，不启动线程。 */
int dynamic_mode_init(dynamic_mode_context_t* ctx, fpga_mem_t* fpga_mem);
/* 更新正式 Dynamic 配置；FPGA 正在运行时返回 -2，停止后才允许修改。 */
int dynamic_mode_update_config(dynamic_mode_context_t* ctx, const dynamic_mode_config_t* config);
/* 配置并启动一次 FPGA dynamic_ctrl；运行期间不读取 INT_VECTOR。 */
int dynamic_mode_start(dynamic_mode_context_t* ctx);
/* 请求停止，写 DYNC_STOP 并等待 DYNC_STATE 回到 idle。 */
int dynamic_mode_stop(dynamic_mode_context_t* ctx);
/* 读取线程安全的状态快照。 */
void dynamic_mode_get_status(dynamic_mode_context_t* ctx, dynamic_mode_status_t* status);
/* 读取当前保存的配置；运行期间即为本轮实际下发给 FPGA 的配置。 */
void dynamic_mode_get_config(dynamic_mode_context_t* ctx, dynamic_mode_config_t* config);
/* 判断工作线程是否仍在运行；已结束待 join 不算占用硬件资源。 */
bool dynamic_mode_is_active(dynamic_mode_context_t* ctx);

const char* dynamic_mode_state_name(dynamic_state_t state);
const char* dynamic_mode_phase_name(dynamic_phase_t phase);
