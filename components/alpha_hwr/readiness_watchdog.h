#pragma once

#include <cstdint>

#include "link_watchdog.h"

// Progress watchdog for the GENI link: connected, data arriving, never usable.
//
// link_watchdog.h watches LIVENESS. Its observable is "connected and heard
// nothing", and it is re-armed by every inbound notification. That is right for
// what it covers and useless for what this covers, because this pump volunteers
// Class 10 telemetry unprompted: a session stuck anywhere at all keeps being
// handed frames, so the data watchdog is re-armed forever and never fires.
//
// The result, reported from a live installation (issue #211): connected,
// pairing on, telemetry updating in Home Assistant, `Pump Ready` off
// indefinitely, no fault raised, no recycle, and an automation gated on
// `Pump Ready` waiting silently for something that is never coming. The
// reporter's words for why that shape is the worst one: "it's the one failure
// shape where the diagnostics actively point away from the problem."
//
// So liveness was watched and progress was not. This is the progress half, and
// the distinction is the whole design:
//
//   - The data watchdog asks "is anything arriving?" and is reset by arrival.
//   - This asks "has the link become usable?" and is reset by NOTHING except
//     becoming usable. Not by data, not by a session-state transition, not by
//     a cache filling partway.
//
// That second point is a specific warning the reporter raised before any code
// existed, and it is worth restating because the trap has already been walked
// into once in this file's sibling. link_watchdog.h documents the Pump Link
// Status ladder refreshing `link_last_open_ms_` on every evaluation, which kept
// the rung below it permanently unreachable -- a check that fed itself and so
// could never fire. A readiness timer re-armed by anything readiness-adjacent
// has exactly that shape. Hence one arming site, the connection-open, and one
// disarming condition, the thing being waited for.
//
// What it catches, which is broader than any single bug: a session stuck in
// service discovery or subscribing, a subscribe that returned early and left
// the session short of READY, a session that reached READY with caches that
// never fill, and whatever the next one turns out to be. All of them present
// identically from outside -- connected but not usable -- and none of them has
// any other backstop since the opening sequence's went away with the sequence
// (issue #174).
//
// **The bound is deliberately generous, and now partly measured.** It was
// shipped unmeasured on purpose -- waiting for a number before shipping
// anything would have left the failure in place indefinitely, which is the
// trade issue #176 made once and regretted -- and then measured on a second
// specimen by setting the window deliberately short and watching which value
// fired:
//
//   * 10 s window: fired. 20 s window: fired. 40 s window: did not.
//
// So a fresh connection on a bonded pump reaches usable somewhere between 20
// and 40 s -- about 22 s from the recycle stamp, with the read chain (device
// info, statistics, control mode, twenty event-log entries, alarms, warnings,
// four trend channels, ten cycle timestamps) dominating at roughly 175 ms per
// reply. 300 s -- the suggested value if the watchdog is enabled, since it is off
// by default -- is therefore around twelve times the measured figure.
//
// Two cautions on that number. It is one pump, and it is a BONDED reconnect: a
// first pairing, with the SMP exchange in front of the same chain, is still
// unmeasured. And an earlier figure of 15.45 s from this same specimen was
// misleading -- it was taken from a reboot where the BLE link survived, so it
// timed the read chain alone and none of the connect. If a first pairing is
// ever timed and comes in far above this, raise the suggested value rather
// than trusting the bracket above.
//
// The asymmetry that justifies erring high is unchanged: a bound that is too
// loose still converts "silent forever" into "recovers eventually", which is
// the entire win, while one that is too tight recycles a merely slow pump.
//
// The recycle count and the backoff are shared with the data watchdog rather
// than reinvented: link_data_timeout_next() is the same doubling with the same
// ceiling, for the same reason -- a link that can recover does so on the first
// or second try, and one that cannot must not spend the day re-entering the
// encryption-on-open path where a failure can erase the bond (issue #14).

