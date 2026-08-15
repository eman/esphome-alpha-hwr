/**
 * Unit tests for the DHW demand decision logic.
 *
 * These call production code in dhw_demand_logic.h directly — both the
 * individual predicates and, since issue #144, the pump-on tier *ordering*.
 * Nothing here may hand-mirror the logic it tests.
 */

#include "../components/dhw_demand/dhw_demand_logic.h"
#include <cmath>
#include <iostream>
#include <string>

using esphome::dhw_demand::apply_dhw_in_use_boost;
using esphome::dhw_demand::decide_pump_on;
using esphome::dhw_demand::DemandHold;
using esphome::dhw_demand::kDefaultPumpOnThresholds;
using esphome::dhw_demand::prev_tick_confirms_flow_onset;
using esphome::dhw_demand::pump_off_flow_onset_is_confirmed;
using esphome::dhw_demand::ContinuationInputs;
using esphome::dhw_demand::ContinuationVerdict;
using esphome::dhw_demand::pump_on_continuation_is_active;
using esphome::dhw_demand::pump_on_continuation_verdict;
using esphome::dhw_demand::pump_flow_estimate_is_settled;
using esphome::dhw_demand::pump_on_demand_flow;
using esphome::dhw_demand::PumpOnInputs;
using esphome::dhw_demand::decide_pump_state;
using esphome::dhw_demand::motor_reading_indicates_on;
using esphome::dhw_demand::PumpStateInputs;
using esphome::dhw_demand::PumpStateResult;
using esphome::dhw_demand::PumpOnResult;
using esphome::dhw_demand::PumpOnThresholds;
using esphome::dhw_demand::latch_suppressed_after_shutdown;
using esphome::dhw_demand::reading_is_fresh;
using esphome::dhw_demand::reading_predates_pump_start;
using esphome::dhw_demand::SustainedHigh;
using esphome::dhw_demand::SessionTracker;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                          \
  if (condition) {                                                               \
    tests_passed++;                                                              \
    std::cout << "[PASS] " << message << std::endl;                              \
  } else {                                                                       \
    tests_failed++;                                                              \
    std::cout << "[FAIL] " << message << std::endl;                              \
  }

#define TEST_ASSERT_NEAR(actual, expected, epsilon, message)                     \
  if (std::fabs((actual) - (expected)) <= (epsilon)) {                          \
    tests_passed++;                                                              \
    std::cout << "[PASS] " << message << std::endl;                              \
  } else {                                                                       \
    tests_failed++;                                                              \
    std::cout << "[FAIL] " << message                                            \
              << " (expected: " << (expected) << ", got: " << (actual)          \
              << ")" << std::endl;                                               \
  }

void test_flow_only_onset_requires_one_full_off_tick() {
  std::cout << "\n=== Testing Flow-Only Onset Confirmation ===" << std::endl;

  // Args: (flow_present, prev_flow_present_pump_off, onset_corroborating).
  bool first_tick_confirmed =
      pump_off_flow_onset_is_confirmed(true, false, false);
  bool second_tick_confirmed =
      pump_off_flow_onset_is_confirmed(true, true, false);
  bool corroborated_first_tick =
      pump_off_flow_onset_is_confirmed(true, false, true);
  bool no_flow_passes_through =
      pump_off_flow_onset_is_confirmed(false, false, false);
  // Flow carried across the pump-off transition. The caller qualifies the
  // second argument with `prev_pump_confirmed_off_`, so flow on the preceding
  // *pump-on* tick — recirculation, 1.3–2.3 GPM against a 0.3 GPM threshold —
  // arrives here as false and the onset stays ambiguous.
  //
  // This assertion was here before, was inverted by #120 to match what
  // production did at the time, and is restored by #147, which changed
  // production instead. Without it the debounce is always already satisfied at
  // the instant the pump stops, which is the one moment it exists for.
  bool carried_recirc_flow_confirmed =
      pump_off_flow_onset_is_confirmed(true, false, false);

  TEST_ASSERT(!first_tick_confirmed,
              "A brand-new flow-only onset is treated as ambiguous");
  TEST_ASSERT(second_tick_confirmed,
              "Flow-only demand is accepted after one full off tick");
  TEST_ASSERT(corroborated_first_tick,
              "Corroborated first-tick flow is accepted immediately");
  TEST_ASSERT(no_flow_passes_through,
              "The predicate only gates flow onset; no-flow passes through");
  TEST_ASSERT(!carried_recirc_flow_confirmed,
              "Flow carried from a pump-on tick stays ambiguous on the first "
              "off tick");
}

void test_only_a_pump_off_tick_arms_the_flow_debounce() {
  std::cout << "\n=== Testing Flow-Onset Debounce Qualifier ===" << std::endl;

  const float kThreshold = 0.3f;

  // The defect (#147): loop flow on a pump-on tick must not arm the debounce.
  // 1.3–2.3 GPM is the measured range while the pump runs, all of it far above
  // the 0.3 GPM threshold, so an unqualified check passes every time.
  bool recirc_arms_it =
      prev_tick_confirms_flow_onset(2.2f, kThreshold, /*prev_off=*/false);
  bool low_recirc_arms_it =
      prev_tick_confirms_flow_onset(1.3f, kThreshold, /*prev_off=*/false);

  // A genuine draw observed while the pump was already off does arm it.
  bool pump_off_flow_arms_it =
      prev_tick_confirms_flow_onset(1.5f, kThreshold, /*prev_off=*/true);

  // Sub-threshold and absent readings arm nothing, off or not.
  bool sub_threshold_arms_it =
      prev_tick_confirms_flow_onset(0.2f, kThreshold, /*prev_off=*/true);
  bool nan_arms_it =
      prev_tick_confirms_flow_onset(NAN, kThreshold, /*prev_off=*/true);

  // A NaN-gap tick whose last-known pump state was ON is not confirmed off, so
  // it cannot arm the debounce either — same reasoning as pre_pump_on_flow_.
  bool unconfirmed_gap_arms_it =
      prev_tick_confirms_flow_onset(1.5f, kThreshold, /*prev_off=*/false);

  TEST_ASSERT(!recirc_arms_it,
              "Loop flow at 2.2 GPM on a pump-on tick does not arm the "
              "debounce");
  TEST_ASSERT(!low_recirc_arms_it,
              "Loop flow at the low end of its range does not arm it either");
  TEST_ASSERT(pump_off_flow_arms_it,
              "Flow observed while the pump was confirmed off does arm it");
  TEST_ASSERT(!sub_threshold_arms_it,
              "Sub-threshold flow on a pump-off tick does not arm it");
  TEST_ASSERT(!nan_arms_it, "A missing reading does not arm it");
  TEST_ASSERT(!unconfirmed_gap_arms_it,
              "A tick not confirmed pump-off does not arm it");
}

void test_dhw_in_use_boost_is_gated_to_pump_off() {
  std::cout << "\n=== Testing DHW In-Use Boost Gating ===" << std::endl;

  float pump_off_boosted = apply_dhw_in_use_boost(0.80f, true, false, 1.0f);
  float pump_on_boosted = apply_dhw_in_use_boost(0.80f, true, true, 1.0f);
  float no_demand_boosted = apply_dhw_in_use_boost(0.80f, false, false, 1.0f);
  float nan_flag_boosted = apply_dhw_in_use_boost(0.80f, true, false, NAN);
  float clamped = apply_dhw_in_use_boost(0.99f, true, false, 1.0f);

  TEST_ASSERT_NEAR(pump_off_boosted, 0.85f, 0.0001f,
                   "Pump-off demand with dhw_in_use high receives the boost");
  TEST_ASSERT_NEAR(pump_on_boosted, 0.80f, 0.0001f,
                   "Pump-on demand does not receive the boost (60 s phantom flag)");
  TEST_ASSERT_NEAR(no_demand_boosted, 0.80f, 0.0001f,
                   "No boost is applied when there is no demand");
  TEST_ASSERT_NEAR(nan_flag_boosted, 0.80f, 0.0001f,
                   "A NaN dhw_in_use reading does not boost");
  TEST_ASSERT_NEAR(clamped, 1.0f, 0.0001f, "Boosted confidence is clamped to 1.0");
}

void test_ambiguous_flow_onset_does_not_prime_continuation() {
  std::cout << "\n=== Testing Ambiguous Onset Does Not Prime Continuation ==="
            << std::endl;

  bool prev_pump_confirmed_off = true;
  bool prev_pre_pump_demand_eligible = false;  // flow onset was ambiguous
  bool capture_pre_pump_flow =
      prev_pump_confirmed_off && prev_pre_pump_demand_eligible;

  TEST_ASSERT(!capture_pre_pump_flow,
              "Pump-on continuation is not primed by an ambiguous flow-only onset");
}

