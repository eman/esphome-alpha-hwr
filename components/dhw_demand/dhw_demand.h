#pragma once

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

// 30 samples × 10 s = 5-minute Droplet flow history
static const int DROPLET_BUF_SIZE = 30;

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
  void set_session_duration_sensor(sensor::Sensor *s) {
    session_duration_sensor_ = s;
  }
  void set_detection_method_sensor(text_sensor::TextSensor *s) {
    detection_method_sensor_ = s;
  }

  // ── Input sensor setters ───────────────────────────────────────────────────
  void set_motor_speed_sensor(sensor::Sensor *s) { motor_speed_ = s; }
  void set_motor_current_sensor(sensor::Sensor *s) { motor_current_ = s; }
  void set_inlet_pressure_sensor(sensor::Sensor *s) { inlet_pressure_ = s; }
  void set_pump_flow_sensor(sensor::Sensor *s) { pump_flow_ = s; }
  void set_pump_power_sensor(sensor::Sensor *s) { pump_power_ = s; }
  void set_flow_sensor(sensor::Sensor *s) { flow_sensor_ = s; }
  void set_tank_lower_temp_sensor(sensor::Sensor *s) { tank_lower_temp_ = s; }
  void set_dhw_charge_sensor(sensor::Sensor *s) { dhw_charge_ = s; }
  void set_dhw_in_use_sensor(sensor::Sensor *s) { dhw_in_use_ = s; }
  void set_pump_head_rate_sensor(sensor::Sensor *s) { pump_head_rate_ = s; }

  // ── Threshold setters ──────────────────────────────────────────────────────
  void set_pump_off_current_threshold(float v) {
    pump_off_current_threshold_ = v;
  }
  void set_flow_threshold(float v) { flow_threshold_ = v; }
  void set_thermal_collapse_rate(float v) { thermal_collapse_rate_ = v; }
  void set_dhw_charge_drop_rate(float v) { dhw_charge_drop_rate_ = v; }
  void set_inlet_pressure_transient_threshold(float v) {
    inlet_pressure_transient_threshold_ = v;
  }
  void set_inlet_pressure_demand_floor(float v) {
    inlet_pressure_demand_floor_ = v;
  }
  void set_pump_flow_collapse_threshold(float v) {
    pump_flow_collapse_threshold_ = v;
  }
  void set_motor_current_spike_threshold(float v) {
    motor_current_spike_threshold_ = v;
  }
  void set_pump_power_spike_threshold(float v) {
    pump_power_spike_threshold_ = v;
  }
  void set_flow_latch_seconds(int v) { flow_latch_seconds_ = v; }
  void set_session_gap_tolerance_seconds(int v) {
    session_gap_tolerance_seconds_ = v;
  }
  void set_pump_head_rate_threshold(float v) { pump_head_rate_threshold_ = v; }

 protected:
  // ── Detection helpers ──────────────────────────────────────────────────────
  float read_sensor_(sensor::Sensor *s);
  float compute_deriv_(float current, float &prev, uint32_t &prev_ms,
                      uint32_t now);
  bool flow_latch_active_();
  bool detect_pump_on_(float motor_speed, float motor_current);

  // Pump-off branch: returns confidence > 0 if demand detected, else 0
  float detect_pump_off_(float flow, bool prev_flow_present,
                         bool prev_pump_confirmed_off, float temp_deriv,
                         float charge_deriv, float tank_temp,
                         bool *pre_pump_demand_eligible_out,
                         const char **method_out);

  // Pump-on branch: returns confidence > 0 if demand detected, else 0
  float detect_pump_on_continuation_(float flow,
                                     const char **method_out);
  float detect_pump_on_deterministic_(float inlet_deriv, float inlet_psi,
                                       float pump_flow, float current_deriv,
                                       float power_deriv, float head_rate_peak,
                                       bool suppress_transient_votes,
                                       const char **method_out);

  void publish_result_(bool demand, float confidence, float demand_level,
                       const char *method);
  void update_session_(bool demand);

  // ── Output sensors ─────────────────────────────────────────────────────────
  binary_sensor::BinarySensor *demand_sensor_{nullptr};
  sensor::Sensor *confidence_sensor_{nullptr};
  sensor::Sensor *session_duration_sensor_{nullptr};
  text_sensor::TextSensor *detection_method_sensor_{nullptr};

  // ── Input sensors ──────────────────────────────────────────────────────────
  sensor::Sensor *motor_speed_{nullptr};
  sensor::Sensor *motor_current_{nullptr};
  sensor::Sensor *inlet_pressure_{nullptr};
  sensor::Sensor *pump_flow_{nullptr};
  sensor::Sensor *pump_power_{nullptr};
  sensor::Sensor *flow_sensor_{nullptr};
  sensor::Sensor *tank_lower_temp_{nullptr};
  sensor::Sensor *dhw_charge_{nullptr};
  sensor::Sensor *dhw_in_use_{nullptr};
  sensor::Sensor *pump_head_rate_{nullptr};

  // ── Thresholds (defaults match Python DetectorConfig) ─────────────────────
  float pump_off_current_threshold_{0.03f};    // A
  float flow_threshold_{0.3f};         // GPM
  float thermal_collapse_rate_{0.05f};         // °F/s
  float dhw_charge_drop_rate_{0.005f};         // %/s
  float inlet_pressure_transient_threshold_{0.07f};  // PSI/s
  float inlet_pressure_demand_floor_{5.0f};    // PSI
  float pump_flow_collapse_threshold_{0.2f};   // GPM
  float motor_current_spike_threshold_{0.001f};  // A/s
  float pump_power_spike_threshold_{5.0f};     // W/s
  float pump_head_rate_threshold_{3.0f};       // kPa/s
  int flow_latch_seconds_{30};                 // s
  int session_gap_tolerance_seconds_{60};      // s

  // ── Head-rate peak tracker (updated by callback at ~1–2 Hz; reset per tick) ─
  float head_rate_peak_{0.0f};  // Maximum |kPa/s| seen since last update()

  // ── Circular buffer — Droplet flow (30 samples × 10 s = 5 min) ───────────
  float flow_buf_[DROPLET_BUF_SIZE];
  int flow_buf_head_{0};

  // ── Previous-value registers (for derivative computation) ─────────────────
  float prev_inlet_pressure_{NAN};
  float prev_motor_current_{NAN};
  float prev_tank_lower_temp_{NAN};
  float prev_dhw_charge_{NAN};
  float prev_pump_power_{NAN};

  // Per-sensor timestamps for accurate derivative dt across NAN gaps
  uint32_t prev_inlet_pressure_ms_{0};
  uint32_t prev_motor_current_ms_{0};
  uint32_t prev_tank_lower_temp_ms_{0};
  uint32_t prev_dhw_charge_ms_{0};
  uint32_t prev_pump_power_ms_{0};

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
  uint32_t pump_on_started_ms_{0}; // Start time for startup-transient suppression

  // ── Tick timing ───────────────────────────────────────────────────────────
  // (per-sensor timestamps are used for derivative dt; see prev_*_ms_ above)

  // ── Session state ─────────────────────────────────────────────────────────
  bool session_active_{false};
  uint32_t session_start_ms_{0};
  uint32_t last_demand_ms_{0};
};

}  // namespace dhw_demand
}  // namespace esphome