// In namespace alpha_hwr rather than alpha_hwr::core, matching link_watchdog.h.
// The other decision headers sit in core; these two do not, and splitting a
// watchdog from its sibling over a namespace would be worse than either
// convention.
namespace esphome {
namespace alpha_hwr {

/// Has the link been connected this long without becoming usable?
///
/// @param connected          The BLE link is open.
/// @param pump_ready         The component has reached its usable state --
///                           session READY, initial reads done, caches valid.
/// @param now_ms             millis().
/// @param connected_since_ms Stamp of the connection-open. NOT refreshed by
///                           anything else; see the header note.
/// @param timeout_ms         The window in force, 0 to disable.
///
/// Subtraction on unsigned values, like link_data_timeout_expired(), so a
/// millis() rollover at 49.7 days yields the true elapsed interval rather than
/// an enormous one that fires instantly.
inline bool link_readiness_timeout_expired(bool connected, bool pump_ready,
                                           uint32_t now_ms, uint32_t connected_since_ms,
                                           uint32_t timeout_ms) {
  // Three statements rather than one `||` chain, and not for readability: a
  // mutation entry's search field is split on '|', so a guard containing `||`
  // truncates silently at the first one and applies an edit nobody wrote. The
  // occurrence check cannot catch it either -- the truncated fragment is still
  // unique, so it reports a clean match and the build then fails on garbage.
  // Two entries for this function were written that way and both scored
  // BUILD_BROKEN. Split, each clause is anchorable on its own.
  if (!connected)
    return false;
  if (pump_ready)
    return false;
  if (timeout_ms == 0)
    return false;
  return static_cast<uint32_t>(now_ms - connected_since_ms) > timeout_ms;
}

/// Ceiling for the readiness backoff. Same hour as the data watchdog's, and
/// deliberately the same value rather than a second tunable: both answer the
/// same question -- how often may a link that cannot recover be re-opened --
/// and two different answers to it would be a distinction with no reason.
static const uint32_t LINK_READY_TIMEOUT_BACKOFF_CAP_MS = LINK_DATA_TIMEOUT_BACKOFF_CAP_MS;

/// The window for the next cycle after a recycle that never reached readiness.
///
/// Shares link_data_timeout_next() outright. The alternative was a copy, and a
/// copy of a doubling rule is a copy of its rollover clamp, its
/// disabled-stays-disabled rule and its already-past-the-cap rule -- three
/// decisions that would then be pinned by mutation entries in one place and not
/// the other.
inline uint32_t link_readiness_timeout_next(uint32_t current_ms, uint32_t cap_ms) {
  return link_data_timeout_next(current_ms, cap_ms);
}

/**
 * May the readiness watchdog recycle the link this time? (issue #257)
 *
 * `ready_recycle` used to be a boolean -- off, or forever -- and the case the
 * reporter cared about is neither. If a bonded, connected link that will not
 * finish its opening GENI reads is a one-off glitch, one reconnect clears it;
 * if it is not, another fifty will not either, while each one takes another run
 * at the encryption-on-open window that can erase a bond (issue #14). So the
 * option is a count: 0 never recycles, N recycles at most N consecutive times
 * and then leaves the fault standing for an automation to notice.
 *
 * @param limit       Configured allowance. 0 = never; READY_RECYCLE_FOREVER
 *                    (0xFFFFFFFF) = unbounded, which is what a YAML `true`
 *                    still maps to.
 * @param consecutive Recycles already spent in THIS episode --
 *                    link_recycles_without_ready_, which the pump becoming
 *                    ready resets. Read before the caller increments it, so a
 *                    limit of N yields exactly N recycles.
 *
 * The unbounded case needs no special-casing: nothing reaches 0xFFFFFFFF
 * consecutive recycles, so the same comparison covers it.
 */
inline bool link_ready_may_recycle(uint32_t limit, uint32_t consecutive) {
  // One comparison, and both special cases fall out of it rather than being
  // guarded. A limit of 0 needs no test: `consecutive` is unsigned, so nothing
  // is below zero. READY_RECYCLE_FOREVER needs none either: nothing reaches
  // 0xFFFFFFFF consecutive recycles. An explicit `if (limit == 0) return false`
  // stood here first and was removed as dead -- it could not be mutated,
  // because removing it changes no outcome, which is the definition of the
  // equivalent mutant this repo has been bitten by before (issue #282).
  return consecutive < limit;
}

}  // namespace alpha_hwr
}  // namespace esphome