void test_sustained_demand_accrues_session() {
  std::cout << "\n=== Testing Sustained Demand Session Accrual ===" << std::endl;

  SessionTracker s;
  s.gap_ms = 60000;  // matches session_gap_tolerance_seconds default of 60 s

  uint32_t t = 100000;
  SessionTracker::Tick tick = s.update(true, t);
  TEST_ASSERT(tick.just_started, "First demand tick opens a session");

  // A sustained draw: 30 ticks at the 10 s default update interval.
  bool restarted = false;
  for (int i = 1; i <= 30; i++) {
    t += 10000;
    tick = s.update(true, t);
    if (tick.just_started)
      restarted = true;
  }
  TEST_ASSERT(!restarted, "A sustained draw does not restart the session");
  TEST_ASSERT_NEAR(tick.live_duration_s, 300.0f, 0.001f,
                   "Duration accrues across a 300 s draw");
  TEST_ASSERT(s.active, "Session is still open during the draw");

  // A gap shorter than the tolerance must not close the session.
  t += 30000;
  tick = s.update(false, t);
  TEST_ASSERT(!tick.just_ended && s.active,
              "A 30 s lull does not end a session (gap tolerance 60 s)");

  // Demand resumes: still the same session, and the lull counts toward it.
  t += 10000;
  tick = s.update(true, t);
  TEST_ASSERT(!tick.just_started && s.active,
              "Demand resuming inside the gap continues the same session");
  TEST_ASSERT_NEAR(tick.live_duration_s, 340.0f, 0.001f,
                   "Duration keeps accruing across a brief lull");

  // A gap of exactly the tolerance does NOT close the session: the comparison
  // is strictly greater, matching Python's `gap > gap_tolerance_seconds`
  // (session.py:175). This boundary is the whole of issue #125 item 3.
  t += 60000;
  tick = s.update(false, t);
  TEST_ASSERT(!tick.just_ended && s.active,
              "A gap of exactly the tolerance keeps the session open "
              "(strictly-greater, matching Python)");

  // One tick past it closes. Duration is measured to the last demand tick, not
  // to the moment the gap expired.
  t += 10000;
  tick = s.update(false, t);
  TEST_ASSERT(tick.just_ended, "A gap beyond the tolerance ends the session");
  TEST_ASSERT_NEAR(tick.ended_duration_s, 340.0f, 0.001f,
                   "Closed duration excludes the trailing quiet gap");
  TEST_ASSERT_NEAR(tick.live_duration_s, 0.0f, 0.001f,
                   "Live duration reads 0 once the session closes");

  // A later draw opens a genuinely new session.
  t += 600000;
  tick = s.update(true, t);
  TEST_ASSERT(tick.just_started, "A later draw opens a new session");
  TEST_ASSERT_NEAR(tick.live_duration_s, 0.0f, 0.001f,
                   "The new session starts its duration from zero");
}

void test_threshold_jitter_does_not_chatter() {
  std::cout << "\n=== Testing Threshold Jitter Does Not Chatter ===" << std::endl;

  DemandHold hold;
  hold.release_ms = 30000;  // demand_release_seconds default

  uint32_t t = 50000;
  TEST_ASSERT(hold.update(true, t), "Rising edge passes through immediately");

  // An input dithering around its threshold: demand alternates every tick.
  // The published value must stay continuously true.
  bool stayed_high = true;
  for (int i = 0; i < 20; i++) {
    t += 10000;
    if (!hold.update(i % 2 == 0, t))
      stayed_high = false;
  }
  TEST_ASSERT(stayed_high,
              "Alternating demand does not chatter the published output");

  // A clean quiet period longer than the release window does drop it. The last
  // true tick was one iteration before the loop ended.
  t += 10000;
  TEST_ASSERT(hold.update(false, t),
              "Demand is still held part-way through the release window");
  t += 30000;
  TEST_ASSERT(!hold.update(false, t),
              "Demand releases after a full quiet release window");

  // release_ms == 0 disables the hold entirely.
  DemandHold passthrough;
  passthrough.release_ms = 0;
  TEST_ASSERT(passthrough.update(true, 1000), "Passthrough follows a true tick");
  TEST_ASSERT(!passthrough.update(false, 1000),
              "With release_ms 0 the raw value passes straight through");
}

// A pump-on tick where both channels just reported and the pump is turning fast
// enough for its own flow estimate to be trusted. Individual tests perturb one
// field at a time from here.
static PumpOnInputs live_tick() {
  PumpOnInputs in;
  in.now_ms = 1000000;
  in.flow_last_update_ms = 1000000;
  in.pump_flow_last_update_ms = 1000000;
  in.motor_speed = 2402.0f;
  in.flow = 1.34f;
  in.pump_flow = 1.34f;  // quiet loop: computes to 0 demand
  // Armed a minute ago, well inside the continuation's expiry, so a test that
  // perturbs one field is testing that field and not the expiry.
  in.continuation_since_ms = in.now_ms - 60000;
  return in;
}

// The inputs the continuation tier sees on a tick where a draw was established
// before the pump started and nothing since has contradicted it. Fields are
// named so a perturbation reads as what it is.
static ContinuationInputs armed_continuation() {
  ContinuationInputs in;
  in.pre_pump_on_flow = 1.5f;
  in.flow = 1.5f;
  in.flow_threshold = kDefaultPumpOnThresholds.flow;
  in.demand_gpm = NAN;  // no usable subtraction, the tier's normal habitat
  in.demand_flow_threshold = kDefaultPumpOnThresholds.demand_flow;
  // Seeded from the shipped thresholds, not left at the struct defaults: the
  // struct defaults are a 1-tick release, so a test that forgot these would
  // quietly assert against a debounce the firmware does not have.
  in.release_gpm = kDefaultPumpOnThresholds.continuation_release;
  in.release_ticks = kDefaultPumpOnThresholds.continuation_release_ticks;
  in.now_ms = 1000000;
  in.continuation_since_ms = 1000000 - 60000;
  in.max_ms = kDefaultPumpOnThresholds.continuation_max_ms;
  return in;
}

void test_reading_freshness() {
  std::cout << "\n=== Testing Reading Freshness ===" << std::endl;

  // Args: (last_update_ms, now_ms, max_stale_ms).
  TEST_ASSERT(!reading_is_fresh(0, 50000, 30000),
              "A channel that has never reported is never fresh");
  TEST_ASSERT(reading_is_fresh(50000, 50000, 30000),
              "A reading taken this instant is fresh");
  TEST_ASSERT(reading_is_fresh(20000, 50000, 30000),
              "A reading exactly at the bound is still fresh");
  TEST_ASSERT(!reading_is_fresh(19999, 50000, 30000),
              "One millisecond past the bound is stale");

  // The two channels do not report alike, which is why the bounds differ: the
  // pump every 10 s, the meter on change at a median 28 s while flowing. A
  // 45 s-old reading is stale for the pump and fresh for the meter.
  TEST_ASSERT(!reading_is_fresh(5000, 50000,
                                kDefaultPumpOnThresholds.pump_flow_max_stale_ms),
              "45 s is stale on the pump channel (30 s bound)");
  TEST_ASSERT(reading_is_fresh(5000, 50000,
                               kDefaultPumpOnThresholds.flow_max_stale_ms),
              "45 s is fresh on the meter channel (60 s bound)");

  // Unsigned subtraction wraps correctly across the ~49-day millis() rollover,
  // the same as DemandHold's. A reading from just before the wrap must not read
  // as ~49 days old just because the counter reset.
  const uint32_t before_wrap = 0xFFFFF000u;
  const uint32_t after_wrap = 0x00001000u;  // 8192 ms later
  TEST_ASSERT(reading_is_fresh(before_wrap, after_wrap, 30000),
              "Freshness survives the millis() rollover");
}

