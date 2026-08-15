#pragma once

// Pure decision logic for the DHW demand detector: the pump-on tier ordering
// and its demand measurement, the pump-off flow-onset predicate, the confidence
// boost, demand release-hold, and session accounting. Deliberately free of
// ESPHome dependencies — no millis(),
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

// Configured values the pump-on branch decides against. Single source of truth
// for both DhwDemandComponent's member initializers and the host test.
//
// Defaults match the Python DetectorConfig; see the notes on each for what was
// measured to place it.
struct PumpOnThresholds {
  // Household flow above which a draw was in progress when the pump started.
  // Gates continuation only — see the warning on the subtraction below for why
  // raw meter flow may never decide a pump-on detection on its own.
  float flow;  // GPM

  // Computed demand (meter − loop) above which the subtraction declares a
  // draw. Shares the value of `flow` deliberately, so both pump regimes agree
  // on what counts as flow.
  float demand_flow;  // GPM

  // Pump speed below which the subtraction is not trustworthy.
  //
  // The pump does not meter its loop flow; it estimates it. Near the bottom of
  // its range the estimate reads low, so `meter − loop` goes spuriously
  // positive with the tap shut. Measured with no draw, ground truth by
  // construction:
  //
  //     achieved rpm   meter     loop   difference
  //             1650      0.71   0.25       +0.45   <- would fire falsely
  //             1800      0.83   0.56       +0.27   <- would fire falsely
  //             2001      0.99   0.99       -0.00
  //             2398      1.34   1.47       -0.14
  //             3600      2.22   2.09       +0.12
  //
  // Without this floor the bias produced 20 false positives of its own.
  //
  // 1950 rather than 2000 because the *pump* never chooses a speed this low:
  // over 29 days of production the minimum observed was 1971 rpm (p1 1994), so
  // a 2000 floor would cut through the middle of the mode the pump actually
  // idles in, while 1950 excludes only bench-commanded speeds.
  //
  // Two caveats worth keeping visible. The lowest *measured* clean point is
  // 1996 rpm, so production's 1971 rpm minimum sits 25 rpm below any
  // measurement — interpolating 1800→2001 puts the bias there near +0.04
  // against a 0.3 threshold, roughly 0.25 GPM of headroom, but it is an
  // extrapolation. And this is a property of the ALPHA's own flow estimator
  // rather than of one plumbing installation, so it should transfer to other
  // installs — measured at one, which is worth stating rather than implying.
  float min_speed_rpm;

  // How stale each side of the subtraction may be. A difference of two
  // quantities is only meaningful if both are current, and the two channels do
  // not report alike — which is why these are separate numbers rather than one
  // shared bound.
  //
  // The pump reports every 10 s, but gaps beyond 20 s are 1.0 % of gaps and
  // 14.1 % of pump *running time*: a last-known-value read is stale about a
  // seventh of the time the pump runs, and during one bench run a reading that
  // looked live was 90 s old, from before the tap was opened.
  //
  // The meter reports on change, at a median 28 s cadence while flowing (p90
  // 32 s), so matching the pump's 30 s here would reject half of normal
  // cadence.
  uint32_t pump_flow_max_stale_ms;
  uint32_t flow_max_stale_ms;

  // How long after a pump start the loop-flow estimate is still ramping and
  // must not be differenced. See pump_flow_estimate_is_settled.
  uint32_t pump_on_settle_ms;

  // How long the continuation tier may keep asserting a draw that nothing has
  // measured since. See pump_on_continuation_is_active for why an expiry is
  // needed at all; 0 disables the tier outright.
  uint32_t continuation_max_ms;

  // Computed demand at or below which the subtraction is taken to have
  // *falsified* the continuation, as opposed to merely failing to confirm it.
  //
  // Deliberately NOT demand_flow. That threshold is where the subtraction is
  // confident enough to *raise* a claim, and the asymmetry is the whole point:
  // the pump's loop estimate carries a residual measured at −0.10 ± 0.06 GPM
  // (−0.14 at 2398 rpm in the table above), so a genuine draw smaller than that
  // residual plus 0.3 computes below 0.3 and would be read as "stopped" while
  // the tap is still open. Borrowing the firing threshold to falsify inverts
  // the safety argument three paragraphs down in pump_on_demand_flow: a
  // negative bias is conservative when it can only suppress a claim, and
  // becomes a false-negative generator the moment it can retire one.
  //
  // Zero is the defensible line. The meter reading no more than the pump says
  // its own loop is moving is the only unambiguous statement that no household
  // draw exists; any positive difference is evidence of *something*, and the
  // expiry below is what handles the cases this cannot decide.
  //
  // The band this rescues is bounded on both sides, which is worth stating so
  // the fix is not oversold. Below 0.3 GPM a draw never arms the tier at all
  // (the capture needs a pump-off reading above `flow`), and above
  // 0.3 − residual the old comparison held anyway — so steadily it is roughly
  // 0.3 to 0.4 GPM, widening only during the transients the tick count covers.
  // Measured on hardware: a 0.60 GPM draw reduced mid-run held at +0.226 GPM
  // of computed demand for 26 s with water still running, where the old
  // comparison would have retired the capture.
  float continuation_release;  // GPM of computed demand

