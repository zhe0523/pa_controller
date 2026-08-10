#include "auto_offset_plan.h"

auto_offset_config_t auto_offset_default_config(void) {
  auto_offset_config_t config = {
    .enabled = false,
    .sample_frames = 8,
    .idle_stable_ms = 3000,
    .max_allowed_pixel = 4095,
    .max_mean_delta = 256,
    .max_row_noise = 64,
  };
  return config;
}

const char* auto_offset_state_name(auto_offset_state_t state) {
  switch (state) {
  case AUTO_OFFSET_DISABLED:
    return "disabled";
  case AUTO_OFFSET_WAIT_IDLE:
    return "wait_idle";
  case AUTO_OFFSET_COLLECTING:
    return "collecting";
  case AUTO_OFFSET_VALIDATING:
    return "validating";
  case AUTO_OFFSET_COMMITTING:
    return "committing";
  case AUTO_OFFSET_FAILED:
    return "failed";
  }
  return "unknown";
}

