#ifndef PA_SDK_H
#define PA_SDK_H

/*
 * PA SDK 对外 C 接口。
 *
 * 本头文件面向客户上位机软件使用，目标是屏蔽 RS422 协议细节、FPGA 寄存器细节
 * 和 PCIe 图像接收细节。客户软件只应调用这里的业务 API，不应直接拼调试命令。
 *
 * ABI 约定：
 * - 使用 C ABI，便于 C/C++/Qt/C#/Python 等语言封装。
 * - 结构体中的 size 字段用于版本兼容，调用者创建结构体后应先 memset 为 0，再设置 size。
 * - 字符串参数使用 UTF-8 或系统本地路径编码；Linux/Kylin 路径通常按 UTF-8 处理。
 * - 返回值统一使用 pa_status_t，业务判断不要依赖错误文本。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#if defined(PA_SDK_BUILD)
#define PA_SDK_API __declspec(dllexport)
#else
#define PA_SDK_API __declspec(dllimport)
#endif
#else
#define PA_SDK_API __attribute__((visibility("default")))
#endif

#define PA_SDK_VERSION_MAJOR 0u
#define PA_SDK_VERSION_MINOR 1u
#define PA_SDK_VERSION_PATCH 0u

/* 不透明设备句柄。内部包含串口、协议状态、seq、超时和最近错误等信息。 */
typedef struct pa_device pa_device_t;

/* 不透明 PCIe 接收句柄。内部包含 event/c2h/bar0 fd 和接收缓冲。 */
typedef struct pa_pcie_receiver pa_pcie_receiver_t;

/* SDK 稳定错误码。客户业务逻辑应判断错误码，而不是解析中文/英文错误文本。 */
typedef enum {
  PA_OK = 0,
  PA_ERR_BAD_ARGUMENT = -1,
  PA_ERR_NOT_OPEN = -2,
  PA_ERR_SERIAL_OPEN = -3,
  PA_ERR_SERIAL_IO = -4,
  PA_ERR_PROTOCOL = -5,
  PA_ERR_CRC = -6,
  PA_ERR_TIMEOUT = -7,
  PA_ERR_BUSY = -8,
  PA_ERR_INVALID_STATE = -9,
  PA_ERR_DEVICE_FAULT = -10,
  PA_ERR_NO_TEMPLATE = -11,
  PA_ERR_PCIE_OPEN = -12,
  PA_ERR_PCIE_IO = -13,
  PA_ERR_UNSUPPORTED = -14,
  PA_ERR_INTERNAL = -15,
} pa_status_t;

typedef enum {
  PA_WORK_MODE_IDLE = 0,
  PA_WORK_MODE_AED = 2,
  PA_WORK_MODE_SYNC_OUT = 3,
  PA_WORK_MODE_SYNC_IN = 5,
  PA_WORK_MODE_PREP = 6,
  PA_WORK_MODE_CONTINUOUS = 7,
  PA_WORK_MODE_INNER = 8,
  PA_WORK_MODE_FREE_SYNC = 9,
  PA_WORK_MODE_DDR = 10,
} pa_work_mode_t;

typedef enum {
  PA_WORK_STATE_UNKNOWN = 0,
  PA_WORK_STATE_STOPPED,
  PA_WORK_STATE_IDLE,
  PA_WORK_STATE_STATIC_CAPTURING,
  PA_WORK_STATE_DYNAMIC_STARTING,
  PA_WORK_STATE_DYNAMIC_RUNNING,
  PA_WORK_STATE_DYNAMIC_STOPPING,
  PA_WORK_STATE_TEMPLATE_RUNNING,
  PA_WORK_STATE_UPLOAD_RUNNING,
  PA_WORK_STATE_ERROR,
} pa_work_state_t;

typedef enum {
  PA_TEMPLATE_OFFSET = 0,
  PA_TEMPLATE_GAIN = 1,
} pa_template_type_t;

/*
 * 配置项 ID。
 *
 * 第一版只列正式业务需要的核心项。ROIC 详细寄存器、Dynamic 完整 step 表等大配置
 * 后续通过配置组 API 扩展，不建议塞到单个业务命令里。
 */
