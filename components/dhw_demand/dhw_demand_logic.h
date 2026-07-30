#pragma once

// Pure decision logic for the DHW demand detector: pump-on vote counting, the
// pump-off flow-onset predicate, the confidence boost, demand release-hold, and
// session accounting. Deliberately free of ESPHome dependencies — no millis(),
// no sensor objects; anything time-dependent takes now_ms as a parameter — so
// the host test suite (tests/test_dhw_demand_logic.cpp) can include this header
// directly and exercise the exact production logic and threshold defaults.
// Nothing here may be hand-mirrored into the test; that drift is what PR #119
// and issue #120 exist to eliminate (see c311aab).

#include <algorithm>
#include <cmath>
#include <cstdint>

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

// Vote counts from one pump-on evaluation. The two differ by at most one: the
// head-rate vote (signal 6) is firmware-only, so it is excluded from `shared`.
//
// Which count to use depends on whether the quantity is on the wire. Confidence
// is a firmware-local judgement of its own evidence, so it uses `total` — the
// firmware knows more here and should say so. demand_level is part of the
// RFC-006 detector contract and is published for consumers that must behave the
// same whichever detector is feeding them, so it uses `shared` (issue #125).
struct PumpOnVotes {
  int total{0};   // signals 1–6, including the firmware-only head-rate vote
  int shared{0};  // signals 1–5 only, the ones the Python detector also has
};

// Estimated draw intensity for a pump-on hydraulic detection, from the number
// of signals that voted. Matches the Python detector's
// `demand_level = 0.3 + 0.15 * (signal_count - 1)` (detection.py:732): a lone
// vote is weak evidence, corroboration strengthens it.
//
// Pass PumpOnVotes::shared, not ::total — see the note there. With five shared
// signals the range is 0.3–0.9, the same ceiling Python reaches; the 1.0 cap is
// a guard, not a reachable value.
//
// Only meaningful for votes >= 1; the caller never reaches here otherwise.
inline float pump_on_demand_level(int votes) {
  return std::min(1.0f, 0.3f + 0.15f * static_cast<float>(votes - 1));
}

// Signals 1–6 of the pump-on branch: pressure transient, absolute low
// pressure, pump-side flow collapse, current spike, power spike, and
// corroborating head-rate spike. Returns 0.0f when no signal voted.
// method_out and votes_out are optional — pass nullptr to skip either.
inline float compute_pump_on_confidence(float inlet_deriv, float inlet_psi,
                                        float pump_flow, float current_deriv,
                                        float power_deriv, float head_rate_peak,
                                        bool suppress_transient_votes,
                                        const PumpOnVoteThresholds &t,
                                        const char **method_out,
                                        PumpOnVotes *votes_out = nullptr) {
  int votes = 0;
  bool head_voted = false;

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
    //
    // Intentionally firmware-only, and permanently so (issue #120 item 1). The
    // pump publishes head natively over BLE, so this channel is cheap here; the
    // Python detector in dhw-sensor-apps has no equivalent and deprecated its
    // head channel outright (7a4c972 dropped the 6-signal entry from its
    // confidence map; SIGNAL_SCHEMA.md lists pressure_head as deprecated), so
    // "add it to Python" is closed. The vote is safe to keep divergent because
    // it moves nothing that a cross-detector consumer can see: the votes >= 1
    // gate means it can never declare demand on its own, and it is excluded
    // from PumpOnVotes::shared so it cannot shift the published demand_level
    // either (issue #125). It only sharpens confidence once a real signal has
    // already fired.
    if (!std::isnan(head_rate_peak) && votes >= 1 &&
        head_rate_peak > t.pump_head_rate) {
      votes++;
      head_voted = true;
    }
  }

  if (votes == 0)
    return 0.0f;

  // Confidence scales with vote count; cap at 0.95 (votes not independent).
  // Index 0 unused; indices 1–6 map 1–5+ votes.
  static const float conf_map[7] = {0.0f, 0.50f, 0.65f, 0.80f, 0.90f, 0.95f, 0.95f};
  float confidence = (votes < 7) ? conf_map[votes] : 0.95f;

  if (votes_out != nullptr)
    *votes_out = PumpOnVotes{votes, votes - (head_voted ? 1 : 0)};
  if (method_out != nullptr)
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

// Does the previous tick contribute a confirming observation to the flow-onset
// debounce below? Only a tick where the pump was confirmed off can: while the
// pump runs the meter reads the recirculation loop, so its flow says nothing
// about household demand.
//
// This composition is the defect issue #147 is about, and it lives here rather
// than inline in the caller for the reason at the top of this file — the
// component itself is not host-testable (#144), so logic left in
// `dhw_demand.cpp` ships unpinned. Feeding the predicate a bare "flow was
// present last tick" makes the debounce a no-op at the pump-off transition.
inline bool prev_tick_confirms_flow_onset(float prev_flow, float flow_threshold,
                                          bool prev_pump_confirmed_off) {
  return prev_pump_confirmed_off && !std::isnan(prev_flow) &&
         prev_flow > flow_threshold;
}