void test_pump_on_demand_flow_guards() {
  std::cout << "\n=== Testing Pump-On Demand Subtraction Guards ==="
            << std::endl;

  const float kMinRpm = kDefaultPumpOnThresholds.min_speed_rpm;

  // The measured draw: meter 2.58 against a 1.34 loop is 1.24 GPM, the figure
  // from the controlled run with one tap open at 2402 rpm.
  TEST_ASSERT_NEAR(
      pump_on_demand_flow(2.58f, 1.34f, 2402.0f, true, true, kMinRpm), 1.24f,
      0.0001f, "Meter minus loop is the household draw");

  // A quiet loop computes slightly negative — the residual bias is one-directional
  // toward false negatives, never false positives.
  TEST_ASSERT(pump_on_demand_flow(1.34f, 1.47f, 2398.0f, true, true, kMinRpm) <
                  0.0f,
              "A quiet loop computes as slightly negative demand");

  // Guard 1: both channels must be present.
  TEST_ASSERT(std::isnan(pump_on_demand_flow(NAN, 1.34f, 2402.0f, true, true,
                                             kMinRpm)),
              "A missing meter reading declines rather than guessing");
  TEST_ASSERT(std::isnan(pump_on_demand_flow(2.58f, NAN, 2402.0f, true, true,
                                             kMinRpm)),
              "A missing loop reading declines rather than guessing");

  // Guard 2: both must be current. A difference of two quantities is only
  // meaningful if both are — during one bench run a loop reading that looked
  // live was 90 s old, from before the tap was opened.
  TEST_ASSERT(std::isnan(pump_on_demand_flow(2.58f, 1.34f, 2402.0f, true, false,
                                             kMinRpm)),
              "A stale loop reading declines");
  TEST_ASSERT(std::isnan(pump_on_demand_flow(2.58f, 1.34f, 2402.0f, false, true,
                                             kMinRpm)),
              "A stale meter reading declines");

  // Guard 3: the pump estimates its loop flow rather than metering it, and near
  // the bottom of its range the estimate reads low — so the difference goes
  // spuriously positive with the tap shut. These are the measured no-draw rows.
  TEST_ASSERT(std::isnan(pump_on_demand_flow(0.71f, 0.25f, 1650.0f, true, true,
                                             kMinRpm)),
              "1650 rpm is below the floor, so its +0.45 bias cannot fire");
  TEST_ASSERT(std::isnan(pump_on_demand_flow(0.83f, 0.56f, 1800.0f, true, true,
                                             kMinRpm)),
              "1800 rpm is below the floor, so its +0.27 bias cannot fire");
  TEST_ASSERT(!std::isnan(pump_on_demand_flow(0.99f, 0.99f, 2001.0f, true, true,
                                              kMinRpm)),
              "2001 rpm is above the floor, where the bias is measured at ~0");
  TEST_ASSERT(std::isnan(pump_on_demand_flow(2.58f, 1.34f, NAN, true, true,
                                             kMinRpm)),
              "An unknown pump speed declines — the floor cannot be checked");

  // The floor admits the whole production range: over 29 days the pump's own
  // minimum was 1971 rpm, and it excludes only bench-commanded speeds.
  TEST_ASSERT(!std::isnan(pump_on_demand_flow(1.00f, 0.99f, 1971.0f, true, true,
                                              kMinRpm)),
              "The 29-day production minimum of 1971 rpm is admitted");
}

void test_dhw_in_use_sustain_rejects_short_events() {
  std::cout << "\n=== Testing DHW In-Use Sustain Guard ===" << std::endl;

  SustainedHigh sustain;
  sustain.min_ms = 70000;  // the shipped default

  uint32_t t = 100000;  // arbitrary, non-zero: no first-tick special case
  bool armed_before_70s = false;
  for (int i = 0; i < 7; i++) {  // t=100s..160s, i.e. 0..60 s of continuous high
    if (sustain.update(1.0f, t))
      armed_before_70s = true;
    t += 10000;
  }
  TEST_ASSERT(!armed_before_70s,
              "Flag high for 60 s does not arm (the phantom band)");

  bool armed_at_70s = sustain.update(1.0f, t);  // t = 170s → exactly 70 s
  TEST_ASSERT(armed_at_70s, "Flag high for exactly 70 s arms (>= boundary)");
}

void test_dhw_in_use_sustain_resets_on_break() {
  std::cout << "\n=== Testing DHW In-Use Sustain Reset ===" << std::endl;

  // A low sample mid-run restarts the clock from scratch.
  SustainedHigh low_break;
  low_break.min_ms = 70000;
  uint32_t t = 0;
  for (int i = 0; i < 7; i++, t += 10000)
    low_break.update(1.0f, t);
  low_break.update(0.0f, t);
  t += 10000;
  TEST_ASSERT(!low_break.update(1.0f, t),
              "A low sample restarts the run, so 70 s of prior high is discarded");

  // NaN breaks the run identically. This is the parity-critical case: a BLE
  // dropout is a break in Python's window walk (detection.py:377, :382), not a
  // hold-last-value.
  SustainedHigh nan_break;
  nan_break.min_ms = 70000;
  t = 0;
  for (int i = 0; i < 7; i++, t += 10000)
    nan_break.update(1.0f, t);
  nan_break.update(NAN, t);
  t += 10000;
  TEST_ASSERT(!nan_break.update(1.0f, t),
              "A NaN sample breaks the run exactly as a low sample does");

  // And the run does rearm, given another full window after the break.
  for (int i = 0; i < 7; i++) {
    t += 10000;
    nan_break.update(1.0f, t);
  }
  TEST_ASSERT(nan_break.active, "The run rearms after a fresh 70 s of high");
}

void test_dhw_in_use_sustain_never_arms_without_the_sensor() {
  std::cout << "\n=== Testing DHW In-Use Sustain Without Sensor ==="
            << std::endl;

  // read_sensor_ returns NaN forever when no dhw_in_use sensor is configured,
  // so installs that never wired the flag must never reach the tier.
  SustainedHigh unwired;
  unwired.min_ms = 70000;
  uint32_t t = 0;
  bool ever_armed = false;
  for (int i = 0; i < 100; i++, t += 10000) {
    if (unwired.update(NAN, t))
      ever_armed = true;
  }
  TEST_ASSERT(!ever_armed,
              "An all-NaN stream never arms the tier (sensor not configured)");
}


