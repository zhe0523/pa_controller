#include "calibration_builder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"
#include "pa_pu.h"

#ifndef CAL_GAIN_DIR
/* 校准中间文件目录。均值图较大，放在可持久化文件系统中便于断点排查。 */
#define CAL_GAIN_DIR "/usr/local/calib"
#endif

enum {
  CAL_GAIN_CAPTURE_WAIT_MASK = PA_PU_IRQ_IMG_CORR_END | PA_PU_IRQ_IMG_WR_END | PA_PU_IRQ_GIC_END,
  FILTER_RADIUS = 5,
  FILTER_SIZE = FILTER_RADIUS * 2 + 1,
};

typedef struct {
  uint32_t level;
  bool ready;
  uint32_t median;
  char mean_path[128];
} cal_gain_level_t;

typedef struct {
  bool active;
  uint32_t level_count;
  uint32_t frames_per_level;
  float defect_threshold;
  uint32_t bad_pixel_count;
  cal_gain_level_t levels[CAL_GAIN_MAX_LEVELS];
} cal_gain_context_t;

static cal_gain_context_t g_cal_gain;

static size_t active_pixel_count(void) {
  return (size_t)IMAGE_WIDTH * IMAGE_HEIGHT;
}

static size_t active_row_offset(unsigned row) {
  return ((size_t)(row + ROW_OFFSET) * DEVICE_WIDTH + COL_OFFSET);
}

static uint16_t clamp_u16_from_u64(uint64_t value) {
  return value > 0xffffu ? 0xffffu : (uint16_t)value;
}

static int ensure_calib_dir(void) {
  if (mkdir(CAL_GAIN_DIR, 0755) == 0 || errno == EEXIST) {
    return 0;
  }
  log_error("create calibration dir %s failed: %d", CAL_GAIN_DIR, errno);
  return -1;
}

static int write_all(int fd, const void* data, size_t size) {
  const uint8_t* p = (const uint8_t*)data;
  while (size > 0) {
    ssize_t n = write(fd, p, size);
    if (n <= 0) {
      return -1;
    }
    p += n;
    size -= (size_t)n;
  }
  return 0;
}

static int read_all(int fd, void* data, size_t size) {
  uint8_t* p = (uint8_t*)data;
  while (size > 0) {
    ssize_t n = read(fd, p, size);
    if (n <= 0) {
      return -1;
    }
    p += n;
    size -= (size_t)n;
  }
  return 0;
}

static cal_gain_level_t* find_level(uint32_t level) {
  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    if (g_cal_gain.levels[i].level == level) {
      return &g_cal_gain.levels[i];
    }
  }
  return NULL;
}

static int capture_one_raw_frame(fpga_mem_t* mem) {
  pa_pu_gic_config_t gic = {
    .req_code = PA_PU_GIC_REQ_SERIAL_SCAN,
    .dout_enable = true,
    .line_time_ns = GIC_DEFAULT_LINE_TIME_NS,
    .oe_raising_edge_ns = GIC_DEFAULT_OE_RISE_NS,
    .oe_falling_edge_ns = GIC_DEFAULT_OE_FALL_NS,
    .start_row = GIC_DEFAULT_START_ROW,
    .end_row = GIC_DEFAULT_END_ROW,
    .binning_mode = GIC_DEFAULT_BINNING,
  };
  pa_pu_corr_config_t corr = {
    .pkg_num = CORR_DEFAULT_PKG_NUM,
    .row_num = CORR_DEFAULT_ROW_NUM,
    .col_num = CORR_DEFAULT_COL_NUM,
    .offset_enable = false,
    .offset_template_addr = mem->offset_phys_base,
    .offset_adder_value = CORR_DEFAULT_OFFSET_ADDER_VALUE,
    .gain_enable = false,
    .gain_template_addr = mem->gain_phys_base,
    .gain_clipping_value = CORR_DEFAULT_GAIN_CLIPPING_VALUE,
    .defect_enable = false,
  };
  uint32_t int_vector = 0;

  /*
   * gain 校准采原始灰阶帧，校正全关，只验证 GIC/IMG_WR/IMG_CORR 三模块完成。
   * 图像固定写入 uio2 起始地址，采完立即由 ARM 读取有效区域参与均值累加。
   */
  pa_pu_configure_gic(&gic);
  pa_pu_configure_image_write(mem->image_pool_phys_base);
  pa_pu_configure_correction(&corr);
  pa_pu_prepare_irq_wait();
  pa_pu_start_capture_triplet();
  int ret = pa_pu_wait_int_vector_all(CAL_GAIN_CAPTURE_WAIT_MASK, PA_PU_IRQ_TIMEOUT_MS, &int_vector);
  if (ret <= 0) {
    log_error("cal gain raw capture failed ret=%d int_vector=0x%08x wait_mask=0x%08x",
              ret,
              int_vector,
              CAL_GAIN_CAPTURE_WAIT_MASK);
    pa_pu_dump_all_registers("cal_gain_capture");
    return -1;
  }
  return 0;
}

