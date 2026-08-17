#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
namespace esphome {
namespace api {
class CustomAPIDevice {
 public:
  template<typename T, typename... Ts>
  void register_service(void (T::*cb)(Ts...), const std::string &name,
                        const std::vector<std::string> &args) { (void)cb;(void)name;(void)args; }
  template<typename T>
  void register_service(void (T::*cb)(), const std::string &name) { (void)cb;(void)name; }
  void fire_homeassistant_event(const std::string &n,
                                const std::map<std::string, std::string> &d) { (void)n;(void)d; }
};
}  // namespace api
}  // namespace esphome