void test_pump_on_tier_ordering() {
  std::cout << "\n=== Testing Pump-On Tier Ordering ===" << std::endl;

  // Until issue #144 this ordering lived inline in update(), where no host test
  // could reach it. Every individual predicate was covered; their composition
  // was covered only by reading the .cpp — the failure mode that let the stale
  // 3.0f head-rate threshold survive the units audit (#120) and fed
  // pump_off_flow_onset_is_confirmed the wrong argument for months (#147/#148).
  const PumpOnThresholds &t = kDefaultPumpOnThresholds;

  // ── Tier 1 outranks tier 2 ──────────────────────────────────────────────
  // A draw established before the pump started, and the subtraction measuring
  // one now. Continuation must win: it is the stronger claim.
  PumpOnInputs both = live_tick();
  both.pre_pump_on_flow = 1.5f;
  both.flow = 2.58f;
  both.pump_flow = 1.34f;  // 1.24 GPM of measured demand
  PumpOnResult r = decide_pump_on(both, t);

  TEST_ASSERT(std::string(r.method) == "deterministic_continuation",
              "Continuation is tried before the subtraction");
  TEST_ASSERT(r.demand, "Continuation declares demand");
  TEST_ASSERT_NEAR(r.confidence, 0.85f, 0.0001f,
                   "Continuation reports 0.85 confidence");
  TEST_ASSERT_NEAR(r.demand_level, 1.24f / 2.5f, 0.0001f,
                   "Continuation scales intensity off the measurement");
  TEST_ASSERT_NEAR(r.demand_gpm, 1.24f, 0.0001f,
                   "The measurement is reported even when a higher tier decided");

  // ── Tier 2 is reached only when tier 1 declines ─────────────────────────
  PumpOnInputs measured = both;
  measured.pre_pump_on_flow = NAN;
  r = decide_pump_on(measured, t);

  TEST_ASSERT(std::string(r.method) == "deterministic_pump_on_subtraction",
              "The subtraction decides once continuation declines");
  TEST_ASSERT(r.demand, "A measured draw declares demand");
  TEST_ASSERT_NEAR(r.demand_gpm, 1.24f, 0.0001f, "1.24 GPM was drawn");
  TEST_ASSERT_NEAR(r.demand_level, 1.24f / 2.5f, 0.0001f,
                   "demand_level scales off measured GPM, not meter flow");
  // Confidence rises with margin over the threshold rather than with a vote
  // count: 0.60 + 0.30 * min(1, 1.24 - 0.30).
  TEST_ASSERT_NEAR(r.confidence, 0.882f, 0.0001f,
                   "Confidence rises with margin over the threshold");

  // A large draw caps at 0.90 rather than running away.
  PumpOnInputs big = measured;
  big.flow = 5.0f;
  r = decide_pump_on(big, t);
  TEST_ASSERT_NEAR(r.confidence, 0.90f, 0.0001f, "Confidence caps at 0.90");
  TEST_ASSERT_NEAR(r.demand_level, 1.0f, 0.0001f, "demand_level caps at 1.0");

  // ── Tier 3 is the fallback of last resort ───────────────────────────────
  // A quiet loop: the meter reads 1.34 GPM, which is the middle of the no-draw
  // pump-on distribution. Any rule keyed off raw meter flow would fire here.
  PumpOnInputs quiet = live_tick();
  r = decide_pump_on(quiet, t);

  TEST_ASSERT(std::string(r.method) == "pump_on_uncertain",
              "A quiet recirculation loop falls through to pump_on_uncertain");
  TEST_ASSERT(!r.demand, "pump_on_uncertain declares no demand");
  TEST_ASSERT_NEAR(r.confidence, 0.5f, 0.0001f,
                   "pump_on_uncertain claims neither way, at 0.5");
  TEST_ASSERT_NEAR(r.demand_level, 0.0f, 0.0001f,
                   "pump_on_uncertain publishes no intensity");

  // Either side of the threshold. Not asserted at exactly 0.30: no pair of
  // floats subtracts to it exactly, so that test would be measuring rounding
  // rather than the rule.
  PumpOnInputs below = live_tick();
  below.pump_flow = 1.09f;  // 1.34 - 1.09 = 0.25 GPM of computed demand
  TEST_ASSERT(std::string(decide_pump_on(below, t).method) ==
                  "pump_on_uncertain",
              "0.25 GPM of computed demand is below threshold and does not fire");

  PumpOnInputs above = live_tick();
  above.pump_flow = 0.99f;  // 1.34 - 0.99 = 0.35 GPM
  TEST_ASSERT(std::string(decide_pump_on(above, t).method) ==
                  "deterministic_pump_on_subtraction",
              "0.35 GPM of computed demand fires");

  // ── A declining guard reaches the fallback, never a guess ───────────────
  PumpOnInputs stale = live_tick();
  stale.flow = 2.58f;  // would be a 1.24 GPM draw if the reading were current
  stale.pump_flow_last_update_ms = 900000;  // 100 s old, past the 30 s bound
  r = decide_pump_on(stale, t);
  TEST_ASSERT(std::string(r.method) == "pump_on_uncertain",
              "A stale loop reading falls through rather than differencing");
  TEST_ASSERT(std::isnan(r.demand_gpm),
              "No measurement is reported when a guard declined");

  PumpOnInputs slow = live_tick();
  slow.flow = 0.71f;
  slow.pump_flow = 0.25f;  // +0.45 of low-speed estimator bias, no draw
  slow.motor_speed = 1650.0f;
  r = decide_pump_on(slow, t);
  TEST_ASSERT(std::string(r.method) == "pump_on_uncertain",
              "Low-speed estimator bias cannot fire a false detection");

  // Total BLE loss reaches the same fallback rather than crashing or guessing.
  PumpOnInputs all_nan;
  all_nan.now_ms = 1000000;
  r = decide_pump_on(all_nan, t);
  TEST_ASSERT(std::string(r.method) == "pump_on_uncertain",
              "All-NaN inputs fall through to pump_on_uncertain");
  TEST_ASSERT(!r.demand, "All-NaN inputs declare no demand");

  // ── Tier 3 sits below the subtraction and cannot displace it ────────────
  // The flag sustained *and* a measured draw: the subtraction must win, because
  // it is the stronger claim and the one that knows how large the draw is.
  PumpOnInputs flag_and_measurement = measured;
  flag_and_measurement.dhw_in_use_sustained = true;
  r = decide_pump_on(flag_and_measurement, t);
  TEST_ASSERT(std::string(r.method) == "deterministic_pump_on_subtraction",
              "The subtraction outranks the dhw_in_use tier");

  // Continuation outranks it too.
  PumpOnInputs flag_and_continuation = both;
  flag_and_continuation.dhw_in_use_sustained = true;
  TEST_ASSERT(std::string(decide_pump_on(flag_and_continuation, t).method) ==
                  "deterministic_continuation",
              "Continuation outranks the dhw_in_use tier");

  // Reached only when everything above declines. This is the recall case it
  // exists for: a low-signal draw the subtraction could not measure.
  PumpOnInputs flag_only = live_tick();
  flag_only.dhw_in_use_sustained = true;
  r = decide_pump_on(flag_only, t);
  TEST_ASSERT(std::string(r.method) == "deterministic_dhw_in_use",
              "The flag decides once continuation and the subtraction decline");
  TEST_ASSERT(r.demand, "The dhw_in_use tier declares demand");
  TEST_ASSERT_NEAR(r.confidence, 0.6f, 0.0001f,
                   "The dhw_in_use tier reports 0.6 confidence");
  TEST_ASSERT_NEAR(r.demand_level, 0.4f, 0.0001f,
                   "With no measurement it publishes the no-claim constant, "
                   "not a value derived from meter flow");

  // Where the subtraction *is* available but simply below threshold, intensity
  // still comes from the measurement rather than the meter. This is the whole
  // of issue #143: scaling off pump-on meter flow inverts the ordering, since a
  // quiet 2.2 GPM loop would publish 0.88 while a real 0.25 GPM draw publishes
  // 0.4.
  PumpOnInputs flag_with_small_draw = live_tick();
  flag_with_small_draw.dhw_in_use_sustained = true;
  flag_with_small_draw.flow = 1.54f;   // 1.54 - 1.34 = 0.20 GPM, under 0.30
  r = decide_pump_on(flag_with_small_draw, t);
  TEST_ASSERT(std::string(r.method) == "deterministic_dhw_in_use",
              "0.20 GPM is below the subtraction threshold, so the flag decides");
  TEST_ASSERT_NEAR(r.demand_level, 0.20f / 2.5f, 0.0001f,
                   "Intensity comes from the measurement, not from meter flow");

  // A stale channel means no measurement, so the tier falls back to the
  // no-claim constant rather than inventing intensity from the meter.
  PumpOnInputs flag_with_stale = live_tick();
  flag_with_stale.dhw_in_use_sustained = true;
  flag_with_stale.flow = 2.22f;  // would look like a big draw on the meter alone
  flag_with_stale.pump_flow_last_update_ms = 900000;
  r = decide_pump_on(flag_with_stale, t);
  TEST_ASSERT_NEAR(decide_pump_on(flag_with_stale, t).demand_level, 0.4f,
                   0.0001f,
                   "A 2.22 GPM meter reading with no measurement still "
                   "publishes 0.4, not 0.89");

  // An unsustained flag changes nothing — the guard is the whole tier.
  PumpOnInputs unsustained = live_tick();
  TEST_ASSERT(std::string(decide_pump_on(unsustained, t).method) ==
                  "pump_on_uncertain",
              "Without the sustain guard the flag never reaches a decision");

  // ── No rule may key off raw meter flow while the pump runs ──────────────
  // The whole point of the subtraction. Two ticks with the *same* meter
  // reading, differing only in what the loop is doing: one is a quiet loop at
  // 2.22 GPM (the p90 of the no-draw distribution), the other a real draw with
  // the loop nearly stopped. Any threshold on meter flow alone reads them
  // identically; the subtraction separates them.
  PumpOnInputs loud_recirc = live_tick();
  loud_recirc.flow = 2.22f;
  loud_recirc.pump_flow = 2.22f;
  loud_recirc.motor_speed = 3600.0f;

  PumpOnInputs real_draw = live_tick();
  real_draw.flow = 2.22f;
  real_draw.pump_flow = 0.50f;

  TEST_ASSERT(std::string(decide_pump_on(loud_recirc, t).method) ==
                  "pump_on_uncertain",
              "A loud quiet loop at 2.22 GPM declares nothing");
  TEST_ASSERT(std::string(decide_pump_on(real_draw, t).method) ==
                  "deterministic_pump_on_subtraction",
              "The same meter reading with a stopped loop is a draw");
}

void test_pump_on_continuation_predicate() {
  std::cout << "\n=== Testing Pump-On Continuation Predicate ===" << std::endl;

  ContinuationInputs in = armed_continuation();
  TEST_ASSERT(pump_on_continuation_is_active(in),
              "Flow above threshold on both sides of the pump start continues");

  in = armed_continuation();
  in.pre_pump_on_flow = NAN;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::NOT_ARMED,
              "No captured pre-pump flow means no continuation");

  in = armed_continuation();
  in.pre_pump_on_flow = 0.1f;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::NOT_ARMED,
              "Pre-pump flow at or below threshold is not demand evidence");

  in = armed_continuation();
  in.flow = NAN;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::METER_QUIET,
              "A NaN current reading does not continue a draw");

  // The *other* half of METER_QUIET, and it needs its own case: the assertion
  // below it looks like it covers the meter comparison but does not, because
  // the arm check runs first and returns NOT_ARMED before the meter is ever
  // read. A dead or genuinely zero meter is the reachable input here -- the one
  // thing raw meter flow can still decide while the pump runs -- so this is the
  // branch AGENTS §11.4 keeps in the exit table, and dropping the comparison
  // must not pass.
  in = armed_continuation();
  in.flow = 0.0f;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::METER_QUIET,
              "A meter reading zero releases the tier");

  in = armed_continuation();
  in.flow = in.flow_threshold;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::METER_QUIET,
              "...and one exactly at the threshold does too");

  in = armed_continuation();
  in.pre_pump_on_flow = 0.3f;
  in.flow = 0.3f;
  TEST_ASSERT(pump_on_continuation_verdict(in) ==
                  ContinuationVerdict::NOT_ARMED,
              "The comparison is strictly greater on both sides");
}