  // How many consecutive ticks must measure the draw as stopped before the
  // capture is retired.
  //
  // One tick is not enough, and the retirement is irreversible within a pump
  // run, so a single bad reading costs the rest of it. The bad readings are
  // real and measured: the two channels do not report together (the meter led
  // the pump by 13 s at one recorded start), so a mid-run speed change lets the
  // meter follow the loop down while the pump's own last report is still high
  // and inside its 30 s freshness bound — the difference goes sharply negative
  // for a tick or two with the tap still open. The −0.47 GPM logged on the
  // bench that verified this tier is itself such a transient: it was recorded
  // while the pump wound down from 3667 to 2499 rpm, 3–5× the steady residual.
  //
  // Two ticks at the 10 s cadence still releases a genuinely stopped draw
  // within 20 s, against the whole-run hold this replaces.
  uint8_t continuation_release_ticks;
};

inline constexpr PumpOnThresholds kDefaultPumpOnThresholds{
    /*flow=*/0.3f,               // GPM
    /*demand_flow=*/0.3f,        // GPM of computed demand
    /*min_speed_rpm=*/1950.0f,   // rpm
    /*pump_flow_max_stale_ms=*/30000,
    /*flow_max_stale_ms=*/60000,
    /*pump_on_settle_ms=*/10000,
    /*continuation_max_ms=*/300000,
    /*continuation_release=*/0.0f,  // GPM of computed demand
    /*continuation_release_ticks=*/2,
};

// The intensity a tier publishes when it is confident a draw is happening but
// has no measurement of how large it is. Shared with the Python detector, where
// it is _NO_INTENSITY_CLAIM — a deliberate constant rather than a guess, so a
// consumer can tell "we do not know" from a measured small draw.
inline constexpr float kNoIntensityClaim = 0.4f;

// Has a sensor reported a real value recently enough to be differenced?
//
// ESPHome has no provenance column, so the component stamps millis() into a
// per-channel register on each state callback and passes it here. A register of
// 0 means the channel has never reported.
//
// Unsigned subtraction is intentional: it wraps correctly across the ~49-day
// millis() rollover, the same as DemandHold's.
inline bool reading_is_fresh(uint32_t last_update_ms, uint32_t now_ms,
                             uint32_t max_stale_ms) {
  if (last_update_ms == 0)
    return false;
  return (now_ms - last_update_ms) <= max_stale_ms;
}

// Does a motor reading say the pump is running? Speed wins when present; the
// current channel is the fallback for installs that wire only that. Both NaN
// answers "not running", which callers must not confuse with "known off" — that
// distinction is decide_pump_state()'s job, below.
inline bool motor_reading_indicates_on(float motor_speed, float motor_current,
                                       float pump_off_current_threshold) {
  // Threshold mirrors Python: pump_off = motor_speed < 10 RPM
  if (!std::isnan(motor_speed))
    return motor_speed >= 10.0f;
  if (!std::isnan(motor_current))
    return motor_current >= pump_off_current_threshold;
  return false;
}

struct PumpStateInputs {
  float motor_speed{NAN};
  float motor_current{NAN};
  uint32_t motor_speed_last_update_ms{0};
  uint32_t motor_current_last_update_ms{0};
  uint32_t now_ms{0};
  /// Age bound for the motor channels. Same window the pump-flow channel uses,
  /// since both arrive on the same BLE poll.
  uint32_t motor_max_stale_ms{0};
  float pump_off_current_threshold{0.0f};
  /// Caller-held forward-fill state from previous ticks.
  bool pump_state_ever_known{false};
  bool last_known_pump_on{false};
};

struct PumpStateResult {
  /// A fresh motor reading was available on at least one channel.
  bool pump_state_known{false};
  /// Reporting, but not refreshing: values are arriving stale-dated.
  bool motor_frozen{false};
  bool pump_on{true};
  /// Positive evidence the pump is off. Gates the pump-OFF demand branch, so a
  /// false one hands the pump's own recirculation flow to the household-demand
  /// scorer at full confidence.
  bool pump_confirmed_off{false};
  /// The age-masked readings the decision was actually taken on: the input
  /// value where its channel was fresh, NaN where it was not. Nothing in the
  /// component consumes these — they exist so a test can assert that a stale
  /// channel was masked out rather than merely that its verdict came out right,
  /// which two different bugs can both satisfy.
  float speed_used{NAN};
  float current_used{NAN};
};

