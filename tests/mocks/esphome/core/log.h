#pragma once

// The real esphome/core/log.h pulls in the string helpers transitively;
// mirror that so log-only helper calls (format_hex_pretty, ...) resolve.
#include "helpers.h"

// Mock loggers: compile to nothing, but still mark every argument as used
// (inside a dead, unevaluated branch) so host builds don't drown in
// -Wunused-variable / -Wunused-lambda-capture / -Wunused-parameter warnings
// for TAGs and values that only appear in log statements.
namespace esphome {
namespace mock_log {
template <typename... Args> inline void sink(const Args &...) {}
}  // namespace mock_log
}  // namespace esphome

#define ESPHOME_MOCK_LOG(...) \
  do { \
    if (false) { \
      esphome::mock_log::sink(__VA_ARGS__); \
    } \
  } while (0)

#define ESP_LOGCONFIG(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGD(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGI(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGW(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGE(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGV(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
#define ESP_LOGVV(...) ESPHOME_MOCK_LOG(__VA_ARGS__)