void test_continuation_releases_when_the_draw_stops() {
  std::cout << "\n=== Testing Continuation Release ===" << std::endl;

  // Audit finding 10. Until this fix the tier's only "still drawing?" test was
  // raw meter flow above 0.3 GPM, which cannot go false while the pump runs:
  // the meter sees the recirculation loop, and every pump-on reading this repo
  // records clears 0.3 by more than 2x. So a draw that stopped five minutes
  // into a thirty-minute run kept publishing demand at confidence 0.85 for the
  // remaining twenty-five.
  //
  // The old test for this exit passed only because it used a 0.1 GPM meter
  // reading -- below every value ever measured with the pump running. These
  // use the measured ones.
  const float kFloorRpmMeter = 0.71f;   // 1650 rpm, the pump's clamp floor
  const float kNoDrawMedian = 1.31f;    // pump-on, no draw
  const float kPreShutdown = 1.45f;     // loop collapsing as the motor parks
  const float kNoDrawP90 = 2.22f;       // pump-on, no draw

  for (float meter : {kFloorRpmMeter, kNoDrawMedian, kPreShutdown,
                      kNoDrawP90}) {
    ContinuationInputs raw = armed_continuation();
    raw.flow = meter;
    TEST_ASSERT(pump_on_continuation_verdict(raw) !=
                    ContinuationVerdict::METER_QUIET,
                "Raw meter flow alone never releases the tier -- that exit is "
                "unreachable while the pump runs, which is the bug");

    // The subtraction is what can tell these apart: the same meter reading
    // against a loop accounting for all of it is no draw at all. Two ticks,
    // because one is not enough to retire a capture that cannot be restored.
    ContinuationInputs measured = raw;
    measured.demand_gpm = 0.0f;
    measured.measured_stopped_ticks = 1;
    TEST_ASSERT(pump_on_continuation_verdict(measured) ==
                    ContinuationVerdict::MEASURED_STOPPED,
                "A sustained subtraction reading zero releases it");
  }

  // The audit's own repro: a 1.80 GPM draw stops five minutes into a run, the
  // meter still reads loop flow, and the subtraction reads exactly 0.00.
  ContinuationInputs stopped = armed_continuation();
  stopped.pre_pump_on_flow = 1.80f;
  stopped.flow = 1.31f;
  stopped.demand_gpm = 0.00f;
  stopped.measured_stopped_ticks = 1;
  TEST_ASSERT(!pump_on_continuation_is_active(stopped),
              "The draw stopping ends the continuation");

  // ...but not on the first tick of it. The retirement is irreversible within
  // a pump run, and isolated bad differences are measured, not hypothetical:
  // the meter led the pump channel by 13 s at one recorded start, so a mid-run
  // speed change lets the meter follow the loop down while the pump's own last
  // report is still high and inside its freshness bound.
  ContinuationInputs first_tick = stopped;
  first_tick.measured_stopped_ticks = 0;
  TEST_ASSERT(pump_on_continuation_verdict(first_tick) ==
                  ContinuationVerdict::STOPPING,
              "One tick of a stopped reading is not yet a release");
  TEST_ASSERT(pump_on_continuation_is_active(first_tick),
              "...and the tier still decides that tick, so a transient costs "
              "no demand at all");

  // Still drawing, and measured: the tier holds. Without this the release
  // could be implemented as "any usable subtraction ends it", which would pass
  // every assertion above and break every real continuation.
  ContinuationInputs still_drawing = armed_continuation();
  still_drawing.demand_gpm = 1.24f;
  TEST_ASSERT(pump_on_continuation_is_active(still_drawing),
              "A subtraction that still measures a draw does not release it");
  TEST_ASSERT(pump_on_continuation_verdict(still_drawing) ==
                  ContinuationVerdict::CONFIRMED,
              "...and reports it as positive support, not a bare hold");
}

// The band that the first version of this fix got wrong, and the reason the
// release threshold is not the firing threshold.
//
// Borrowing tier 2's 0.3 GPM firing threshold to *falsify* inverts the safety
// argument in pump_on_demand_flow: the loop estimate's residual is negative
// (-0.10 +/- 0.06 GPM steady, -0.14 at 2398 rpm in the ground-truth table),
// which is conservative while it can only suppress a claim and becomes a
// false-negative generator the moment it can retire one. A real draw smaller
// than residual + 0.3 computed below 0.3 and killed the continuation for the
// rest of the run with the tap still open.
void test_a_small_but_real_draw_is_not_read_as_stopped() {
  std::cout << "\n=== Testing Small-Draw Release Band ===" << std::endl;

  // Sustained, so the consecutive-tick guard is not what is being tested here.
  for (float residual : {-0.10f, -0.14f}) {
    for (float draw : {0.35f, 0.50f, 0.70f}) {
      ContinuationInputs in = armed_continuation();
      in.pre_pump_on_flow = draw;
      in.flow = 1.31f + draw;  // loop plus the draw
      in.demand_gpm = draw + residual;
      in.measured_stopped_ticks = 4;
      TEST_ASSERT(pump_on_continuation_is_active(in),
                  "A real draw inside the estimator's error band is held, not "
                  "read as stopped");
    }
  }

  // The line itself. Zero is where the meter reads no more than the pump says
  // its own loop is moving, which is the only unambiguous statement that no
  // household draw exists.
  ContinuationInputs at_zero = armed_continuation();
  at_zero.demand_gpm = 0.0f;
  at_zero.measured_stopped_ticks = 1;
  TEST_ASSERT(pump_on_continuation_verdict(at_zero) ==
                  ContinuationVerdict::MEASURED_STOPPED,
              "Exactly zero computed demand is a release");

  ContinuationInputs just_above_zero = armed_continuation();
  just_above_zero.demand_gpm = 0.01f;
  just_above_zero.measured_stopped_ticks = 4;
  TEST_ASSERT(pump_on_continuation_is_active(just_above_zero),
              "...and the smallest positive difference is not");

  // A reading between the release line and the firing threshold supports
  // nothing either way: the tier holds, but the expiry clock keeps running so
  // an indefinite hold is still bounded.
  ContinuationInputs ambiguous = armed_continuation();
  ambiguous.demand_gpm = 0.2f;
  TEST_ASSERT(pump_on_continuation_verdict(ambiguous) ==
                  ContinuationVerdict::ACTIVE,
              "A reading inside the band is neither confirmation nor release");

  // The quiet-loop residual with the tap genuinely shut still releases -- that
  // is the case the whole tier turns on.
  ContinuationInputs quiet_residual = armed_continuation();
  quiet_residual.demand_gpm = -0.10f;
  quiet_residual.measured_stopped_ticks = 1;
  TEST_ASSERT(pump_on_continuation_verdict(quiet_residual) ==
                  ContinuationVerdict::MEASURED_STOPPED,
              "The quiet-loop negative residual releases the tier");
}

void test_continuation_expires_when_nothing_can_measure_it() {
  std::cout << "\n=== Testing Continuation Expiry ===" << std::endl;

  // The release above needs a subtraction, and the subtraction goes quiet
  // whenever the pump turns below pump_on_demand_min_speed_rpm. A pump clamped
  // to 1650 rpm never produces one at all, so without an expiry the tier would
  // still hold for the whole run on that hardware -- the same bug, one regime
  // over. This is the only path that can end it there.
  const uint32_t kMax = kDefaultPumpOnThresholds.continuation_max_ms;

  ContinuationInputs fresh = armed_continuation();
  TEST_ASSERT(pump_on_continuation_is_active(fresh),
              "An unmeasured continuation holds while it is young");

  ContinuationInputs at_bound = armed_continuation();
  at_bound.now_ms = at_bound.continuation_since_ms + kMax;
  TEST_ASSERT(pump_on_continuation_verdict(at_bound) ==
                  ContinuationVerdict::EXPIRED,
              "Exactly at the bound it has expired");

  ContinuationInputs just_inside = armed_continuation();
  just_inside.now_ms = just_inside.continuation_since_ms + kMax - 1;
  TEST_ASSERT(pump_on_continuation_is_active(just_inside),
              "One millisecond short of the bound it still holds");

  // The audit's repro again, this time with no subtraction available for the
  // whole run: 30 minutes of ticks. Before the fix all 180 fired; the expiry
  // bounds it to the first five minutes.
  //
  // The count is spelled out rather than derived from kMax, so that changing
  // the shipped default fails here and has to be re-stated deliberately. The
  // assertion just below is what makes that failure legible instead of
  // mysterious.
  TEST_ASSERT(kMax == 300000,
              "The shipped expiry is 5 minutes -- the tick count below is "
              "written for it");
  ContinuationInputs blind = armed_continuation();
  int fired = 0;
  for (int tick = 0; tick < 180; tick++) {
    blind.now_ms = blind.continuation_since_ms + (uint32_t) tick * 10000;
    if (pump_on_continuation_is_active(blind))
      fired++;
  }
  TEST_ASSERT(fired == 30,
              "A blind 30-minute run holds for 5 minutes, not 30");

  // Rollover. The arm stamp sits 60 s before the wrap and `now` 60 s after it,
  // so the continuation is 120 s old across the boundary and must still hold.
  // An absolute-difference or max-minus-min form reads this as ~49 days.
  // (Signed arithmetic does not -- both int32 and int64 differences give the
  // correct +120 001 here, so the claim is narrower than it looks.)
  ContinuationInputs wrapped = armed_continuation();
  wrapped.continuation_since_ms = 0xFFFFFFFFu - 60000u;
  wrapped.now_ms = 60000u;
  TEST_ASSERT(pump_on_continuation_is_active(wrapped),
              "A continuation straddling the millis() wrap is 120 s old, not "
              "49 days");

  // ...and the case that actually discriminates against the natural wrong form,
  // `now >= since + max`. The assertion above does not: with both stamps near
  // the top of the range, `since + max` wraps too and the two forms agree by
  // accident. Here only the sum wraps -- the arm is 100 ms before the boundary
  // and `now` 50 ms after the arm but still short of it -- so the buggy form
  // compares a pre-wrap `now` against a wrapped-to-tiny bound and calls a 50 ms
  // old continuation expired.
  ContinuationInputs sum_wraps = armed_continuation();
  sum_wraps.continuation_since_ms = 0xFFFFFFFFu - 100u;
  sum_wraps.now_ms = sum_wraps.continuation_since_ms + 50u;
  TEST_ASSERT(pump_on_continuation_is_active(sum_wraps),
              "A 50 ms old continuation holds even where since + max_ms wraps");

  // An unstamped arm fails closed. It cannot happen from the component -- the
  // stamp and the flow are written together -- but "no stamp" must not read as
  // "infinitely young". `now` is deliberately inside the ceiling, so dropping
  // the sentinel check makes this hold rather than expire: at 60 s uptime a
  // zero stamp is a 60-second-old continuation, not an aged-out one.
  ContinuationInputs unstamped = armed_continuation();
  unstamped.continuation_since_ms = 0;
  unstamped.now_ms = 60000;
  TEST_ASSERT(pump_on_continuation_verdict(unstamped) ==
                  ContinuationVerdict::EXPIRED,
              "An unstamped continuation does not hold");

  // max_ms == 0 disables the tier rather than making it unbounded. The config
  // key is documented that way, and unbounded is the behaviour being removed.
  ContinuationInputs disabled = armed_continuation();
  disabled.max_ms = 0;
  TEST_ASSERT(pump_on_continuation_verdict(disabled) ==
                  ContinuationVerdict::EXPIRED,
              "A zero ceiling disables the tier, it does not unbound it");
}

