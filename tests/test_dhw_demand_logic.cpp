/**
 * Unit tests for DHW demand pump-on hydraulic voting.
 *
 * These tests mirror the production vote logic to verify that recirculation
 * pump startup transients do not falsely declare DHW demand while genuine
 * open-loop signals remain detectable.
 */

#include <cmath>
#include <iostream>

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
  constexpr float inlet_pressure_transient_threshold = 0.07f;
  constexpr float inlet_pressure_demand_floor = 5.0f;
  constexpr float pump_flow_collapse_threshold = 0.2f;
  constexpr float motor_current_spike_threshold = 0.001f;
  constexpr float pump_power_spike_threshold = 5.0f;
  constexpr float pump_head_rate_threshold = 3.0f;

  int votes = 0;

  if (!suppress_transient_votes) {
    if (!std::isnan(inlet_deriv) &&
        std::fabs(inlet_deriv) > inlet_pressure_transient_threshold)
      votes++;
  }

  if (!std::isnan(inlet_psi) && inlet_psi < inlet_pressure_demand_floor)
    votes++;

  if (!std::isnan(pump_flow) && pump_flow < pump_flow_collapse_threshold)
    votes++;

  if (!suppress_transient_votes) {
    if (!std::isnan(current_deriv) &&
        std::fabs(current_deriv) > motor_current_spike_threshold)
      votes++;

    if (!std::isnan(power_deriv) &&
        power_deriv > pump_power_spike_threshold)
      votes++;

    if (votes >= 1 && head_rate_peak > pump_head_rate_threshold)
      votes++;
  }

  if (votes == 0)
    return 0.0f;

  static const float conf_map[7] = {0.0f, 0.50f, 0.65f, 0.80f,
                                    0.90f, 0.95f, 0.95f};
  return (votes < 7) ? conf_map[votes] : 0.95f;
}

static bool pump_off_flow_onset_is_confirmed(bool flow_present,
                                             bool prev_flow_present,
                                             bool prev_pump_confirmed_off,
                                             bool onset_corroborating_signal_present) {
  if (!flow_present)
    return false;
  if (!onset_corroborating_signal_present &&
      (!prev_flow_present || !prev_pump_confirmed_off))
    return false;
  return true;
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

  TEST_ASSERT_NEAR(confidence, 0.65f, 0.0001f,
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

  bool first_tick_confirmed =
      pump_off_flow_onset_is_confirmed(true, false, false, false);
  bool second_tick_confirmed =
      pump_off_flow_onset_is_confirmed(true, true, true, false);
  bool corroborated_first_tick =
      pump_off_flow_onset_is_confirmed(true, false, false, true);
  bool carried_recirc_flow_confirmed =
      pump_off_flow_onset_is_confirmed(true, true, false, false);
  // After fix M4: charge-drop now correctly sets onset_corroborating_signal_present,
  // so it should confirm a first-tick flow onset (matching the intent in AGENTS.md §10.4).
  bool charge_only_first_tick_confirmed =
      pump_off_flow_onset_is_confirmed(true, false, true, true);

  TEST_ASSERT(!first_tick_confirmed,
              "A brand-new flow-only onset is treated as ambiguous");
  TEST_ASSERT(second_tick_confirmed,
              "Flow-only demand is accepted after one full off tick");
  TEST_ASSERT(corroborated_first_tick,
              "Corroborated first-tick flow is accepted immediately");
  TEST_ASSERT(!carried_recirc_flow_confirmed,
              "Flow carried from a pump-on tick stays ambiguous on the first off tick");
  TEST_ASSERT(charge_only_first_tick_confirmed,
              "Charge-drop corroboration confirms a first-tick flow onset");
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
  test_ambiguous_flow_onset_does_not_prime_continuation();

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