static int write_mean_file(const cal_gain_level_t* level, const uint32_t* sums, uint32_t frames) {
  int fd = open(level->mean_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    log_error("open %s failed: %d", level->mean_path, errno);
    return -1;
  }

  uint16_t* row = (uint16_t*)malloc((size_t)IMAGE_WIDTH * sizeof(uint16_t));
  if (row == NULL) {
    close(fd);
    return -1;
  }

  for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
    const uint32_t* sum_row = sums + (size_t)r * IMAGE_WIDTH;
    for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
      row[c] = clamp_u16_from_u64(((uint64_t)sum_row[c] + frames / 2u) / frames);
    }
    if (write_all(fd, row, (size_t)IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
      log_error("write %s failed: %d", level->mean_path, errno);
      free(row);
      close(fd);
      return -1;
    }
  }

  free(row);
  close(fd);
  return 0;
}

static int compute_file_median(const char* path, uint32_t* median_out) {
  uint32_t* histogram = (uint32_t*)calloc(65536u, sizeof(uint32_t));
  uint16_t* row = (uint16_t*)malloc((size_t)IMAGE_WIDTH * sizeof(uint16_t));
  if (histogram == NULL || row == NULL) {
    free(histogram);
    free(row);
    return -1;
  }

  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    free(histogram);
    free(row);
    return -1;
  }

  for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
    if (read_all(fd, row, (size_t)IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
      close(fd);
      free(histogram);
      free(row);
      return -1;
    }
    for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
      histogram[row[c]]++;
    }
  }
  close(fd);

  uint64_t target = (uint64_t)active_pixel_count() / 2u;
  uint64_t accum = 0;
  uint32_t median = 0;
  for (uint32_t v = 0; v <= 0xffffu; ++v) {
    accum += histogram[v];
    if (accum > target) {
      median = v;
      break;
    }
  }

  *median_out = median;
  free(histogram);
  free(row);
  return 0;
}

