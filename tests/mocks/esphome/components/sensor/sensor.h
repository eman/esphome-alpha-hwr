#pragma once
#include <cmath>
#include <functional>
#include <utility>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

// Mock of esphome/components/sensor/sensor.h.
//
// Fidelity notes, checked against the real Sensor (esphome 2026.x):
//   * state and raw_state both start at NAN, and has_state() stays false until
//     the first publish.
//   * publish_state() does NOT de-duplicate. It assigns raw_state, then
//     internal_send_state_to_frontend() sets has_state, assigns state and fires
//     every registered callback -- unconditionally, even when the value is
//     unchanged. That asymmetry against BinarySensor is the whole subject of
//     publish_gate.h (issue #129), so a de-duplicating mock here would make the
//     gate tests pass against a bug that does not exist.
//   * With no filters configured, raw_state == state and the raw callback is
//     the same callback list, which is what this mock models. The components
//     under test never install filters.
//
// publish_count has no counterpart in real ESPHome; it is the test's way of
// counting API frames, one per publish per subscriber.
namespace esphome {
namespace sensor {

class Sensor : public EntityBase {
 public:
  float state{NAN};
  float raw_state{NAN};

  float get_state() const { return this->state; }
  float get_raw_state() const { return this->raw_state; }

  bool get_force_update() const { return this->force_update_; }
  void set_force_update(bool force_update) { this->force_update_ = force_update; }

  void publish_state(float state) {
    this->raw_state = state;
    this->internal_send_state_to_frontend(state);
  }

  void internal_send_state_to_frontend(float state) {
    this->set_has_state(true);
    this->state = state;
    this->publish_count++;
    for (auto &callback : this->callbacks_)
      callback(state);
  }

  template<typename F> void add_on_state_callback(F &&callback) {
    this->callbacks_.emplace_back(std::forward<F>(callback));
  }
  template<typename F> void add_on_raw_state_callback(F &&callback) {
    // No filters in the mock, so raw and filtered are the same stream -- which
    // is also what the real header does when USE_SENSOR_FILTER is off.
    this->callbacks_.emplace_back(std::forward<F>(callback));
  }

  /// Test-only: publishes since construction. Not part of the ESPHome API.
  int publish_count{0};

 protected:
  std::vector<std::function<void(float)>> callbacks_;
  bool force_update_{false};
};

}  // namespace sensor
}  // namespace esphome

#define LOG_SENSOR(prefix, type, obj) ESPHOME_MOCK_LOG(prefix, type, obj)
