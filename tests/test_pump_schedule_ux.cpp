/**
 * Unit tests for the "Run Pump" vs "Schedule Enabled" reconciliation
 * (pump_schedule_ux.h) — the pure target/display logic behind the two
 * mutually-exclusive UI switches.
 *
 * Bench-proven behavior these tests lock in (motor RPM ground truth, 2026-07):
 *   - the schedule only runs the pump while operation_mode == AUTO, so
 *     enabling the schedule must force AUTO (never a dead STOP+schedule state);
 *   - "Run Pump" means running continuously *now* = AUTO && schedule OFF, which
 *     is what makes the two switches read as mutually exclusive.
 */

#include <iostream>
#include "../components/alpha_hwr/pump_schedule_ux.h"

using namespace esphome::alpha_hwr::ux;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

// A UI action's target, once applied, settles the pump at (pump_enabled,
// schedule_enabled). Feed that back through the display predicate to model
// exactly what the two switches would show.
static void check_state(const char *label, PumpScheduleTarget t,
                        bool expect_run_pump, bool expect_schedule) {
  bool run_pump = run_pump_display(t.pump_enabled, t.schedule_enabled);
  bool schedule = t.schedule_enabled;
  TEST_ASSERT(run_pump == expect_run_pump,
              std::string(label) + ": Run Pump switch reads " +
                  (expect_run_pump ? "ON" : "off"));
  TEST_ASSERT(schedule == expect_schedule,
              std::string(label) + ": Schedule switch reads " +
                  (expect_schedule ? "ON" : "off"));
  // The core invariant: the two switches are never both on.
  TEST_ASSERT(!(run_pump && schedule),
              std::string(label) + ": switches are mutually exclusive");
}

int main() {
  std::cout << "=== Pump/Schedule reconciliation (pump_schedule_ux) ===" << std::endl;

  // ---- run_pump_display predicate across all four raw states -------------
  // args: run_pump_display(pump_auto, schedule_on)
  TEST_ASSERT(run_pump_display(true, false) == true,
              "AUTO + schedule off  -> Run Pump ON (continuous)");
  TEST_ASSERT(run_pump_display(true, true) == false,
              "AUTO + schedule on   -> Run Pump off (gated, not continuous)");
  TEST_ASSERT(run_pump_display(false, false) == false,
              "STOP + schedule off  -> Run Pump off (Off)");
  TEST_ASSERT(run_pump_display(false, true) == false,
              "STOP + schedule on   -> Run Pump off");

  // ---- the four UI actions settle in the expected states -----------------
  // check_state(label, target, expect_run_pump, expect_schedule)
  check_state("Run Pump ON", run_pump_on_target(), true, false);    // Run (continuous)
  check_state("Run Pump OFF", run_pump_off_target(), false, false);  // Off
  check_state("Schedule ON", schedule_on_target(), false, true);     // Scheduled (AUTO so it can run)
  check_state("Schedule OFF", schedule_off_target(), false, false);  // Off (stop pump)

  // ---- the key correctness fix: enabling the schedule forces AUTO --------
  TEST_ASSERT(schedule_on_target().pump_enabled == true,
              "Schedule ON forces pump AUTO (never a dead STOP+schedule)");
  TEST_ASSERT(run_pump_on_target().schedule_enabled == false,
              "Run Pump ON disables the schedule (continuous run)");
  TEST_ASSERT(schedule_off_target().pump_enabled == false,
              "Schedule OFF stops the pump");

  std::cout << "\n" << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