static void median_filter_radius5(const uint16_t* src, uint16_t* dst) {
  /*
   * 精确 11x11 中值滤波。
   * 每一行从左侧窗口建立 16bit 直方图，随后列方向滑动更新直方图；
   * median/less_count 随直方图增删做局部调整，避免每个像素都重新排序窗口。
   */
  uint32_t* hist = (uint32_t*)calloc(65536u, sizeof(uint32_t));
  if (hist == NULL) {
    memset(dst, 0, active_pixel_count() * sizeof(uint16_t));
    return;
  }

  for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
    memset(hist, 0, 65536u * sizeof(uint32_t));
    int row_begin = (int)r - FILTER_RADIUS;
    int row_end = (int)r + FILTER_RADIUS;
    int col_begin = 0;
    int col_end = FILTER_RADIUS;
    uint32_t window_count = 0;

    for (int rr = row_begin; rr <= row_end; ++rr) {
      if (rr < 0 || rr >= (int)IMAGE_HEIGHT) {
        continue;
      }
      for (int cc = col_begin; cc <= col_end && cc < (int)IMAGE_WIDTH; ++cc) {
        uint16_t v = src[(size_t)rr * IMAGE_WIDTH + (size_t)cc];
        hist[v]++;
        window_count++;
      }
    }

    uint32_t target = window_count / 2u;
    uint32_t median = 0;
    uint32_t less_count = 0;
    uint32_t accum = 0;
    while (median <= 0xffffu) {
      if (accum + hist[median] > target) {
        less_count = accum;
        break;
      }
      accum += hist[median];
      ++median;
    }

    for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
      if (c > 0) {
        int remove_col = (int)c - FILTER_RADIUS - 1;
        int add_col = (int)c + FILTER_RADIUS;
        for (int rr = row_begin; rr <= row_end; ++rr) {
          if (rr < 0 || rr >= (int)IMAGE_HEIGHT) {
            continue;
          }
          if (remove_col >= 0 && remove_col < (int)IMAGE_WIDTH) {
            uint16_t v = src[(size_t)rr * IMAGE_WIDTH + (size_t)remove_col];
            hist[v]--;
            window_count--;
            if (v < median) {
              less_count--;
            }
          }
          if (add_col >= 0 && add_col < (int)IMAGE_WIDTH) {
            uint16_t v = src[(size_t)rr * IMAGE_WIDTH + (size_t)add_col];
            hist[v]++;
            window_count++;
            if (v < median) {
              less_count++;
            }
          }
        }

        target = window_count / 2u;
        while (median < 0xffffu && less_count + hist[median] <= target) {
          less_count += hist[median];
          ++median;
        }
        while (median > 0 && less_count > target) {
          --median;
          less_count -= hist[median];
        }
      }

      dst[(size_t)r * IMAGE_WIDTH + c] = (uint16_t)median;
    }
    if ((r % 256u) == 0) {
      log_info("cal gain median filter progress row=%u/%u", r, IMAGE_HEIGHT);
    }
  }

  free(hist);
}

static void mean_filter_radius5(const uint16_t* src, uint16_t* dst) {
  /*
   * 11x11 均值滤波使用列方向滑动和 + 行方向滑动和。
   * 这样只需要 IMAGE_WIDTH 个列累加值，不需要额外的大积分图。
   */
  uint32_t* col_sum = (uint32_t*)calloc(IMAGE_WIDTH, sizeof(uint32_t));
  if (col_sum == NULL) {
    memset(dst, 0, active_pixel_count() * sizeof(uint16_t));
    return;
  }

  int current_top = 0;
  int current_bottom = -1;
  for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
    int target_top = (int)r - FILTER_RADIUS;
    int target_bottom = (int)r + FILTER_RADIUS;
    if (target_top < 0) {
      target_top = 0;
    }
    if (target_bottom >= (int)IMAGE_HEIGHT) {
      target_bottom = (int)IMAGE_HEIGHT - 1;
    }

    while (current_bottom < target_bottom) {
      ++current_bottom;
      const uint16_t* add_row = src + (size_t)current_bottom * IMAGE_WIDTH;
      for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
        col_sum[c] += add_row[c];
      }
    }
    while (current_top < target_top) {
      const uint16_t* remove_row = src + (size_t)current_top * IMAGE_WIDTH;
      for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
        col_sum[c] -= remove_row[c];
      }
      ++current_top;
    }

    uint32_t row_count = (uint32_t)(target_bottom - target_top + 1);
    uint64_t window_sum = 0;
    int current_left = 0;
    int current_right = -1;
    for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
      int target_left = (int)c - FILTER_RADIUS;
      int target_right = (int)c + FILTER_RADIUS;
      if (target_left < 0) {
        target_left = 0;
      }
      if (target_right >= (int)IMAGE_WIDTH) {
        target_right = (int)IMAGE_WIDTH - 1;
      }

      while (current_right < target_right) {
        ++current_right;
        window_sum += col_sum[current_right];
      }
      while (current_left < target_left) {
        window_sum -= col_sum[current_left];
        ++current_left;
      }

      uint32_t col_count = (uint32_t)(target_right - target_left + 1);
      uint32_t count = row_count * col_count;
      dst[(size_t)r * IMAGE_WIDTH + c] = (uint16_t)((window_sum + count / 2u) / count);
    }
  }

  free(col_sum);
}

