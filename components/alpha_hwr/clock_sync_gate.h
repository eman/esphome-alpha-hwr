#pragma once

#include <cstdint>

// Should the daily clock sync run, and if not, is that worth telling anybody?
//
// The pump keeps its own RTC and runs schedule windows off it. Nothing else
// corrects that clock, so when a sync cannot happen the visible symptom is a
// schedule that fires at the wrong hour -- days or weeks later, with every
// sensor still reporting healthily. That is the failure this gate exists to
// make legible.
//
// Three states get confused easily, and the cost of confusing them is why this
// is an enum rather than a bool:
//
//   * No `time_id` at all. Permanent for the life of the run: the option is
//     optional in the schema, both entry packages set it, and a hand-written
//     `alpha_hwr:` block that omits it will never sync, ever.
//   * `time_id` set, but its source has not answered. This is the *likelier*
//     failure in practice, precisely because the packages set `time_id` -- a
//     `homeassistant` time platform on a node that cannot reach Home Assistant
//     looks exactly like a configured clock and never produces a valid one.
//     Indistinguishable from the next case until enough time has passed.
//   * `time_id` set and the source simply has not answered *yet*. Normal at
//     boot, resolves on its own, and must stay silent.
//
// Reporting the second as though it were the third is what let a permanently
// unsynced pump look like a node that was still settling. Waiting out a grace
// window is the only thing that separates them.
//
// The other half of this is that neither non-syncing state should drive the
// retry loop in check_and_sync_time(). That loop deliberately does not stamp
// an attempt when nothing was written, so a sync blocked by an unsynchronized
// pump is retried on the next poll rather than backed off fifteen minutes.
// With no usable wall clock that same property is a spin: every 10 s poll walks
// the full path, fails, and re-arms, forever. At the INFO level this component
// ships at that costs only a few compares -- ESPHome compiles ESP_LOGD out
// entirely below DEBUG -- but on a node built at DEBUG it is four log lines per
// poll, and every log line is an API frame fanned out to every subscriber.
//
// Extracted rather than written inline for the reason failure_hold.h,
// subscribe_outcome.h and gap_security_policy.h were: alpha_hwr.cpp is compiled
// by no host test, so a rule expressed there cannot be checked.

namespace esphome {
namespace alpha_hwr {
namespace core {

/// How long a configured time source may stay silent before it is reported.
///
/// Generous on purpose. SNTP normally answers within seconds and the
/// `homeassistant` platform within seconds of the API connecting, so anything
/// still unset after this is not "settling" — but a node that boots while its
/// router is coming up should not be accused of being misconfigured.
constexpr uint32_t CLOCK_SOURCE_GRACE_MS = 15 * 60 * 1000;  // 15 minutes

enum class ClockSyncAction : uint8_t {
  SYNC,             ///< A usable wall clock exists; run the normal sync path.
  WAIT,             ///< No clock yet, but it is early. Stay silent.
  WARN_NO_TIME_ID,  ///< Nothing to sync from, and there never will be.
  WARN_NO_SOURCE,   ///< A clock is configured, but its source never answered.
};

/// Decide what the periodic clock-sync check should do this tick.
///
/// @param has_time_id      a `time:` component is wired to this component
/// @param wall_clock_set   that component has produced a plausible wall clock
/// @param uptime_ms        millis() — how long the node has had to get one
/// @param grace_ms         how long a silent source is given before reporting
inline ClockSyncAction clock_sync_action(bool has_time_id, bool wall_clock_set,
                                         uint32_t uptime_ms, uint32_t grace_ms) {
  if (!has_time_id) {
    // Reported from the first tick. Nothing about waiting longer can change
    // this answer, and staying quiet is what made it invisible before.
    return ClockSyncAction::WARN_NO_TIME_ID;
  }
  if (wall_clock_set) {
    return ClockSyncAction::SYNC;
  }
  // Strictly less than, so a grace of 0 reports immediately rather than
  // requiring one further tick.
  if (uptime_ms < grace_ms) {
    return ClockSyncAction::WAIT;
  }
  return ClockSyncAction::WARN_NO_SOURCE;
}

/// True when this action means no sync can be attempted this tick.
inline bool clock_sync_blocked(ClockSyncAction a) {
  return a != ClockSyncAction::SYNC;
}

/// True when this action is worth telling the user about (rate-limited by the
/// caller — the condition persists, so an unthrottled warning would be as bad
/// as the spin it replaces).
inline bool clock_sync_warns(ClockSyncAction a) {
  return a == ClockSyncAction::WARN_NO_TIME_ID || a == ClockSyncAction::WARN_NO_SOURCE;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