// Decide what the motor channels say about the pump, given that they may be
// stale, frozen, absent, or genuinely NaN.
//
// This is the branch selector for the whole detector: pump_confirmed_off gates
// the pump-OFF path, which scores raw meter flow as household demand. Getting it
// wrong in the OFF direction is the expensive mistake, because the loop's own
// recirculation (~2.2 GPM) then reads as a draw at confidence 1.0.
//
// The subtle case, and the one this exists for: alpha_hwr keeps publishing the
// last value on a BLE drop rather than publishing NaN, so `motor_speed` stays a
// perfectly valid 0.0 indefinitely. NaN-checking alone therefore reads a dead
// link as a confident "pump off" forever. Age is the real test — an unrefreshed
// reading is *unknown*, not *off* — so each channel is masked by its own
// staleness before anything looks at its value.
//
// Three regimes, deliberately distinguished:
//
//   fresh   — a real measurement; believe it in both directions.
//   frozen  — reporting but not refreshing. Assume ON and refuse to assert
//             confirmed-off. Assuming ON is safe because the pump-ON branch
//             measures demand as (flow - pump_flow) and carries its own
//             staleness bound, so with the pump-flow channel equally stale it
//             declines and reports uncertainty. Assuming OFF would instead hand
//             recirculation to deterministic_flow at confidence 1.
//   absent  — genuine NaN gap, or nothing wired at all. Forward-fill the last
//             known state, defaulting to ON before anything has been known.
//             A never-configured sensor has last_update 0, which
//             reading_is_fresh() reports as not fresh, so it lands here and
//             behaves exactly as it did before any of this existed.
inline PumpStateResult decide_pump_state(const PumpStateInputs &in) {
  PumpStateResult out;

  // Mask each channel by its own age, so a frozen value cannot be read as a
  // measurement.
  out.speed_used = reading_is_fresh(in.motor_speed_last_update_ms, in.now_ms, in.motor_max_stale_ms)
                       ? in.motor_speed
                       : NAN;
  out.current_used =
      reading_is_fresh(in.motor_current_last_update_ms, in.now_ms, in.motor_max_stale_ms)
          ? in.motor_current
          : NAN;

  const bool motor_reported = !std::isnan(in.motor_speed) || !std::isnan(in.motor_current);
  out.pump_state_known = !std::isnan(out.speed_used) || !std::isnan(out.current_used);
  out.motor_frozen = motor_reported && !out.pump_state_known;

  if (out.pump_state_known) {
    out.pump_on =
        motor_reading_indicates_on(out.speed_used, out.current_used, in.pump_off_current_threshold);
    out.pump_confirmed_off = !out.pump_on;
  } else if (out.motor_frozen) {
    out.pump_on = true;
    // A frozen reading is not evidence of anything, least of all "off". Letting
    // it assert confirmed-off is what reopened the pump-off branch even after
    // the staleness test had already rejected the reading.
    out.pump_confirmed_off = false;
  } else {
    out.pump_on = in.pump_state_ever_known ? in.last_known_pump_on : true;
    // Forward-filled: confirmed-off only if the last *known* state was off.
    out.pump_confirmed_off = in.pump_state_ever_known && !in.last_known_pump_on;
  }

  return out;
}

// Was this reading taken before the pump run it is being used to describe?
//
// reading_is_fresh bounds a reading's *age*, and age is the wrong question at a
// pump start, because the two flow channels do not begin reporting together.
// Measured on the Python side at a real start, with no tap open for another
// 2 m 16 s:
//
//     16:07:24  meter 0.000  pump_flow 0.000  speed 0     pump off
//     16:07:31  meter 1.429                   speed 2397  meter sees the loop
//     16:07:44                pump_flow 1.397 speed 2404  pump reports, 13 s on
//
// For those 13 s the difference is taken against a stale zero from *before the
// motor started* — well inside the 30 s staleness bound, and past the speed
// floor. 1.43 − 0.00 publishes as demand_level 0.57 at confidence 0.90, the top
// of this tier's range, which is the worst place to put a false positive. Over
// 30 days of Python's live output, 42 % of all pump-on subtraction firings fell
// within 10 s of a pump-on edge, against 8.8 % of pump-on cells overall.
//
// This is a regime test, not a suppression window: nothing is blocked for a
// fixed time and the tier resumes on the pump's very next reading. The fixed
// window that used to live here (PUMP_STARTUP_TRANSIENT_SUPPRESSION_MS, with
// pump_on_started_ms_) was retired with the vote tier it gated, and it is
// deliberately not reinstated in that shape.
//
// Ages are compared rather than absolute stamps so this wraps correctly across
// the ~49-day millis() rollover, the same as reading_is_fresh: the older
// reading has the larger age.
//
// pump_on_since_ms == 0 means no pump start has been observed — booted with the
// pump already running, say — so there is no boundary to test and this abstains,
// leaving the staleness bound in charge. Mirrors Python's
// _pump_flow_predates_startup returning False when the window holds no
// transition.
inline bool reading_predates_pump_start(uint32_t last_update_ms,
                                        uint32_t pump_on_since_ms,
                                        uint32_t now_ms) {
  if (last_update_ms == 0)
    return true;  // never reported: it cannot postdate anything
  if (pump_on_since_ms == 0)
    return false;
  return (uint32_t) (now_ms - last_update_ms) >
         (uint32_t) (now_ms - pump_on_since_ms);
}

