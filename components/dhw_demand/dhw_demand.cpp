#include "dhw_demand.h"
#include "publish_gate.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace dhw_demand {

static const uint32_t PUMP_STARTUP_TRANSIENT_SUPPRESSION_MS = 15000;

void DhwDemandComponent::setup() {
  // Initialise circular buffer to NaN so early ticks don't misread history.
  for (int i = 0; i < DROPLET_BUF_SIZE; i++) {
    flow_buf_[i] = NAN;
  }
  // Per-sensor derivative timestamps are initialized to 0 (first-call sentinel).

  // Config setters have all run by now, so seed the pure-logic helpers.
  demand_hold_.release_ms = (uint32_t) demand_release_seconds_ * 1000;
  session_.gap_ms = (uint32_t) session_gap_tolerance_seconds_ * 1000;

  // Register callback so head_rate_peak_ is updated at notification rate (~1–2 Hz).
  // Using the peak within each 10s window ensures transients aren't missed at tick time.
  if (pump_head_rate_ != nullptr) {
    pump_head_rate_->add_on_state_callback([this](float rate) {
      if (!std::isnan(rate)) {
        float abs_rate = std::fabs(rate);
        if (abs_rate > head_rate_peak_)
          head_rate_peak_ = abs_rate;
      }
    });
  }

  ESP_LOGI(TAG, "DHW Demand Detector initialised (update interval %.0f s)",
           get_update_interval() / 1000.0f);
}

void DhwDemandComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "DHW Demand Detector:");
  ESP_LOGCONFIG(TAG, "  Thresholds:");
  ESP_LOGCONFIG(TAG, "    pump_off_current: %.3f A",
                pump_off_current_threshold_);
  ESP_LOGCONFIG(TAG, "    flow: %.2f GPM", flow_threshold_);
  ESP_LOGCONFIG(TAG, "    thermal_collapse_rate: %.4f °F/s",
                thermal_collapse_rate_);
  ESP_LOGCONFIG(TAG, "    dhw_charge_drop_rate: %.4f %%/s",
                dhw_charge_drop_rate_);
  ESP_LOGCONFIG(TAG, "    inlet_pressure_transient: %.3f PSI/s",
                inlet_pressure_transient_threshold_);
  ESP_LOGCONFIG(TAG, "    inlet_pressure_demand_floor: %.1f PSI",
                inlet_pressure_demand_floor_);
  ESP_LOGCONFIG(TAG, "    pump_flow_collapse: %.2f GPM",
                pump_flow_collapse_threshold_);
  ESP_LOGCONFIG(TAG, "    motor_current_spike: %.4f A/s",
                motor_current_spike_threshold_);
  ESP_LOGCONFIG(TAG, "    pump_power_spike: %.1f W/s",
                pump_power_spike_threshold_);
  ESP_LOGCONFIG(TAG, "    pump_head_rate: %.3f m/s",
                pump_head_rate_threshold_);
  ESP_LOGCONFIG(TAG, "    flow_latch: %d s", flow_latch_seconds_);
  ESP_LOGCONFIG(TAG, "    session_gap_tolerance: %d s",
                session_gap_tolerance_seconds_);
  ESP_LOGCONFIG(TAG, "    demand_release: %d s", demand_release_seconds_);
  LOG_BINARY_SENSOR("  ", "Demand", demand_sensor_);
  LOG_SENSOR("  ", "Confidence", confidence_sensor_);
  LOG_SENSOR("  ", "Demand Level", demand_level_sensor_);
  LOG_SENSOR("  ", "Session Duration", session_duration_sensor_);
  LOG_TEXT_SENSOR("  ", "Detection Method", detection_method_sensor_);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

float DhwDemandComponent::read_sensor_(sensor::Sensor *s) {
  if (s == nullptr || !s->has_state())
    return NAN;
  float v = s->state;
  return std::isnan(v) ? NAN : v;
}

float DhwDemandComponent::compute_deriv_(float current, float &prev,
                                          uint32_t &prev_ms, uint32_t now) {
  if (std::isnan(current)) {
    // Both prev and prev_ms are intentionally left unchanged so the next valid
    // reading computes dt_s over the true elapsed time (spanning any NaN gap),
    // not just a single tick.
    return NAN;
  }
  if (std::isnan(prev) || prev_ms == 0) {
    prev = current;
    prev_ms = now;
    return NAN;
  }
  float dt_s = (float)(now - prev_ms) / 1000.0f;
  if (dt_s <= 0.0f) {
    prev = current;
    prev_ms = now;
    return NAN;
  }
  float deriv = (current - prev) / dt_s;
  prev = current;
  prev_ms = now;
  return deriv;
}

