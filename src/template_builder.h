#pragma once

#include <stdbool.h>

#include "fpga_mem.h"

/*
 * 模板文件和 FPGA 模板内存之间的转换接口。
 *
 * 文件中只保存有效图像区域 IMAGE_WIDTH x IMAGE_HEIGHT；
 * 写入 FPGA 内存时会铺回 DEVICE_WIDTH x DEVICE_HEIGHT 的整幅图布局，
 * 并按照 ROW_OFFSET/COL_OFFSET 放到正确位置。
 */

/* 从 TEMPLATE_OFFSET_FILE / TEMPLATE_GAIN_FILE 加载模板到 FPGA/PA 共享内存。 */
int template_load_files(fpga_mem_t* mem);

/* 使用运行时配置指定的模板文件加载；路径为空时回退到编译默认路径。 */
int template_load_files_from_paths(fpga_mem_t* mem,
                                   const char* offset_path,
                                   const char* gain_path);

/* 用当前原始图像生成 offset 模板，并同时写入文件和 FPGA 模板内存。 */
int template_make_offset(fpga_mem_t* mem);

/* 用当前原始图像和 offset 模板生成 gain 模板，并同时写入文件和 FPGA 模板内存。 */
int template_make_gain(fpga_mem_t* mem);

/* 后台模板任务使用的可取消版本；回调在行边界返回 true 时安全退出。 */
typedef bool (*template_cancel_fn)(void* opaque);
int template_make_offset_cancellable(fpga_mem_t* mem, template_cancel_fn cancel, void* opaque);
int template_make_gain_cancellable(fpga_mem_t* mem, template_cancel_fn cancel, void* opaque);
