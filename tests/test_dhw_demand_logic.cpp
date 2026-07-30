/**
 * Unit tests for DHW demand pump-on hydraulic voting.
 *
 * These tests call the production vote logic directly (dhw_demand_logic.h)
 * to verify that recirculation pump startup transients do not falsely
 * declare DHW demand while genuine open-loop signals remain detectable.
 */

#include "../components/dhw_demand/dhw_demand_logic.h"
#include <cmath>
#include <iostream>
#include <string>

using esphome::dhw_demand::apply_dhw_in_use_boost;
using esphome::dhw_demand::compute_pump_on_confidence;
using esphome::dhw_demand::DemandHold;
using esphome::dhw_demand::kDefaultPumpOnVoteThresholds;
using esphome::dhw_demand::prev_tick_confirms_flow_onset;
using esphome::dhw_demand::pump_off_flow_onset_is_confirmed;
using esphome::dhw_demand::pump_on_demand_level;
using esphome::dhw_demand::PumpOnVotes;
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

static float deterministic_pump_on_conf(float inlet_deriv, float inlet_psi,
                                        float pump_flow, float current_deriv,
                                        float power_deriv, float head_rate_peak,
                                        bool suppress_transient_votes) {
  const char *method = nullptr;
  return compute_pump_on_confidence(inlet_deriv, inlet_psi, pump_flow,
                                    current_deriv, power_deriv, head_rate_peak,
                                    suppress_transient_votes,
                                    kDefaultPumpOnVoteThresholds, &method);
}

void test_startup_transients_are_suppressed() {
  std::cout << "\n=== Testing Startup Transient Suppression ===" << std::endl;

  // Mirrors the false-positive trace from Home Assistant:
  // inlet pressure jumps from 0 -> ~13 PSI and current from 0 -> ~0.155 A
  // when the recirculation pump starts, but there is no low-pressure or
  // flow-collapse evidence of an open-loop DHW draw.
  float confidence =
      deterministic_pump_on_conf(1.32f, 13.2f, 9.56f, 0.015f, 1.4f, 2.94f, true);

  TEST_ASSERT_NEAR(confidence, 0.0f, 0.0001f,
                   "Startup-only transients do not declare demand");
}

void test_startup_transients_still_trigger_without_guard() {
  std::cout << "\n=== Testing Unsuppressed Startup Transients ===" << std::endl;

  float confidence = deterministic_pump_on_conf(1.32f, 13.2f, 9.56f, 0.015f,
                                                1.4f, 2.94f, false);

  // Three votes without the guard: inlet-pressure transient, current spike,
  // and the head-rate vote that rides along once something else has voted.
  TEST_ASSERT_NEAR(confidence, 0.80f, 0.0001f,
                   "Pressure + current startup spikes would have caused a false positive");
}

