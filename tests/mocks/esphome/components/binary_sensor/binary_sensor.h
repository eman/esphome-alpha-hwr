#pragma once
#include <functional>
#include <utility>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

// Mock of esphome/components/binary_sensor/binary_sensor.h.
//
// Fidelity note: unlike Sensor, BinarySensor DOES de-duplicate. The real
// send_state_internal() returns early when has_state() is true and the value is
// unchanged, so a repeated publish costs nothing. That is why publish_gate.h
// leaves the `demand` output ungated, and modelling it any other way here would
// make that decision look like an oversight.
namespace esphome {
namespace binary_sensor {

class BinarySensor : public EntityBase {
 public:
  bool state{false};

  bool get_state() const { return this->state; }

  void publish_state(bool new_state) { this->send_state_internal(new_state); }

  void send_state_internal(bool new_state) {
    if (this->has_state() && this->state == new_state)
      return;
    this->set_has_state(true);
    this->state = new_state;
    this->publish_count++;
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
};

}  // namespace binary_sensor
}  // namespace esphome

#define LOG_BINARY_SENSOR(prefix, type, obj) ESPHOME_MOCK_LOG(prefix, type, obj)