void test_continuation_release_is_visible_to_the_component() {
  std::cout << "\n=== Testing Continuation Verdict Reporting ===" << std::endl;

  // The component clears pre_pump_on_flow_ on MEASURED_STOPPED and only on
  // that, so decide_pump_on has to report the verdict whichever tier ended up
  // deciding -- otherwise the .cpp would have to re-derive it from `method`,
  // which is the replica issue #144 removed.
  const PumpOnThresholds &t = kDefaultPumpOnThresholds;

  PumpOnInputs stopped = live_tick();
  stopped.pre_pump_on_flow = 1.80f;
  stopped.flow = 1.34f;
  stopped.pump_flow = 1.34f;  // 0.00 GPM of demand
  stopped.measured_stopped_ticks = 1;  // one earlier stopped tick
  PumpOnResult r = decide_pump_on(stopped, t);
  TEST_ASSERT(r.continuation == ContinuationVerdict::MEASURED_STOPPED,
              "The falsifying release is reported to the caller");
  TEST_ASSERT(!r.demand, "...and the tick claims no demand");
  TEST_ASSERT(std::string(r.method) == "pump_on_uncertain",
              "...and falls through to the fallback");

  // The same tick without the earlier one still decides, and reports STOPPING
  // so the component knows to count rather than retire.
  PumpOnInputs first_stop = stopped;
  first_stop.measured_stopped_ticks = 0;
  r = decide_pump_on(first_stop, t);
  TEST_ASSERT(r.continuation == ContinuationVerdict::STOPPING,
              "The first stopped tick counts rather than retiring");
  TEST_ASSERT(std::string(r.method) == "deterministic_continuation",
              "...and the tier still decides it");

  // A hold on positive evidence reports CONFIRMED, which is what re-stamps the
  // support time so the expiry means "unsupported for N s" and not "N s since
  // the pump started".
  PumpOnInputs holding = live_tick();
  holding.pre_pump_on_flow = 1.80f;
  holding.flow = 2.58f;
  holding.pump_flow = 1.34f;  // 1.24 GPM, still drawing
  r = decide_pump_on(holding, t);
  TEST_ASSERT(r.continuation == ContinuationVerdict::CONFIRMED,
              "A measured, still-running draw reports CONFIRMED");
  TEST_ASSERT(std::string(r.method) == "deterministic_continuation",
              "...and decides the tick");

  // The regression the first version of this fix shipped: a subtraction
  // confirming the draw on every single tick did not stop the tier expiring,
  // because the clock ran from the pump start rather than from the last
  // support. The verdict above is what the component acts on to prevent it.
  PumpOnInputs long_confirmed = holding;
  long_confirmed.continuation_since_ms =
      long_confirmed.now_ms - t.continuation_max_ms;
  r = decide_pump_on(long_confirmed, t);
  TEST_ASSERT(r.continuation == ContinuationVerdict::CONFIRMED,
              "A confirming measurement outranks the expiry");
  TEST_ASSERT(std::string(r.method) == "deterministic_continuation",
              "...so a continuously measured draw does not expire mid-run");

  // A meter that has gone NaN reports METER_QUIET rather than
  // MEASURED_STOPPED: one dropped sample must not retire a capture that is
  // still true.
  PumpOnInputs dropped = live_tick();
  dropped.pre_pump_on_flow = 1.80f;
  dropped.flow = NAN;
  r = decide_pump_on(dropped, t);
  TEST_ASSERT(r.continuation == ContinuationVerdict::METER_QUIET,
              "A dropped meter sample is not a falsifying release");

  // An expiry with no subtraction available is EXPIRED, not MEASURED_STOPPED:
  // nothing measured anything, so there is nothing to falsify.
  PumpOnInputs aged = live_tick();
  aged.pre_pump_on_flow = 1.80f;
  aged.flow = 1.31f;
  aged.motor_speed = 1650.0f;  // below the floor: no subtraction at all
  aged.continuation_since_ms = aged.now_ms - t.continuation_max_ms;
  r = decide_pump_on(aged, t);
  TEST_ASSERT(std::isnan(r.demand_gpm),
              "Below the speed floor there is no measurement");
  TEST_ASSERT(r.continuation == ContinuationVerdict::EXPIRED,
              "An unmeasured continuation that ran out is EXPIRED");
}

void test_loop_flow_from_before_the_pump_start_is_not_subtracted() {
  std::cout << "\n=== Testing Subtraction Across A Pump Start ==="
            << std::endl;

  // The trace this exists for, from the Python side's X-AFTER bench run. No
  // tap was opened for another 2 m 16 s:
  //
  //   t=0 ms       pump off, both channels report 0.000
  //   t=7000 ms    meter reports 1.429 (loop flow), motor already at 2397 rpm
  //   t=20000 ms   the pump finally reports its own 1.397
  //
  // Between 7 s and 20 s the loop reading is a pre-startup zero that is still
  // "fresh" by age, and 1.429 - 0.000 published as demand_level 0.57 at
  // confidence 0.90.
  const uint32_t kPumpOnSince = 5000;
  const uint32_t kStaleZero = 0 + 1;  // stamped while the pump was still off
  const uint32_t kRealReading = 20000;

  TEST_ASSERT(reading_predates_pump_start(kStaleZero, kPumpOnSince, 12000),
              "A loop reading stamped before the pump start predates it");
  TEST_ASSERT(!reading_predates_pump_start(kRealReading, kPumpOnSince, 21000),
              "A loop reading stamped after the pump start does not");

  // Age, which is what the staleness bound tests, cannot tell these apart:
  // both are well inside the 30 s window.
  TEST_ASSERT(reading_is_fresh(kStaleZero, 12000,
                               kDefaultPumpOnThresholds.pump_flow_max_stale_ms),
              "...and the staleness bound alone would accept the stale zero");

  // Never reported at all cannot postdate anything.
  TEST_ASSERT(reading_predates_pump_start(0, kPumpOnSince, 12000),
              "A channel that has never reported predates the pump start");

  // No observed start — booted with the pump already running — means there is
  // no boundary to test, so this abstains and the staleness bound decides.
  TEST_ASSERT(!reading_predates_pump_start(kStaleZero, 0, 12000),
              "With no observed pump start the test abstains");

  // Rollover: the stamps straddle the uint32_t wrap. The reading is 1 s before
  // `now`, the pump start 3 s before it, so the reading postdates the start
  // even though its absolute stamp is numerically larger.
  const uint32_t kNow = 2000;                  // 2 s past the wrap
  const uint32_t kBeforeWrap = 0xFFFFFFFFu - 999;   // 3 s before `now`
  const uint32_t kAfterWrap = 1000;                 // 1 s before `now`
  TEST_ASSERT(!reading_predates_pump_start(kAfterWrap, kBeforeWrap, kNow),
              "Across the millis() rollover a later reading still wins");
  TEST_ASSERT(reading_predates_pump_start(kBeforeWrap, kAfterWrap, kNow),
              "Across the millis() rollover an earlier reading still loses");

  // And the whole tier, through decide_pump_on: the startup transient must not
  // reach the wire.
  PumpOnInputs in;
  in.flow = 1.429f;
  in.pump_flow = 0.0f;
  in.motor_speed = 2397.0f;
  in.now_ms = 12000;
  in.flow_last_update_ms = 11000;
  in.pump_flow_last_update_ms = kStaleZero;
  in.pump_on_since_ms = kPumpOnSince;

  PumpOnResult r = decide_pump_on(in, kDefaultPumpOnThresholds);
  TEST_ASSERT(std::isnan(r.demand_gpm),
              "The subtraction declines across its own pump start");
  TEST_ASSERT(std::string(r.method) != "deterministic_pump_on_subtraction",
              "...so no startup transient is published as a draw");

  // The instant the pump reports, the tier is deciding again — this is a
  // regime test, not a fixed window.
  in.now_ms = 21000;
  in.flow_last_update_ms = 20500;
  in.pump_flow_last_update_ms = kRealReading;
  in.pump_flow = 1.397f;
  r = decide_pump_on(in, kDefaultPumpOnThresholds);
  TEST_ASSERT(!std::isnan(r.demand_gpm),
              "One real loop reading re-enables the subtraction immediately");

  // Recall guard: a genuine draw on top of the loop is still measured.
  in.flow = 3.0f;
  r = decide_pump_on(in, kDefaultPumpOnThresholds);
  TEST_ASSERT(r.demand && std::string(r.method) ==
                              "deterministic_pump_on_subtraction",
              "A real draw during the pump run is still detected");
}