bool DhwDemandComponent::flow_latch_active_() {
  // Derive sample count from the actual update interval rather than
  // hardcoding a 10s assumption.
  int interval_s = std::max(1, static_cast<int>(get_update_interval() / 1000));
  int samples = (flow_latch_seconds_ + interval_s - 1) / interval_s;
  if (samples < 1)
    samples = 1;
  if (samples > DROPLET_BUF_SIZE)
    samples = DROPLET_BUF_SIZE;

  for (int i = 0; i < samples; i++) {
    int idx = (flow_buf_head_ - 1 - i + DROPLET_BUF_SIZE) % DROPLET_BUF_SIZE;
    if (!std::isnan(flow_buf_[idx]) &&
        flow_buf_[idx] > flow_threshold_) {
      return true;
    }
  }
  return false;
}

bool DhwDemandComponent::detect_pump_on_(float motor_speed,
                                          float motor_current) {
  // Threshold mirrors Python: pump_off = motor_speed < 10 RPM
  if (!std::isnan(motor_speed))
    return motor_speed >= 10.0f;
  if (!std::isnan(motor_current))
    return motor_current >= pump_off_current_threshold_;
  return false;
}

// ── Pump-OFF detection ────────────────────────────────────────────────────────

float DhwDemandComponent::detect_pump_off_(float flow, bool prev_flow_present,
                                           float temp_deriv,
                                           float charge_deriv,
                                           bool *pre_pump_demand_eligible_out,
                                           const char **method_out) {
  *pre_pump_demand_eligible_out = false;

  // Each signal carries a confidence weight (matching Python DemandDetector).
  struct Signal {
    const char *name;
    float weight;
  };
  Signal signals[3];
  int count = 0;
  bool onset_corroborating_signal_present = false;
  bool flow_present = (!std::isnan(flow) && flow > flow_threshold_);

  if (flow_present) {
    signals[count++] = {"deterministic_flow", 1.0f};
  }

  if (!std::isnan(temp_deriv) && temp_deriv < -thermal_collapse_rate_) {
    signals[count++] = {"deterministic_thermal", 0.9f};
    onset_corroborating_signal_present = true;
  }

  if (!std::isnan(charge_deriv) && charge_deriv < -dhw_charge_drop_rate_) {
    // Suppress if tank is actively warming (recirculation returning heat).
    bool tank_warming = (!std::isnan(temp_deriv) && temp_deriv > 0.001f);
    if (!tank_warming) {
      signals[count++] = {"deterministic_charge", 0.7f};
      onset_corroborating_signal_present = true;
    }
  }

  if (count == 0)
    return 0.0f;

  // Flow onset is ambiguous on its first tick; see the predicate's contract in
  // dhw_demand_logic.h. Shared with the host test so the two cannot drift.
  if (!pump_off_flow_onset_is_confirmed(flow_present, prev_flow_present,
                                        onset_corroborating_signal_present)) {
    *method_out = "flow_onset_pending";
    return 0.0f;
  }

  *pre_pump_demand_eligible_out = flow_present;

  // Sort descending by weight to find best method.
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (signals[j].weight > signals[i].weight) {
        Signal tmp = signals[i];
        signals[i] = signals[j];
        signals[j] = tmp;
      }
    }
  }

  *method_out = signals[0].name;
  float confidence = signals[0].weight;
  if (count >= 2) {
    confidence += 0.05f * (count - 1);
    if (confidence > 1.0f)
      confidence = 1.0f;
  }

  // No-flow guard: if current flow is below threshold and latch has expired,
  // suppress demand.
  if (!flow_present && !flow_latch_active_()) {
    *method_out = "no_flow";
    *pre_pump_demand_eligible_out = false;
    return 0.0f;
  }

  return confidence;
}

// ── Pump-ON detection ─────────────────────────────────────────────────────────

float DhwDemandComponent::detect_pump_on_continuation_(
    float flow, const char **method_out) {
  // Continuation: demand was active just before the pump turned on, and
  // Droplet flow is still above threshold now.
  if (std::isnan(pre_pump_on_flow_))
    return 0.0f;
  if (pre_pump_on_flow_ <= flow_threshold_)
    return 0.0f;
  if (std::isnan(flow) || flow <= flow_threshold_)
    return 0.0f;

  *method_out = "deterministic_continuation";
  return 0.85f;
}

