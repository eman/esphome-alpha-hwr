#pragma once

#include <cstdint>

// What happened when we tried to subscribe to GATT notifications, and what the
// caller should do about it.
//
// BLEConnectionManager::subscribe_to_notifications() has six terminal paths and
// used to discard the answer on five of them (issue #175). Four returned early
// after logging -- no client, no service, no characteristic,
// esp_ble_gattc_register_for_notify() failed -- announcing nothing to anyone.
// Since subscribed_callback_() is the only thing that advances the session out
// of SUBSCRIBING, those four parked it there. The fifth, a CCCD write that
// failed synchronously, fell through to the same callback as success, so a
// failed subscribe was indistinguishable from a good one and the session went
// on to READY.
//
// The link watchdog (link_watchdog.h) already catches all of them, and that
// design is not being revisited here: one timer covers every cause, including
// a link that goes deaf later, which no return code can see. What it cannot do
// is say *which* thing failed, and it cannot say anything for 60 s. In five of
// the six cases the component knows at the moment it happens. This header is
// that knowledge, in a form the caller can act on.
//
// It lives in its own header rather than inline in the .cpp for the reason
// link_watchdog.h and initial_read_retry.h do: ble_connection_manager.cpp is
// compiled by no host test, so anything expressed there is unverifiable. The
// decision is the part worth pinning, so the decision is what is extracted.

namespace esphome {
namespace alpha_hwr {
namespace core {

enum class SubscribeOutcome : uint8_t {
  OK,                  // CCCD write accepted; notifications should flow
  NO_CLIENT,           // no BLE client object at all
  NO_SERVICE,          // the GENI service UUID was not in the parsed table
  NO_CHARACTERISTIC,   // the service was there, the characteristic was not
  REGISTER_FAILED,     // esp_ble_gattc_register_for_notify() returned nonzero
  CCCD_WRITE_FAILED,   // esp_ble_gattc_write_char_descr() returned nonzero
};

/// True when the subscribe did not succeed.
inline bool subscribe_failed(SubscribeOutcome o) {
  return o != SubscribeOutcome::OK;
}

/// True when this outcome leaves the session unable to advance on its own.
///
/// These are the four paths that return before subscribed_callback_(). Nothing
/// else advances the session out of SUBSCRIBING, so on these the link is
/// finished -- it will sit connected, idle and useless until the data watchdog
/// recycles it. Note the BLE supervision timeout does *not* rescue it: at 4 s
/// (conn_params.timeout = 400) it measures link-layer loss, and an idle-but-
/// healthy link keeps exchanging empty PDUs every connection interval, so it
/// never trips.
///
/// This deliberately does NOT trigger an immediate forced disconnect, which an
/// earlier version of this change did. The subscribe decision point is ~2-3 s
/// after connection-open, and a forced disconnect on a bonded pump re-arms in
/// ~2 s, so recycling here would run a ~6-8 s cycle against the watchdog's
/// ~66 s -- roughly a ninefold increase in passes through the encryption-on-
/// open window where a failure can erase the bond (issue #14). Naming the
/// cause is the safe half of this change and the half worth having; choosing
/// the recycle cadence belongs to the watchdog, which backs off.
inline bool subscribe_outcome_blocks_session(SubscribeOutcome o) {
  return o == SubscribeOutcome::NO_CLIENT ||
         o == SubscribeOutcome::NO_SERVICE ||
         o == SubscribeOutcome::NO_CHARACTERISTIC ||
         o == SubscribeOutcome::REGISTER_FAILED;
}

/// True when this outcome should be latched as a HELD fault reason.
///
/// The blocking outcomes are terminal for the session: nothing else will
/// produce a reason, so theirs must survive the reconnect that follows.
///
/// CCCD_WRITE_FAILED is excluded, and not for symmetry. One documented
/// synchronous failure of esp_ble_gattc_write_char_descr is ESP_ERR_INVALID_
/// STATE when the connection is already gone -- i.e. the write failed *because*
/// the link dropped. Holding "CCCD write failed" over that would relabel a link
/// loss as a subscribe fault and suppress the real disconnect reason for the
/// whole reconnect episode, which is the exact inversion of what this change is
/// for. It is still recorded; it just does not outrank a genuine reason.
inline bool subscribe_outcome_holds_fault(SubscribeOutcome o) {
  return subscribe_outcome_blocks_session(o);
}

/// A short, specific fault string for the Pump Link Fault surface.
///
/// The point of the whole change: "Subscribe: no characteristic" names a cause
/// an operator can act on, where the watchdog's "No data from pump (60s)" names
/// only a symptom shared by every failure in this file.
inline const char *subscribe_outcome_to_string(SubscribeOutcome o) {
  switch (o) {
    case SubscribeOutcome::OK:
      return "Subscribe: ok";
    case SubscribeOutcome::NO_CLIENT:
      return "Subscribe: no BLE client";
    case SubscribeOutcome::NO_SERVICE:
      return "Subscribe: service not found";
    case SubscribeOutcome::NO_CHARACTERISTIC:
      return "Subscribe: characteristic not found";
    case SubscribeOutcome::REGISTER_FAILED:
      return "Subscribe: register-for-notify failed";
    case SubscribeOutcome::CCCD_WRITE_FAILED:
      return "Subscribe: CCCD write failed";
  }
  return "Subscribe: unknown";
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
