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

// Pump-on continuation. Demand was already established just before the pump
// turned on, and household flow is still above threshold now — so the draw that
// was being detected has not stopped, whatever the recirculation loop is doing
// on top of it. `pre_pump_on_flow` is NaN unless the last confirmed pump-off
// tick carried unambiguous demand evidence; the caller owns that capture.
inline bool pump_on_continuation_is_active(float pre_pump_on_flow, float flow,
                                           float flow_threshold) {
  if (std::isnan(pre_pump_on_flow) || pre_pump_on_flow <= flow_threshold)
    return false;
  if (std::isnan(flow) || flow <= flow_threshold)
    return false;
  return true;
}

// Everything the pump-on branch decides on, in one place so the ordering below
// is a pure function of its inputs. The component fills this in from sensor
// reads and its own transition tracking; nothing here reads a clock.
struct PumpOnInputs {
  // Continuation
  float pre_pump_on_flow{NAN};  // household flow at the last pump-off tick
  float flow{NAN};              // household flow now, GPM
  float flow_threshold{0.3f};   // GPM

  // Hydraulic votes
  float inlet_deriv{NAN};      // PSI/s
  float inlet_psi{NAN};        // PSI
  float pump_flow{NAN};        // GPM, the pump's own loop reading
  float current_deriv{NAN};    // A/s
  float power_deriv{NAN};      // W/s
  float head_rate_peak{NAN};   // m/s, peak since the last tick
  bool suppress_transient_votes{false};
};

// One pump-on decision. `votes` is only meaningful when the vote tier is what
// decided; it is left at its default otherwise.
struct PumpOnResult {
  bool demand{false};
  float confidence{0.0f};
  float demand_level{0.0f};
  const char *method{"pump_on_uncertain"};
  PumpOnVotes votes{};
};

// The pump-on branch, in priority order. Extracted from
// DhwDemandComponent::update() so the *ordering* is under test and not only the
// individual predicates — issue #144. Reading the tiers off the .cpp is how the
// stale 3.0f head-rate threshold survived the units audit (#120) and how
// pump_off_flow_onset_is_confirmed was fed the wrong argument for months
// (#147/#148).
//
// Tiers, strongest first:
//   1. continuation      — a draw already established before the pump started
//   2. hydraulic votes   — indirect evidence, scaled by how many agree
//   3. pump_on_uncertain — the fallback; recirculation and a draw are not
//                          separable from what is left, so claim nothing at
//                          0.5 confidence rather than guess either way
//
// Each tier is reached only when everything above it declines.
inline PumpOnResult decide_pump_on(const PumpOnInputs &in,
                                   const PumpOnVoteThresholds &t) {
  PumpOnResult r;

  if (pump_on_continuation_is_active(in.pre_pump_on_flow, in.flow,
                                     in.flow_threshold)) {
    r.demand = true;
    r.confidence = 0.85f;
    // Continuation requires flow > threshold, so this is never NaN-scaled.
    r.demand_level = std::min(1.0f, in.flow / 2.5f);
    r.method = "deterministic_continuation";
    return r;
  }

  const char *vote_method = nullptr;
  PumpOnVotes votes;
  float vote_confidence = compute_pump_on_confidence(
      in.inlet_deriv, in.inlet_psi, in.pump_flow, in.current_deriv,
      in.power_deriv, in.head_rate_peak, in.suppress_transient_votes, t,
      &vote_method, &votes);
  if (vote_confidence > 0.0f) {
    r.demand = true;
    r.confidence = vote_confidence;
    // Deliberately the shared count, not the total: demand_level is on the
    // wire, so the firmware-only head-rate vote must not shift it (#125).
    r.demand_level = pump_on_demand_level(votes.shared);
    r.method = vote_method;
    r.votes = votes;
    return r;
  }

  r.demand = false;
  r.confidence = 0.5f;  // Cannot distinguish demand from recirculation
  r.demand_level = 0.0f;
  r.method = "pump_on_uncertain";
  return r;
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

// Pump-off flow onset. A brand-new flow reading is ambiguous on its very first
// tick — it may be a single noisy sample, or recirculation flow carried over
// from the pump-on state. It is confirmed either by a corroborating signal
// (thermal collapse / charge drop), or by having been present on the previous
// tick too, which is the 2-tick debounce. Per AGENTS.md §10.4 the household
// flow sensor is the unambiguous ground-truth signal while the pump is off, so
// sustained flow is accepted without thermal confirmation.
//
// Returns true for the non-flow case as well: this predicate only gates flow
// onset, and callers apply their own no-flow handling.
inline bool pump_off_flow_onset_is_confirmed(bool flow_present,
                                             bool prev_flow_present,
                                             bool onset_corroborating) {
  if (!flow_present)
    return true;
  return onset_corroborating || prev_flow_present;
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
