#pragma once

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "fpga_mem.h"
#include "pa_pu.h"

/*
 * 工作模式编号沿用上一代产品定义，便于后续和上位机/历史协议对齐。
 * 当前第一阶段只实现 Static Idle，即 WORK_MODE_IDLE 下的静态空闲采图流程。
 */
typedef enum {
  WORK_MODE_IDLE = 0,
  WORK_MODE_AED = 2,
  WORK_MODE_SYNC_OUT = 3,
  WORK_MODE_SYNC_IN = 5,
  WORK_MODE_PREP = 6,
  WORK_MODE_CONTINUOUS = 7,
  WORK_MODE_INNER = 8,
  WORK_MODE_FREE_SYNC = 9,
  WORK_MODE_DDR = 10,
} work_mode_t;

/* 工作线程当前状态。命令层用这些状态判断调试寄存器写入是否允许。 */
typedef enum {
  WORK_STATE_STOPPED = 0,
  WORK_STATE_IDLE_WAIT,
  WORK_STATE_IDLE_CLEANING,
  WORK_STATE_EXPOSURE_WINDOW,
  WORK_STATE_BRIGHT_CAPTURE,
  WORK_STATE_DARK_WINDOW,
  WORK_STATE_DARK_CAPTURE,
  WORK_STATE_ERROR,
} work_state_t;

/*
 * 最近一次执行到的业务阶段。
 * state 更偏运行时互斥，phase 更偏错误定位和上位机显示。
 */
typedef enum {
  WORK_PHASE_NONE = 0,
  WORK_PHASE_IDLE_CLEAN,
  WORK_PHASE_EXPOSURE_WINDOW,
  WORK_PHASE_BRIGHT_CAPTURE,
  WORK_PHASE_DARK_WINDOW,
  WORK_PHASE_DARK_CAPTURE,
} work_phase_t;

/* Static Idle 模式配置：一次上位机采图请求会依次产生 offset 模板帧和实际输出帧。 */
typedef struct {
  /* 空闲自清空间隔；到点后执行一次 dout 关闭的 GIC 串扫，用于清空 GIC。 */
  uint32_t idle_clean_interval_ms;
  /* 收到采图请求并等待自清空结束后，进入第一帧 offset 模板采集前的曝光窗口。 */
  uint32_t exposure_window_ms;
  /* 第一帧 offset 模板采集完成后，进入第二帧实际输出图采集前的等待窗口。 */
  uint32_t dark_window_ms;
  /* 空闲自清空 GIC 配置，dout_enable 固定为 false。 */
  pa_pu_gic_config_t clean_gic;
  /* 第一帧未校正 light 的 GIC 配置，dout_enable 固定为 true，写入 offset 模板区。 */
  pa_pu_gic_config_t bright_gic;
  /* 第二帧实际输出图的 GIC 配置，dout_enable 固定为 true，写入 uio2 图像池。 */
  pa_pu_gic_config_t dark_gic;
  /* 第一帧作为 offset 模板，校正模块强制全关，只保留尺寸和模板地址等公共参数。 */
  pa_pu_corr_config_t bright_corr;
  /* 第二帧实际输出图按上位机配置决定 offset/gain/defect 是否启用。 */
  pa_pu_corr_config_t dark_corr;
} static_idle_config_t;

/* 工作线程状态快照，GET_WORK_STATE 和失败日志都会使用这一组字段。 */
typedef struct {
  work_mode_t mode;
  work_state_t state;
  bool pending_capture;
  bool stop_requested;
  int last_error;
  work_phase_t last_phase;
  uint32_t last_int_vector;
  uint32_t last_wait_mask;
  /* 最近一次采图失败时读取到的硬件状态快照。 */
  uint32_t last_wr_state;
  uint32_t last_wr_end;
  uint32_t last_corr_state;
  uint32_t last_corr_end;
  uint32_t last_gic_state;
  uint32_t last_gic_end;
  uint32_t last_gic_dfx;
  /* 兼容当前回包字段名：bright_addr 表示第一帧 offset 模板地址。 */
  uint32_t last_bright_addr;
  /* 兼容当前回包字段名：dark_addr 表示第二帧实际输出图地址。 */
  uint32_t last_dark_addr;
  uint32_t capture_id;
  /* uio2 环形 DDR 池的下一帧偏移；尾部不足一帧时回到 0。 */
  uint32_t ddr_next_offset;
  /* uio2 DDR 池单帧需要的对齐步进。 */
  size_t ddr_frame_stride;
} work_mode_status_t;

/*
 * 工作模式运行上下文。
 * 结构体暴露在头文件中是为了 main.c 可以栈上分配；字段只应由 work_mode.c 内部修改。
 */
typedef struct work_mode_context {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  pthread_t thread;
  bool initialized;
  bool thread_running;
  bool stop_requested;
  bool pending_capture;
  bool request_done;
  int request_result;
  fpga_mem_t* fpga_mem;
  static_idle_config_t config;
  work_mode_status_t status;
} work_mode_context_t;

/* 生成 Static Idle 默认配置，默认值来自 app_config.h/Makefile。 */
void work_mode_default_static_idle_config(static_idle_config_t* config);
/* 初始化工作模式上下文，并计算 uio2 环形图像池使用的单帧步进。 */
int work_mode_init(work_mode_context_t* wm, fpga_mem_t* fpga_mem);
/* 启动后台工作线程，进入 Static Idle 空闲自清空循环。 */
int work_mode_start(work_mode_context_t* wm);
/* 请求工作线程退出并等待线程结束。 */
void work_mode_stop(work_mode_context_t* wm);
/* 更新 Static Idle 参数；采图窗口和采图阶段会拒绝更新。 */
int work_mode_update_static_idle_config(work_mode_context_t* wm, const static_idle_config_t* config);
/* 读取当前 Static Idle 参数快照。 */
void work_mode_get_static_idle_config(work_mode_context_t* wm, static_idle_config_t* config);
/* 发起一次同步 Static Idle 采图请求，等待 offset 模板帧和实际输出帧完成后返回。 */
int work_mode_start_static_idle_capture(work_mode_context_t* wm, work_mode_status_t* result);
/* 读取工作线程状态快照。 */
void work_mode_get_status(work_mode_context_t* wm, work_mode_status_t* status);
/* 判断当前是否允许 WRITE_REG 直接写寄存器；采图窗口和采图阶段禁止写。 */
bool work_mode_allows_write_reg(work_mode_context_t* wm);
/* 判断当前是否允许调试型硬件动作命令，避免和正式采图时序交叉。 */
bool work_mode_allows_hardware_action(work_mode_context_t* wm);
const char* work_mode_state_name(work_state_t state);
const char* work_mode_phase_name(work_phase_t phase);