float DhwDemandComponent::detect_pump_on_deterministic_(
    float inlet_deriv, float inlet_psi, float pump_flow,
    float current_deriv, float power_deriv, float head_rate_peak,
    bool suppress_transient_votes,
    const char **method_out, PumpOnVotes *votes_out) {
  PumpOnVoteThresholds thresholds{
      inlet_pressure_transient_threshold_, inlet_pressure_demand_floor_,
      pump_flow_collapse_threshold_,       motor_current_spike_threshold_,
      pump_power_spike_threshold_,         pump_head_rate_threshold_};
  return compute_pump_on_confidence(inlet_deriv, inlet_psi, pump_flow,
                                    current_deriv, power_deriv, head_rate_peak,
                                    suppress_transient_votes, thresholds,
                                    method_out, votes_out);
}

// ── Publish & session helpers ─────────────────────────────────────────────────

void DhwDemandComponent::publish_result_(bool demand, float confidence,
                                          float demand_level,
                                          const char *method) {
  // Every publish below is gated on change (publish_gate.h, issue #129): these
  // are step-valued outputs re-derived from scratch each tick, and on an idle
  // system every tick recomputes the same four values. BinarySensor already
  // dedups internally, so `demand` needs no gate.
  if (demand_sensor_ != nullptr)
    demand_sensor_->publish_state(demand);

  // Confidence is tracked internally as a 0.0–1.0 weight; publish as a percent.
  if (confidence_sensor_ != nullptr)
    publish_sensor_if_changed(confidence_sensor_, confidence * 100.0f);

  // Estimated draw intensity, 0.0–1.0. Part of the RFC-006 detector contract;
  // the Python detector publishes the same field on its demand entity. Its
  // ~10 s MQTT cadence is load-bearing there (the HA discovery config carries
  // expire_after: 30), which is why that side stays unconditional; over the
  // native API availability follows the connection, so repeats buy nothing.
  if (demand_level_sensor_ != nullptr)
    publish_sensor_if_changed(demand_level_sensor_, demand_level);

  if (detection_method_sensor_ != nullptr)
    publish_text_sensor_if_changed(detection_method_sensor_, method);

  // Session duration is updated in update_session_(); we don't overwrite it
  // here to avoid clearing a running session count on a false tick.
}

void DhwDemandComponent::update_session_(bool demand, uint32_t now) {
  // All the accounting lives in SessionTracker (dhw_demand_logic.h); this is
  // just the ESPHome shell that supplies the clock and publishes the result.
  //
  // `now` is the caller's tick timestamp rather than a fresh millis(): the
  // release hold and the session are deliberately coupled — the tracker sees
  // the held value — so they must agree on the clock too (issue #125 item 2).
  SessionTracker::Tick tick = session_.update(demand, now);

  if (tick.just_started)
    ESP_LOGI(TAG, "DHW session started");
  if (tick.just_ended)
    ESP_LOGI(TAG, "DHW session ended: %.0f s", tick.ended_duration_s);

  // Publish live session duration (0 when inactive). Gated on change: while a
  // session runs the value moves every tick so the gate costs nothing, and it
  // silences the idle 0 s stream that is the common case (issue #129).
  if (session_duration_sensor_ != nullptr)
    publish_sensor_if_changed(session_duration_sensor_, tick.live_duration_s);
}

// ── Main update tick ──────────────────────────────────────────────────────────

