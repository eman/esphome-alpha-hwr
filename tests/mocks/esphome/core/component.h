#pragma once

// The real esphome/core/component.h pulls hal.h in transitively, and component
// sources rely on that for millis(). Mirror it so they compile unchanged.
#include "hal.h"
#include <string>
#include <functional>
#include <cstdint>
namespace esphome {

// Mirrors the constants in esphome/core/component.h. Only the tiers the
// host-compiled components actually name are listed; the values match so a
// test can compare priorities meaningfully.
namespace setup_priority {
inline constexpr float BUS = 1000.0f;
inline constexpr float HARDWARE = 800.0f;
inline constexpr float DATA = 600.0f;
inline constexpr float PROCESSOR = 400.0f;
inline constexpr float AFTER_CONNECTION = 100.0f;
inline constexpr float LATE = -100.0f;
}  // namespace setup_priority

inline constexpr uint32_t SCHEDULER_DONT_RUN = 4294967295UL;

class Component {
public:
  virtual ~Component() {}
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return setup_priority::DATA; }
  virtual void set_timeout(const std::string &/*name*/, uint32_t /*timeout*/, std::function<void()> /*f*/) {}
  virtual void set_timeout(uint32_t /*timeout*/, std::function<void()> /*f*/) {}
  virtual void cancel_timeout(const std::string &/*name*/) {}
};

// The mock does not run a scheduler: a test calls update() itself, on its own
// clock, which is the point of driving the component from the host. The
// interval still matters because components read it back -- dhw_demand derives
// its flow-latch sample count from get_update_interval() rather than assuming
// 10 s -- so the default matches the real one (SCHEDULER_DONT_RUN, i.e. codegen
// has not called set_update_interval() yet) instead of a friendlier stand-in.
class PollingComponent : public Component {
public:
  PollingComponent() : PollingComponent(SCHEDULER_DONT_RUN) {}
  explicit PollingComponent(uint32_t update_interval) : update_interval_(update_interval) {}

  void set_update_interval(uint32_t update_interval) { this->update_interval_ = update_interval; }
  virtual uint32_t get_update_interval() const { return this->update_interval_; }

  virtual void update() = 0;

protected:
  uint32_t update_interval_;
};

}  // namespace esphome
