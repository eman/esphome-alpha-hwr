#pragma once
#include <string>
#include <functional>
#include <cstdint>
namespace esphome {
class Component {
public:
  virtual ~Component() {}
  virtual void set_timeout(const std::string &/*name*/, uint32_t /*timeout*/, std::function<void()> /*f*/) {}
  virtual void set_timeout(uint32_t /*timeout*/, std::function<void()> /*f*/) {}
  virtual void cancel_timeout(const std::string &/*name*/) {}
};
}
