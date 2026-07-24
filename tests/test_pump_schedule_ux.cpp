/**
 * Unit tests for the "Engage Pump" vs "Schedule Enabled" reconciliation
 * (pump_schedule_ux.h) — the pure target/display logic behind the two
 * mutually-exclusive UI switches.
 *
 * Bench-proven behavior these tests lock in (motor RPM ground truth, 2026-07):
 *   - the schedule only runs the pump while operation_mode == AUTO, so
 *     enabling the schedule must force AUTO (never a dead STOP+schedule state);
 *   - "Engage Pump" means the mode is engaged continuously *now* = AUTO &&
 *     schedule OFF, which makes the two switches read as mutually exclusive.
 */

#include <iostream>
#include <string>
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
                        bool expect_engage_pump, bool expect_schedule) {
  bool engage_pump = engage_pump_display(t.pump_enabled, t.schedule_enabled);
  bool schedule = t.schedule_enabled;
  TEST_ASSERT(engage_pump == expect_engage_pump,
              std::string(label) + ": Engage Pump switch reads " +
                  (expect_engage_pump ? "ON" : "off"));
  TEST_ASSERT(schedule == expect_schedule,
              std::string(label) + ": Schedule switch reads " +
                  (expect_schedule ? "ON" : "off"));
  // The core invariant: the two switches are never both on.
  TEST_ASSERT(!(engage_pump && schedule),
              std::string(label) + ": switches are mutually exclusive");
}

int main() {
  std::cout << "=== Pump/Schedule reconciliation (pump_schedule_ux) ===" << std::endl;

  // ---- engage_pump_display predicate across all four raw states ----------
  // args: engage_pump_display(pump_auto, schedule_on)
  TEST_ASSERT(engage_pump_display(true, false) == true,
              "AUTO + schedule off  -> Engage Pump ON (engaged continuously)");
  TEST_ASSERT(engage_pump_display(true, true) == false,
              "AUTO + schedule on   -> Engage Pump off (gated to windows)");
  TEST_ASSERT(engage_pump_display(false, false) == false,
              "STOP + schedule off  -> Engage Pump off (Off)");
  TEST_ASSERT(engage_pump_display(false, true) == false,
              "STOP + schedule on   -> Engage Pump off");

  // ---- the four UI actions settle in the expected states -----------------
  // check_state(label, target, expect_engage_pump, expect_schedule)
  check_state("Engage Pump ON", engage_pump_on_target(), true, false);    // Engaged (continuous)
  check_state("Engage Pump OFF", engage_pump_off_target(), false, false);  // Off
  check_state("Schedule ON", schedule_on_target(), false, true);           // Scheduled (AUTO so it can run)
  check_state("Schedule OFF", schedule_off_target(), false, false);        // Off (stop pump)

  // ---- the key correctness fix: enabling the schedule forces AUTO --------
  TEST_ASSERT(schedule_on_target().pump_enabled == true,
              "Schedule ON forces pump AUTO (never a dead STOP+schedule)");
  TEST_ASSERT(engage_pump_on_target().schedule_enabled == false,
              "Engage Pump ON disables the schedule (engaged continuously)");
  TEST_ASSERT(schedule_off_target().pump_enabled == false,
              "Schedule OFF stops the pump");

  // ---- pump_set_state service: three-state targets ----------------------
  TEST_ASSERT(!state_off_target().pump_enabled && !state_off_target().schedule_enabled,
              "state 'off' -> STOP + schedule off");
  TEST_ASSERT(state_engaged_target().pump_enabled && !state_engaged_target().schedule_enabled,
              "state 'engaged' -> AUTO + schedule off");
  TEST_ASSERT(state_scheduled_target().pump_enabled && state_scheduled_target().schedule_enabled,
              "state 'scheduled' -> AUTO + schedule on");

  // ---- parse_pump_state: valid values round-trip, unknown rejected -------
  PumpScheduleTarget t{};
  TEST_ASSERT(parse_pump_state("off", &t) && !t.pump_enabled && !t.schedule_enabled,
              "parse 'off'");
  TEST_ASSERT(parse_pump_state("engaged", &t) && t.pump_enabled && !t.schedule_enabled,
              "parse 'engaged'");
  TEST_ASSERT(parse_pump_state("scheduled", &t) && t.pump_enabled && t.schedule_enabled,
              "parse 'scheduled'");
  TEST_ASSERT(!parse_pump_state("running", &t), "parse unknown -> false");
  TEST_ASSERT(!parse_pump_state("", &t), "parse empty -> false");

  // ---- state_name: inverse of the targets (settled-state reporting) ------
  TEST_ASSERT(std::string(state_name(false, false)) == "off", "state_name STOP -> off");
  TEST_ASSERT(std::string(state_name(false, true)) == "off", "state_name STOP+sched -> off");
  TEST_ASSERT(std::string(state_name(true, false)) == "engaged", "state_name AUTO -> engaged");
  TEST_ASSERT(std::string(state_name(true, true)) == "scheduled", "state_name AUTO+sched -> scheduled");

  std::cout << "\n" << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
