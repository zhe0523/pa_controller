#include "work_mode.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

enum {
  /*
   * 一次正式采图同时启动 IMG_CORR、IMG_WR、GIC 三个模块。
   * 只有三个完成 bit 都出现，才认为这一帧在 FPGA 侧处理完成。
   */
  STATIC_CAPTURE_WAIT_MASK = PA_PU_IRQ_IMG_CORR_END | PA_PU_IRQ_IMG_WR_END | PA_PU_IRQ_GIC_END,
};

static size_t align_up_size(size_t value, size_t align) {
  if (align == 0) {
    return value;
  }
  size_t rem = value % align;
  return rem == 0 ? value : value + (align - rem);
}

static uint64_t monotonic_ms_local(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms_interruptible(unsigned ms) {
  /* 长窗口拆成短 sleep，避免信号到来时 Ctrl+C 退出被明显拖慢。 */
  uint64_t start = monotonic_ms_local();
  while (monotonic_ms_local() - start < ms) {
    uint64_t elapsed = monotonic_ms_local() - start;
    uint64_t remain = ms > elapsed ? ms - elapsed : 0;
    useconds_t chunk = remain > 10u ? 10000u : (useconds_t)(remain * 1000u);
    if (chunk == 0) {
      break;
    }
    if (usleep(chunk) != 0 && errno == EINTR) {
      return;
    }
  }
}

const char* work_mode_state_name(work_state_t state) {
  switch (state) {
    case WORK_STATE_STOPPED: return "STOPPED";
    case WORK_STATE_IDLE_WAIT: return "IDLE_WAIT";
    case WORK_STATE_IDLE_CLEANING: return "IDLE_CLEANING";
    case WORK_STATE_EXPOSURE_WINDOW: return "EXPOSURE_WINDOW";
    case WORK_STATE_BRIGHT_CAPTURE: return "BRIGHT_CAPTURE";
    case WORK_STATE_DARK_WINDOW: return "DARK_WINDOW";
    case WORK_STATE_DARK_CAPTURE: return "DARK_CAPTURE";
    case WORK_STATE_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char* work_mode_phase_name(work_phase_t phase) {
  switch (phase) {
    case WORK_PHASE_NONE: return "none";
    case WORK_PHASE_IDLE_CLEAN: return "idle_clean";
    case WORK_PHASE_EXPOSURE_WINDOW: return "exposure_window";
    case WORK_PHASE_BRIGHT_CAPTURE: return "bright_capture";
    case WORK_PHASE_DARK_WINDOW: return "dark_window";
    case WORK_PHASE_DARK_CAPTURE: return "dark_capture";
    default: return "unknown";
  }
}

void work_mode_default_static_idle_config(static_idle_config_t* config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->idle_clean_interval_ms = STATIC_IDLE_CLEAN_INTERVAL_MS;
  config->exposure_window_ms = STATIC_IDLE_EXPOSURE_MS;
  config->dark_window_ms = STATIC_IDLE_DARK_WINDOW_MS;

  /* 空闲自清空只驱动 GIC 串扫，不输出图像数据。 */
  config->clean_gic = (pa_pu_gic_config_t){
    .req_code = PA_PU_GIC_REQ_SERIAL_SCAN,
    .dout_enable = false,
    .line_time_ns = GIC_DEFAULT_LINE_TIME_NS,
    .oe_raising_edge_ns = GIC_DEFAULT_OE_RISE_NS,
    .oe_falling_edge_ns = GIC_DEFAULT_OE_FALL_NS,
    .start_row = GIC_DEFAULT_START_ROW,
    .end_row = GIC_DEFAULT_END_ROW,
    .binning_mode = GIC_DEFAULT_BINNING,
  };

  /* 两次采图都需要输出数据：第一帧写入 offset 模板区，第二帧写入 uio2 输出区。 */
  config->bright_gic = config->clean_gic;
  config->bright_gic.dout_enable = true;

  config->dark_gic = config->bright_gic;

  /* 第一帧 light 采集用于生成 offset 模板，因此校正模块强制全关。 */
  config->bright_corr = (pa_pu_corr_config_t){
    .pkg_num = CORR_DEFAULT_PKG_NUM,
    .row_num = CORR_DEFAULT_ROW_NUM,
    .col_num = CORR_DEFAULT_COL_NUM,
    .offset_enable = false,
    .offset_template_addr = CORR_DEFAULT_OFFSET_ADDR,
    .offset_adder_value = CORR_DEFAULT_OFFSET_ADDER_VALUE,
    .gain_enable = false,
    .gain_template_addr = CORR_DEFAULT_GAIN_ADDR,
    .gain_clipping_value = CORR_DEFAULT_GAIN_CLIPPING_VALUE,
    .defect_enable = false,
  };

  /* 第二帧实际输出图使用默认校正开关，后续可由 CONFIG_STATIC_IDLE 覆盖。 */
  config->dark_corr = config->bright_corr;
  config->dark_corr.offset_enable = CORR_DEFAULT_OFFSET_EN != 0;
  config->dark_corr.gain_enable = CORR_DEFAULT_GAIN_EN != 0;
  config->dark_corr.defect_enable = CORR_DEFAULT_DEFECT_EN != 0;
}

static void set_state_locked(work_mode_context_t* wm, work_state_t state, work_phase_t phase) {
  /* 调用者必须已经持有 mutex；状态变化后唤醒等待命令和状态查询。 */
  wm->status.state = state;
  wm->status.last_phase = phase;
  pthread_cond_broadcast(&wm->cond);
}

static void record_error_locked(work_mode_context_t* wm,
                                int error,
                                work_phase_t phase,
                                uint32_t wait_mask,
                                uint32_t int_vector) {
  /* 保存失败现场，便于 GET_WORK_STATE 和命令错误回包定位是哪一阶段缺中断。 */
  wm->status.last_error = error;
  wm->status.last_phase = phase;
  wm->status.last_wait_mask = wait_mask;
  wm->status.last_int_vector = int_vector;
  pthread_cond_broadcast(&wm->cond);
}

static bool alloc_frame_locked(work_mode_context_t* wm, const char* phase, uint32_t* addr_out) {
  /*
   * uio2 作为实际输出图像池，按整帧步进做环形分配。
   * STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU=1 时，第 0 帧保留给 bright 临时帧，
   * 实际输出图从第 1 帧开始回环，避免 bright scratch 和 output 复用同一地址。
   * 边界行为：如果尾部剩余空间不足一帧，下一帧从池起始地址重新开始；
   * 单帧永远不跨越 uio2 尾部。
   */
  const size_t first_output_offset =
      STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ? wm->status.ddr_frame_stride : 0u;
  size_t offset = wm->status.ddr_next_offset;
  size_t pool_limit = wm->status.ddr_frame_stride * wm->status.ddr_frame_count;
  bool wrapped = false;

  if (offset < first_output_offset) {
    offset = first_output_offset;
  }
  if (offset + wm->status.ddr_frame_stride > pool_limit) {
    offset = first_output_offset;
    wrapped = true;
  }

  uint32_t addr = wm->fpga_mem->image_pool_phys_base + (uint32_t)offset;
  wm->status.ddr_next_offset = (uint32_t)(offset + wm->status.ddr_frame_stride);
  if (addr_out != NULL) {
    *addr_out = addr;
  }

  log_info("ddr image pool alloc capture_id=%u phase=%s addr=0x%08x offset=0x%lx stride=0x%lx size=0x%lx frames=%lu first_output_offset=0x%lx wrapped=%u",
           wm->status.capture_id,
           phase,
           addr,
           (unsigned long)offset,
           (unsigned long)wm->status.ddr_frame_stride,
           (unsigned long)pool_limit,
           (unsigned long)wm->status.ddr_frame_count,
           (unsigned long)first_output_offset,
           wrapped ? 1u : 0u);
  return true;
}

static int run_idle_clean(work_mode_context_t* wm, const static_idle_config_t* config) {
  uint32_t int_vector = 0;

  pthread_mutex_lock(&wm->mutex);
  set_state_locked(wm, WORK_STATE_IDLE_CLEANING, WORK_PHASE_IDLE_CLEAN);
  pthread_mutex_unlock(&wm->mutex);

  /* 空闲清空只启动 GIC，等待 GIC 完成 bit；不会触发 IMG_WR/IMG_CORR。 */
  pa_pu_configure_gic(&config->clean_gic);
  pa_pu_prepare_irq_wait();
  pa_pu_start_gic();

  int ret = pa_pu_wait_int_vector(PA_PU_IRQ_GIC_END, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
  pthread_mutex_lock(&wm->mutex);
  wm->status.last_int_vector = int_vector;
  wm->status.last_wait_mask = PA_PU_IRQ_GIC_END;
  if (ret <= 0) {
    record_error_locked(wm, ret == 0 ? ETIMEDOUT : EIO, WORK_PHASE_IDLE_CLEAN, PA_PU_IRQ_GIC_END, int_vector);
    log_error("static idle clean failed ret=%d wait_mask=0x%08x int_vector=0x%08x", ret, PA_PU_IRQ_GIC_END, int_vector);
  }
  pthread_mutex_unlock(&wm->mutex);
  return ret > 0 ? 0 : -1;
}

static int wait_capture_modules_idle(const char* phase_name, unsigned timeout_ms) {
  /*
   * 连续采图时，上一帧完成中断到各子模块 state 回到空闲之间可能存在短暂延迟。
   * 启动下一帧前先确认 GIC/IMG_WR/IMG_CORR 都空闲，避免 start 脉冲打在模块忙碌窗口里。
   */
  const uint64_t start_ms = monotonic_ms_local();
  pa_pu_status_t status;

  for (;;) {
    pa_pu_read_status(&status);
    if (status.img_wr_state == 0 && status.img_corr_state == 0 && status.gic_state == 0) {
      return 0;
    }

    uint64_t now_ms = monotonic_ms_local();
    if (now_ms - start_ms >= timeout_ms) {
      log_error("static capture wait idle timeout phase=%s wr_state=0x%08x corr_state=0x%08x gic_state=0x%08x wr_end=0x%08x corr_end=0x%08x gic_end=0x%08x",
                phase_name,
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

static int run_capture_phase(work_mode_context_t* wm,
                             const char* phase_name,
                             work_state_t state,
                             work_phase_t phase,
                             const pa_pu_gic_config_t* gic,
                             const pa_pu_corr_config_t* corr,
                             uint32_t image_addr) {
  uint32_t int_vector = 0;

  pthread_mutex_lock(&wm->mutex);
  set_state_locked(wm, state, phase);
  pthread_mutex_unlock(&wm->mutex);

  if (wait_capture_modules_idle(phase_name, 1000u) != 0) {
    pthread_mutex_lock(&wm->mutex);
    record_error_locked(wm, EBUSY, phase, 0, 0);
    pthread_mutex_unlock(&wm->mutex);
    pa_pu_dump_all_registers("wait_idle_failed");
    return -1;
  }

  /*
   * 每帧采图的硬件配置顺序：
   * 1. 配置 GIC 时序/行范围；
   * 2. 配置本帧 DDR 写图首地址；
   * 3. 配置校正模块；
   * 4. 清理上一轮中断后连续写三个 STR 寄存器。
   */
  log_info("static capture phase start phase=%s addr=0x%08x offset_en=%u gain_en=%u defect_en=%u",
           phase_name,
           image_addr,
           corr->offset_enable ? 1u : 0u,
           corr->gain_enable ? 1u : 0u,
           corr->defect_enable ? 1u : 0u);
  log_info("static capture phase step phase=%s step=config_gic begin", phase_name);
  pa_pu_configure_gic(gic);
  log_info("static capture phase step phase=%s step=config_gic done", phase_name);
  log_info("static capture phase step phase=%s step=config_image_write begin addr=0x%08x", phase_name, image_addr);
  pa_pu_configure_image_write(image_addr);
  log_info("static capture phase step phase=%s step=config_image_write done", phase_name);
  log_info("static capture phase step phase=%s step=config_correction begin offset_addr=0x%08x gain_addr=0x%08x",
           phase_name,
           corr->offset_template_addr,
           corr->gain_template_addr);
  pa_pu_configure_correction(corr);
  log_info("static capture phase step phase=%s step=config_correction done", phase_name);
  log_info("static capture phase step phase=%s step=prepare_irq begin", phase_name);
  pa_pu_prepare_irq_wait();
  log_info("static capture phase step phase=%s step=prepare_irq done", phase_name);
  log_info("static capture phase step phase=%s step=start_triplet begin", phase_name);
  pa_pu_start_capture_triplet();
  log_info("static capture phase step phase=%s step=start_triplet done", phase_name);
  log_info("static capture phase wait phase=%s wait_mask=0x%08x", phase_name, STATIC_CAPTURE_WAIT_MASK);

  int ret = pa_pu_wait_int_vector_all(STATIC_CAPTURE_WAIT_MASK, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
  pthread_mutex_lock(&wm->mutex);
  wm->status.last_int_vector = int_vector;
  wm->status.last_wait_mask = STATIC_CAPTURE_WAIT_MASK;
  if (ret <= 0) {
    pa_pu_status_t status;
    pa_pu_read_status(&status);
    record_error_locked(wm, ret == 0 ? ETIMEDOUT : EIO, phase, STATIC_CAPTURE_WAIT_MASK, int_vector);
    wm->status.last_wr_state = status.img_wr_state;
    wm->status.last_wr_end = status.img_wr_end;
    wm->status.last_corr_state = status.img_corr_state;
    wm->status.last_corr_end = status.img_corr_end;
    wm->status.last_gic_state = status.gic_state;
    wm->status.last_gic_end = status.gic_end;
    wm->status.last_gic_dfx = status.gic_dfx;
    log_error("static capture %s failed ret=%d addr=0x%08x wait_mask=0x%08x int_vector=0x%08x offset_addr=0x%08x output_addr=0x%08x",
              phase_name,
              ret,
              image_addr,
              STATIC_CAPTURE_WAIT_MASK,
              int_vector,
              wm->status.last_bright_addr,
              wm->status.last_dark_addr);
    pa_pu_dump_all_registers(phase_name);
  }
  pthread_mutex_unlock(&wm->mutex);

  if (ret > 0) {
    log_info("static capture phase done phase=%s addr=0x%08x int_vector=0x%08x", phase_name, image_addr, int_vector);
  }
  return ret > 0 ? 0 : -1;
}

static int copy_bright_to_offset_template(work_mode_context_t* wm, uint32_t bright_addr) {
  /*
   * 调试模式：避免 FPGA 直接写 uio0/offset，先让 FPGA 写 uio2 基地址。
   * bright 完成后由 ARM 复制到 uio0，用来判断卡死是否和 FPGA 写 offset 区有关。
   */
  if (wm->fpga_mem->image_pool == NULL ||
      wm->fpga_mem->offset_template == NULL ||
      bright_addr != wm->fpga_mem->image_pool_phys_base ||
      wm->fpga_mem->image_pool_map_size < ACTIVE_IMAGE_BYTES ||
      wm->fpga_mem->offset_map_size < ACTIVE_IMAGE_BYTES) {
    log_error("static bright copy invalid bright_addr=0x%08x image_pool=0x%08x image_size=0x%lx offset=0x%08x offset_size=0x%lx",
              bright_addr,
              wm->fpga_mem->image_pool_phys_base,
              (unsigned long)wm->fpga_mem->image_pool_map_size,
              wm->fpga_mem->offset_phys_base,
              (unsigned long)wm->fpga_mem->offset_map_size);
    return -1;
  }

  memcpy(wm->fpga_mem->offset_template, wm->fpga_mem->image_pool, ACTIVE_IMAGE_BYTES);
  log_info("static bright copied to offset template src=0x%08x dst=0x%08x bytes=0x%lx",
           bright_addr,
           wm->fpga_mem->offset_phys_base,
           (unsigned long)ACTIVE_IMAGE_BYTES);
  return 0;
}

static int run_static_capture(work_mode_context_t* wm, const static_idle_config_t* config) {
  uint32_t bright_addr = 0;
  uint32_t dark_addr = 0;

  pthread_mutex_lock(&wm->mutex);
  wm->status.capture_id++;
  /*
   * 第一帧未矫正 light 作为 offset 模板。
   * 默认 FPGA 直接写 uio0/offset；调试模式可改为先写 uio2，再由 ARM 复制到 uio0。
   * 第二帧才是实际输出图，从 uio2 环形图像池分配地址。
   */
#if STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU
  bright_addr = wm->fpga_mem->image_pool_phys_base;
#else
  bright_addr = wm->fpga_mem->offset_phys_base;
#endif
  bool output_ok = alloc_frame_locked(wm, "output", &dark_addr);
  if (!output_ok) {
    record_error_locked(wm, ENOSPC, WORK_PHASE_EXPOSURE_WINDOW, 0, 0);
    pthread_mutex_unlock(&wm->mutex);
    pa_pu_dump_all_registers("ddr_alloc_failed");
    return -1;
  }
  log_info("static offset light fixed addr=0x%08x offset_addr=0x%08x output_addr=0x%08x via_cpu=%u",
           bright_addr,
           wm->fpga_mem->offset_phys_base,
           dark_addr,
           STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ? 1u : 0u);
  wm->status.last_bright_addr = bright_addr;
  wm->status.last_dark_addr = dark_addr;
  set_state_locked(wm, WORK_STATE_EXPOSURE_WINDOW, WORK_PHASE_EXPOSURE_WINDOW);
  pthread_mutex_unlock(&wm->mutex);

  log_info("static capture start capture_id=%u offset_addr=0x%08x output_addr=0x%08x",
           wm->status.capture_id,
           bright_addr,
           dark_addr);

  /* 曝光窗口结束后先采未校正 light 写 offset 模板，再进入窗口采实际输出图。 */
  sleep_ms_interruptible(config->exposure_window_ms);
  if (run_capture_phase(wm, "bright", WORK_STATE_BRIGHT_CAPTURE, WORK_PHASE_BRIGHT_CAPTURE,
                        &config->bright_gic, &config->bright_corr, bright_addr) != 0) {
    return -1;
  }
#if STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU
  if (copy_bright_to_offset_template(wm, bright_addr) != 0) {
    pthread_mutex_lock(&wm->mutex);
    record_error_locked(wm, EIO, WORK_PHASE_BRIGHT_CAPTURE, STATIC_CAPTURE_WAIT_MASK, wm->status.last_int_vector);
    pthread_mutex_unlock(&wm->mutex);
    pa_pu_dump_all_registers("copy_offset_failed");
    return -1;
  }
#endif
  log_info("static bright phase done, enter dark window capture_id=%u dark_window_ms=%u output_addr=0x%08x",
           wm->status.capture_id,
           config->dark_window_ms,
           dark_addr);

  pthread_mutex_lock(&wm->mutex);
  set_state_locked(wm, WORK_STATE_DARK_WINDOW, WORK_PHASE_DARK_WINDOW);
  pthread_mutex_unlock(&wm->mutex);

  sleep_ms_interruptible(config->dark_window_ms);
  if (run_capture_phase(wm, "dark", WORK_STATE_DARK_CAPTURE, WORK_PHASE_DARK_CAPTURE,
                        &config->dark_gic, &config->dark_corr, dark_addr) != 0) {
    return -1;
  }

  log_info("static capture done capture_id=%u offset_addr=0x%08x output_addr=0x%08x",
           wm->status.capture_id,
           bright_addr,
           dark_addr);
  return 0;
}

static bool wait_until_or_request_locked(work_mode_context_t* wm, uint64_t deadline_ms) {
  /*
   * 空闲阶段等待两类事件：自清空定时到期，或上位机请求正式采图。
   * 如果采图请求先到，当前自清空周期被跳过，采图结束后重新计算下一次自清空时间。
   */
  while (!wm->stop_requested && !wm->pending_capture) {
    uint64_t now = monotonic_ms_local();
    if (now >= deadline_ms) {
      return false;
    }

    uint64_t remain = deadline_ms - now;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(remain / 1000u);
    ts.tv_nsec += (long)((remain % 1000u) * 1000000u);
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }

    int ret = pthread_cond_timedwait(&wm->cond, &wm->mutex, &ts);
    if (ret == ETIMEDOUT) {
      return false;
    }
  }
  return wm->pending_capture;
}

static void* work_thread_main(void* arg) {
  work_mode_context_t* wm = (work_mode_context_t*)arg;
  uint64_t next_clean_ms = 0;

  /* Static Idle 启动时先按默认参数配置一次 ROIC，后续仍允许调试命令重新配置。 */
  pa_pu_configure_roic_defaults();
  log_info("work mode thread started static idle roic configured once");

  pthread_mutex_lock(&wm->mutex);
  set_state_locked(wm, WORK_STATE_IDLE_WAIT, WORK_PHASE_NONE);
  next_clean_ms = monotonic_ms_local() + wm->config.idle_clean_interval_ms;
  pthread_mutex_unlock(&wm->mutex);

  while (true) {
    static_idle_config_t config;
    bool do_capture = false;

    pthread_mutex_lock(&wm->mutex);
    if (wm->stop_requested) {
      pthread_mutex_unlock(&wm->mutex);
      break;
    }

    wm->status.state = WORK_STATE_IDLE_WAIT;
    if (wm->status.last_error == 0) {
      wm->status.last_phase = WORK_PHASE_NONE;
    }
    pthread_cond_broadcast(&wm->cond);

    do_capture = wait_until_or_request_locked(wm, next_clean_ms);
    if (wm->stop_requested) {
      pthread_mutex_unlock(&wm->mutex);
      break;
    }
    if (do_capture) {
      wm->pending_capture = false;
    }
    config = wm->config;
    pthread_mutex_unlock(&wm->mutex);

    if (do_capture) {
      /* START_STATIC_IDLE_CAPTURE 是同步命令，这里执行完成后通过 cond 通知命令线程返回。 */
      int ret = run_static_capture(wm, &config);
      pthread_mutex_lock(&wm->mutex);
      wm->request_result = ret;
      wm->request_done = true;
      if (ret == 0) {
        wm->status.state = WORK_STATE_IDLE_WAIT;
        wm->status.last_error = 0;
        wm->status.last_phase = WORK_PHASE_NONE;
      } else {
        wm->status.state = WORK_STATE_IDLE_WAIT;
      }
      pthread_cond_broadcast(&wm->cond);
      pthread_mutex_unlock(&wm->mutex);
      next_clean_ms = monotonic_ms_local() + config.idle_clean_interval_ms;
      continue;
    }

    (void)run_idle_clean(wm, &config);
    next_clean_ms = monotonic_ms_local() + config.idle_clean_interval_ms;
  }

  pthread_mutex_lock(&wm->mutex);
  wm->thread_running = false;
  wm->status.state = WORK_STATE_STOPPED;
  wm->status.stop_requested = true;
  pthread_cond_broadcast(&wm->cond);
  pthread_mutex_unlock(&wm->mutex);
  log_info("work mode thread stopped");
  return NULL;
}

int work_mode_init(work_mode_context_t* wm, fpga_mem_t* fpga_mem) {
  if (wm == NULL || !fpga_mem_is_open(fpga_mem)) {
    return -1;
  }

  memset(wm, 0, sizeof(*wm));
  if (pthread_mutex_init(&wm->mutex, NULL) != 0) {
    return -1;
  }
  if (pthread_cond_init(&wm->cond, NULL) != 0) {
    pthread_mutex_destroy(&wm->mutex);
    return -1;
  }

  wm->initialized = true;
  wm->fpga_mem = fpga_mem;
  work_mode_default_static_idle_config(&wm->config);
  wm->status.mode = WORK_MODE_IDLE;
  wm->status.state = WORK_STATE_STOPPED;
  /* 模板物理地址来自 fpga_mem 映射结果，当前由 Makefile/app_config.h 显式配置。 */
  wm->config.bright_corr.offset_template_addr = fpga_mem->offset_phys_base;
  wm->config.dark_corr.offset_template_addr = fpga_mem->offset_phys_base;
  wm->config.bright_corr.gain_template_addr = fpga_mem->gain_phys_base;
  wm->config.dark_corr.gain_template_addr = fpga_mem->gain_phys_base;
  /* frame_stride 是 uio2 环形图像池的单帧步进，需要能容纳完整有效图像。 */
  wm->status.ddr_frame_stride = align_up_size(ACTIVE_IMAGE_BYTES, DDR_IMAGE_FRAME_ALIGN);
  size_t max_frame_count = wm->status.ddr_frame_stride == 0 ? 0 : fpga_mem->image_pool_map_size / wm->status.ddr_frame_stride;
  wm->status.ddr_frame_count = DDR_IMAGE_POOL_FRAME_COUNT == 0 ? max_frame_count : (size_t)DDR_IMAGE_POOL_FRAME_COUNT;
  if (STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU) {
    wm->status.ddr_next_offset = (uint32_t)wm->status.ddr_frame_stride;
  }

  if (wm->status.ddr_frame_stride == 0 ||
      wm->status.ddr_frame_stride > UINT32_MAX ||
      wm->status.ddr_frame_stride > fpga_mem->image_pool_map_size ||
      wm->status.ddr_frame_count == 0 ||
      wm->status.ddr_frame_count > max_frame_count ||
      (STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU && wm->status.ddr_frame_count < 2u) ||
      fpga_mem->image_pool_phys_base == 0) {
    log_error("static idle image pool invalid pool_base=0x%08x pool_size=0x%lx frame_stride=0x%lx frame_count=%lu max_frame_count=%lu via_cpu=%u",
              fpga_mem->image_pool_phys_base,
              (unsigned long)fpga_mem->image_pool_map_size,
              (unsigned long)wm->status.ddr_frame_stride,
              (unsigned long)wm->status.ddr_frame_count,
              (unsigned long)max_frame_count,
              STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ? 1u : 0u);
    return -1;
  }

  log_info("work mode init image_pool_base=0x%08x image_pool_size=0x%lx frame_stride=0x%lx frame_count=%lu max_frame_count=%lu via_cpu=%u ring=1",
           fpga_mem->image_pool_phys_base,
           (unsigned long)fpga_mem->image_pool_map_size,
           (unsigned long)wm->status.ddr_frame_stride,
           (unsigned long)wm->status.ddr_frame_count,
           (unsigned long)max_frame_count,
           STATIC_IDLE_BRIGHT_TO_OFFSET_VIA_CPU ? 1u : 0u);
  return 0;
}

int work_mode_start(work_mode_context_t* wm) {
  if (wm == NULL || !wm->initialized) {
    return -1;
  }
  pthread_mutex_lock(&wm->mutex);
  if (wm->thread_running) {
    pthread_mutex_unlock(&wm->mutex);
    return 0;
  }
  wm->stop_requested = false;
  wm->thread_running = true;
  pthread_mutex_unlock(&wm->mutex);

  if (pthread_create(&wm->thread, NULL, work_thread_main, wm) != 0) {
    pthread_mutex_lock(&wm->mutex);
    wm->thread_running = false;
    pthread_mutex_unlock(&wm->mutex);
    return -1;
  }
  return 0;
}

void work_mode_stop(work_mode_context_t* wm) {
  if (wm == NULL || !wm->initialized) {
    return;
  }

  pthread_mutex_lock(&wm->mutex);
  wm->stop_requested = true;
  pthread_cond_broadcast(&wm->cond);
  bool running = wm->thread_running;
  pthread_mutex_unlock(&wm->mutex);

  if (running) {
    pthread_join(wm->thread, NULL);
  }
}

int work_mode_update_static_idle_config(work_mode_context_t* wm, const static_idle_config_t* config) {
  if (wm == NULL || config == NULL) {
    return -1;
  }

  pthread_mutex_lock(&wm->mutex);
  /* 采图窗口和采图阶段由 ARM 保证完整时序，中途不接受配置更新。 */
  if (wm->status.state == WORK_STATE_BRIGHT_CAPTURE ||
      wm->status.state == WORK_STATE_DARK_CAPTURE ||
      wm->status.state == WORK_STATE_EXPOSURE_WINDOW ||
      wm->status.state == WORK_STATE_DARK_WINDOW) {
    pthread_mutex_unlock(&wm->mutex);
    return -2;
  }
  wm->config = *config;
  pthread_cond_broadcast(&wm->cond);
  pthread_mutex_unlock(&wm->mutex);
  return 0;
}

void work_mode_get_static_idle_config(work_mode_context_t* wm, static_idle_config_t* config) {
  if (wm == NULL || config == NULL) {
    return;
  }
  pthread_mutex_lock(&wm->mutex);
  *config = wm->config;
  pthread_mutex_unlock(&wm->mutex);
}

int work_mode_start_static_idle_capture(work_mode_context_t* wm, work_mode_status_t* result) {
  if (wm == NULL) {
    return -1;
  }

  pthread_mutex_lock(&wm->mutex);
  if (!wm->thread_running) {
    if (result != NULL) {
      *result = wm->status;
    }
    pthread_mutex_unlock(&wm->mutex);
    return -1;
  }
  if (wm->pending_capture ||
      wm->status.state == WORK_STATE_EXPOSURE_WINDOW ||
      wm->status.state == WORK_STATE_BRIGHT_CAPTURE ||
      wm->status.state == WORK_STATE_DARK_WINDOW ||
      wm->status.state == WORK_STATE_DARK_CAPTURE) {
    if (result != NULL) {
      *result = wm->status;
    }
    pthread_mutex_unlock(&wm->mutex);
    return -2;
  }

  /* 设置 pending_capture 后由工作线程在 IDLE_WAIT 中接管真实硬件时序。 */
  wm->pending_capture = true;
  wm->request_done = false;
  wm->request_result = 0;
  pthread_cond_broadcast(&wm->cond);

  while (!wm->request_done && wm->thread_running && !wm->stop_requested) {
    pthread_cond_wait(&wm->cond, &wm->mutex);
  }

  if (result != NULL) {
    *result = wm->status;
  }
  int ret = wm->request_done ? wm->request_result : -1;
  pthread_mutex_unlock(&wm->mutex);
  return ret;
}

void work_mode_get_status(work_mode_context_t* wm, work_mode_status_t* status) {
  if (wm == NULL || status == NULL) {
    return;
  }
  pthread_mutex_lock(&wm->mutex);
  *status = wm->status;
  status->pending_capture = wm->pending_capture;
  status->stop_requested = wm->stop_requested;
  pthread_mutex_unlock(&wm->mutex);
}

bool work_mode_allows_write_reg(work_mode_context_t* wm) {
  if (wm == NULL) {
    return true;
  }
  pthread_mutex_lock(&wm->mutex);
  /* 调试写寄存器只允许在空闲等待或工作线程停止时进行，避免破坏正式采图链路。 */
  bool allowed = wm->status.state == WORK_STATE_IDLE_WAIT || wm->status.state == WORK_STATE_STOPPED;
  pthread_mutex_unlock(&wm->mutex);
  return allowed;
}

bool work_mode_allows_hardware_action(work_mode_context_t* wm) {
  if (wm == NULL) {
    return true;
  }
  pthread_mutex_lock(&wm->mutex);
  /* CONFIG/START 类调试命令和 WRITE_REG 共用同一套忙碌保护策略。 */
  bool allowed = wm->status.state == WORK_STATE_IDLE_WAIT || wm->status.state == WORK_STATE_STOPPED;
  pthread_mutex_unlock(&wm->mutex);
  return allowed;
}