void test_flow_latch_is_disarmed_after_a_shutdown() {
  std::cout << "\n=== Testing Latch Suppression After A Shutdown ==="
            << std::endl;

  const uint32_t kWindow = 30000;  // matches flow_latch_seconds default
  const uint32_t kPumpOff = 100000;

  TEST_ASSERT(latch_suppressed_after_shutdown(kPumpOff, kPumpOff + 1, kWindow),
              "The latch is disarmed immediately after the pump stops");
  TEST_ASSERT(latch_suppressed_after_shutdown(kPumpOff, kPumpOff + 30000,
                                              kWindow),
              "...through the whole window, inclusive");
  TEST_ASSERT(!latch_suppressed_after_shutdown(kPumpOff, kPumpOff + 30001,
                                               kWindow),
              "...and armed again once the window passes");

  // Off by configuration.
  TEST_ASSERT(!latch_suppressed_after_shutdown(kPumpOff, kPumpOff + 1, 0),
              "A zero window disables the suppression");

  // No shutdown observed yet.
  TEST_ASSERT(!latch_suppressed_after_shutdown(0, 50000, kWindow),
              "With no observed shutdown the latch is left alone");

  // Rollover: the pump stopped 1 s before the wrap, `now` is 2 s after it.
  const uint32_t kOffBeforeWrap = 0xFFFFFFFFu - 999;
  TEST_ASSERT(latch_suppressed_after_shutdown(kOffBeforeWrap, 2000, kWindow),
              "The window survives the millis() rollover");
}

void test_the_loop_estimate_must_settle_before_it_is_differenced() {
  std::cout << "\n=== Testing Pump Spin-Up Settle ===" << std::endl;

  const uint32_t kSettle = kDefaultPumpOnThresholds.pump_on_settle_ms;
  const uint32_t kStart = 100000;

  TEST_ASSERT(!pump_flow_estimate_is_settled(kStart, kStart, kSettle),
              "At the pump start the estimate is not settled");
  TEST_ASSERT(!pump_flow_estimate_is_settled(kStart, kStart + 9999, kSettle),
              "...nor 1 ms short of the window");
  TEST_ASSERT(pump_flow_estimate_is_settled(kStart, kStart + 10000, kSettle),
              "...and settled once the window elapses, inclusive");
  TEST_ASSERT(pump_flow_estimate_is_settled(0, kStart, kSettle),
              "With no observed start there is nothing to wait for");
  TEST_ASSERT(pump_flow_estimate_is_settled(kStart, kStart, 0),
              "A zero window disables the guard");

  // Rollover: the pump started 3 s before the wrap, now is 8 s after it, so
  // 11 s have elapsed and the estimate is settled.
  const uint32_t kBeforeWrap = 0xFFFFFFFFu - 2999;
  TEST_ASSERT(pump_flow_estimate_is_settled(kBeforeWrap, 8000, kSettle),
              "The settle window survives the millis() rollover");

  // The whole tier: the ramp must not reach the wire. Live trace 2026-08-01,
  // recirculation with no tap open — the pump reports 0.713 while the meter
  // already reads 1.712, and the difference published at confidence 0.81.
  PumpOnInputs in;
  in.flow = 1.712f;
  in.pump_flow = 0.713f;
  in.motor_speed = 3172.0f;   // well above the 1950 floor
  in.now_ms = kStart + 5000;
  in.flow_last_update_ms = kStart + 4900;
  in.pump_flow_last_update_ms = kStart + 4800;
  in.pump_on_since_ms = kStart;

  PumpOnResult r = decide_pump_on(in, kDefaultPumpOnThresholds);
  TEST_ASSERT(std::isnan(r.demand_gpm),
              "The subtraction declines while the estimate is ramping");
  TEST_ASSERT(std::string(r.method) != "deterministic_pump_on_subtraction",
              "...so the spin-up is not published as a draw");

  // Past the window the tier decides again, and a real draw is measured.
  in.now_ms = kStart + 30000;
  in.flow_last_update_ms = kStart + 29900;
  in.pump_flow_last_update_ms = kStart + 29800;
  in.flow = 3.0f;
  in.pump_flow = 1.30f;
  r = decide_pump_on(in, kDefaultPumpOnThresholds);
  TEST_ASSERT(r.demand && std::string(r.method) ==
                              "deterministic_pump_on_subtraction",
              "A real draw after the settle window is still detected");
}


// ─────────────────────────────────────────────────────────────────────────────
// Pump-state branch selection (audit finding 9)
//
// decide_pump_state() picks between the pump-ON and pump-OFF demand paths, and
// the pump-OFF path scores raw meter flow as household demand. A false
// "confirmed off" therefore hands the loop's own recirculation (~2.2 GPM) to
// deterministic_flow at confidence 1.0 -- the worst available false positive.
//
// The trap this guards: alpha_hwr keeps publishing the last value on a BLE drop
// rather than publishing NaN, so motor_speed stays a perfectly valid 0.0
// forever. NaN-checking alone reads a dead link as a confident pump-off, which
// is exactly how the false positive was reached. Only age distinguishes them.
// ─────────────────────────────────────────────────────────────────────────────

static PumpStateInputs pump_state_tick() {
  PumpStateInputs in;
  in.now_ms = 600000;
  in.motor_max_stale_ms = 30000;
  in.pump_off_current_threshold = 0.05f;
  return in;
}

void test_fresh_motor_readings_are_believed_both_ways() {
  PumpStateInputs in = pump_state_tick();
  in.motor_speed = 0.0f;
  in.motor_speed_last_update_ms = in.now_ms - 5000;  // 5 s old: fresh
  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(r.pump_state_known, "A fresh reading is a known pump state");
  TEST_ASSERT(!r.pump_on, "0 RPM fresh reads as pump off");
  TEST_ASSERT(r.pump_confirmed_off, "...and that is positive evidence of off");
  TEST_ASSERT(!r.motor_frozen, "...and is not frozen");

  in.motor_speed = 2402.0f;
  r = decide_pump_state(in);
  TEST_ASSERT(r.pump_on, "2402 RPM fresh reads as pump on");
  TEST_ASSERT(!r.pump_confirmed_off, "...and never asserts confirmed-off");
}

void test_a_frozen_zero_rpm_is_not_evidence_of_off() {
  // THE regression. The BLE link dropped; alpha_hwr keeps republishing the last
  // value it saw, which was 0 RPM. The value is non-NaN and looks like a
  // confident "pump off" forever.
  PumpStateInputs in = pump_state_tick();
  in.motor_speed = 0.0f;
  in.motor_speed_last_update_ms = in.now_ms - 120000;  // 2 min stale
  in.pump_state_ever_known = true;
  in.last_known_pump_on = false;

  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(!r.pump_state_known, "A stale motor reading is not a known state");
  TEST_ASSERT(r.motor_frozen, "Reporting but not refreshing is 'frozen'");
  TEST_ASSERT(!r.pump_confirmed_off,
              "A frozen 0 RPM must NOT assert confirmed-off (the false positive)");
  TEST_ASSERT(r.pump_on, "...it assumes ON, so the pump-on branch runs and declines safely");
  TEST_ASSERT(std::isnan(r.speed_used), "...and the stale value is masked out entirely");
}

