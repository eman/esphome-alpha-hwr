#pragma once

// Pure hydraulic vote-counting and confidence-boost logic for the DHW demand
// detector's pump-on branch. Deliberately free of ESPHome dependencies so the
// host test suite (tests/test_dhw_demand_logic.cpp) can include this header
// directly and exercise the exact production logic and threshold defaults —
// no hand-mirrored copy to drift out of sync (see c311aab / PR #119).

#include <algorithm>
#include <cmath>

namespace esphome {
namespace dhw_demand {

struct PumpOnVoteThresholds {
  float inlet_pressure_transient;
  float inlet_pressure_demand_floor;
  float pump_flow_collapse;
  float motor_current_spike;
  float pump_power_spike;
  float pump_head_rate;
};

// Defaults match the Python DetectorConfig. Single source of truth for both
// DhwDemandComponent's member initializers and the host test.
inline constexpr PumpOnVoteThresholds kDefaultPumpOnVoteThresholds{
    /*inlet_pressure_transient=*/0.07f,    // PSI/s
    /*inlet_pressure_demand_floor=*/5.0f,  // PSI
    /*pump_flow_collapse=*/0.2f,           // GPM
    /*motor_current_spike=*/0.001f,        // A/s
    /*pump_power_spike=*/5.0f,             // W/s
    /*pump_head_rate=*/0.31f,              // m/s
};

// Signals 1–6 of the pump-on branch: pressure transient, absolute low
// pressure, pump-side flow collapse, current spike, power spike, and
// corroborating head-rate spike. Returns 0.0f when no signal voted.
inline float compute_pump_on_confidence(float inlet_deriv, float inlet_psi,
                                        float pump_flow, float current_deriv,
                                        float power_deriv, float head_rate_peak,
                                        bool suppress_transient_votes,
                                        const PumpOnVoteThresholds &t,
                                        const char **method_out) {
  int votes = 0;

  if (!suppress_transient_votes) {
    // Pressure/current/power/head-rate spikes also occur when the recirculation
    // pump itself starts, so ignore them for a short post-start window. During
    // that window, continuation detection still works and steady-state
    // open-loop signals below remain active.

    // Signal 1: Pressure transient (valve-open shock)
    if (!std::isnan(inlet_deriv) &&
        std::abs(inlet_deriv) > t.inlet_pressure_transient)
      votes++;
  }

  // Signal 2: Absolute inlet pressure below demand floor (open circuit)
  if (!std::isnan(inlet_psi) && inlet_psi < t.inlet_pressure_demand_floor)
    votes++;

  // Signal 3: Pump-side flow collapse (flow diverted to house)
  if (!std::isnan(pump_flow) && pump_flow < t.pump_flow_collapse)
    votes++;

  if (!suppress_transient_votes) {
    // Signal 4: Current spike (load change at valve opening)
    if (!std::isnan(current_deriv) &&
        std::abs(current_deriv) > t.motor_current_spike)
      votes++;

    // Signal 5: Power spike (corroborates current spike)
    if (!std::isnan(power_deriv) && power_deriv > t.pump_power_spike)
      votes++;

    // Signal 6: Head-pressure rate spike — corroborating only.
    // Captured at ~1–2 Hz via callback so valve-open transients aren't missed.
    // Only counts when at least one other signal has voted to avoid false triggers.
    if (votes >= 1 && head_rate_peak > t.pump_head_rate)
      votes++;
  }

  if (votes == 0)
    return 0.0f;

  // Confidence scales with vote count; cap at 0.95 (votes not independent).
  // Index 0 unused; indices 1–6 map 1–5+ votes.
  static const float conf_map[7] = {0.0f, 0.50f, 0.65f, 0.80f, 0.90f, 0.95f, 0.95f};
  float confidence = (votes < 7) ? conf_map[votes] : 0.95f;

  *method_out = "deterministic_pump_on";
  return confidence;
}

// DHW in-use confidence boost. Only trustworthy while the pump is off — with
// the pump running the flag routinely latches high for a fixed ~60 s with no
// real draw behind it, so boosting a pump-on detection with it just adds
// confidence to a phantom.
inline float apply_dhw_in_use_boost(float confidence, bool demand, bool pump_on,
                                    float dhw_in_use) {
  if (demand && !pump_on && !std::isnan(dhw_in_use) && dhw_in_use >= 0.5f)
    return std::min(1.0f, confidence + 0.05f);
  return confidence;
}

}  // namespace dhw_demand
}  // namespace esphome
