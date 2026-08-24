#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <optional>
namespace esphome {
  template <typename... Args>
  inline std::string str_snprintf(const char * /*fmt*/, size_t /*len*/, Args... /*args*/) { return ""; }
  inline std::string hexencode(const uint8_t * /*data*/, size_t /*len*/) { return ""; }
  // Mirrors the real signature, which takes a separator and a show_length flag:
  //   std::string format_hex_pretty(const uint8_t *, size_t, char = '.', bool = true);
  // Both are defaulted, so existing two-argument call sites are unaffected.
  inline std::string format_hex_pretty(const uint8_t * /*data*/, size_t /*len*/,
                                       char /*separator*/ = '.',
                                       bool /*show_length*/ = true) { return ""; }
  template<typename T> using optional = std::optional<T>;
}