// Has the pump's own loop-flow estimate settled after the motor started?
//
// The sibling of reading_predates_pump_start, for the failure that one does
// *not* catch — and the one that dominates in production. There the pump
// channel is silent across the start and the difference is taken against a
// pre-start zero. Here the pump reports promptly, so the reading is fresh and
// postdates the start, but the impeller is still accelerating and the estimate
// reads low against a loop the meter already sees moving:
//
//     16:16:57  pump_flow 0.713  speed 3172   <- estimate still ramping
//     16:16:59  meter     1.712               <- loop already at speed
//               1.712 - 0.713 = 1.00 GPM published at confidence 0.81
//
// pump_on_demand_min_speed_rpm does not cover it: that floor is for a low
// *steady* speed, and this fires at 3172 rpm during the overshoot.
//
// Measured on the Python side across 296 pump starts in 30 days,
// meter - pump_flow by age of the run:
//
//     0-10 s    p90 0.820 GPM   26.6 % above the 0.3 threshold
//     10-20 s   p90 0.171        6.2 %
//     20-30 s   p90 0.145        5.6 %
//     30-60 s   p90 0.191        6.5 %
//     60-120 s  p90 0.132        7.3 %
//
// The artifact is confined to the first band and the distribution is flat from
// 10 s on. The residual 6-9 % later is real draws during recirculation, which
// this tier exists to catch.
//
// This *is* a fixed window, as the retired PUMP_STARTUP_TRANSIENT_SUPPRESSION_MS
// was, so the difference matters: that one gated derivative votes and failed by
// not covering a speed change while already running. This gates one tier's
// input over the only interval where that input is measurably invalid, at a
// boundary measured across 296 starts rather than chosen. It costs up to 10 s
// of latency on a draw beginning within 10 s of a pump start; a draw already in
// progress is carried by the continuation tier instead.
//
// pump_on_since_ms == 0 means no start has been observed, so there is nothing
// to wait for and this reports settled.
inline bool pump_flow_estimate_is_settled(uint32_t pump_on_since_ms,
                                          uint32_t now_ms,
                                          uint32_t settle_ms) {
  if (settle_ms == 0 || pump_on_since_ms == 0)
    return true;
  return (uint32_t) (now_ms - pump_on_since_ms) >= settle_ms;
}

// Is the falling-edge flow latch disarmed because the pump just stopped?
//
// The latch exists for the gaps between meter reports *during a draw*. A pump
// shutdown presents the same shape for a different reason: loop flow runs a
// median 1.45 GPM and collapses through the flow threshold within seconds of
// the motor parking, so for flow_latch_seconds afterwards the latch finds the
// pump's own flow and holds demand alive on it — while the thermal vote fires
// because the pump has been returning cooled loop water to the tank bottom.
// Both votes are pump artifacts, and one rescues the other.
//
// Because the flow vote outranks thermal and charge on confidence, a *published*
// thermal or charge verdict means flow was below threshold at that instant and
// above it within the latch window. Measured on the Python side over 30 days,
// 71 % of thermal and 62 % of charge firings fell within 30 s of a pump-off
// edge, against 0.38 % of pump-off cells overall. Suppressing the latch for
// 30 s removed 93 false positives at a cost of 1 true positive (precision
// 0.768 → 0.810, recall flat at 0.993).
//
// The window is matched to flow_latch_seconds so the latch cannot reach past it
// into a shutdown reading. A real draw puts the meter above threshold on its own
// and never consults the latch, which is why this costs almost no recall:
// controlled runs of a draw spanning a shutdown, and of a draw starting 41 s
// after one, are detected identically with it on and off.
//
// window_ms == 0 disables it; pump_off_since_ms == 0 means no shutdown has been
// observed yet.
inline bool latch_suppressed_after_shutdown(uint32_t pump_off_since_ms,
                                            uint32_t now_ms,
                                            uint32_t window_ms) {
  if (window_ms == 0 || pump_off_since_ms == 0)
    return false;
  return (uint32_t) (now_ms - pump_off_since_ms) <= window_ms;
}

// Household demand while the pump runs, in GPM, or NaN.
//
// The meter reads everything leaving the mains; the pump reports its own
// recirculation loop. The difference is what the house drew — no rpm term, no
// fitted curve. Measured on controlled runs with a human opening one tap, so
// ground truth by construction:
//
//     no draw, 2400–3600 rpm     −0.10 ± 0.06 GPM   (four measurements)
//     draw open, 2402 rpm        +1.24 GPM
//
// Non-overlapping, roughly 25:1, and the residual bias is *negative* — a quiet
// loop computes as slightly negative demand and clamps to zero, so the error
// pushes toward false negatives, never false positives.
//
// Returns NaN when either channel is missing, either reading is too stale to
// difference, or the pump is turning too slowly for its own flow estimate to be
// trusted — each the caller's signal to fall through rather than guess.
//
// Because the pump reports 0 flow when stopped, this same expression collapses
// to the pump-off rule when it is off: one oracle for both regimes.
inline float pump_on_demand_flow(float meter_flow, float pump_flow,
                                 float motor_speed, bool meter_fresh,
                                 bool pump_flow_fresh, float min_speed_rpm) {
  if (std::isnan(meter_flow) || std::isnan(pump_flow))
    return NAN;
  if (!pump_flow_fresh || !meter_fresh)
    return NAN;
  if (std::isnan(motor_speed) || motor_speed < min_speed_rpm)
    return NAN;
  return meter_flow - pump_flow;
}