typedef enum {
  PA_CFG_IMAGE_WIDTH = 0x0100,
  PA_CFG_IMAGE_HEIGHT = 0x0101,
  PA_CFG_IMAGE_FRAME_COUNT = 0x0102,

  PA_CFG_STATIC_IDLE_CLEAN_INTERVAL_MS = 0x0200,
  PA_CFG_STATIC_EXPOSURE_MS = 0x0201,
  PA_CFG_STATIC_DARK_WINDOW_MS = 0x0202,
  PA_CFG_STATIC_CAPTURE_TIMEOUT_MS = 0x0203,

  PA_CFG_CORR_OFFSET_ENABLE = 0x0300,
  PA_CFG_CORR_GAIN_ENABLE = 0x0301,
  PA_CFG_CORR_DEFECT_ENABLE = 0x0302,
  PA_CFG_CORR_OFFSET_ADDER_VALUE = 0x0303,
  PA_CFG_CORR_GAIN_CLIPPING_VALUE = 0x0304,

  PA_CFG_GIC_LINE_TIME_NS = 0x0400,
  PA_CFG_GIC_START_ROW = 0x0401,
  PA_CFG_GIC_END_ROW = 0x0402,
  PA_CFG_GIC_BINNING = 0x0403,

  PA_CFG_ROIC_START_COL = 0x0500,
  PA_CFG_ROIC_END_COL = 0x0501,
  PA_CFG_ROIC_BINNING = 0x0502,

  PA_CFG_DYNAMIC_CYCLE_COUNT = 0x0600,
  PA_CFG_DYNAMIC_STATE_POLL_INTERVAL_MS = 0x0601,
  PA_CFG_DYNAMIC_STOP_TIMEOUT_MS = 0x0602,

  PA_CFG_TEMPLATE_DYNAMIC_OFFSET_TOTAL_FRAMES = 0x0700,
  PA_CFG_TEMPLATE_DYNAMIC_OFFSET_VALID_FRAMES = 0x0701,
} pa_config_item_id_t;

typedef enum {
  PA_CFG_GROUP_STATIC = 0x01,
  PA_CFG_GROUP_CORR = 0x02,
  PA_CFG_GROUP_GIC = 0x03,
  PA_CFG_GROUP_ROIC = 0x04,
  PA_CFG_GROUP_DYNAMIC = 0x05,
  PA_CFG_GROUP_TEMPLATE = 0x06,
} pa_config_group_id_t;

typedef struct {
  uint32_t size;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
} pa_sdk_version_t;

typedef struct {
  uint32_t size;
  const char* serial_port;
  uint32_t baudrate;
  uint32_t timeout_ms;
} pa_open_options_t;

typedef struct {
  uint32_t size;
  char app_version[32];
  char app_build_time[64];
  uint32_t pa_version_raw;
  char pa_version[16];
  uint32_t pa_build_information_raw;
  uint32_t pa_pu_com_version_raw;
  char pa_pu_com_version[16];
} pa_device_version_t;

typedef struct {
  uint32_t size;
  pa_work_mode_t mode;
  pa_work_state_t state;
  uint32_t last_error;
  uint32_t capture_id;
  uint32_t output_addr;
  uint32_t offset_addr;
  uint32_t gain_addr;
  uint32_t image_width;
  uint32_t image_height;
  bool offset_template_valid;
  bool gain_template_valid;
} pa_device_status_t;

typedef struct {
  uint32_t size;
  uint32_t capture_id;
  uint32_t offset_addr;
  uint32_t output_addr;
  uint32_t image_width;
  uint32_t image_height;
} pa_static_capture_result_t;

typedef struct {
  uint32_t size;
  uint32_t acquisition_id;
  uint32_t dynamic_state;
  uint32_t final_image_addr;
  uint32_t frame_count;
} pa_dynamic_result_t;

typedef struct {
  uint32_t size;
  uint32_t job_id;
  uint32_t progress;
  uint32_t state;
  uint32_t last_error;
} pa_job_status_t;

typedef struct {
  uint32_t size;
  const char* event_device;
  const char* c2h_device;
  const char* bar0_resource;
  uint32_t timeout_ms;
} pa_pcie_open_options_t;

typedef struct {
  uint32_t size;
  uint32_t image_id;
  uint32_t image_type;
  uint32_t width;
  uint32_t height;
  uint64_t final_image_addr;
  uint64_t timestamp_us;
  size_t data_size;
  uint16_t* data;
} pa_image_frame_t;

typedef void (*pa_log_callback_t)(int level, const char* message, void* user_data);
typedef void (*pa_frame_callback_t)(const pa_image_frame_t* frame, void* user_data);

/* 返回 SDK 自身版本，不需要连接设备。 */
PA_SDK_API pa_status_t pa_get_sdk_version(pa_sdk_version_t* version);

/* 返回稳定错误文本，主要用于日志和 UI 显示。 */
PA_SDK_API const char* pa_error_string(pa_status_t status);

/* 设置 SDK 内部日志回调；callback 为空表示关闭回调。 */
PA_SDK_API void pa_set_log_callback(pa_log_callback_t callback, void* user_data);

/* 打开 RS422 控制连接，并完成基础握手。 */
PA_SDK_API pa_status_t pa_open(const pa_open_options_t* options, pa_device_t** device);

/* 关闭 RS422 控制连接并释放句柄。 */
PA_SDK_API void pa_close(pa_device_t* device);

/* 查询设备软件、FPGA/PA 和通信模块版本。 */
PA_SDK_API pa_status_t pa_get_version(pa_device_t* device, pa_device_version_t* version);

/* 查询当前工作状态、模板状态和最近输出图信息。 */
PA_SDK_API pa_status_t pa_get_status(pa_device_t* device, pa_device_status_t* status);

/* 写 bool 配置项。正式用户模式下，设备端写成功后应自动保存配置文件。 */
PA_SDK_API pa_status_t pa_set_config_bool(pa_device_t* device, pa_config_item_id_t item, bool value);

