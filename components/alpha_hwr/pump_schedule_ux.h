#pragma once

#include <cstring>

// Pure, ESPHome-free reconciliation logic for the two user-facing pump
// controls — "Engage Pump" (engage the pump's configured mode now) and
// "Schedule Enabled" — so they behave like the Grundfos GO app: mutually
// exclusive, and never leaving a "dead schedule" that can't run.
//
// "Engage Pump" toggles operation_mode AUTO (engaged) vs STOP. It engages the
// pump's *mode*; whether the motor physically spins is mode-dependent — it runs
// continuously in the constant modes (speed/flow/pressure) and cycles per the
// mode in Temperature/Cycle-Time. So this is "engage", not literally "run".
//
// Bench-proven rule (2026-07, motor RPM as ground truth):
//   effective run = (operation_mode == AUTO) AND (schedule OFF OR inside a window)
// The schedule can therefore only run the pump while it is AUTO; forcing STOP
// while the schedule is enabled produces a schedule that never runs. The three
// reachable states are:
//   Off       = STOP
//   Engaged   = AUTO + schedule OFF   (mode engaged continuously; app start/stop)
//   Scheduled = AUTO + schedule ON    (mode engaged only inside weekly windows)
//
// Extracted as a header so tests/test_pump_schedule_ux.cpp exercises the exact
// production logic — no hand-mirrored copy to drift (same pattern as
// dhw_demand_logic.h). See docs/schedule-management.md ("Run state and the
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

// "Engage Pump" reads true only when the pump's mode is engaged *continuously*
// right now: AUTO and not gated by an enabled schedule. Deriving the switch
// state this way (rather than from raw operation_mode) is what makes the two
// switches read as mutually exclusive without any optimistic faking — enabling
// the schedule forces AUTO, but "Engage Pump" still shows off because it is
// gated to windows.
inline bool engage_pump_display(bool pump_auto, bool schedule_on) {
  return pump_auto && !schedule_on;
}

// The illegal fourth state: the weekly schedule is enabled but operation_mode
// is STOP, so no window can ever run the pump ("dead schedule"). The two
// switches cannot produce it — but the raw uncoupled services, the Grundfos GO
// app, or a state carried over from before this reconciliation existed all can,
// and it survives reboots because nothing writes run state at boot (issue #124).
//
// stop_event_active = a Stop single-event (vacation) currently covers now. That
// is a *commanded* stop overriding the weekly schedule, so STOP is the expected
// state there and must not be reported or repaired as dead — otherwise the
// repair would run the pump through the user's vacation.
inline bool is_dead_schedule(bool pump_auto, bool schedule_on, bool stop_event_active) {
  return schedule_on && !pump_auto && !stop_event_active;
}

// Repairing a dead schedule keeps the user's schedule intent and engages AUTO
// so windows can actually run — i.e. it converges to Scheduled, the state the
// schedule flag was asking for. Never repair by clearing the schedule: that
// would silently discard intent.
inline PumpScheduleTarget dead_schedule_repair_target() { return {/*pump*/ true, /*schedule*/ true}; }

// Targets for the four switch actions.
inline PumpScheduleTarget engage_pump_on_target()  { return {/*pump*/ true,  /*schedule*/ false}; }  // Engaged (continuous)
inline PumpScheduleTarget engage_pump_off_target() { return {/*pump*/ false, /*schedule*/ false}; }  // Off
inline PumpScheduleTarget schedule_on_target()     { return {/*pump*/ true,  /*schedule*/ true};  }  // Scheduled (AUTO so never dead)
inline PumpScheduleTarget schedule_off_target()    { return {/*pump*/ false, /*schedule*/ false}; }  // Off (stop pump)

// ---- The three legal states as first-class targets, for the `pump_set_state`
// service (a single selector over the same three-state machine the two switches
// express jointly). off = STOP; engaged = AUTO + schedule off; scheduled =
// AUTO + schedule on.
inline PumpScheduleTarget state_off_target()       { return {/*pump*/ false, /*schedule*/ false}; }
inline PumpScheduleTarget state_engaged_target()   { return {/*pump*/ true,  /*schedule*/ false}; }
inline PumpScheduleTarget state_scheduled_target() { return {/*pump*/ true,  /*schedule*/ true};  }

// Parse a `pump_set_state` value ("off" | "engaged" | "scheduled") into a
// target. Returns false on an unknown string (caller settles `invalid`).
inline bool parse_pump_state(const char *s, PumpScheduleTarget *out) {
  if (std::strcmp(s, "off") == 0)       { *out = state_off_target();       return true; }
  if (std::strcmp(s, "engaged") == 0)   { *out = state_engaged_target();   return true; }
  if (std::strcmp(s, "scheduled") == 0) { *out = state_scheduled_target(); return true; }
  return false;
}

// The state name for a given (operation_mode AUTO?, schedule on?) — the inverse
// of the targets above, used to report the settled state back to callers.
inline const char *state_name(bool pump_auto, bool schedule_on) {
  if (!pump_auto) return "off";
  return schedule_on ? "scheduled" : "engaged";
}

// Display value for the "Pump Run State" diagnostic sensor. Same vocabulary as
// state_name() plus a distinct "stalled" for the dead schedule, which
// state_name() must keep reporting as "off" (that is the pump_set_state service
// contract — off/engaged/scheduled — and callers parse it). Publishing this is
// what makes a dead schedule visible in Home Assistant at all: "Engage Pump"
// reads off for both AUTO and STOP once the schedule is on, so before this the
// dead state was byte-identical to a healthy Scheduled pump (issue #124).
inline const char *run_state_display(bool pump_auto, bool schedule_on, bool stop_event_active) {
  if (is_dead_schedule(pump_auto, schedule_on, stop_event_active)) return "stalled";
  return state_name(pump_auto, schedule_on);
}

}  // namespace ux
}  // namespace alpha_hwr
}  // namespace esphome