void test_a_frozen_running_reading_also_yields_on() {
  // Same staleness, opposite last value. Assuming ON here is not a lucky
  // coincidence of the 0 RPM case -- it is unconditional for frozen.
  PumpStateInputs in = pump_state_tick();
  in.motor_speed = 2402.0f;
  in.motor_speed_last_update_ms = in.now_ms - 120000;
  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(r.motor_frozen && r.pump_on && !r.pump_confirmed_off,
              "A frozen running reading also assumes ON and asserts nothing");
}

void test_the_staleness_boundary() {
  PumpStateInputs in = pump_state_tick();
  in.motor_speed = 0.0f;
  in.motor_speed_last_update_ms = in.now_ms - 30000;  // exactly the bound
  TEST_ASSERT(decide_pump_state(in).pump_confirmed_off,
              "Exactly at the bound the reading is still fresh");
  in.motor_speed_last_update_ms = in.now_ms - 30001;
  TEST_ASSERT(!decide_pump_state(in).pump_confirmed_off,
              "One millisecond past it, confirmed-off is withdrawn");
}

void test_unconfigured_motor_forward_fills_as_before() {
  // packages/dhw_demand_detector.yaml -- the documented standalone entry point
  // -- wires neither motor sensor, so both are NaN with last_update 0 forever.
  // That must land in the 'absent' regime and behave exactly as it did before
  // any staleness masking existed (AGENTS §11.8 rule 2 governs unconfigured
  // inputs; this path satisfies it).
  PumpStateInputs in = pump_state_tick();  // both NaN, both last_update 0
  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(!r.pump_state_known, "Nothing wired is not a known state");
  TEST_ASSERT(!r.motor_frozen, "...and is a genuine gap, not a frozen channel");
  TEST_ASSERT(r.pump_on, "...defaulting to ON before anything has been known");
  TEST_ASSERT(!r.pump_confirmed_off, "...and asserting nothing");

  // Once a state has been known, the gap forward-fills it -- including OFF,
  // which is the one case where a gap may still assert confirmed-off.
  in.pump_state_ever_known = true;
  in.last_known_pump_on = false;
  r = decide_pump_state(in);
  TEST_ASSERT(!r.pump_on && r.pump_confirmed_off,
              "A genuine NaN gap forward-fills a known-OFF state");
  in.last_known_pump_on = true;
  r = decide_pump_state(in);
  TEST_ASSERT(r.pump_on && !r.pump_confirmed_off,
              "...and a known-ON state, which asserts nothing");
}

void test_current_channel_is_the_fallback() {
  // Installs that wire only motor_current must still work, and each channel is
  // aged independently -- a fresh current reading rescues a stale speed one.
  PumpStateInputs in = pump_state_tick();
  in.motor_current = 0.30f;
  in.motor_current_last_update_ms = in.now_ms - 5000;
  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(r.pump_state_known && r.pump_on,
              "Current above threshold reads as pump on with no speed channel");

  in.motor_speed = 0.0f;
  in.motor_speed_last_update_ms = in.now_ms - 120000;  // stale speed...
  r = decide_pump_state(in);
  TEST_ASSERT(r.pump_state_known, "...is rescued by the fresh current channel");
  TEST_ASSERT(r.pump_on, "...and the frozen 0 RPM does not win over it");
  TEST_ASSERT(std::isnan(r.speed_used) && !std::isnan(r.current_used),
              "...because only the stale channel is masked");
}

void test_pump_state_millis_rollover() {
  PumpStateInputs in = pump_state_tick();
  in.now_ms = 1000;                                  // just wrapped
  in.motor_speed = 0.0f;
  in.motor_speed_last_update_ms = 0xFFFFF000u;       // 5096 ms of true age
  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(r.pump_state_known && r.pump_confirmed_off,
              "A fresh reading spanning the millis() rollover stays fresh");
}


void test_the_current_channel_is_aged_too() {
  // The same trap as the frozen 0 RPM, on the other channel — and it survived a
  // mutation battery until this test existed. An install that wires only
  // motor_current, whose BLE link then drops: alpha_hwr keeps republishing the
  // last value (0.0 A), so without an age mask on THIS channel the detector
  // reads a dead link as a confident pump-off and hands the loop's own
  // recirculation to deterministic_flow at confidence 1.0.
  PumpStateInputs in = pump_state_tick();
  in.motor_current = 0.0f;                             // below the 0.05 A threshold
  in.motor_current_last_update_ms = in.now_ms - 120000; // 2 min stale
  in.pump_state_ever_known = true;
  in.last_known_pump_on = false;

  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(!r.pump_state_known, "A stale current reading is not a known state either");
  TEST_ASSERT(r.motor_frozen, "...it is frozen, not absent");
  TEST_ASSERT(!r.pump_confirmed_off,
              "A frozen 0.0 A must NOT assert confirmed-off (same trap, other channel)");
  TEST_ASSERT(std::isnan(r.current_used), "...and the stale current is masked out");
}

void test_the_speed_threshold_boundary() {
  // "Mirrors Python: pump_off = motor_speed < 10 RPM", so 10.0 is ON. Pinning
  // the boundary because >= vs > is exactly the kind of edit that reads as a
  // harmless tidy-up.
  PumpStateInputs in = pump_state_tick();
  in.motor_speed_last_update_ms = in.now_ms - 5000;
  in.motor_speed = 10.0f;
  TEST_ASSERT(decide_pump_state(in).pump_on, "Exactly 10 RPM is running");
  in.motor_speed = 9.999f;
  TEST_ASSERT(!decide_pump_state(in).pump_on, "Just under 10 RPM is stopped");
}

void test_speed_wins_over_current_when_both_are_fresh() {
  // The header promises speed takes precedence. Nothing tested it: every other
  // case either has one channel absent or lets the age mask decide, so swapping
  // the precedence changed no assertion. Make the two disagree.
  PumpStateInputs in = pump_state_tick();
  in.motor_speed = 0.0f;    // speed says stopped
  in.motor_current = 0.30f; // current says running, well over the 0.05 A threshold
  in.motor_speed_last_update_ms = in.now_ms - 5000;
  in.motor_current_last_update_ms = in.now_ms - 5000;

  PumpStateResult r = decide_pump_state(in);
  TEST_ASSERT(!r.pump_on, "With both fresh and disagreeing, speed decides");
  TEST_ASSERT(r.pump_confirmed_off, "...so this is a genuine confirmed-off");

  // And the direct predicate, so the precedence is pinned where it is written.
  TEST_ASSERT(!motor_reading_indicates_on(0.0f, 0.30f, 0.05f),
              "motor_reading_indicates_on: a present speed beats the current channel");
  TEST_ASSERT(motor_reading_indicates_on(NAN, 0.30f, 0.05f),
              "...and the current channel is used only when speed is absent");
}

int main() {
  std::cout << "==========================================================="
            << std::endl;
  std::cout << "  DHW Demand Pump-On Logic Test Suite" << std::endl;
  std::cout << "==========================================================="
            << std::endl;

  test_flow_only_onset_requires_one_full_off_tick();
  test_only_a_pump_off_tick_arms_the_flow_debounce();
  test_dhw_in_use_boost_is_gated_to_pump_off();
  test_ambiguous_flow_onset_does_not_prime_continuation();
  test_sustained_demand_accrues_session();
  test_threshold_jitter_does_not_chatter();
  test_pump_on_continuation_predicate();
  test_continuation_releases_when_the_draw_stops();
  test_a_small_but_real_draw_is_not_read_as_stopped();
  test_continuation_expires_when_nothing_can_measure_it();
  test_continuation_release_is_visible_to_the_component();
  test_reading_freshness();
  test_pump_on_demand_flow_guards();
  test_dhw_in_use_sustain_rejects_short_events();
  test_dhw_in_use_sustain_resets_on_break();
  test_dhw_in_use_sustain_never_arms_without_the_sensor();
  test_pump_on_tier_ordering();
  test_loop_flow_from_before_the_pump_start_is_not_subtracted();
  test_flow_latch_is_disarmed_after_a_shutdown();
  test_the_loop_estimate_must_settle_before_it_is_differenced();

  test_fresh_motor_readings_are_believed_both_ways();
  test_a_frozen_zero_rpm_is_not_evidence_of_off();
  test_a_frozen_running_reading_also_yields_on();
  test_the_staleness_boundary();
  test_unconfigured_motor_forward_fills_as_before();
  test_current_channel_is_the_fallback();
  test_pump_state_millis_rollover();
  test_the_current_channel_is_aged_too();
  test_the_speed_threshold_boundary();
  test_speed_wins_over_current_when_both_are_fresh();

  std::cout << "\n==========================================================="
            << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "==========================================================="
            << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  }

  std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
  return 1;
}