// Pump-off flow onset. A brand-new flow reading is ambiguous on its very first
// tick — it may be a single noisy sample, or recirculation flow carried over
// from the pump-on state. It is confirmed either by a corroborating signal
// (thermal collapse / charge drop), or by having been present on the previous
// tick *while the pump was already confirmed off*, which is the 2-tick
// debounce. Sustained pump-off flow is accepted without thermal confirmation.
//
// The second argument's qualifier is the whole point (issue #147). Fed a bare
// "flow was present last tick" this degenerates: during pump-on the meter
// reads the recirculation loop at 1.3–2.3 GPM against a 0.3 GPM threshold, so
// the debounce is already satisfied on the very first pump-off tick, by the
// pump-on tick before it — the one moment it exists for. Qualifying it buys
// back exactly that tick and no more: post-shutdown coast-down outlasts two
// ticks and still arms the debounce legitimately on its second, which is why
// #147 stays open. The host test asserted this distinction until #120 aligned
// it *down* to what production did at the time; production has now been
// brought up instead, so the assertion is back.
//
// This narrows AGENTS.md's "when the pump is off the Droplet D1 reads only
// genuine demand flow". True in steady state; for the first seconds to minutes
// after a shutdown the meter reads the loop coasting down, and by magnitude
// that is indistinguishable from a draw — pre-shutdown loop flow runs a median
// 1.45 GPM, larger than most draws at this house.
//
// Returns true for the non-flow case as well: this predicate only gates flow
// onset, and callers apply their own no-flow handling.
inline bool pump_off_flow_onset_is_confirmed(bool flow_present,
                                             bool prev_flow_present_pump_off,
                                             bool onset_corroborating) {
  if (!flow_present)
    return true;
  return onset_corroborating || prev_flow_present_pump_off;
}

// Release-hold hysteresis for the demand output. Without it, an input dithering
// around its threshold chatters the binary sensor on every tick, since demand is
// recomputed from scratch each update. Rising edges pass through immediately —
// latency on detecting a draw is worse than a little trailing latch — while
// falling edges are held for release_ms past the last true reading.
//
// release_ms == 0 disables the hold entirely (raw passthrough).
struct DemandHold {
  uint32_t release_ms{0};
  bool held{false};
  uint32_t last_true_ms{0};

  bool update(bool raw_demand, uint32_t now_ms) {
    if (raw_demand) {
      held = true;
      last_true_ms = now_ms;
    } else if (held) {
      // Unsigned subtraction is intentional: it wraps correctly across the
      // ~49-day millis() rollover.
      if (now_ms - last_true_ms >= release_ms)
        held = false;
    }
    return held;
  }
};

// DHW session accounting. A session spans a whole draw even if demand flickers
// off briefly: it closes only once demand has been absent for longer than
// gap_ms. Duration is measured to the last demand tick, not to the moment the
// gap expired, so a trailing quiet period is not counted as part of the draw.
//
// The comparison is strictly greater, matching the Python SessionAggregator's
// `gap > self.gap_tolerance_seconds` (session.py:175, :197). A gap of exactly
// gap_ms keeps the session open in both. Immaterial at a 10 s tick, but the two
// are meant to be the same rule (issue #125 item 3).
struct SessionTracker {
  uint32_t gap_ms{0};
  bool active{false};
  uint32_t start_ms{0};
  uint32_t last_demand_ms{0};

  // Result of one tick. `just_started` / `just_ended` are edge flags for
  // logging; `ended_duration_s` is only meaningful when just_ended is true.
  struct Tick {
    bool just_started{false};
    bool just_ended{false};
    float ended_duration_s{0.0f};
    float live_duration_s{0.0f};
  };

  Tick update(bool demand, uint32_t now_ms) {
    Tick t;
    if (demand) {
      if (!active) {
        active = true;
        start_ms = now_ms;
        t.just_started = true;
      }
      last_demand_ms = now_ms;
    } else if (active && now_ms - last_demand_ms > gap_ms) {
      t.just_ended = true;
      t.ended_duration_s = (last_demand_ms - start_ms) / 1000.0f;
      active = false;
    }
    t.live_duration_s = active ? (now_ms - start_ms) / 1000.0f : 0.0f;
    return t;
  }
};

}  // namespace dhw_demand
}  // namespace esphome
