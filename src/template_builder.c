#include "template_builder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "log.h"

/* 计算一行有效图像在整幅 FPGA 图像中的起始像素。 */
static size_t active_row_offset(unsigned row) {
  return ((size_t)(row + ROW_OFFSET) * DEVICE_WIDTH + COL_OFFSET);
}

/* 将一行有效模板写入 FPGA 模板内存中的整幅图布局。 */
static void copy_active_row_to_device(uint16_t* dst, unsigned row, const uint16_t* src_row) {
  uint16_t* dst_row = dst + active_row_offset(row);
  for (unsigned col = 0; col < IMAGE_WIDTH; ++col) {
    dst_row[col] = src_row[col];
  }
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

static int read_all_or_fail(int fd, void* data, size_t size) {
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

/*
 * 写模板文件前创建父目录。O_CREAT 只能创建文件本身，不能创建不存在的目录。
 */
static int ensure_parent_dir(const char* path) {
  char dir[256];
  const char* slash = NULL;
  size_t len = 0;

  if (path == NULL) {
    return -1;
  }

  slash = strrchr(path, '/');
  if (slash == NULL || slash == path) {
    return 0;
  }

  len = (size_t)(slash - path);
  if (len >= sizeof(dir)) {
    log_error("template parent path too long: %s", path);
    return -1;
  }

  memcpy(dir, path, len);
  dir[len] = '\0';

  for (char* p = dir + 1; *p != '\0'; ++p) {
    if (*p != '/') {
      continue;
    }
    *p = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
      log_error("create template dir %s failed: %d", dir, errno);
      *p = '/';
      return -1;
    }
    *p = '/';
  }

  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    log_error("create template dir %s failed: %d", dir, errno);
    return -1;
  }

  return 0;
}

/* 从模板文件读取有效区域，再铺回 FPGA 期望的整幅图内存布局。 */
static int load_active_template(const char* path, uint16_t* dst) {
  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    log_warn("template %s not found: %d", path, errno);
    return -1;
  }

  uint16_t row[IMAGE_WIDTH];
  for (unsigned row_index = 0; row_index < IMAGE_HEIGHT; ++row_index) {
    if (read_all_or_fail(fd, row, sizeof(row)) != 0) {
      log_error("read template %s failed: %d", path, errno);
      close(fd);
      return -1;
    }
    copy_active_row_to_device(dst, row_index, row);
  }

  close(fd);
  return 0;
}

int template_load_files(fpga_mem_t* mem) {
  if (!fpga_mem_is_open(mem)) {
    return -1;
  }

  int ret = 0;
  if (load_active_template(TEMPLATE_OFFSET_FILE, (uint16_t*)mem->offset_template) != 0) {
    ret = -1;
  }

  /* gain 区只保存一份完整模板，物理窗口大小由设备树 UIO map0 决定。 */
  if (load_active_template(TEMPLATE_GAIN_FILE, (uint16_t*)mem->gain_template) != 0) {
    ret = -1;
  }

  return ret;
}

int template_make_offset(fpga_mem_t* mem) {
  if (!fpga_mem_is_open(mem)) {
    return -1;
  }

  /*
   * 手动 MAKE_OFFSET 保持当前单帧行为，便于现场明确触发。
   * 自动暗场更新后续应新增多帧平均和质量校验路径，不直接复用这里覆盖模板。
   */
  if (ensure_parent_dir(TEMPLATE_OFFSET_FILE) != 0) {
    return -1;
  }

  int fd = open(TEMPLATE_OFFSET_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    log_error("open %s failed: %d", TEMPLATE_OFFSET_FILE, errno);
    return -1;
  }

  const uint16_t* image = (const uint16_t*)mem->image;
  uint16_t* offset = (uint16_t*)mem->offset_template;

  for (unsigned row = 0; row < IMAGE_HEIGHT; ++row) {
    const uint16_t* src_row = image + active_row_offset(row);
    copy_active_row_to_device(offset, row, src_row);
    if (write_all(fd, src_row, IMAGE_WIDTH * sizeof(uint16_t)) != 0) {
      log_error("write %s failed: %d", TEMPLATE_OFFSET_FILE, errno);
      close(fd);
      return -1;
    }
  }

  close(fd);
  log_info("offset template created: %s", TEMPLATE_OFFSET_FILE);
  return 0;
}

int template_make_gain(fpga_mem_t* mem) {
  if (!fpga_mem_is_open(mem)) {
    return -1;
  }

  const uint16_t* image = (const uint16_t*)mem->image;
  const uint16_t* offset = (const uint16_t*)mem->offset_template;
  uint16_t* gain = (uint16_t*)mem->gain_template;

  uint64_t sum = 0;
  uint64_t count = 0;

  /* 第一遍统计扣 offset 后的平均亮场值。 */
  for (unsigned row = 0; row < IMAGE_HEIGHT; ++row) {
    const uint16_t* image_row = image + active_row_offset(row);
    const uint16_t* offset_row = offset + active_row_offset(row);
    for (unsigned col = 0; col < IMAGE_WIDTH; ++col) {
      int corrected = (int)image_row[col] - (int)offset_row[col];
      if (corrected > 0) {
        sum += (uint32_t)corrected;
        ++count;
      }
    }
  }

  if (count == 0) {
    log_error("gain template cannot be created from empty flat image");
    return -1;
  }

  uint32_t mean = (uint32_t)(sum / count);
  if (ensure_parent_dir(TEMPLATE_GAIN_FILE) != 0) {
    return -1;
  }

  int fd = open(TEMPLATE_GAIN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    log_error("open %s failed: %d", TEMPLATE_GAIN_FILE, errno);
    return -1;
  }

  uint16_t row_buf[IMAGE_WIDTH];

  /*
   * 第二遍生成 16 位增益系数。
   * 当前实现采用 Q12 定点：gain = mean * 4096 / corrected，并做 16 位饱和。
   * 如果 PA 端增益格式不同，只需要替换这一段换算逻辑。
   */
  for (unsigned row = 0; row < IMAGE_HEIGHT; ++row) {
    const uint16_t* image_row = image + active_row_offset(row);
    const uint16_t* offset_row = offset + active_row_offset(row);
    for (unsigned col = 0; col < IMAGE_WIDTH; ++col) {
      int corrected = (int)image_row[col] - (int)offset_row[col];
      uint32_t value = corrected > 0 ? (mean * 4096u) / (uint32_t)corrected : 0xffffu;
      row_buf[col] = (uint16_t)(value > 0xffffu ? 0xffffu : value);
    }

    copy_active_row_to_device(gain, row, row_buf);
    if (write_all(fd, row_buf, sizeof(row_buf)) != 0) {
      log_error("write %s failed: %d", TEMPLATE_GAIN_FILE, errno);
      close(fd);
      return -1;
    }
  }

  close(fd);
  log_info("gain template created: %s mean=%u", TEMPLATE_GAIN_FILE, mean);
  return 0;
}

