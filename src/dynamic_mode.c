#include "dynamic_mode.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"
#include "pa_pu_regs.h"

static size_t align_up_size(size_t value, size_t align) {
  if (align == 0u) {
    return value;
  }
  size_t remainder = value % align;
  return remainder == 0u ? value : value + (align - remainder);
}

static uint64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0u;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const char* dynamic_mode_state_name(dynamic_state_t state) {
  switch (state) {
    case DYNAMIC_STATE_STOPPED: return "STOPPED";
    case DYNAMIC_STATE_STARTING: return "STARTING";
    case DYNAMIC_STATE_RUNNING: return "RUNNING";
    case DYNAMIC_STATE_STOPPING: return "STOPPING";
    case DYNAMIC_STATE_COMPLETED: return "COMPLETED";
    case DYNAMIC_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char* dynamic_mode_phase_name(dynamic_phase_t phase) {
  switch (phase) {
    case DYNAMIC_PHASE_NONE: return "none";
    case DYNAMIC_PHASE_CONFIGURE: return "configure";
    case DYNAMIC_PHASE_START_WAIT: return "start_wait";
    case DYNAMIC_PHASE_RUNNING: return "running";
    case DYNAMIC_PHASE_STOP_WAIT: return "stop_wait";
    default: return "unknown";
  }
}

static bool stop_requested(dynamic_mode_context_t* ctx) {
  pthread_mutex_lock(&ctx->mutex);
  bool requested = ctx->stop_requested;
  pthread_mutex_unlock(&ctx->mutex);
  return requested;
}

static void set_runtime_state(dynamic_mode_context_t* ctx,
                              dynamic_state_t state,
                              dynamic_phase_t phase) {
  pthread_mutex_lock(&ctx->mutex);
  ctx->status.state = state;
  ctx->status.phase = phase;
  pthread_cond_broadcast(&ctx->cond);
  pthread_mutex_unlock(&ctx->mutex);
}

static uint32_t snapshot_dync_registers(dynamic_mode_context_t* ctx) {
  uint32_t state = pa_pu_read(PA_PU_DYNC_STATE_REG);
  uint32_t end = pa_pu_read(PA_PU_DYNC_END_REG);
  uint32_t debug_out = pa_pu_read(PA_PU_DYNC_DEBUG_OUT_REG);

  pthread_mutex_lock(&ctx->mutex);
  ctx->status.last_dync_state = state;
  ctx->status.last_dync_end = end;
  ctx->status.last_dync_debug_out = debug_out;
  pthread_mutex_unlock(&ctx->mutex);
  return state;
}

static int sleep_poll_interval(unsigned interval_ms) {
  unsigned delay_ms = interval_ms == 0u ? 1u : interval_ms;
  if (usleep(delay_ms * 1000u) != 0 && errno == EINTR) {
    return -EINTR;
  }
  return 0;
}

int dynamic_mode_default_config(const fpga_mem_t* fpga_mem, dynamic_mode_config_t* config) {
  if (fpga_mem == NULL || config == NULL || !fpga_mem_is_open(fpga_mem)) {
    return -1;
  }

  memset(config, 0, sizeof(*config));
  config->frame_stride = align_up_size(ACTIVE_IMAGE_BYTES, DDR_IMAGE_FRAME_ALIGN);
  size_t max_frame_count = config->frame_stride == 0u
      ? 0u
      : fpga_mem->image_pool_map_size / config->frame_stride;
  config->frame_count = DDR_IMAGE_POOL_FRAME_COUNT == 0u
      ? max_frame_count
      : (size_t)DDR_IMAGE_POOL_FRAME_COUNT;
  if (config->frame_stride == 0u ||
      config->frame_count == 0u ||
      config->frame_count > max_frame_count ||
      fpga_mem->image_pool_phys_base == 0u) {
    return -1;
  }

  if (DYNAMIC_IMG_START_ADDR == 0u || DYNAMIC_IMG_END_ADDR < DYNAMIC_IMG_START_ADDR) {
    return -1;
  }

  /* 正式默认值由 Makefile 固化；CONFIG_DYNC 只能在 FPGA Dynamic 停止后覆盖。 */
  config->dync.cycle_num = DYNAMIC_CYCLE_NUM;
  config->dync.image_start_addr = DYNAMIC_IMG_START_ADDR;
  config->dync.image_end_addr = DYNAMIC_IMG_END_ADDR;
  static const uint32_t default_step_h[PA_PU_DYNC_STEP_COUNT] = {
    DYNAMIC_STEP_0_CFG_H, DYNAMIC_STEP_1_CFG_H, DYNAMIC_STEP_2_CFG_H, DYNAMIC_STEP_3_CFG_H,
    DYNAMIC_STEP_4_CFG_H, DYNAMIC_STEP_5_CFG_H, DYNAMIC_STEP_6_CFG_H, DYNAMIC_STEP_7_CFG_H,
    DYNAMIC_STEP_8_CFG_H, DYNAMIC_STEP_9_CFG_H,
  };
  static const uint32_t default_step_l[PA_PU_DYNC_STEP_COUNT] = {
    DYNAMIC_STEP_0_CFG_L, DYNAMIC_STEP_1_CFG_L, DYNAMIC_STEP_2_CFG_L, DYNAMIC_STEP_3_CFG_L,
    DYNAMIC_STEP_4_CFG_L, DYNAMIC_STEP_5_CFG_L, DYNAMIC_STEP_6_CFG_L, DYNAMIC_STEP_7_CFG_L,
    DYNAMIC_STEP_8_CFG_L, DYNAMIC_STEP_9_CFG_L,
  };
  memcpy(config->dync.step_cfg_h, default_step_h, sizeof(default_step_h));
  memcpy(config->dync.step_cfg_l, default_step_l, sizeof(default_step_l));
  config->start_timeout_ms = DYNAMIC_START_TIMEOUT_MS;
  config->state_poll_interval_ms = DYNAMIC_STATE_POLL_INTERVAL_MS;
  config->stop_timeout_ms = DYNAMIC_STOP_TIMEOUT_MS;
  return 0;
}

int dynamic_mode_init(dynamic_mode_context_t* ctx, fpga_mem_t* fpga_mem) {
  if (ctx == NULL || fpga_mem == NULL) {
    return -1;
  }

  memset(ctx, 0, sizeof(*ctx));
  if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
    return -1;
  }
  if (pthread_cond_init(&ctx->cond, NULL) != 0) {
    pthread_mutex_destroy(&ctx->mutex);
    return -1;
  }
  if (dynamic_mode_default_config(fpga_mem, &ctx->config) != 0) {
    pthread_cond_destroy(&ctx->cond);
    pthread_mutex_destroy(&ctx->mutex);
    return -1;
  }

  ctx->initialized = true;
  ctx->fpga_mem = fpga_mem;
  ctx->status.state = DYNAMIC_STATE_STOPPED;
  ctx->status.phase = DYNAMIC_PHASE_NONE;
  return 0;
}

int dynamic_mode_update_config(dynamic_mode_context_t* ctx, const dynamic_mode_config_t* config) {
  if (ctx == NULL || config == NULL || !ctx->initialized) {
    return -1;
  }
  if (config->frame_stride == 0u ||
      config->frame_count == 0u ||
      config->start_timeout_ms == 0u ||
      config->state_poll_interval_ms == 0u ||
      config->stop_timeout_ms == 0u) {
    return -1;
  }

  pthread_mutex_lock(&ctx->mutex);
  /* FPGA Dynamic 运行期间禁止修改 cycle/地址/step，仅允许走 DYNC_STOP 停止。 */
  if (ctx->thread_running) {
    pthread_mutex_unlock(&ctx->mutex);
    return -2;
  }
  ctx->config = *config;
  pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static void* dynamic_thread_main(void* arg) {
  dynamic_mode_context_t* ctx = (dynamic_mode_context_t*)arg;
  dynamic_mode_config_t config;
  int final_error = 0;

  pthread_mutex_lock(&ctx->mutex);
  config = ctx->config;
  ctx->status.state = DYNAMIC_STATE_STARTING;
  ctx->status.phase = DYNAMIC_PHASE_CONFIGURE;
  pthread_cond_broadcast(&ctx->cond);
  pthread_mutex_unlock(&ctx->mutex);

  /* Continuous 启动时完整下发一次基础配置，之后由 FPGA 按 step/cycle 自主运行。 */
  pa_pu_configure_gic_defaults();
  pa_pu_configure_roic_defaults();
  pa_pu_configure_dync(&config.dync);

  if (!stop_requested(ctx)) {
    set_runtime_state(ctx, DYNAMIC_STATE_STARTING, DYNAMIC_PHASE_START_WAIT);
    pa_pu_start_dync();

    /* 确认 FPGA 状态机确实进入 busy，避免只凭线程创建成功判断设备已运行。 */
    uint64_t start_ms = monotonic_ms();
    for (;;) {
      uint32_t dync_state = snapshot_dync_registers(ctx);
      if ((dync_state & 1u) != 0u) {
        break;
      }
      if (stop_requested(ctx)) {
        break;
      }
      if (monotonic_ms() - start_ms >= config.start_timeout_ms) {
        final_error = ETIMEDOUT;
        break;
      }
      if (sleep_poll_interval(config.state_poll_interval_ms) != 0) {
        final_error = EINTR;
        break;
      }
    }
  }

  if (final_error == 0 && !stop_requested(ctx)) {
    set_runtime_state(ctx, DYNAMIC_STATE_RUNNING, DYNAMIC_PHASE_RUNNING);
    log_info("dynamic work started cycle=%u ring_start=0x%08x ring_end=0x%08x step0_h=0x%08x step0_l=0x%08x gic_dout=%u",
             config.dync.cycle_num,
             config.dync.image_start_addr,
             config.dync.image_end_addr,
             config.dync.step_cfg_h[0],
             config.dync.step_cfg_l[0],
             GIC_DEFAULT_DOUT_EN != 0u ? 1u : 0u);

    while (!stop_requested(ctx)) {
      uint32_t dync_state = snapshot_dync_registers(ctx);
      if ((dync_state & 1u) == 0u) {
        /* 有限 cycle 正常完成；cycle=0 却自行退出视为硬件异常。 */
        if (config.dync.cycle_num == 0u) {
          final_error = EIO;
        }
        break;
      }
      if (sleep_poll_interval(config.state_poll_interval_ms) != 0) {
        final_error = EINTR;
        break;
      }
    }
  }

  if (final_error == 0 && stop_requested(ctx)) {
    set_runtime_state(ctx, DYNAMIC_STATE_STOPPING, DYNAMIC_PHASE_STOP_WAIT);
    pa_pu_stop_dync();
    uint64_t stop_ms = monotonic_ms();
    for (;;) {
      uint32_t dync_state = snapshot_dync_registers(ctx);
      if ((dync_state & 1u) == 0u) {
        break;
      }
      if (monotonic_ms() - stop_ms >= config.stop_timeout_ms) {
        final_error = ETIMEDOUT;
        break;
      }
      if (sleep_poll_interval(config.state_poll_interval_ms) != 0) {
        final_error = EINTR;
        break;
      }
    }
  }

  (void)snapshot_dync_registers(ctx);
  pthread_mutex_lock(&ctx->mutex);
  ctx->thread_running = false;
  ctx->status.stop_requested = ctx->stop_requested;
  ctx->status.last_error = final_error;
  if (final_error != 0) {
    ctx->status.state = DYNAMIC_STATE_ERROR;
  } else if (ctx->stop_requested) {
    /* 只有真实收到停止请求才报告 STOPPED。 */
    ctx->status.state = DYNAMIC_STATE_STOPPED;
    ctx->status.phase = DYNAMIC_PHASE_NONE;
  } else {
    /* 有限 cycle 结束属于正常完成，不应伪装成人工停止。 */
    ctx->status.state = DYNAMIC_STATE_COMPLETED;
    ctx->status.phase = DYNAMIC_PHASE_NONE;
  }
  pthread_cond_broadcast(&ctx->cond);
  pthread_mutex_unlock(&ctx->mutex);

  /* get_status 有参数保护分支；显式初始化可消除 GCC 无法跨函数证明必定赋值的告警。 */
  dynamic_mode_status_t final_status = {0};
  dynamic_mode_get_status(ctx, &final_status);
  log_info("dynamic work finished state=%s error=%d dync_state=0x%08x dync_end=0x%08x dync_debug=0x%08x",
           dynamic_mode_state_name(final_status.state),
           final_error,
           final_status.last_dync_state,
           final_status.last_dync_end,
           final_status.last_dync_debug_out);
  return NULL;
}

int dynamic_mode_start(dynamic_mode_context_t* ctx) {
  if (ctx == NULL || !ctx->initialized) {
    return -1;
  }

  pthread_mutex_lock(&ctx->mutex);
  bool join_old = ctx->thread_joinable && !ctx->thread_running;
  pthread_t old_thread = ctx->thread;
  pthread_mutex_unlock(&ctx->mutex);
  if (join_old) {
    pthread_join(old_thread, NULL);
    pthread_mutex_lock(&ctx->mutex);
    ctx->thread_joinable = false;
    pthread_mutex_unlock(&ctx->mutex);
  }

  pthread_mutex_lock(&ctx->mutex);
  if (ctx->thread_running || ctx->thread_joinable) {
    pthread_mutex_unlock(&ctx->mutex);
    return -2;
  }
  ctx->stop_requested = false;
  memset(&ctx->status, 0, sizeof(ctx->status));
  ctx->status.state = DYNAMIC_STATE_STARTING;
  ctx->thread_running = true;
  ctx->thread_joinable = true;
  if (pthread_create(&ctx->thread, NULL, dynamic_thread_main, ctx) != 0) {
    ctx->thread_running = false;
    ctx->thread_joinable = false;
    ctx->status.state = DYNAMIC_STATE_ERROR;
    ctx->status.last_error = EIO;
    pthread_mutex_unlock(&ctx->mutex);
    return -1;
  }

  /*
   * START_CONTINUOUS 的 OK 必须表示 FPGA DYNC_STATE 已经进入 busy，
   * 不能只表示 pthread 创建成功。工作线程会在 start_timeout 内转入 RUNNING 或 ERROR。
   */
  while (ctx->thread_running && ctx->status.state == DYNAMIC_STATE_STARTING) {
    pthread_cond_wait(&ctx->cond, &ctx->mutex);
  }
  int start_result = ctx->status.state == DYNAMIC_STATE_RUNNING ? 0 : -1;
  pthread_mutex_unlock(&ctx->mutex);
  return start_result;
}

int dynamic_mode_stop(dynamic_mode_context_t* ctx) {
  if (ctx == NULL || !ctx->initialized) {
    return -1;
  }

  pthread_mutex_lock(&ctx->mutex);
  bool joinable = ctx->thread_joinable;
  bool running = ctx->thread_running;
  pthread_t thread = ctx->thread;
  if (running) {
    ctx->stop_requested = true;
    ctx->status.stop_requested = true;
    pthread_cond_broadcast(&ctx->cond);
  }
  pthread_mutex_unlock(&ctx->mutex);

  if (joinable) {
    pthread_join(thread, NULL);
    pthread_mutex_lock(&ctx->mutex);
    ctx->thread_joinable = false;
    pthread_mutex_unlock(&ctx->mutex);
  }

  pthread_mutex_lock(&ctx->mutex);
  int result = ctx->status.state == DYNAMIC_STATE_ERROR ? -1 : 0;
  pthread_mutex_unlock(&ctx->mutex);
  return result;
}

void dynamic_mode_get_status(dynamic_mode_context_t* ctx, dynamic_mode_status_t* status) {
  if (ctx == NULL || status == NULL || !ctx->initialized) {
    return;
  }
  pthread_mutex_lock(&ctx->mutex);
  *status = ctx->status;
  status->stop_requested = ctx->stop_requested;
  pthread_mutex_unlock(&ctx->mutex);
}

void dynamic_mode_get_config(dynamic_mode_context_t* ctx, dynamic_mode_config_t* config) {
  if (ctx == NULL || config == NULL || !ctx->initialized) {
    return;
  }
  pthread_mutex_lock(&ctx->mutex);
  *config = ctx->config;
  pthread_mutex_unlock(&ctx->mutex);
}

bool dynamic_mode_is_active(dynamic_mode_context_t* ctx) {
  if (ctx == NULL || !ctx->initialized) {
    return false;
  }
  pthread_mutex_lock(&ctx->mutex);
  /* 已自然结束但尚未 join 的线程不再占用硬件资源，不应阻止配置和状态查询。 */
  bool active = ctx->thread_running;
  pthread_mutex_unlock(&ctx->mutex);
  return active;
}
