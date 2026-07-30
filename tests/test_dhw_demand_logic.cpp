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
using esphome::dhw_demand::pump_on_continuation_is_active;
using esphome::dhw_demand::pump_on_demand_flow;
using esphome::dhw_demand::PumpOnInputs;
using esphome::dhw_demand::PumpOnResult;
using esphome::dhw_demand::PumpOnThresholds;
using esphome::dhw_demand::reading_is_fresh;
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
                               kDefaultPumpOnThresholds.droplet_max_stale_ms),
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

  // Args: (pre_pump_on_flow, flow, flow_threshold).
  TEST_ASSERT(pump_on_continuation_is_active(1.5f, 1.5f, 0.3f),
              "Flow above threshold on both sides of the pump start continues");
  TEST_ASSERT(!pump_on_continuation_is_active(NAN, 1.5f, 0.3f),
              "No captured pre-pump flow means no continuation");
  TEST_ASSERT(!pump_on_continuation_is_active(0.1f, 1.5f, 0.3f),
              "Pre-pump flow at or below threshold is not demand evidence");
  TEST_ASSERT(!pump_on_continuation_is_active(1.5f, NAN, 0.3f),
              "A NaN current reading does not continue a draw");
  TEST_ASSERT(!pump_on_continuation_is_active(1.5f, 0.1f, 0.3f),
              "The draw stopping ends the continuation");
  TEST_ASSERT(!pump_on_continuation_is_active(0.3f, 0.3f, 0.3f),
              "The comparison is strictly greater on both sides");
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
  test_reading_freshness();
  test_pump_on_demand_flow_guards();
  test_pump_on_tier_ordering();

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