static int load_mean_image(const char* path, uint16_t* image) {
  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    return -1;
  }
  int ret = read_all(fd, image, active_pixel_count() * sizeof(uint16_t));
  close(fd);
  return ret;
}

static int build_bad_pixel_map(uint8_t* bad_map, uint32_t* bad_count) {
  size_t pixels = active_pixel_count();
  uint16_t* mean_image = (uint16_t*)malloc(pixels * sizeof(uint16_t));
  uint16_t* median_image = (uint16_t*)malloc(pixels * sizeof(uint16_t));
  uint16_t* filtered_image = (uint16_t*)malloc(pixels * sizeof(uint16_t));
  if (mean_image == NULL || median_image == NULL || filtered_image == NULL) {
    free(mean_image);
    free(median_image);
    free(filtered_image);
    return -1;
  }

  memset(bad_map, 0, pixels);
  for (uint32_t level_index = 0; level_index < g_cal_gain.level_count; ++level_index) {
    const cal_gain_level_t* level = &g_cal_gain.levels[level_index];
    if (load_mean_image(level->mean_path, mean_image) != 0) {
      free(mean_image);
      free(median_image);
      free(filtered_image);
      return -1;
    }

    log_info("cal gain defect filter begin level=%u threshold=%.3f", level->level, g_cal_gain.defect_threshold);
    median_filter_radius5(mean_image, median_image);
    mean_filter_radius5(median_image, filtered_image);

    for (size_t i = 0; i < pixels; ++i) {
      uint16_t original = mean_image[i];
      if (original == 0) {
        bad_map[i] = 1u;
        continue;
      }
      uint32_t diff = filtered_image[i] > original ? filtered_image[i] - original : original - filtered_image[i];
      float ratio = (float)diff / (float)original;
      if (ratio > g_cal_gain.defect_threshold) {
        bad_map[i] = 1u;
      }
    }
  }

  uint32_t count = 0;
  for (size_t i = 0; i < pixels; ++i) {
    if (bad_map[i]) {
      ++count;
    }
  }
  *bad_count = count;

  free(mean_image);
  free(median_image);
  free(filtered_image);
  return 0;
}

static uint16_t gain_from_slope(double slope, bool bad_pixel) {
  if (bad_pixel || slope < 0.0 || slope >= 3.99) {
    return 0u;
  }

  double scaled = slope * 16384.0;
  if (scaled < 0.0) {
    scaled = -scaled + 32768.0;
  }
  if (scaled > 65535.0) {
    scaled = 65535.0;
  }
  return (uint16_t)(scaled + 0.5);
}