// Continuous-high tracker for the heater's DHW in-use flag. The flag is a weak
// proxy for movement, not ground truth: measured over 2026-07-21→28 it fires
// ~77 times a day with a median duration of 15 s, and 89.7 % of those events
// are at or under 70 s. Requiring it to stay continuously high past that point
// is what separates the blips from a real draw, and it is the only thing that
// makes the flag usable as a standalone pump-on demand signal (issue #138).
//
// The threshold's rationale has been restated since it was first written. It
// originally rested on a "60-second phantom event" story; that mechanism turned
// out to account for only 4.5 % of this flag's errors. The 70 s survives on the
// duration distribution above, not on the 60 s story.
//
// Mirrors Python's _dhw_in_use_sustained, including the part that looks like a
// bug and is not: NaN breaks the run exactly as a low sample does. Python
// re-derives the run from a rolling window each tick and a missing sample there
// is a break, so a BLE dropout must reset the timer here too. Hold-last-value
// would be the more forgiving choice and would diverge.
//
// min_ms == 0 means "high right now is enough" (no sustain requirement).
struct SustainedHigh {
  uint32_t min_ms{0};
  bool high{false};
  uint32_t high_since_ms{0};
  bool active{false};

  bool update(float value, uint32_t now_ms) {
    if (std::isnan(value) || value < 0.5f) {
      high = false;
      active = false;
      return false;
    }
    if (!high) {
      high = true;
      high_since_ms = now_ms;
    }
    // Unsigned subtraction is intentional: it wraps correctly across the
    // ~49-day millis() rollover.
    active = (now_ms - high_since_ms) >= min_ms;
    return active;
  }
};

// Everything the continuation tier decides on. A struct rather than seven
// positional arguments because two of the five numbers are GPM thresholds that
// share a default value and two more are millisecond stamps — exactly the shape
// that let pump_off_flow_onset_is_confirmed be fed the wrong argument for
// months (#147/#148). Named initialisation makes that misorder unrepresentable.
struct ContinuationInputs {
  float pre_pump_on_flow{NAN};  // meter flow at the last confirmed pump-off tick
  float flow{NAN};              // meter flow now, GPM
  float flow_threshold{0.0f};   // GPM, PumpOnThresholds::flow

  // The subtraction's verdict for this same tick — NaN when it could not be
  // computed. Passing it in rather than recomputing keeps one oracle for the
  // whole branch: the tier that releases on a measurement and the tier that
  // fires on it must never be able to read different numbers.
  float demand_gpm{NAN};
  float demand_flow_threshold{0.0f};  // GPM, PumpOnThresholds::demand_flow
  float release_gpm{0.0f};            // GPM, PumpOnThresholds::continuation_release

  // How many consecutive earlier ticks already measured the draw as stopped.
  // The caller owns the count; see continuation_release_ticks for why one tick
  // is not enough.
  uint8_t measured_stopped_ticks{0};
  uint8_t release_ticks{1};  // PumpOnThresholds::continuation_release_ticks

  uint32_t now_ms{0};
  // When this continuation was last *supported* — the pump-on edge that
  // captured pre_pump_on_flow, or the most recent tick on which the
  // subtraction positively confirmed the draw. 0 means never armed.
  uint32_t continuation_since_ms{0};
  uint32_t max_ms{0};  // PumpOnThresholds::continuation_max_ms
};