void DhwDemandComponent::update() {
  uint32_t now = millis();

  // ── 1. Read current sensor values ─────────────────────────────────────────
  float motor_speed = read_sensor_(motor_speed_);
  float motor_current = read_sensor_(motor_current_);
  float inlet_psi = read_sensor_(inlet_pressure_);
  float pump_flow = read_sensor_(pump_flow_);
  float pump_power = read_sensor_(pump_power_);
  float flow = read_sensor_(flow_sensor_);
  float tank_temp = read_sensor_(tank_lower_temp_);
  float dhw_charge = read_sensor_(dhw_charge_);
  float dhw_in_use = read_sensor_(dhw_in_use_);
  bool prev_flow_present =
      !std::isnan(prev_flow_) && prev_flow_ > flow_threshold_;

  // ── 2. Compute derivatives (per-sensor dt tracks NAN gaps correctly) ──────
  float inlet_deriv = compute_deriv_(inlet_psi, prev_inlet_pressure_, prev_inlet_pressure_ms_, now);
  float current_deriv =
      compute_deriv_(motor_current, prev_motor_current_, prev_motor_current_ms_, now);
  float temp_deriv = compute_deriv_(tank_temp, prev_tank_lower_temp_, prev_tank_lower_temp_ms_, now);
  float charge_deriv = compute_deriv_(dhw_charge, prev_dhw_charge_, prev_dhw_charge_ms_, now);
  float power_deriv = compute_deriv_(pump_power, prev_pump_power_, prev_pump_power_ms_, now);

  // ── 3. Push Droplet flow into circular buffer ─────────────────────────────
  flow_buf_[flow_buf_head_] = flow;
  flow_buf_head_ = (flow_buf_head_ + 1) % DROPLET_BUF_SIZE;

  // ── 4. Determine pump state ───────────────────────────────────────────────
  // When both motor sensors are NaN (BLE disconnected), forward-fill the last
  // known pump state.  This mirrors Python DemandDetector._last_row() which
  // forward-fills all columns from the most recent non-null row in the window.
  // Default is "on" (conservative): avoids entering the pump-OFF branch when
  // the pump may already be running and the Droplet is showing recirculation
  // flow (~2.2 GPM), which would cause a false "deterministic_flow" detection.
  bool pump_state_known =
      !std::isnan(motor_speed) || !std::isnan(motor_current);
  bool pump_on;
  if (pump_state_known) {
    pump_on = detect_pump_on_(motor_speed, motor_current);
    last_known_pump_on_ = pump_on;
    pump_state_ever_known_ = true;
  } else {
    // Unknown state — forward-fill or use conservative default.
    pump_on = pump_state_ever_known_ ? last_known_pump_on_ : true;
    ESP_LOGD(TAG, "Pump state unknown (BLE gap) — forward-filling as %s",
             pump_on ? "ON" : "OFF");
  }

  // Track pump-on transition for continuation detection.
  //
  // pump_confirmed_off mirrors Python's forward-fill + "not np.isnan(spd) and spd < 10":
  //   - True when a real sensor reading shows pump off.
  //   - True when forward-filling from a last-known-OFF state
  //     (Python would fill speed = 0 RPM → pump_off = True for those rows).
  //   - False when defaulting to ON at boot or when last-known was ON.
  //     (Conservative: prevents the Droplet's recirculation flow from
  //      triggering a pump-off false positive through the NaN gap.)
  //
  // observed_pump_off_ guards against a false trigger at boot when prev_pump_on_
  // starts as false but the pump is already running.  Only set it on a confirmed
  // off reading so that a BLE reconnect (NaN → known-on) does not falsely prime
  // the continuation path.
  bool pump_confirmed_off;
  if (pump_state_known) {
    pump_confirmed_off = !pump_on;
  } else {
    // Forward-filled: treat as confirmed-off only if last-known was off.
    pump_confirmed_off = pump_state_ever_known_ && !last_known_pump_on_;
  }
  if (pump_confirmed_off) {
    observed_pump_off_ = true;
    pre_pump_on_flow_ = NAN;  // Clear stale transition state
    pump_on_started_ms_ = 0;
  }

  // Only capture pre_pump_on_flow_ when the PREVIOUS tick was confirmed off.
  // This mirrors Python's "not np.isnan(spd) and spd < 10" check which
  // requires a pump-off reading (real or forward-filled from known-OFF) to
  // count as pre-pump demand evidence.  NaN-gap ticks where the last-known
  // state was ON do not qualify, preventing false continuation detections
  // on BLE reconnect when the pump was already running through the gap.
  if (!prev_pump_on_ && pump_on) {
    if (observed_pump_off_ && prev_pump_confirmed_off_ &&
        prev_pre_pump_demand_eligible_) {
      // Pump just turned ON — record the previous tick's Droplet flow only if
      // the previous pump-off tick had non-ambiguous demand evidence.
      pre_pump_on_flow_ = prev_flow_;
      ESP_LOGD(TAG, "Pump turned ON; pre-pump flow: %.2f GPM",
               std::isnan(pre_pump_on_flow_) ? -1.0f
                                             : pre_pump_on_flow_);
    } else {
      pre_pump_on_flow_ = NAN;
      ESP_LOGD(TAG, "Pump turned ON without confirmed pre-pump demand evidence");
    }
    pump_on_started_ms_ = now;
  }

  // ── 5. Run detection branch ───────────────────────────────────────────────
  bool demand = false;
  float confidence = 0.0f;
  float demand_level = 0.0f;
  const char *method = "idle";
  bool pre_pump_demand_eligible = false;

  if (!pump_on) {
    // ── Pump-OFF branch ───────────────────────────────────────────────────
    confidence = detect_pump_off_(flow, prev_flow_present, temp_deriv,
                                  charge_deriv, &pre_pump_demand_eligible,
                                  &method);
    if (confidence > 0.0f) {
      demand = true;
      // Demand level: scale by flow if available, else moderate default.
      if (!std::isnan(flow) && flow > flow_threshold_) {
        demand_level = std::min(1.0f, flow / 2.5f);
      } else {
        demand_level = 0.5f;
      }
    } else if (strcmp(method, "flow_onset_pending") == 0) {
      confidence = 0.5f;  // Ambiguous onset: wait one full off tick
    } else {
      method = "deterministic_idle";
      confidence = 1.0f;  // High confidence that there is no demand
    }
  } else {
    // ── Pump-ON branch ────────────────────────────────────────────────────
    confidence = detect_pump_on_continuation_(flow, &method);
    if (confidence > 0.0f) {
      demand = true;
      demand_level = std::min(1.0f, flow / 2.5f);
    } else {
      bool suppress_transient_votes =
          pump_on_started_ms_ != 0 &&
          (now - pump_on_started_ms_) < PUMP_STARTUP_TRANSIENT_SUPPRESSION_MS;
      if (suppress_transient_votes) {
        ESP_LOGD(TAG, "Suppressing startup transient votes (pump on for %.1f s)",
                 (now - pump_on_started_ms_) / 1000.0f);
      }
      PumpOnVotes votes;
      confidence = detect_pump_on_deterministic_(inlet_deriv, inlet_psi,
                                                   pump_flow, current_deriv,
                                                   power_deriv, head_rate_peak_,
                                                   suppress_transient_votes,
                                                   &method, &votes);
      if (confidence > 0.0f) {
        // Hydraulic signals are indirect, so a lone vote stays low; each
        // corroborating vote raises it. Matches Python's detection.py:732.
        // Deliberately the shared count, not the total: demand_level is on the
        // wire, so the firmware-only head-rate vote must not shift it (#125).
        demand_level = pump_on_demand_level(votes.shared);
        demand = true;
      } else {
        method = "pump_on_uncertain";
        confidence = 0.5f;  // Cannot distinguish demand from recirculation
      }
    }
  }

  // ── 6. DHW in-use confidence boost ────────────────────────────────────────
  confidence = apply_dhw_in_use_boost(confidence, demand, pump_on, dhw_in_use);

  prev_flow_ = flow;
  prev_pump_on_ = pump_on;
  prev_pump_confirmed_off_ = pump_confirmed_off;
  prev_pre_pump_demand_eligible_ = pre_pump_demand_eligible;

  // ── 7. Release-hold, then publish & session tracking ──────────────────────
  // Demand is recomputed from scratch each tick, so an input dithering around
  // its threshold would otherwise chatter the binary sensor. Hold the falling
  // edge; the session tracker sees the held value too so the two agree.
  bool raw_demand = demand;
  if (raw_demand)
    last_true_demand_level_ = demand_level;
  demand = demand_hold_.update(raw_demand, now);
  if (demand && !raw_demand) {
    // Report the hold rather than whatever the no-demand branch concluded —
    // otherwise the sensor reads ON alongside "deterministic_idle" at 100 %
    // confidence, which says the exact opposite. Carry the last live intensity
    // through for the same reason: a demand_level of 0.0 while the binary
    // sensor is ON is that same contradiction in the other field.
    method = "demand_release_hold";
    confidence = 0.5f;
    demand_level = last_true_demand_level_;
    ESP_LOGD(TAG, "Demand held through release window (raw=OFF)");
  }

  publish_result_(demand, confidence, demand_level, method);
  update_session_(demand, now);

  // Reset peak tracker for next window (callback will repopulate it between ticks)
  head_rate_peak_ = 0.0f;

  ESP_LOGV(TAG,
           "tick: pump=%s demand=%s conf=%.2f level=%.2f method=%s "
           "droplet_flow=%.2f inlet_psi=%.2f pump_flow=%.2f motor_current=%.3f",
           pump_on ? "ON" : "OFF", demand ? "ON" : "OFF", confidence,
           demand_level, method,
           std::isnan(flow) ? -1.0f : flow,
            std::isnan(inlet_psi) ? -1.0f : inlet_psi,
           std::isnan(pump_flow) ? -1.0f : pump_flow,
           std::isnan(motor_current) ? -1.0f : motor_current);
}

}  // namespace dhw_demand
}  // namespace esphome
