#include "dhw_demand.h"
#include "publish_gate.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include <cstring>

namespace esphome {
namespace dhw_demand {

void DhwDemandComponent::setup() {
  // Initialise circular buffer to NaN so early ticks don't misread history.
  for (int i = 0; i < FLOW_BUF_SIZE; i++) {
    flow_buf_[i] = NAN;
  }
  // Per-sensor derivative timestamps are initialized to 0 (first-call sentinel).

  // Config setters have all run by now, so seed the pure-logic helpers.
  demand_hold_.release_ms = (uint32_t) demand_release_seconds_ * 1000;
  session_.gap_ms = (uint32_t) session_gap_tolerance_seconds_ * 1000;
  dhw_in_use_sustained_.min_ms = (uint32_t) dhw_in_use_min_seconds_ * 1000;

  // Stamp when each side of the pump-on subtraction last reported. ESPHome has
  // no provenance on a sensor's state — `has_state()` says a value exists, not
  // how old it is — so differencing two channels needs its own notion of
  // reading age (issue #149). Only a real (non-NaN) reading counts: a NaN
  // publish is the channel going away, not reporting.
  if (flow_sensor_ != nullptr) {
    flow_sensor_->add_on_state_callback([this](float v) {
      if (!std::isnan(v))
        flow_last_update_ms_ = millis();
    });
  }
  if (pump_flow_ != nullptr) {
    pump_flow_->add_on_state_callback([this](float v) {
      if (!std::isnan(v))
        pump_flow_last_update_ms_ = millis();
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
  ESP_LOGCONFIG(TAG, "    pump_on_demand_flow: %.2f GPM",
                pump_on_demand_flow_threshold_);
  ESP_LOGCONFIG(TAG, "    pump_on_demand_min_speed: %.0f RPM",
                pump_on_demand_min_speed_rpm_);
  ESP_LOGCONFIG(TAG, "    pump_on_demand_max_stale: %d s",
                pump_on_demand_max_stale_seconds_);
  ESP_LOGCONFIG(TAG, "    flow_max_stale: %d s", flow_max_stale_seconds_);
  ESP_LOGCONFIG(TAG, "    dhw_in_use_min: %d s", dhw_in_use_min_seconds_);
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
  // Disarmed for a window after a shutdown: inside it, any reading the scan
  // below could find is the pump's own collapsing loop flow. See
  // latch_suppressed_after_shutdown in dhw_demand_logic.h for the measurement.
  if (latch_suppressed_after_shutdown(
          pump_off_since_ms_, millis(),
          (uint32_t) latch_pump_off_suppression_seconds_ * 1000))
    return false;

  // Derive sample count from the actual update interval rather than
  // hardcoding a 10s assumption.
  int interval_s = std::max(1, static_cast<int>(get_update_interval() / 1000));
  int samples = (flow_latch_seconds_ + interval_s - 1) / interval_s;
  if (samples < 1)
    samples = 1;
  if (samples > FLOW_BUF_SIZE)
    samples = FLOW_BUF_SIZE;

  for (int i = 0; i < samples; i++) {
    int idx = (flow_buf_head_ - 1 - i + FLOW_BUF_SIZE) % FLOW_BUF_SIZE;
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

float DhwDemandComponent::detect_pump_off_(float flow,
                                           bool prev_flow_present_pump_off,
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
  if (!pump_off_flow_onset_is_confirmed(flow_present,
                                        prev_flow_present_pump_off,
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

PumpOnThresholds DhwDemandComponent::pump_on_thresholds_() const {
  return PumpOnThresholds{
      flow_threshold_,
      pump_on_demand_flow_threshold_,
      pump_on_demand_min_speed_rpm_,
      (uint32_t) pump_on_demand_max_stale_seconds_ * 1000,
      (uint32_t) flow_max_stale_seconds_ * 1000,
      (uint32_t) pump_on_demand_settle_seconds_ * 1000};
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
  float pump_flow = read_sensor_(pump_flow_);
  float flow = read_sensor_(flow_sensor_);
  float tank_temp = read_sensor_(tank_lower_temp_);
  float dhw_charge = read_sensor_(dhw_charge_);
  float dhw_in_use = read_sensor_(dhw_in_use_);
  // Accrued every tick in both branches — see the member's comment.
  bool dhw_in_use_sustained = dhw_in_use_sustained_.update(dhw_in_use, now);

  // The 2-tick flow-onset debounce; the contract, and why the pump-off
  // qualifier is load-bearing, are with the predicate in dhw_demand_logic.h.
  // `prev_pump_confirmed_off_` still holds the previous tick's value here — it
  // is not updated until the end of this one.
  bool prev_flow_present_pump_off = prev_tick_confirms_flow_onset(
      prev_flow_, flow_threshold_, prev_pump_confirmed_off_);

  // ── 2. Compute derivatives (per-sensor dt tracks NAN gaps correctly) ──────
  // Only the two pump-off thermal signals need rates now. The pressure, current
  // and power derivatives went with the vote tier they fed (issue #149): 73 % of
  // their production firings over 29 days fell within 25 s of a self-initiated
  // pump speed change, so they were reading the pump's own modulation.
  float temp_deriv = compute_deriv_(tank_temp, prev_tank_lower_temp_, prev_tank_lower_temp_ms_, now);
  float charge_deriv = compute_deriv_(dhw_charge, prev_dhw_charge_, prev_dhw_charge_ms_, now);

  // ── 3. Push household flow into circular buffer ───────────────────────────
  flow_buf_[flow_buf_head_] = flow;
  flow_buf_head_ = (flow_buf_head_ + 1) % FLOW_BUF_SIZE;

  // ── 4. Determine pump state ───────────────────────────────────────────────
  // When both motor sensors are NaN (BLE disconnected), forward-fill the last
  // known pump state.  This mirrors Python DemandDetector._last_row() which
  // forward-fills all columns from the most recent non-null row in the window.
  // Default is "on" (conservative): avoids entering the pump-OFF branch when
  // the pump may already be running and the meter is showing recirculation
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
  //     (Conservative: prevents the meter's recirculation flow from
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
  }

  // Stamp both regime boundaries. Only ever from a *known* pump state: a
  // boot default or a BLE-gap forward-fill is a guess, and a guessed edge
  // would hand reading_predates_pump_start and latch_suppressed_after_shutdown
  // a boundary that never happened. Leaving the stamp at 0 makes both abstain,
  // which is the safe direction — the staleness bound and the latch keep
  // behaving exactly as they did before.
  if (prev_pump_on_ && pump_confirmed_off) {
    pump_off_since_ms_ = now;
    ESP_LOGD(TAG, "Pump off edge at %u ms; flow latch disarmed for %d s", now,
             latch_pump_off_suppression_seconds_);
  }
  if (!prev_pump_on_ && pump_on && pump_state_known) {
    pump_on_since_ms_ = now;
    ESP_LOGD(TAG, "Pump on edge at %u ms; loop-flow readings before it are "
                  "not subtractable",
             now);
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
      // Pump just turned ON — record the previous tick's meter flow only if
      // the previous pump-off tick had non-ambiguous demand evidence.
      pre_pump_on_flow_ = prev_flow_;
      ESP_LOGD(TAG, "Pump turned ON; pre-pump flow: %.2f GPM",
               std::isnan(pre_pump_on_flow_) ? -1.0f
                                             : pre_pump_on_flow_);
    } else {
      pre_pump_on_flow_ = NAN;
      ESP_LOGD(TAG, "Pump turned ON without confirmed pre-pump demand evidence");
    }
  }

  // ── 5. Run detection branch ───────────────────────────────────────────────
  bool demand = false;
  float confidence = 0.0f;
  float demand_level = 0.0f;
  const char *method = "idle";
  bool pre_pump_demand_eligible = false;

  if (!pump_on) {
    // ── Pump-OFF branch ───────────────────────────────────────────────────
    confidence = detect_pump_off_(flow, prev_flow_present_pump_off, temp_deriv,
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
    // The tier ordering lives in decide_pump_on() (dhw_demand_logic.h) so the
    // host test can assert it directly (issue #144). This block only gathers
    // the inputs; everything time-dependent is resolved before the call.
    PumpOnInputs in;
    in.pre_pump_on_flow = pre_pump_on_flow_;
    in.flow = flow;
    in.pump_flow = pump_flow;
    in.motor_speed = motor_speed;
    in.now_ms = now;
    in.flow_last_update_ms = flow_last_update_ms_;
    in.pump_flow_last_update_ms = pump_flow_last_update_ms_;
    in.pump_on_since_ms = pump_on_since_ms_;
    in.dhw_in_use_sustained = dhw_in_use_sustained;

    PumpOnResult result = decide_pump_on(in, pump_on_thresholds_());
    demand = result.demand;
    confidence = result.confidence;
    demand_level = result.demand_level;
    method = result.method;

    if (std::isnan(result.demand_gpm)) {
      ESP_LOGD(TAG, "Pump-on subtraction unavailable (missing, stale or "
                    "below %.0f RPM)",
               pump_on_demand_min_speed_rpm_);
    } else {
      ESP_LOGD(TAG, "Pump-on demand: meter %.2f - loop %.2f = %.2f GPM "
                    "(threshold %.2f)",
               flow, pump_flow, result.demand_gpm,
               pump_on_demand_flow_threshold_);
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

  ESP_LOGV(TAG,
           "tick: pump=%s demand=%s conf=%.2f level=%.2f method=%s "
           "meter_flow=%.2f loop_flow=%.2f motor_speed=%.0f motor_current=%.3f",
           pump_on ? "ON" : "OFF", demand ? "ON" : "OFF", confidence,
           demand_level, method,
           std::isnan(flow) ? -1.0f : flow,
           std::isnan(pump_flow) ? -1.0f : pump_flow,
           std::isnan(motor_speed) ? -1.0f : motor_speed,
           std::isnan(motor_current) ? -1.0f : motor_current);
}

}  // namespace dhw_demand
}  // namespace esphome