// Pump-on continuation. Demand was already established just before the pump
// turned on, so the draw that was being detected is presumed to still be
// running until something says otherwise. `pre_pump_on_flow` is NaN unless the
// last confirmed pump-off tick carried unambiguous demand evidence; the caller
// owns that capture.
//
// The hard part is not arming this tier but releasing it, and until issue #10
// of the 2026-08-12 audit it effectively never released. The only exit was
// `flow <= flow_threshold` on the raw meter, which cannot happen while the pump
// runs: the meter sees the recirculation loop, and every pump-on reading this
// repo records — 0.71 GPM at the 1650 rpm floor, 1.31 no-draw median, 1.45
// pre-shutdown, 2.22 no-draw p90 — clears the 0.3 threshold by at least 2.4×.
// So a draw that stopped five minutes into a thirty-minute run went on being
// published as demand at confidence 0.85 for the remaining twenty-five, with
// the subtraction sitting right there reading 0.00 GPM.
//
// Two exits replace it, and the raw-flow test is kept only for the cases it can
// still decide (a dead or genuinely zero meter):
//
//  1. **A measurement that contradicts it.** When the subtraction is available
//     it is the only pump-on reading of household draw there is, and a
//     first-hand measurement outranks a memory of one. Note this is not a new
//     signal and not a raw-flow rule: it is tier 2's own oracle, under tier 2's
//     own guards, used to release rather than to fire.
//
//  2. **An expiry, for when no measurement ever arrives.** The subtraction goes
//     quiet whenever the pump turns below min_speed_rpm — a pump clamped to
//     1650 rpm never offers one at all — and an unfalsifiable claim that only
//     gets older has to end somewhere. Once it expires the lower tiers decide,
//     and if the draw is real *and* measurable the subtraction picks it straight
//     back up, so the expiry costs recall only in the regime where nothing can
//     see the draw anyway.
// Why the tier fired or declined. The caller needs the distinction, not just
// the boolean: MEASURED_STOPPED is the one release that *falsifies* the stored
// evidence rather than merely failing to confirm it, so it is the one that must
// clear the capture. Releasing on a NaN meter reading must not, or a single
// dropped sample would permanently kill a continuation that is still true.
enum class ContinuationVerdict : uint8_t {
  ACTIVE,
  NOT_ARMED,    // no pre-pump demand evidence was captured
  METER_QUIET,  // the meter reads at or below threshold, or reads nothing
  // The subtraction read at or below the release threshold, but not yet on
  // enough consecutive ticks to be believed. The tier stays ACTIVE for this
  // tick; the caller counts.
  STOPPING,
  MEASURED_STOPPED,  // ...and now it has. The draw is over.
  // The draw has not been confirmed by a measurement for max_ms. Note this is
  // "unsupported for max_ms", not "max_ms since the pump started": the caller
  // re-stamps continuation_since_ms whenever the subtraction confirms.
  EXPIRED,
  // The subtraction positively confirms the draw is still running. Distinct
  // from ACTIVE so the caller knows to re-stamp the support time; both hold.
  CONFIRMED,
};

inline ContinuationVerdict
pump_on_continuation_verdict(const ContinuationInputs &in) {
  if (std::isnan(in.pre_pump_on_flow) ||
      in.pre_pump_on_flow <= in.flow_threshold)
    return ContinuationVerdict::NOT_ARMED;

  // Order matters, and METER_QUIET goes LAST of the three releases rather than
  // first. It is the only one that keeps the capture, so testing it first lets
  // it mask both of the others: a quiet meter alongside a subtraction that has
  // measured the draw as over would keep a falsified capture alive, and -- the
  // worse half -- a meter that stays NaN or quiet would never reach the age
  // test at all, so the tier could sit armed indefinitely and resume claiming
  // demand whenever the meter came back. That is the stuck-on bug this whole
  // change exists to remove, reached by a different door.
  //
  // The two authoritative exits therefore run first. This costs nothing in the
  // NaN case: pump_on_demand_flow returns NaN when the meter does, so a NaN
  // meter cannot produce a subtraction verdict here and falls through to the
  // age test, which is exactly what it should reach.
  if (!std::isnan(in.demand_gpm)) {
    // Exit 1, in two stages. See continuation_release for why the falsifying
    // threshold is not the firing one, and continuation_release_ticks for why
    // a single tick does not retire a capture it cannot restore.
    if (in.demand_gpm <= in.release_gpm) {
      const uint32_t seen = (uint32_t) in.measured_stopped_ticks + 1u;
      return seen >= (uint32_t) in.release_ticks
                 ? ContinuationVerdict::MEASURED_STOPPED
                 : ContinuationVerdict::STOPPING;
    }
    // A measurement that clears the *firing* threshold is positive evidence
    // the draw is still running, which is what the expiry below is counting
    // time from. Between release_gpm and demand_flow the subtraction is
    // saying neither thing, so the tier simply holds and the clock keeps
    // running -- the honest treatment of a reading inside the estimator's own
    // error band.
    if (in.demand_gpm > in.demand_flow_threshold)
      return ContinuationVerdict::CONFIRMED;
  }

  // Exit 2. An unstamped arm is treated as never armed rather than as
  // infinitely old: failing closed here is the direction that cannot resurrect
  // the stuck-on behaviour this exists to remove.
  if (in.continuation_since_ms == 0)
    return ContinuationVerdict::EXPIRED;
  // Unsigned subtraction is intentional: it wraps correctly across the ~49-day
  // millis() rollover, the same as reading_is_fresh's.
  if ((in.now_ms - in.continuation_since_ms) >= in.max_ms)
    return ContinuationVerdict::EXPIRED;

  // Last of the three releases, and the only one that keeps the capture: a
  // single dropped meter sample must not permanently end a continuation that
  // is still true. Reaching here means neither authoritative exit fired, so
  // this is the raw-flow test doing the one job it can still do while the pump
  // runs -- deciding a dead or genuinely zero meter.
  if (std::isnan(in.flow) || in.flow <= in.flow_threshold)
    return ContinuationVerdict::METER_QUIET;

  return ContinuationVerdict::ACTIVE;
}

