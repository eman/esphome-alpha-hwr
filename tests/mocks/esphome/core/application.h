#pragma once

#include <cstdio>
#include <string>
namespace esphome {
class Application {
 public:
  const char *get_name() const { return "test-node"; }
  /// Pre-2026.1 spelling. alpha_hwr.cpp selects between this and
  /// get_build_time_string() on ESPHOME_VERSION_CODE, and version.h below
  /// pins which branch the host build takes.
  std::string get_compilation_time() const { return "2026-01-01 00:00:00"; }
  void get_build_time_string(char *buf) const {
    std::snprintf(buf, 32, "2026-01-01 00:00:00");
  }
};
extern Application App;
}  // namespace esphome
