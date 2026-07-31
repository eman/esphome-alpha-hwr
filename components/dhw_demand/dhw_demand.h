#pragma once

#include "dhw_demand_logic.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <cmath>
#include <cstdint>

namespace esphome {
namespace dhw_demand {

static const char *const TAG = "dhw_demand";

// 30 samples × 10 s = 5-minute household flow history
static const int FLOW_BUF_SIZE = 30;

class DhwDemandComponent : public PollingComponent {
 public:
  // ── PollingComponent ───────────────────────────────────────────────────────
  void setup() override;
  void update() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void dump_config() override;

  // ── Output sensor setters ──────────────────────────────────────────────────
  void set_demand_sensor(binary_sensor::BinarySensor *s) { demand_sensor_ = s; }
  void set_confidence_sensor(sensor::Sensor *s) { confidence_sensor_ = s; }
  void set_demand_level_sensor(sensor::Sensor *s) { demand_level_sensor_ = s; }
  void set_session_duration_sensor(sensor::Sensor *s) {
    session_duration_sensor_ = s;
  }
  void set_detection_method_sensor(text_sensor::TextSensor *s) {
    detection_method_sensor_ = s;
  }

  // ── Input sensor setters ───────────────────────────────────────────────────
  void set_motor_speed_sensor(sensor::Sensor *s) { motor_speed_ = s; }
  void set_motor_current_sensor(sensor::Sensor *s) { motor_current_ = s; }
  void set_pump_flow_sensor(sensor::Sensor *s) { pump_flow_ = s; }
  void set_flow_sensor(sensor::Sensor *s) { flow_sensor_ = s; }
  void set_tank_lower_temp_sensor(sensor::Sensor *s) { tank_lower_temp_ = s; }
  void set_dhw_charge_sensor(sensor::Sensor *s) { dhw_charge_ = s; }
  void set_dhw_in_use_sensor(sensor::Sensor *s) { dhw_in_use_ = s; }

  // ── Threshold setters ──────────────────────────────────────────────────────
  void set_pump_off_current_threshold(float v) {
    pump_off_current_threshold_ = v;
  }
  void set_flow_threshold(float v) { flow_threshold_ = v; }
  void set_thermal_collapse_rate(float v) { thermal_collapse_rate_ = v; }
  void set_dhw_charge_drop_rate(float v) { dhw_charge_drop_rate_ = v; }
  void set_pump_on_demand_flow_threshold(float v) {
    pump_on_demand_flow_threshold_ = v;
  }
  void set_pump_on_demand_min_speed_rpm(float v) {
    pump_on_demand_min_speed_rpm_ = v;
  }
  void set_pump_on_demand_max_stale_seconds(int v) {
    pump_on_demand_max_stale_seconds_ = v;
  }
  void set_flow_max_stale_seconds(int v) { flow_max_stale_seconds_ = v; }
  void set_dhw_in_use_min_seconds(int v) { dhw_in_use_min_seconds_ = v; }
  void set_flow_latch_seconds(int v) { flow_latch_seconds_ = v; }
  void set_demand_release_seconds(int v) { demand_release_seconds_ = v; }
  void set_session_gap_tolerance_seconds(int v) {
    session_gap_tolerance_seconds_ = v;
  }

 protected:
  // ── Detection helpers ──────────────────────────────────────────────────────
  float read_sensor_(sensor::Sensor *s);
  float compute_deriv_(float current, float &prev, uint32_t &prev_ms,
                      uint32_t now);
  bool flow_latch_active_();
  bool detect_pump_on_(float motor_speed, float motor_current);

  // Pump-off branch: returns confidence > 0 if demand detected, else 0
  float detect_pump_off_(float flow, bool prev_flow_present_pump_off,
                         float temp_deriv,
                         float charge_deriv,
                         bool *pre_pump_demand_eligible_out,
                         const char **method_out);

  // Pump-on branch: the tier ordering itself is decide_pump_on() in
  // dhw_demand_logic.h so the host test can assert it (issue #144). This only
  // gathers the configured thresholds for it.
  PumpOnThresholds pump_on_thresholds_() const;

  void publish_result_(bool demand, float confidence, float demand_level,
                       const char *method);
  void update_session_(bool demand, uint32_t now);

  // ── Output sensors ─────────────────────────────────────────────────────────
  binary_sensor::BinarySensor *demand_sensor_{nullptr};
  sensor::Sensor *confidence_sensor_{nullptr};
  sensor::Sensor *demand_level_sensor_{nullptr};
  sensor::Sensor *session_duration_sensor_{nullptr};
  text_sensor::TextSensor *detection_method_sensor_{nullptr};