// Does the tier decide this tick? CONFIRMED and STOPPING both hold: the first
// is a measurement agreeing with it, the second a measurement that disagrees
// but has not yet done so often enough to be trusted against a transient.
inline bool pump_on_continuation_is_active(const ContinuationInputs &in) {
  const ContinuationVerdict v = pump_on_continuation_verdict(in);
  return v == ContinuationVerdict::ACTIVE ||
         v == ContinuationVerdict::CONFIRMED ||
         v == ContinuationVerdict::STOPPING;
}

// Everything the pump-on branch decides on, in one place so the ordering below
// is a pure function of its inputs. The component fills this in from sensor
// reads, its own transition tracking and its per-channel update registers;
// nothing here reads a clock.
struct PumpOnInputs {
  float pre_pump_on_flow{NAN};  // household flow at the last pump-off tick
  float flow{NAN};              // household flow now, GPM
  float pump_flow{NAN};         // the pump's own loop reading, GPM
  float motor_speed{NAN};       // rpm

  // Has the heater's DHW in-use flag been continuously high for
  // dhw_in_use_min_seconds? Driven by SustainedHigh in the component, which
  // ticks in both pump branches — the run has to be free to start while the
  // pump is still off, or the guard would silently cost another 70 s after
  // every pump start.
  bool dhw_in_use_sustained{false};

  uint32_t now_ms{0};
  uint32_t flow_last_update_ms{0};
  uint32_t pump_flow_last_update_ms{0};

  // When the current pump run started, for reading_predates_pump_start. 0 means
  // no start has been observed, which makes that test abstain.
  uint32_t pump_on_since_ms{0};

  // When pre_pump_on_flow was captured, for the continuation expiry. Stamped by
  // the same branch that captures the flow, so the two are always consistent;
  // separate from pump_on_since_ms because that one is stamped only on a
  // *known* pump state and the capture can happen without one. The caller also
  // re-stamps it on a CONFIRMED tick, which is what makes the expiry mean
  // "unsupported for max_ms" rather than "max_ms since the pump started".
  uint32_t continuation_since_ms{0};

  // Consecutive prior ticks on which the subtraction read at or below the
  // release threshold. The caller owns the count; see
  // PumpOnThresholds::continuation_release_ticks.
  uint8_t measured_stopped_ticks{0};
};

// One pump-on decision. `demand_gpm` is the computed household draw where the
// subtraction was available and NaN otherwise; it is reported whichever tier
// decided, so a caller can log what the measurement said even when something
// above it fired.
struct PumpOnResult {
  bool demand{false};
  float confidence{0.0f};
  float demand_level{0.0f};
  const char *method{"pump_on_uncertain"};
  float demand_gpm{NAN};

