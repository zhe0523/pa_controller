#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "dynamic_mode.h"
#include "fpga_mem.h"
#include "work_mode.h"

/*
 * 运行时配置快照。
 *
 * 编译宏只负责提供出厂默认值；程序真正运行时使用这一份快照。配置文件
 * 采用简单 INI 格式，避免 RS422 每次携带完整寄存器参数，也便于现场备份。
 */
typedef struct {
  pa_pu_gic_config_t gic;
  pa_pu_roic_config_t roic;
  pa_pu_corr_config_t corr;
  static_idle_config_t static_idle;
  pa_pu_dync_config_t dync;
  unsigned dynamic_start_timeout_ms;
  unsigned dynamic_state_poll_interval_ms;
  unsigned dynamic_stop_timeout_ms;
  char offset_file[256];
  char gain_file[256];
  char cal_gain_dir[256];
} pa_runtime_config_t;

/* 按 Makefile/app_config.h 生成默认配置，地址从当前 UIO 物理映射获取。 */
int config_store_defaults(const fpga_mem_t* fpga_mem, pa_runtime_config_t* config);

/* 读取配置文件；文件不存在时自动生成默认文件并返回 1，读取成功返回 0。 */
int config_store_load_or_create(const char* path,
                                const fpga_mem_t* fpga_mem,
                                pa_runtime_config_t* config);

/* 将当前配置完整写回 INI 文件。 */
int config_store_save(const char* path, const pa_runtime_config_t* config);

/* 对配置做范围和地址一致性检查。 */
int config_store_validate(const pa_runtime_config_t* config,
                          const fpga_mem_t* fpga_mem);

/* 以短文本摘要返回当前配置，供 GET_CONFIG_SUMMARY 和启动日志使用。 */
int config_store_summary(const pa_runtime_config_t* config,
                         char* output,
                         size_t output_size);

/* 按稳定名称读取/修改常用用户配置项；未知项返回 -1。 */
int config_store_get_item(const pa_runtime_config_t* config,
                          const char* item,
                          char* value,
                          size_t value_size);
int config_store_set_item(pa_runtime_config_t* config,
                          const char* item,
                          const char* value);