static int build_gain_template(const uint8_t* bad_map) {
  int mean_fds[CAL_GAIN_MAX_LEVELS];
  uint16_t* rows[CAL_GAIN_MAX_LEVELS];
  uint16_t* out_row = NULL;
  int out_fd = -1;

  for (uint32_t i = 0; i < CAL_GAIN_MAX_LEVELS; ++i) {
    mean_fds[i] = -1;
    rows[i] = NULL;
  }

  out_row = (uint16_t*)malloc((size_t)IMAGE_WIDTH * sizeof(uint16_t));
  if (out_row == NULL) {
    return -1;
  }

  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    mean_fds[i] = open(g_cal_gain.levels[i].mean_path, O_RDONLY);
    rows[i] = (uint16_t*)malloc((size_t)IMAGE_WIDTH * sizeof(uint16_t));
    if (mean_fds[i] == -1 || rows[i] == NULL) {
      goto fail;
    }
  }

  out_fd = open(TEMPLATE_GAIN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out_fd == -1) {
    goto fail;
  }

  for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
    for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
      if (read_all(mean_fds[i], rows[i], (size_t)IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
        goto fail;
      }
    }

    for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
      double mean_x = 0.0;
      double mean_y = 0.0;
      for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
        mean_x += (double)g_cal_gain.levels[i].median;
        mean_y += (double)rows[i][c];
      }
      mean_x /= (double)g_cal_gain.level_count;
      mean_y /= (double)g_cal_gain.level_count;

      double num = 0.0;
      double den = 0.0;
      for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
        double dx = (double)g_cal_gain.levels[i].median - mean_x;
        double dy = (double)rows[i][c] - mean_y;
        num += dx * dy;
        den += dx * dx;
      }

      double slope = den > 0.0 ? num / den : 0.0;
      size_t index = (size_t)r * IMAGE_WIDTH + c;
      out_row[c] = gain_from_slope(slope, bad_map[index] != 0);
    }

    if (write_all(out_fd, out_row, (size_t)IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
      goto fail;
    }
    if ((r % 512u) == 0) {
      log_info("cal gain build progress row=%u/%u", r, IMAGE_HEIGHT);
    }
  }

  close(out_fd);
  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    close(mean_fds[i]);
    free(rows[i]);
  }
  free(out_row);
  return 0;

fail:
  if (out_fd != -1) {
    close(out_fd);
  }
  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    if (mean_fds[i] != -1) {
      close(mean_fds[i]);
    }
    free(rows[i]);
  }
  free(out_row);
  return -1;
}

static int load_gain_file_to_mem(fpga_mem_t* mem) {
  int fd = open(TEMPLATE_GAIN_FILE, O_RDONLY);
  if (fd == -1) {
    return -1;
  }

  uint16_t* row = (uint16_t*)malloc((size_t)IMAGE_WIDTH * sizeof(uint16_t));
  if (row == NULL) {
    close(fd);
    return -1;
  }

  for (unsigned repeat = 0; repeat < GAIN_TEMPLATE_REPEAT_COUNT; ++repeat) {
    uint16_t* repeat_gain = (uint16_t*)mem->gain_template + (size_t)repeat * DEVICE_WIDTH * DEVICE_HEIGHT;
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
      free(row);
      close(fd);
      return -1;
    }
    for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
      if (read_all(fd, row, (size_t)IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
        free(row);
        close(fd);
        return -1;
      }
      uint16_t* dst = repeat_gain + active_row_offset(r);
      memcpy(dst, row, (size_t)IMAGE_WIDTH * sizeof(uint16_t));
    }
  }

  free(row);
  close(fd);
  return 0;
}

int calibration_gain_begin(const uint32_t* levels,
                           uint32_t level_count,
                           uint32_t frames_per_level,
                           float defect_threshold) {
  if (levels == NULL ||
      level_count == 0u ||
      level_count > CAL_GAIN_MAX_LEVELS ||
      frames_per_level == 0u ||
      frames_per_level > 1024u ||
      defect_threshold <= 0.0f) {
    return -1;
  }
  if (ensure_calib_dir() != 0) {
    return -1;
  }

  memset(&g_cal_gain, 0, sizeof(g_cal_gain));
  g_cal_gain.active = true;
  g_cal_gain.level_count = level_count;
  g_cal_gain.frames_per_level = frames_per_level;
  g_cal_gain.defect_threshold = defect_threshold;
  for (uint32_t i = 0; i < level_count; ++i) {
    g_cal_gain.levels[i].level = levels[i];
    snprintf(g_cal_gain.levels[i].mean_path,
             sizeof(g_cal_gain.levels[i].mean_path),
             "%s/gain_mean_%u.raw",
             CAL_GAIN_DIR,
             levels[i]);
  }

  log_info("cal gain begin levels=%u frames=%u threshold=%.3f", level_count, frames_per_level, defect_threshold);
  return 0;
}