/* 读 bool 配置项。 */
PA_SDK_API pa_status_t pa_get_config_bool(pa_device_t* device, pa_config_item_id_t item, bool* value);

/* 写 u32 配置项。正式用户模式下，设备端写成功后应自动保存配置文件。 */
PA_SDK_API pa_status_t pa_set_config_u32(pa_device_t* device, pa_config_item_id_t item, uint32_t value);

/* 读 u32 配置项。 */
PA_SDK_API pa_status_t pa_get_config_u32(pa_device_t* device, pa_config_item_id_t item, uint32_t* value);

/* 读取配置组，buffer_size 不足时返回 PA_ERR_BAD_ARGUMENT，并通过 actual_size 返回需要大小。 */
PA_SDK_API pa_status_t pa_get_config_group(pa_device_t* device,
                                           pa_config_group_id_t group,
                                           void* buffer,
                                           size_t buffer_size,
                                           size_t* actual_size);

/* 写入配置组。SDK 内部负责按 RS422 帧长度拆包。 */
PA_SDK_API pa_status_t pa_set_config_group(pa_device_t* device,
                                           pa_config_group_id_t group,
                                           const void* buffer,
                                           size_t buffer_size);

/* 触发一次静态采图。采图参数来自设备端当前配置，不在该函数里传一大组参数。 */
PA_SDK_API pa_status_t pa_start_static_capture(pa_device_t* device, pa_static_capture_result_t* result);

/* 启动动态采图。动态 cycle、step 表和校正策略来自设备端当前配置。 */
PA_SDK_API pa_status_t pa_start_dynamic(pa_device_t* device, pa_dynamic_result_t* result);

/* 停止动态采图。 */
PA_SDK_API pa_status_t pa_stop_dynamic(pa_device_t* device, pa_dynamic_result_t* result);

/* 查询动态采图状态。 */
PA_SDK_API pa_status_t pa_query_dynamic(pa_device_t* device, pa_dynamic_result_t* result);

/* 制作动态 offset 模板：总采集 total_frames 张，只取最后 valid_frames 张做均值。 */
PA_SDK_API pa_status_t pa_make_dynamic_offset(pa_device_t* device,
                                              uint32_t total_frames,
                                              uint32_t valid_frames,
                                              pa_job_status_t* job);

/* 开始 gain/defect 标定任务。灰度级由上位机控制，defect 通过 gain=0 标记。 */
PA_SDK_API pa_status_t pa_cal_gain_begin(pa_device_t* device,
                                         const uint32_t* levels,
                                         size_t level_count,
                                         uint32_t frames_per_level,
                                         float defect_threshold,
                                         pa_job_status_t* job);

/* 在当前灰度级下采集 gain 标定帧。 */
PA_SDK_API pa_status_t pa_cal_gain_capture(pa_device_t* device,
                                           uint32_t job_id,
                                           uint32_t level,
                                           pa_job_status_t* job);

/* 根据已采集的多灰度级均值图生成 gain 模板。 */
PA_SDK_API pa_status_t pa_cal_gain_build(pa_device_t* device, uint32_t job_id, pa_job_status_t* job);

/* 取消当前模板任务。 */
PA_SDK_API pa_status_t pa_cancel_job(pa_device_t* device, uint32_t job_id);

/* 查询模板/标定任务状态。 */
PA_SDK_API pa_status_t pa_get_job_status(pa_device_t* device, uint32_t job_id, pa_job_status_t* job);

/* 通过 FPGA 图片上传模块上传 offset 或 gain 模板。 */
PA_SDK_API pa_status_t pa_upload_template(pa_device_t* device,
                                          pa_template_type_t type,
                                          pa_job_status_t* job);

/* 打开 PCIe 图像接收通道。 */
PA_SDK_API pa_status_t pa_pcie_open(const pa_pcie_open_options_t* options, pa_pcie_receiver_t** receiver);

/* 关闭 PCIe 图像接收通道。 */
PA_SDK_API void pa_pcie_close(pa_pcie_receiver_t* receiver);

/*
 * 等待并读取一帧 PCIe 图像。
 * frame->data 由 SDK 分配，调用者使用完必须调用 pa_image_frame_release。
 * 图像尺寸来自 BAR0[0x00c]/BAR0[0x010]，数据格式为 RAW16 小端。
 */
PA_SDK_API pa_status_t pa_pcie_wait_frame(pa_pcie_receiver_t* receiver,
                                          uint32_t timeout_ms,
                                          pa_image_frame_t* frame);

/* 释放 pa_pcie_wait_frame 返回的图像内存。 */
PA_SDK_API void pa_image_frame_release(pa_image_frame_t* frame);

/* 工程调试接口：读取 FPGA/PA 寄存器。正式客户业务流程不应依赖该接口。 */
PA_SDK_API pa_status_t pa_debug_read_reg(pa_device_t* device, const char* reg_name_or_addr, uint32_t* value);

/* 工程调试接口：写 FPGA/PA 寄存器。正式客户业务流程不应依赖该接口。 */
PA_SDK_API pa_status_t pa_debug_write_reg(pa_device_t* device, const char* reg_name_or_addr, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* PA_SDK_H */
