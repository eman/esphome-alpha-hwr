#pragma once

#include <cstdint>

// Which held fault reason wins, and what releases it.
//
// The Pump Link Fault surface shows one string, and five sites write it: the
// disconnect handler, the pairing-stall report alongside it (pairing_stall.h),
// the inbound-data watchdog (via force_disconnect), the subscribe step
// (subscribe_outcome.h) and the auth-completion handler. A
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
  /// The readiness watchdog's "Pump never became ready" (readiness_watchdog.h,
  /// issue #211). Ranks lowest of the real faults, below even DATA, and the
  /// reason is worth stating because it looks backwards at first: this is the
  /// fault that fires when data IS arriving, so it might seem the more specific
  /// of the two.
  ///
  /// It is not. Both describe the same stall, and DATA describes it with more
  /// information. If the link has gone genuinely silent, the data watchdog
  /// fires at 60 s and names that; the readiness watchdog would fire minutes
  /// later and say only "it never became usable", which is true, downstream and
  /// less actionable. When data is flowing, DATA is released by every frame and
  /// never competes at all, so this one writes freely -- which is exactly the
  /// case it exists for.
  ///
  /// Released ONLY by the pump becoming ready. Not by inbound data, which is
  /// the condition it fires under rather than a refutation of it, and not by
  /// the GENI session reaching READY, which is one of the states it is meant to
  /// catch a link stuck past: session ready, caches never filling, unusable.
  READY = 1,
  /// The inbound-data watchdog's "No data from pump". Ranks below the three
  /// causes above it because it is the *symptom* they share -- a failed
  /// subscribe and a failed pairing both surface as silence 60 s later.
  /// Released by any received notification.
  DATA = 2,
  /// A pump that keeps dropping unbonded connections without ever offering to
  /// pair (pairing_stall.h, issue #230). Outranks DATA for the same reason
  /// SUBSCRIBE does -- it names a cause where DATA names the symptom -- and is
  /// outranked by both of the others on a rule worth stating plainly:
  ///
  ///   **an observed event outranks an inference from an absence.**
  ///
  /// SUBSCRIBE and AUTH each record something that happened and was seen. This
  /// records that three connections went by with nothing happening on them, and
  /// concludes. That is a weaker kind of evidence and it earns a weaker rank.
  ///
  /// The ordering is load-bearing in both directions, and both were found by a
  /// skeptic pass on the first version of this change, which put the stall at
  /// AUTH rank:
  ///
  ///   - Below AUTH, because an encryption failure on a bonded reconnect
  ///     (issue #14's 0x61) erases the bond, and the connections that follow it
  ///     are unbonded and unanswered -- so the stall's own precondition is
  ///     manufactured by that failure. At equal rank the stall replaced the
  ///     root cause about fifteen seconds later. 0x61 is the only pointer to
  ///     the reconnect_settle_time mitigation; "Pump not accepting pairing" is
  ///     the treatment, 0x61 is the prevention, and the operator needs the one
  ///     that recurs.
  ///   - Below SUBSCRIBE, because a link that reached the subscribe step got
  ///     far past where a refusing pump drops it (~2 s, before discovery
  ///     completes). A subscribe failure on such a link is a live fault of its
  ///     own, and the stall -- which cannot see how far the link got -- would
  ///     otherwise mask it with a diagnosis that has stopped applying.
  ///
  /// Released by any of the three: inbound data, a successful AUTH_CMPL, or the
  /// session reaching READY. Unlike AUTH it IS released by data, because data
  /// refutes it outright -- the claim is that nothing is getting through, and a
  /// notification is proof that something is.
  PAIRING_STALL = 3,
  /// A subscribe step that failed outright (subscribe_outcome.h). Outranks
  /// DATA: it names the cause of that silence, at the moment it happened,
  /// which is what issue #175 was about. Released like DATA, by any received
  /// notification.
  SUBSCRIBE = 4,
  /// An auth/encryption failure. Ranks highest: it is the only fault here that
  /// erases the bond, and a subscribe failure on an unauthenticated link is
  /// generally its consequence rather than a competing cause. Released by a
  /// successful AUTH_CMPL, or by the session reaching READY.
  AUTH = 5,
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
    case FailureHold::READY:
      return false;  // data arriving is the CONDITION this fires under, not a
                     // refutation of it -- releasing here would erase the
                     // diagnosis on every frame of the telemetry that is
                     // masking the problem
    case FailureHold::DATA:
      return true;
    case FailureHold::PAIRING_STALL:
      return true;  // a notification is proof something is getting through,
                    // which is exactly what the stall claims is not
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
    case FailureHold::READY:
      return false;  // nor does it prove the pump became usable
    case FailureHold::DATA:
      return false;  // a fresh bond does not prove the pump is answering
    case FailureHold::PAIRING_STALL:
      return true;  // the bond it said could not be formed has been formed
    case FailureHold::SUBSCRIBE:
      return false;
    case FailureHold::AUTH:
      return true;
  }
  return false;
}

/// True when the pump becoming READY -- the component's usable state, not the
/// GENI session's -- releases this hold.
///
/// A fourth release function rather than a fourth case in an existing one,
/// because it answers a question none of the others can. Inbound data, a
/// successful AUTH_CMPL and a ready session are all things that happen on the
/// way to being usable; this one is being usable. The readiness hold is the
/// only thing it releases, and it is the only thing that releases the readiness
/// hold -- everything else on the way there is a state that hold is meant to
/// survive.
inline bool failure_hold_released_by_pump_ready(FailureHold h) {
  switch (h) {
    case FailureHold::NONE:
      return false;
    case FailureHold::READY:
      return true;
    case FailureHold::DATA:
      return false;  // a usable pump that then goes silent is a new fault, and
                     // the watchdog will say so on its own schedule
    case FailureHold::PAIRING_STALL:
      return false;
    case FailureHold::SUBSCRIBE:
      return false;
    case FailureHold::AUTH:
      return false;
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
    case FailureHold::READY:
      return false;  // the session reaching READY is one of the states this
                     // hold exists to catch a link stuck PAST, so treating it
                     // as a release would disarm the fault in its own headline
                     // case
    case FailureHold::DATA:
      return false;  // READY does not prove the pump is answering
    case FailureHold::PAIRING_STALL:
      return true;   // a stalled link is dropped before discovery completes,
                     // so reaching READY is far more than it survives
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
