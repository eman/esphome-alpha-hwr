#pragma once
#include <functional>
#include <utility>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

// Mock of esphome/components/binary_sensor/binary_sensor.h.
//
// Fidelity notes:
//
//   * Unlike Sensor, BinarySensor DOES de-duplicate. The real
//     send_state_internal() returns early when has_state() is true and the
//     value is unchanged, so a repeated publish costs nothing. That is why
//     publish_gate.h leaves the `demand` output ungated, and modelling it any
//     other way here would make that decision look like an oversight.
//
//   * State callbacks do NOT fire on the first publish. The real gate is
//     `had_state || get_trigger_on_initial_state()` in
//     StatefulEntityBase::set_new_state(), and while the C++ member defaults to
//     true, the codegen never leaves it there: setup_binary_sensor_core_()
//     unconditionally emits set_trigger_on_initial_state(), defaulting the
//     config key to False. Every binary sensor this project builds therefore
//     runs with it off, so that is what the mock defaults to -- otherwise a
//     host test could assert a callback the firmware would never make.
//
//   * publish_count still counts the suppressed-callback publish, because the
//     frontend notification does go out (ControllerRegistry) even when the
//     state callbacks are gated. It models an API frame, not a callback.
namespace esphome {
namespace binary_sensor {

class BinarySensor : public EntityBase {
 public:
  bool state{false};

  bool get_state() const { return this->state; }
  void set_trigger_on_initial_state(bool value) { this->trigger_on_initial_state_ = value; }
  bool get_trigger_on_initial_state() const { return this->trigger_on_initial_state_; }

  void publish_state(bool new_state) { this->send_state_internal(new_state); }

  void send_state_internal(bool new_state) {
    const bool had_state = this->has_state();
    if (had_state && this->state == new_state)
      return;
    this->set_has_state(true);
    this->state = new_state;
    this->publish_count++;
    if (!had_state && !this->trigger_on_initial_state_)
      return;
    for (auto &callback : this->callbacks_)
      callback(new_state);
  }

  template<typename F> void add_on_state_callback(F &&callback) {
    this->callbacks_.emplace_back(std::forward<F>(callback));
  }

  /// Test-only: publishes that were not de-duplicated away.
  int publish_count{0};

 protected:
  std::vector<std::function<void(bool)>> callbacks_;
  bool trigger_on_initial_state_{false};
};

}  // namespace binary_sensor
}  // namespace esphome

#define LOG_BINARY_SENSOR(prefix, type, obj) ESPHOME_MOCK_LOG(prefix, type, obj)