  // ── Input sensors ──────────────────────────────────────────────────────────
  sensor::Sensor *motor_speed_{nullptr};
  sensor::Sensor *motor_current_{nullptr};
  sensor::Sensor *pump_flow_{nullptr};
  sensor::Sensor *flow_sensor_{nullptr};
  sensor::Sensor *tank_lower_temp_{nullptr};
  sensor::Sensor *dhw_charge_{nullptr};
  sensor::Sensor *dhw_in_use_{nullptr};

  // ── Thresholds (defaults match Python DetectorConfig) ─────────────────────
  float pump_off_current_threshold_{0.03f};    // A
  float flow_threshold_{0.3f};         // GPM
  float thermal_collapse_rate_{0.05f};         // °F/s
  float dhw_charge_drop_rate_{0.005f};         // %/s
  // Pump-on subtraction thresholds default from kDefaultPumpOnThresholds — the
  // single source of truth also used by the host test (dhw_demand_logic.h).
  float pump_on_demand_flow_threshold_{
      kDefaultPumpOnThresholds.demand_flow};  // GPM of computed demand
  float pump_on_demand_min_speed_rpm_{
      kDefaultPumpOnThresholds.min_speed_rpm};  // RPM
  // Derived from the same constant rather than restated, so the header and the
  // host test cannot drift apart — the promise two lines up has to be kept by
  // construction, not by remembering. (Stored in seconds because that is the
  // config surface; pump_on_thresholds_() converts back.)
  int pump_on_demand_max_stale_seconds_{
      static_cast<int>(kDefaultPumpOnThresholds.pump_flow_max_stale_ms /
                       1000)};  // s, pump loop-flow channel
  int flow_max_stale_seconds_{static_cast<int>(
      kDefaultPumpOnThresholds.flow_max_stale_ms / 1000)};  // s, meter
  // How long the heater's DHW in-use flag must stay continuously high before it
  // may declare a pump-on draw on its own. 0 means "high right now is enough".
  int dhw_in_use_min_seconds_{70};              // s
  int flow_latch_seconds_{30};                 // s
  int session_gap_tolerance_seconds_{60};      // s
  int demand_release_seconds_{30};             // s

  // ── Circular buffer — household flow (30 samples × 10 s = 5 min) ─────────
  float flow_buf_[FLOW_BUF_SIZE];
  int flow_buf_head_{0};

  // ── Previous-value registers (for derivative computation) ─────────────────
  float prev_tank_lower_temp_{NAN};
  float prev_dhw_charge_{NAN};

  // Per-sensor timestamps for accurate derivative dt across NAN gaps
  uint32_t prev_tank_lower_temp_ms_{0};
  uint32_t prev_dhw_charge_ms_{0};

  // ── Reading-age registers for the pump-on subtraction ─────────────────────
  // Stamped from each channel's state callback (see setup()). ESPHome exposes
  // no provenance on a sensor value, and differencing two channels is only
  // meaningful if both are current — see PumpOnThresholds for the measured
  // cadences that set the two bounds. 0 means "never reported".
  uint32_t flow_last_update_ms_{0};
  uint32_t pump_flow_last_update_ms_{0};

  // ── DHW in-use sustain guard ──────────────────────────────────────────────
  // Ticked in *both* pump branches: the run has to be free to start while the
  // pump is still off, or the guard would silently cost another
  // dhw_in_use_min_seconds after every pump start.
  SustainedHigh dhw_in_use_sustained_{};

  // ── Pump state tracking ───────────────────────────────────────────────────
  // When both motor sensors are NaN (BLE disconnect), forward-fill the last
  // known pump state instead of defaulting to "off".  Matches Python's
  // _last_row() behaviour which fills NaN rows from the most recent valid row.
  // Default: true (conservative — avoids false pump-off detections at boot or
  // after a long disconnect when the pump may already be running).
  bool last_known_pump_on_{true};
  bool pump_state_ever_known_{false};

  // ── Pump-on transition tracking (for continuation detection) ──────────────
  bool prev_pump_on_{false};
  bool observed_pump_off_{false};       // True once we have seen a CONFIRMED pump-off tick
  bool prev_pump_confirmed_off_{false}; // True when previous tick had confirmed pump-off
  bool prev_pre_pump_demand_eligible_{false};
  float prev_flow_{NAN};           // Flow from the *previous* tick
  float pre_pump_on_flow_{NAN};    // Flow captured from the last confirmed pump-off tick

  // ── Tick timing ───────────────────────────────────────────────────────────
  // (per-sensor timestamps are used for derivative dt; see prev_*_ms_ above)

  // ── Demand hold & session state ───────────────────────────────────────────
  // Both are pure logic living in dhw_demand_logic.h so the host test drives
  // them with injected timestamps; the component only supplies millis().
  DemandHold demand_hold_{};
  SessionTracker session_{};

  // Last demand_level from a tick where demand was raw-true. Republished while
  // the release hold is latching so intensity does not read 0.0 with the demand
  // binary sensor ON.
  float last_true_demand_level_{0.0f};
};

}  // namespace dhw_demand
}  // namespace esphome