  // What tier 1 decided, and why. Reported whichever tier ultimately fired, so
  // the component can retire a capture the subtraction has falsified without
  // re-deriving the reason from `method` — a replica of this decision in the
  // .cpp is exactly what issue #144 removed.
  ContinuationVerdict continuation{ContinuationVerdict::NOT_ARMED};
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
//   2. subtraction       — meter − loop, the draw measured directly
//   3. dhw_in_use        — the heater's own flag, once it has held long enough
//                          to be worth believing; a last-resort recall path for
//                          low-signal draws the subtraction could not measure
//   4. pump_on_uncertain — the fallback; claim nothing at 0.5 confidence rather
//                          than guess either way
//
// Each tier is reached only when everything above it declines.
//
// **No rule here may key off raw household flow while the pump runs**, and that
// is settled rather than merely untried (issue #138). Pump-on runs with no draw
// read a median 1.31 GPM (p90 2.22) against 1.74 (p90 2.27) with a draw — the
// distributions overlap almost entirely, so no threshold exists. The "~2.2 GPM
// recirculation baseline" both repos quoted for years was the p90 of the
// no-draw case, not a baseline. This is why tier 2 subtracts rather than votes,
// and why the tier-1 gate above is a *continuation* of a draw already confirmed
// while the pump was off, not a fresh reading of the meter.
inline PumpOnResult decide_pump_on(const PumpOnInputs &in,
                                   const PumpOnThresholds &t) {
  PumpOnResult r;

  // The loop-flow reading must be both recent enough *and* from this pump run.
  // Fresh-by-age is not sufficient at a startup: the meter reports the loop
  // seconds before the pump does, so the difference would be taken against a
  // pre-startup zero. See reading_predates_pump_start.
  bool pump_flow_usable =
      reading_is_fresh(in.pump_flow_last_update_ms, in.now_ms,
                       t.pump_flow_max_stale_ms) &&
      !reading_predates_pump_start(in.pump_flow_last_update_ms,
                                   in.pump_on_since_ms, in.now_ms) &&
      // Reported since the start, but the impeller may still be accelerating.
      // The two compose: this covers the first seconds, the one above covers a
      // channel still silent past them.
      pump_flow_estimate_is_settled(in.pump_on_since_ms, in.now_ms,
                                    t.pump_on_settle_ms);

  // The meter needs the same provenance test as the loop, and for the same
  // reason: fresh-by-age is not enough at a startup. The meter reports on
  // change at a median 28 s, so a value stamped before the pump started is
  // routinely still inside the 60 s bound, and differencing it against a fresh
  // loop reading understates the draw by the whole loop flow.
  //
  // This asymmetry predates the continuation work and was harmless while the
  // subtraction could only *fire* -- an understated difference merely fails to
  // raise a claim, which is the conservative direction the header argues for.
  // It stops being harmless the moment the same number can retire a capture,
  // so it is fixed here rather than left as the older half of the bug.
  bool meter_usable =
      reading_is_fresh(in.flow_last_update_ms, in.now_ms,
                       t.flow_max_stale_ms) &&
      !reading_predates_pump_start(in.flow_last_update_ms, in.pump_on_since_ms,
                                   in.now_ms);

  r.demand_gpm = pump_on_demand_flow(in.flow, in.pump_flow, in.motor_speed,
                                     meter_usable, pump_flow_usable,
                                     t.min_speed_rpm);

  // Fed r.demand_gpm, computed just above, so the tier that releases on the
  // subtraction and the tier that fires on it read the same number.
  const ContinuationInputs cont{
      /*pre_pump_on_flow=*/in.pre_pump_on_flow,
      /*flow=*/in.flow,
      /*flow_threshold=*/t.flow,
      /*demand_gpm=*/r.demand_gpm,
      /*demand_flow_threshold=*/t.demand_flow,
      /*release_gpm=*/t.continuation_release,
      /*measured_stopped_ticks=*/in.measured_stopped_ticks,
      /*release_ticks=*/t.continuation_release_ticks,
      /*now_ms=*/in.now_ms,
      /*continuation_since_ms=*/in.continuation_since_ms,
      /*max_ms=*/t.continuation_max_ms,
  };

  r.continuation = pump_on_continuation_verdict(cont);

  if (pump_on_continuation_is_active(cont)) {
    r.demand = true;
    r.confidence = 0.85f;
    // Intensity from the measurement where it is available; the meter alone is
    // uninformative in this regime, so fall back to the pre-pump reading that
    // established the draw rather than to the loop-contaminated current one.
    r.demand_level = std::min(1.0f, (!std::isnan(r.demand_gpm) && r.demand_gpm > 0.0f)
                                        ? r.demand_gpm / 2.5f
                                        : in.pre_pump_on_flow / 2.5f);
    r.method = "deterministic_continuation";
    return r;
  }

  if (!std::isnan(r.demand_gpm) && r.demand_gpm > t.demand_flow) {
    r.demand = true;
    // Confidence rises with margin over the threshold rather than with a vote
    // count: 1.24 GPM was the measured draw against a −0.10 ± 0.06 quiet
    // baseline, so ~1 GPM of margin is a strong reading.
    float margin = r.demand_gpm - t.demand_flow;
    r.confidence = std::min(0.90f, 0.60f + 0.30f * std::min(1.0f, margin));
    r.demand_level = std::min(1.0f, r.demand_gpm / 2.5f);
    r.method = "deterministic_pump_on_subtraction";
    return r;
  }

  // Tier 3 — the heater's own flag, guarded. Strictly additive: it sits below
  // everything and cannot displace a stronger tier, and it only ever adds
  // demand. Measured in the companion detector its whole footprint is 121 of
  // 60 480 cells over a week — 9 windows, 20.3 minutes — but those firings
  // corroborate against a channel sharing no sensor with the flag: on pump-on
  // runs ≥60 s the lower tank falls a median −0.390 °F/min when it fires
  // against −0.043 °F/min when it stays silent. A 9× gap, and the right shape:
  // cold makeup water entering the tank, not recirculation's slow bleed.
  //
  // The same flag is a bad *oracle* and a useful *input* — scored against
  // physics, 988 of its positive cells had no water behind them. Those two
  // roles are easy to conflate and the answers are opposite (issue #138).
  if (in.dhw_in_use_sustained) {
    r.demand = true;
    r.confidence = 0.6f;
    // Intensity from the measurement where it is available. Raw meter flow is
    // uninformative in this regime, so where the subtraction declined there is
    // no honest intensity to publish and the no-claim constant says so.
    r.demand_level = (!std::isnan(r.demand_gpm) && r.demand_gpm > 0.0f)
                         ? std::min(1.0f, r.demand_gpm / 2.5f)
                         : kNoIntensityClaim;
    r.method = "deterministic_dhw_in_use";
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
// This narrows AGENTS.md's "when the pump is off the flow meter reads only
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
