#pragma once

#include <cstdint>

// Which held fault reason wins, and what releases it.
//
// The Pump Link Fault surface shows one string, and four sites write it: the
// disconnect handler, the inbound-data watchdog (via force_disconnect), the
// subscribe step (subscribe_outcome.h) and the auth-completion handler. A
// reconnect episode runs all of them within a minute of each other, so without
// a rule the last writer wins and the operator is shown whichever cause spoke
// most recently rather than the one worth acting on.
//
// The rule is the enum order below: a reason may overwrite what is held only
// if the held reason does not outrank it. That is one condition at every write
// site (failure_hold_admits) rather than a list of exceptions -- which is what
// this header is for. Issue #175's first version guarded the watchdog write
// with `!= SUBSCRIBE`, and an AUTH hold, being neither NONE nor SUBSCRIBE, was
// overwritten by "No data from pump" 60 s later: the same overwrite that fix
// existed to prevent, on the hold that records a bond-erasing failure.
//
// The releases are deliberately NOT unified, and the asymmetry is the reason
// this is an enum rather than a bool. Each origin is refuted by different
// evidence:
//
//   - DATA and SUBSCRIBE are released by any inbound notification, which
//     refutes them by construction. They must NOT be released by a successful
//     AUTH_CMPL: with pairing disabled (the default) AUTH_CMPL never fires at
//     all, and reaching one proves nothing about whether the pump is answering
//     -- which is the very defect the watchdog exists for.
//   - AUTH is released by a successful AUTH_CMPL, because the failure it
//     records erases the bond and recovery must pass back through one -- and,
//     failing that, by the GENI session reaching READY. It must NOT be
//     released by inbound data: an SMP failure on an UNBONDED pump latches its
//     reason WITHOUT tearing the link down, and that link then subscribes and
//     delivers notifications normally (passive telemetry needs no bond).
//     Releasing AUTH on data would wipe the pairing diagnostic while it is
//     still the operative fault.
//
// The READY release is what bounds an AUTH hold, and it exists because the
// rank made the hold otherwise unbreakable for the rest of the boot: nothing
// outranks AUTH, and a pump that never pairs successfully never produces the
// AUTH_CMPL that clears it. The fault string is shown only while the session
// is NOT ready (see evaluate_link_status), so a hold surviving past READY can
// no longer inform anyone about the pairing failure -- it can only mask the
// *next* outage's cause with a stale one. Before the rank, the watchdog broke
// such a hold within 60 s by overwriting it; that overwrite was the defect,
// and this release is the part of it worth keeping. The pairing state itself
// survives on its own sensor either way.
//
// One value rather than a "held" flag plus a separate "who set it" flag, on
// purpose: the origin decides the release, so two flags can disagree and every
// site that sets one must remember the other. That went wrong immediately -- a
// successful auth cleared the hold but left the origin set, so a LATER pairing
// failure could be silently erased by the next notification.
//
// In its own header, like link_watchdog.h, initial_read_retry.h and
// subscribe_outcome.h: ble_connection_manager.cpp is compiled by no host test,
// so a policy expressed there is unverifiable. The decision is the part with a
// judgement call in it, so the decision is what is extracted.

namespace esphome {
namespace alpha_hwr {
namespace core {

/// Why last_failure_ is being held over the routine disconnects of a reconnect
/// loop, if it is.
///
/// **The declaration order is the rank** -- later outranks earlier, and
/// failure_hold_admits() compares them. Reordering these changes which reason
/// an operator sees.
enum class FailureHold : uint8_t {
  /// No hold: the next reason to arrive, however generic, may write it.
  NONE = 0,
  /// The inbound-data watchdog's "No data from pump". Ranks lowest of the
  /// three real faults because it is the *symptom* every other cause here
  /// shares -- a failed subscribe and a failed pairing both surface as silence
  /// 60 s later. Released by any received notification.
  DATA = 1,
  /// A subscribe step that failed outright (subscribe_outcome.h). Outranks
  /// DATA: it names the cause of that silence, at the moment it happened,
  /// which is what issue #175 was about. Released like DATA, by any received
  /// notification.
  SUBSCRIBE = 2,
  /// An auth/encryption failure. Ranks highest: it is the only fault here that
  /// erases the bond, and a subscribe failure on an unauthenticated link is
  /// generally its consequence rather than a competing cause. Released by a
  /// successful AUTH_CMPL, or by the session reaching READY.
  AUTH = 3,
};

/// True when a reason arriving at rank `incoming` may overwrite what is held.
///
/// Equal ranks overwrite, so a fault keeps refreshing its own text: the
/// watchdog backs off, and each recycle carries the window it actually waited
/// ("No data from pump (60s)" then "(120s)"). Freezing that at the first fire
/// would hide the escalation from the fault surface.
///
/// Pass FailureHold::NONE as `incoming` for a reason that names something real
/// but must not survive the reconnect -- a plain disconnect code, or a CCCD
/// write that failed because the link was already gone.
inline bool failure_hold_admits(FailureHold held, FailureHold incoming) {
  return held <= incoming;
}

/// True when an inbound notification releases this hold.
///
/// Written as a switch so that -Werror=switch on the test target rejects a new
/// enumerator that never decides how it is released. A hold no site releases is
/// permanent, and a permanent hold silences every later fault.
inline bool failure_hold_released_by_data(FailureHold h) {
  switch (h) {
    case FailureHold::NONE:
      return false;
    case FailureHold::DATA:
      return true;
    case FailureHold::SUBSCRIBE:
      return true;
    case FailureHold::AUTH:
      return false;  // see the asymmetry above: unbonded SMP failures keep
                     // delivering notifications
  }
  return false;
}

/// True when a successful AUTH_CMPL releases this hold.
inline bool failure_hold_released_by_auth(FailureHold h) {
  switch (h) {
    case FailureHold::NONE:
      return false;
    case FailureHold::DATA:
      return false;  // a fresh bond does not prove the pump is answering
    case FailureHold::SUBSCRIBE:
      return false;
    case FailureHold::AUTH:
      return true;
  }
  return false;
}

/// True when the GENI session reaching READY releases this hold.
///
/// Only AUTH, and only as its backstop -- see the note above. READY is a weak
/// signal on purpose: the component's own on_session_stabilized_() records that
/// reaching it proves only that a timer fired, and happens on a deaf link just
/// the same. (AUTH here is BLE pairing, not the GENI opening sequence removed
/// in issue #174.) That is exactly why it must NOT release DATA or SUBSCRIBE, whose
/// whole claim is that no data arrived: a deaf link reaches READY on every
/// cycle and would clear the watchdog's reason each time. For AUTH the
/// weakness does not matter -- if the link is in fact deaf, the watchdog
/// writes the true current cause 60 s later, which is the outcome wanted.
inline bool failure_hold_released_by_session_ready(FailureHold h) {
  switch (h) {
    case FailureHold::NONE:
      return false;
    case FailureHold::DATA:
      return false;  // READY does not prove the pump is answering
    case FailureHold::SUBSCRIBE:
      return false;  // and a blocking subscribe fault never reaches READY
    case FailureHold::AUTH:
      return true;
  }
  return false;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
