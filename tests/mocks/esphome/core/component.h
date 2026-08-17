#pragma once

// The real esphome/core/component.h pulls hal.h in transitively, and component
// sources rely on that for millis(). Mirror it so they compile unchanged.
#include "hal.h"
#include <string>
#include <functional>
#include <cstdint>
#include <vector>
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
  // ── Scheduler ─────────────────────────────────────────────────────────────
  //
  // These used to be no-ops that discarded the callback, which meant any chain
  // running through a timer simply stopped -- and a component whose whole
  // connection sequence is timer-driven could not be host-tested at all
  // (issue #174 audit tail).
  //
  // Now they record, and a test fires them by advancing its own clock and
  // calling mock_run_due_timeouts(). Nothing fires on its own, so tests written
  // against the old no-op behaviour are unaffected.
  //
  // ESPHome's semantics, which these follow because getting them wrong would
  // hide exactly the bugs this is meant to catch:
  //   - a *named* set_timeout replaces any pending timer of the same name
  //   - an unnamed one never replaces anything
  //   - cancel_timeout(name) drops the pending timer with that name
  //   - timers fire in due-time order, and a callback may schedule more
  struct MockTimer {
    std::string name;
    uint32_t due_at{0};
    std::function<void()> fn;
  };

  virtual void set_timeout(const std::string &name, uint32_t timeout, std::function<void()> f) {
    this->cancel_timeout(name);
    this->mock_timers_.push_back(MockTimer{name, millis() + timeout, std::move(f)});
  }
  virtual void set_timeout(uint32_t timeout, std::function<void()> f) {
    this->mock_timers_.push_back(MockTimer{std::string(), millis() + timeout, std::move(f)});
  }
  virtual void cancel_timeout(const std::string &name) {
    if (name.empty()) return;  // unnamed timers are not addressable
    for (size_t i = 0; i < this->mock_timers_.size();) {
      if (this->mock_timers_[i].name == name) {
        this->mock_timers_.erase(this->mock_timers_.begin() + static_cast<long>(i));
      } else {
        i++;
      }
    }
  }

  /// Fire every timer now due, earliest first. Removes each before running it,
  /// so a callback that re-arms the same name schedules a fresh one rather than
  /// cancelling itself. Bounded so a self-rearming timer cannot spin forever.
  void mock_run_due_timeouts() {
    for (int guard = 0; guard < 1000; guard++) {
      size_t best = this->mock_timers_.size();
      for (size_t i = 0; i < this->mock_timers_.size(); i++) {
        if (this->mock_timers_[i].due_at > millis()) continue;
        if (best == this->mock_timers_.size() ||
            this->mock_timers_[i].due_at < this->mock_timers_[best].due_at) {
          best = i;
        }
      }
      if (best == this->mock_timers_.size()) return;
      std::function<void()> fn = this->mock_timers_[best].fn;
      this->mock_timers_.erase(this->mock_timers_.begin() + static_cast<long>(best));
      if (fn) fn();
    }
  }

  size_t mock_pending_timeouts() const { return this->mock_timers_.size(); }

 protected:
  std::vector<MockTimer> mock_timers_;
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