int calibration_gain_capture_level(fpga_mem_t* mem, uint32_t level_value) {
  if (!g_cal_gain.active || !fpga_mem_is_open(mem)) {
    return -1;
  }

  cal_gain_level_t* level = find_level(level_value);
  if (level == NULL) {
    return -1;
  }

  size_t pixels = active_pixel_count();
  uint32_t* sums = (uint32_t*)calloc(pixels, sizeof(uint32_t));
  if (sums == NULL) {
    log_error("cal gain sum buffer alloc failed bytes=0x%lx", (unsigned long)(pixels * sizeof(uint32_t)));
    return -1;
  }

  const uint16_t* image = (const uint16_t*)mem->image_pool;
  for (uint32_t frame = 0; frame < g_cal_gain.frames_per_level; ++frame) {
    if (capture_one_raw_frame(mem) != 0) {
      free(sums);
      return -1;
    }
    for (unsigned r = 0; r < IMAGE_HEIGHT; ++r) {
      const uint16_t* src_row = image + active_row_offset(r);
      uint32_t* sum_row = sums + (size_t)r * IMAGE_WIDTH;
      for (unsigned c = 0; c < IMAGE_WIDTH; ++c) {
        sum_row[c] += src_row[c];
      }
    }
    log_info("cal gain capture level=%u frame=%u/%u", level_value, frame + 1u, g_cal_gain.frames_per_level);
  }

  int ret = write_mean_file(level, sums, g_cal_gain.frames_per_level);
  free(sums);
  if (ret != 0) {
    return -1;
  }

  if (compute_file_median(level->mean_path, &level->median) != 0) {
    return -1;
  }
  level->ready = true;
  log_info("cal gain level ready level=%u median=%u path=%s", level_value, level->median, level->mean_path);
  return 0;
}

int calibration_gain_build(fpga_mem_t* mem) {
  if (!g_cal_gain.active || !fpga_mem_is_open(mem)) {
    return -1;
  }
  if (g_cal_gain.level_count < 2u) {
    log_error("cal gain build requires at least 2 levels for linear fit, current=%u", g_cal_gain.level_count);
    return -1;
  }
  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    if (!g_cal_gain.levels[i].ready) {
      log_error("cal gain build missing level=%u", g_cal_gain.levels[i].level);
      return -1;
    }
  }

  uint8_t* bad_map = (uint8_t*)malloc(active_pixel_count());
  if (bad_map == NULL) {
    return -1;
  }
  uint32_t bad_count = 0;
  if (build_bad_pixel_map(bad_map, &bad_count) != 0) {
    free(bad_map);
    return -1;
  }

  if (build_gain_template(bad_map) != 0) {
    free(bad_map);
    return -1;
  }
  free(bad_map);

  if (load_gain_file_to_mem(mem) != 0) {
    return -1;
  }

  g_cal_gain.bad_pixel_count = bad_count;
  log_info("cal gain build done gain=%s bad_pixels=%u", TEMPLATE_GAIN_FILE, bad_count);
  return 0;
}

void calibration_gain_cancel(void) {
  memset(&g_cal_gain, 0, sizeof(g_cal_gain));
}

void calibration_gain_get_status(cal_gain_status_t* status) {
  if (status == NULL) {
    return;
  }
  memset(status, 0, sizeof(*status));
  status->active = g_cal_gain.active ? 1 : 0;
  status->level_count = g_cal_gain.level_count;
  status->frames_per_level = g_cal_gain.frames_per_level;
  status->defect_threshold = g_cal_gain.defect_threshold;
  status->bad_pixel_count = g_cal_gain.bad_pixel_count;
  for (uint32_t i = 0; i < g_cal_gain.level_count; ++i) {
    status->levels[i].level = g_cal_gain.levels[i].level;
    status->levels[i].median = g_cal_gain.levels[i].median;
    status->levels[i].ready = g_cal_gain.levels[i].ready ? 1 : 0;
    status->levels[i].frames_captured = g_cal_gain.levels[i].ready ? g_cal_gain.frames_per_level : 0u;
    if (g_cal_gain.levels[i].ready) {
      status->levels_ready++;
    }
  }
}
