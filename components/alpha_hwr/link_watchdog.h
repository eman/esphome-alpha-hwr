#pragma once

#include <cstdint>

// Inbound-data watchdog for the GENI link.
//
// The session FSM tracks the *handshake*, not whether the pump is answering.
// Authentication is a pure scheduler chain: auth.cpp sends its packets on
// timers and calls the completion callback 1200 ms later (150 + 100 + 250 + 200
// + 500) without ever inspecting a reply, so READY is reached whether or not a
// single byte came back.
//
// Notification subscription has the same shape. Of the six terminal paths
// through BLEConnectionManager::subscribe_to_notifications(), four return
// early and silently (no client, no service, no characteristic,
// esp_ble_gattc_register_for_notify failed), announcing nothing at all. The
// other two — CCCD write failed and CCCD write succeeded — BOTH fall through to
// subscribed_callback_(), so a failed CCCD write is indistinguishable from a
// good one. The asynchronous ESP_GATTC_WRITE_DESCR_EVT status is not a path
// through this function at all: it is a case in handle_gattc_event() that only
// logs, and it necessarily arrives after subscribed_callback_() has fired, so
// it could not gate anything even if it wanted to.
//
// So a link whose GATT writes succeed but whose notifications never arrive
// reports a healthy session forever. Worse, it is self-sustaining: the Pump
// Link Status ladder's first rung is session_.is_ready(), which refreshes
// link_last_open_ms_ on every evaluation, so the "Unreachable" rung below it
// can never be reached. The user sees Connected + Pairing on, Pump Ready off,
// every sensor frozen at its last value, and a control-cache retry every 5 s
// that never completes. Nothing in the component recovers from this, because
// recovery is driven by BLE disconnection callbacks and the BLE link is fine.
//
// The watchdog below is a liveness check rather than a handshake gate, which
// is deliberate:
//
//   - It does not block READY. Gating READY on "a notification arrived during
//     auth" appears to hold on this pump — control-mode notifications are
//     received during the handshake, which is why alpha_hwr.cpp does not
//     publish a default control mode at setup — but that rests on a single
//     specimen, and no capture or fixture in this repo pins the timing, so it
//     is an observation rather than a measured margin. A pump variant that
//     stayed quiet until first polled would never become ready at all. Failing
//     open and recycling a proven-dead link is the safer trade.
//   - One timer covers every cause: the undetected CCCD failure, the four
//     silent early returns, and a link that goes deaf mid-session. Whatever the
//     reason, "connected and heard nothing" is the observable.
//   - The remedy is a forced BLE disconnect, because that is what the existing
//     recovery machinery listens to (ble_connection_manager.cpp already does
//     this for a bonded reconnect whose encryption failed). Transitioning the
//     session to ERROR instead would only swap a falsely-ready node for a
//     permanently stuck one.
//
// Sizing: on a READY link the component polls five telemetry registers plus the
// schedule every 10 s (AlphaHwrComponent::update()), so the healthy
// inter-notification gap is bounded by the poll interval, not by pump
// behaviour. In steady state the 60 s default therefore tolerates five missed
// poll cycles and acts on the sixth.
//
// The worst case is the handshake, not steady state, because the window is
// timed from connection-open and update() is a free-running poller the
// handshake does not synchronise with: 500 ms post-connect + 3 x 1000 ms
// discovery retries + 2000 ms stabilize + ~1200 ms auth chain = 6.7 s to READY,
// then up to 10 s to the next poll and 500 ms to its schedule read — 17.2 s
// worst case to first inbound data, leaving ~43 s of slack. Measured on
// hardware: open to READY was 5.90/6.17/5.94 s across three reconnects.
//
// That margin cannot be eroded by configuration: the interval is fixed at
// PollingComponent(10000) in the constructor, and `update_interval` is not in
// the component's schema — setting it is rejected as an invalid option rather
// than silently widening the healthy gap past the watchdog budget. If that ever
// becomes configurable, this default has to be derived from it instead.
//
// Two costs are accepted knowingly:
//
//   - Against a pump that is *permanently* deaf this recycles rather than
//     repairs: each cycle is ~60 s deaf-but-READY, then ~6 s to reconnect and
//     reach READY again. So the link status still reads "Connected" for most of
//     the time, and both link text sensors flap once per cycle. The watchdog's
//     purpose is to recover a link that CAN recover; it does not make a dead
//     pump legible, and it is not a substitute for verifying the handshake.
//   - Each recycle re-enters the encryption-on-open path on a bonded pump, so a
//     deaf link now takes one more run at the post-boot window where an
//     encryption request can fail with 0x61 and erase the bond (issue #14),
//     where before it took none. The default 2 s reconnect settle window does
//     arm on this disconnect, which is what that window exists for.
//
// One limit worth knowing: force_disconnect() closes the GATT connection, which
// tears down the ACL only if no other client holds it. If something else on the
// node is connected to the same pump, the close can complete without a
// DISCONNECT event, leaving the session connected and the watchdog re-firing
// once per budget with no recovery. That is a pre-existing property of the
// disconnect path (the encryption-failure teardown shares it), not new here.

namespace esphome {
namespace alpha_hwr {

/// True when the link should be torn down for lack of inbound data.
///
/// @param connected       Session is in any non-IDLE, non-ERROR state
///                        (Session::is_connected()). The window is timed from
///                        connection-open, not from READY, so the handshake
///                        paths that never reach READY are covered too.
/// @param now_ms          Current millis().
/// @param last_inbound_ms millis() at the last received notification, or at
///                        connection-open if none has arrived yet.
/// @param timeout_ms      Budget; 0 disables the watchdog entirely.
///
/// The elapsed test is unsigned subtraction, which is correct across the
/// ~49-day millis() rollover: (now - last) wraps to the true elapsed time.
/// Comparing the two timestamps directly would not.
inline bool link_data_timeout_expired(bool connected, uint32_t now_ms, uint32_t last_inbound_ms,
                                      uint32_t timeout_ms) {
  if (!connected || timeout_ms == 0)
    return false;
  return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;
}

}  // namespace alpha_hwr
}  // namespace esphome
