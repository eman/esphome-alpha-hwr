#pragma once
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

// Mock of esphome/components/text_sensor/text_sensor.h.
//
// Fidelity note: like Sensor and unlike BinarySensor, TextSensor does NOT
// de-duplicate. The real publish_state() skips only the string assignment when
// the text is unchanged -- it still calls notify_frontend_(), which sets
// has_state and fires the callbacks. So a detector republishing
// "deterministic_idle" every tick really does cost a frame per subscriber per
// tick, which is what publish_text_sensor_if_changed() exists to stop.
//
// get_raw_state() returns the same string as get_state() when no filter is
// installed, which is the only configuration the components under test use.
namespace esphome {
namespace text_sensor {

class TextSensor : public EntityBase {
 public:
  std::string state;

  const std::string &get_state() const { return this->state; }
  const std::string &get_raw_state() const { return this->state; }

  void publish_state(const std::string &state) { this->publish_state(state.data(), state.size()); }
  void publish_state(const char *state) { this->publish_state(state, strlen(state)); }

  void publish_state(const char *state, size_t len) {
    if (len != this->state.size() || memcmp(state, this->state.data(), len) != 0) {
      this->state.assign(state, len);
    }
    this->set_has_state(true);
    this->publish_count++;
    for (auto &callback : this->callbacks_)
      callback(this->state);
  }

  template<typename F> void add_on_state_callback(F &&callback) {
    this->callbacks_.emplace_back(std::forward<F>(callback));
  }

  /// Test-only: publishes since construction. Not part of the ESPHome API.
  int publish_count{0};

 protected:
  std::vector<std::function<void(const std::string &)>> callbacks_;
};

}  // namespace text_sensor
}  // namespace esphome

#define LOG_TEXT_SENSOR(prefix, type, obj) ESPHOME_MOCK_LOG(prefix, type, obj)