void test_startup_guard_keeps_open_loop_signals() {
  std::cout << "\n=== Testing Open-Loop Signals During Startup Guard ==="
            << std::endl;

  float low_pressure_conf = deterministic_pump_on_conf(
      1.32f, 2.5f, 9.56f, 0.015f, 1.4f, 2.94f, true);
  float flow_collapse_conf = deterministic_pump_on_conf(
      1.32f, 13.2f, 0.05f, 0.015f, 1.4f, 2.94f, true);

  TEST_ASSERT_NEAR(low_pressure_conf, 0.50f, 0.0001f,
                   "Low inlet pressure still counts during startup suppression");
  TEST_ASSERT_NEAR(flow_collapse_conf, 0.50f, 0.0001f,
                   "Pump-flow collapse still counts during startup suppression");
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

void test_nan_inputs_do_not_vote() {
  std::cout << "\n=== Testing NaN Input Handling (BLE dropouts) ===" << std::endl;

  // Baseline: every input at a value that does NOT vote. Inlet pressure and
  // pump flow must sit above their floors, since those two vote on absolute
  // values and fire when low.
  const float kQuiet[6] = {0.0f, 13.2f, 9.56f, 0.0f, 0.0f, 0.0f};

  TEST_ASSERT_NEAR(
      deterministic_pump_on_conf(kQuiet[0], kQuiet[1], kQuiet[2], kQuiet[3],
                                 kQuiet[4], kQuiet[5], false),
      0.0f, 0.0001f, "Baseline quiet inputs cast no votes");

  // A dropped BLE sensor reads NaN. Each one in turn must neither crash nor
  // vote — an absent signal is not evidence of demand.
  const char *names[6] = {"inlet_deriv",   "inlet_psi",   "pump_flow",
                          "current_deriv", "power_deriv", "head_rate_peak"};
  for (int i = 0; i < 6; i++) {
    float a[6] = {kQuiet[0], kQuiet[1], kQuiet[2],
                  kQuiet[3], kQuiet[4], kQuiet[5]};
    a[i] = NAN;
    float conf =
        deterministic_pump_on_conf(a[0], a[1], a[2], a[3], a[4], a[5], false);
    TEST_ASSERT_NEAR(conf, 0.0f, 0.0001f,
                     std::string("NaN ") + names[i] + " does not vote");
  }

  // Total BLE loss: every input NaN at once.
  TEST_ASSERT_NEAR(
      deterministic_pump_on_conf(NAN, NAN, NAN, NAN, NAN, NAN, false), 0.0f,
      0.0001f, "All-NaN inputs declare no demand");

  // A NaN must not suppress a genuine signal reported by a working sensor.
  TEST_ASSERT_NEAR(
      deterministic_pump_on_conf(NAN, 2.5f, NAN, NAN, NAN, NAN, false), 0.50f,
      0.0001f, "A real low-pressure vote survives NaN on every other input");
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

void test_pump_on_demand_level_scales_with_votes() {
  std::cout << "\n=== Testing Pump-On Demand Level Scaling ===" << std::endl;

  // Mirrors Python's `demand_level = 0.3 + 0.15 * (signal_count - 1)`
  // (detection.py:732), capped at 1.0. Previously the firmware published a
  // flat 0.3 regardless of vote count (issue #125 item 1).
  TEST_ASSERT_NEAR(pump_on_demand_level(1), 0.30f, 0.0001f,
                   "A lone vote yields the 0.3 floor");
  TEST_ASSERT_NEAR(pump_on_demand_level(2), 0.45f, 0.0001f,
                   "Two votes yield 0.45");
  TEST_ASSERT_NEAR(pump_on_demand_level(3), 0.60f, 0.0001f,
                   "Three votes yield 0.60");
  TEST_ASSERT_NEAR(pump_on_demand_level(5), 0.90f, 0.0001f,
                   "All five shared votes reach 0.90, Python's ceiling");

  // The vote count the level is derived from must be the same one that drove
  // confidence, so read it back out of the production vote function. Low inlet
  // pressure plus pump-side flow collapse is the 65-of-73 pair from the July
  // evaluation window: two votes.
  const char *method = nullptr;
  PumpOnVotes votes;
  float confidence = compute_pump_on_confidence(
      NAN, 3.0f, 0.05f, NAN, NAN, NAN, /*suppress_transient_votes=*/false,
      kDefaultPumpOnVoteThresholds, &method, &votes);
  TEST_ASSERT(votes.total == 2,
              "Low pressure + flow collapse reports two votes");
  TEST_ASSERT(votes.shared == 2,
              "Neither is the head-rate vote, so shared matches total");
  TEST_ASSERT_NEAR(confidence, 0.65f, 0.0001f,
                   "Two votes map to 0.65 confidence");
  TEST_ASSERT_NEAR(pump_on_demand_level(votes.shared), 0.45f, 0.0001f,
                   "That same vote count yields a 0.45 demand level");

  // votes_out must be left alone when no signal fires, so a caller reusing the
  // variable across ticks cannot publish a stale level as if it were fresh.
  PumpOnVotes untouched{-1, -1};
  confidence = compute_pump_on_confidence(
      NAN, 13.2f, 9.56f, NAN, NAN, NAN, /*suppress_transient_votes=*/false,
      kDefaultPumpOnVoteThresholds, &method, &untouched);
  TEST_ASSERT_NEAR(confidence, 0.0f, 0.0001f, "No signal, no confidence");
  TEST_ASSERT(untouched.total == -1 && untouched.shared == -1,
              "votes_out is not written when no signal voted");
}

void test_head_rate_vote_moves_confidence_but_not_demand_level() {
  std::cout << "\n=== Testing Head-Rate Vote Isolation ===" << std::endl;

  // The head-rate vote (signal 6) is firmware-only and permanently so: the
  // Python detector deprecated its head channel. Publishing demand_level made
  // that divergence visible on the wire, because the level was derived from the
  // total vote count (issue #125). It now comes from the shared count.
  //
  // Same telemetry twice — low inlet pressure plus pump-side flow collapse,
  // once with a head-rate spike past the 0.31 m/s threshold and once without.
  const char *method = nullptr;
  PumpOnVotes without;
  float conf_without = compute_pump_on_confidence(
      NAN, 3.0f, 0.05f, NAN, NAN, NAN, /*suppress_transient_votes=*/false,
      kDefaultPumpOnVoteThresholds, &method, &without);

  PumpOnVotes with;
  float conf_with = compute_pump_on_confidence(
      NAN, 3.0f, 0.05f, NAN, NAN, 2.94f, /*suppress_transient_votes=*/false,
      kDefaultPumpOnVoteThresholds, &method, &with);

  TEST_ASSERT(with.total == without.total + 1,
              "The head-rate spike casts one extra vote");
  TEST_ASSERT(with.shared == without.shared,
              "That vote is excluded from the shared count");

  // Confidence is firmware-local, so it may reflect the extra evidence.
  TEST_ASSERT_NEAR(conf_without, 0.65f, 0.0001f, "Two votes: 0.65 confidence");
  TEST_ASSERT_NEAR(conf_with, 0.80f, 0.0001f,
                   "The head-rate vote still sharpens confidence to 0.80");

  // demand_level is on the wire and must not move.
  TEST_ASSERT_NEAR(pump_on_demand_level(with.shared),
                   pump_on_demand_level(without.shared), 0.0001f,
                   "demand_level is identical with and without the head vote");
  TEST_ASSERT_NEAR(pump_on_demand_level(with.shared), 0.45f, 0.0001f,
                   "Both report the two-shared-vote level Python would report");

  // All five shared signals plus the head vote: the case that used to publish
  // 1.00, a value Python's five-signal ceiling makes unreachable.
  PumpOnVotes saturated;
  float conf_saturated = compute_pump_on_confidence(
      1.32f, 3.0f, 0.05f, 0.015f, 12.0f, 2.94f,
      /*suppress_transient_votes=*/false, kDefaultPumpOnVoteThresholds, &method,
      &saturated);
  TEST_ASSERT(saturated.total == 6, "All six signals vote");
  TEST_ASSERT(saturated.shared == 5, "Five of them are shared with Python");
  TEST_ASSERT_NEAR(conf_saturated, 0.95f, 0.0001f, "Confidence caps at 0.95");
  TEST_ASSERT_NEAR(pump_on_demand_level(saturated.shared), 0.90f, 0.0001f,
                   "Saturated demand_level is 0.90, not the old 1.00");
}

int main() {
  std::cout << "==========================================================="
            << std::endl;
  std::cout << "  DHW Demand Pump-On Logic Test Suite" << std::endl;
  std::cout << "==========================================================="
            << std::endl;

  test_startup_transients_are_suppressed();
  test_startup_transients_still_trigger_without_guard();
  test_startup_guard_keeps_open_loop_signals();
  test_flow_only_onset_requires_one_full_off_tick();
  test_only_a_pump_off_tick_arms_the_flow_debounce();
  test_dhw_in_use_boost_is_gated_to_pump_off();
  test_ambiguous_flow_onset_does_not_prime_continuation();
  test_nan_inputs_do_not_vote();
  test_sustained_demand_accrues_session();
  test_threshold_jitter_does_not_chatter();
  test_pump_on_demand_level_scales_with_votes();
  test_head_rate_vote_moves_confidence_but_not_demand_level();

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
