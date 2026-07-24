#pragma once

// Pure, ESPHome-free reconciliation logic for the two user-facing pump
// controls — "Run Pump" (run continuously now) and "Schedule Enabled" — so
// they behave like the Grundfos GO app: mutually exclusive, and never leaving
// a "dead schedule" that can't run.
//
// Bench-proven rule (2026-07-23/24, motor RPM as ground truth):
//   effective run = (operation_mode == AUTO) AND (schedule OFF OR inside a window)
// The schedule can therefore only run the pump while it is AUTO; forcing STOP
// while the schedule is enabled produces a schedule that never runs. The three
// reachable states are:
//   Off       = STOP
//   Run       = AUTO + schedule OFF   (continuous — the app's start/stop button)
//   Scheduled = AUTO + schedule ON    (runs only inside weekly windows)
//
// Extracted as a header so tests/test_pump_schedule_ux.cpp exercises the exact
// production logic — no hand-mirrored copy to drift (same pattern as
// dhw_demand_votes.h). See docs/schedule-management.md ("Run state and the
// schedule") for the user-facing behavior.

namespace esphome {
namespace alpha_hwr {
namespace ux {

// Desired end state a UI action converges the pump to. pump_enabled is the
// operation_mode intent (true = AUTO, false = STOP); schedule_enabled is the
// weekly-schedule flag. The apply layer writes only the fields that differ.
struct PumpScheduleTarget {
  bool pump_enabled;
  bool schedule_enabled;
};

// "Run Pump" reads true only when the pump is running continuously *now*:
// AUTO and not gated by an enabled schedule. Deriving the switch state this
// way (rather than from raw operation_mode) is what makes the two switches
// read as mutually exclusive without any optimistic faking — enabling the
// schedule forces AUTO, but "Run Pump" still shows off because it is gated.
inline bool run_pump_display(bool pump_auto, bool schedule_on) {
  return pump_auto && !schedule_on;
}

// Targets for the four switch actions.
inline PumpScheduleTarget run_pump_on_target()  { return {/*pump*/ true,  /*schedule*/ false}; }  // Run (continuous)
inline PumpScheduleTarget run_pump_off_target() { return {/*pump*/ false, /*schedule*/ false}; }  // Off
inline PumpScheduleTarget schedule_on_target()  { return {/*pump*/ true,  /*schedule*/ true};  }  // Scheduled (AUTO so never dead)
inline PumpScheduleTarget schedule_off_target() { return {/*pump*/ false, /*schedule*/ false}; }  // Off (stop pump)

}  // namespace ux
}  // namespace alpha_hwr
}  // namespace esphome
