#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
namespace esphome {
  template <typename... Args>
  inline std::string str_snprintf(const char * /*fmt*/, size_t /*len*/, Args... /*args*/) { return ""; }
  inline std::string hexencode(const uint8_t * /*data*/, size_t /*len*/) { return ""; }
  inline std::string format_hex_pretty(const uint8_t * /*data*/, size_t /*len*/) { return ""; }
  template<typename T> using optional = std::optional<T>;
}
