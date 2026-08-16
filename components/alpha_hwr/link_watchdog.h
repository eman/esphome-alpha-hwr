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

/// Default ceiling for the backoff below: one hour.
static const uint32_t LINK_DATA_TIMEOUT_BACKOFF_CAP_MS = 3600000u;

/// The window to use for the next cycle, after a recycle produced no data.
///
/// Without this, a link that stays deaf is recycled every ~66 s forever —
/// roughly 1,300 passes a day, indefinitely (issue #176). No single recycle is
/// wrong; the problem is the unbounded repetition, and three costs accumulate
/// from it:
///
///   - **Bond loss.** Each recycle re-enters the encryption-on-open path on a
///     bonded pump, so it takes one more run at the post-boot window where an
///     encryption request can fail with 0x61 and erase the bond (issue #14).
///     1,300 runs a day at that window is the risk, not any one of them.
///   - **Anything that leaks in the reconnect path.** The audit's P1 closure
///     leaks measured 150–260 KB/hour under exactly this flapping pattern, OOM
///     in 1–3 hours. Those are fixed, so this is not live — but it is what this
///     codebase costs if a leak is ever reintroduced there and the reconnects
///     never stop.
///   - Hammering a device already in an odd state, on general principle.
///
/// Doubling with a ceiling keeps recovery automatic while bounding all three: a
/// link that can recover still does on the first or second try, and a
/// permanently deaf one drops from ~1,300 recycles a day to about 28. The
/// caller resets to the configured value on a notification received while the
/// session is READY, so a pump that comes back hours later is served by the
/// configured budget again rather than by an hour-wide window. READY-gated
/// rather than on any notification, because a deaf pump still answers the
/// handshake: resetting on those frames would clear the window once per session
/// forever and the backoff would never engage (see the reset site in
/// alpha_hwr.cpp). The widened window therefore also governs the next
/// connection's handshake, which is why a recycle is not the only way for the
/// gap statistic to read above the configured budget.
///
/// @param current_ms The window that just expired.
/// @param cap_ms     Ceiling; the window never grows past this.
///
/// A disabled watchdog (0) stays disabled — backing off from "never" is
/// meaningless, and returning anything else would silently switch it on. A
/// window already at or past the cap is returned unchanged rather than
/// shrunk, so configuring a budget larger than the ceiling is not quietly
/// overridden.
inline uint32_t link_data_timeout_next(uint32_t current_ms, uint32_t cap_ms) {
  if (current_ms == 0)
    return 0;
  if (current_ms >= cap_ms)
    return current_ms;
  const uint32_t doubled = current_ms * 2u;
  // Doubling a window past 2^31 ms wraps. Unreachable from any sane
  // configuration, but the clamp costs one comparison and the alternative is a
  // watchdog that silently fires almost immediately.
  if (doubled < current_ms)
    return cap_ms;
  return doubled > cap_ms ? cap_ms : doubled;
}

/// Running maximum of the quiet intervals the watchdog above measures.
///
/// The statistic exists to choose the `data_timeout` default from what real
/// installations do rather than from a constants calculation (issue #176), and
/// that only works if it samples exactly what the budget governs. The
/// watchdog's clock starts at connection-open and is re-armed by every inbound
/// notification, so those are the intervals — all of them. Two consequences,
/// both of which the first cut of this got wrong:
///
///   - **The open-to-first-notification interval counts.** It is inside the
///     window, and the sizing note above makes it the *binding* case: 17.2 s
///     worst case against a 60 s budget, where steady state is bounded by our
///     own 10 s poll. Excluding it as "the handshake, not the pump's cadence"
///     left the statistic structurally unable to report the case the default is
///     tightest against. Including it costs nothing visible in practice: the
///     interval is a few seconds (the 5.90/6.17/5.94 s measured above is open
///     to READY, and this file records that control-mode notifications arrive
///     *during* the handshake, so first-inbound is earlier than that: 4.9 s and
///     5.2 s on two bench boots), while the running maximum reaches the 10 s
///     poll interval within the first poll cycle of the first connection and
///     stays there — 9.5/9.6 s measured, against a 60 s budget.
///   - **An interval that ends in a recycle counts too**, which is what
///     on_recycle() is for. Without it the sample is censored at exactly the
///     threshold being validated: the watchdog re-arms and disconnects, so no
///     notification ever closes that interval and every excursion past the
///     budget is discarded. The maximum then asymptotes to just under the
///     budget whatever the pump does, and "never above 12 s in a month" reads
///     as "60 s was comfortable" when what it means is "no quiet period between
///     12 s and 60 s ended on its own" — the opposite conclusion.
///
/// An interval is closed by whatever ends it: a notification, a recycle, or the
/// link dropping for some reason of its own. Time between that drop and the
/// next open is not sampled, because the watchdog is not running then either.
///
/// What a reading means, and what it does not:
///
///   - It is a lower bound on how long the link went quiet. When the interval
///     was cut short by a recycle or a drop, how long the quiet would have
///     lasted is unknowable.
///   - A reading above the configured `data_timeout` does NOT by itself mean a
///     recycle happened. The watchdog runs against the window currently in
///     force, which the backoff widens after a recycle and which is only reset
///     by a notification received while READY — so a 90 s interval that ended
///     on its own under a widened 120 s window reads the same as one that hit a
///     60 s ceiling. `link_recycles` and the fault sensor are what distinguish
///     them; this number alone cannot.
class LinkGapSampler {
 public:
  /// Connection open. The watchdog's clock starts here, so this one does too;
  /// it opens an interval without closing one, since what came before the open
  /// was not a quiet link but no link.
  void on_open(uint32_t now_ms) {
    this->last_ms_ = now_ms;
    this->armed_ = true;
  }

  /// Inbound notification: closes an interval and opens the next.
  void on_inbound(uint32_t now_ms) { this->sample_(now_ms); }

  /// The watchdog fired: record the interval it gave up on, and re-arm with the
  /// same stamp check_link_liveness_() re-arms the window with.
  void on_recycle(uint32_t now_ms) { this->sample_(now_ms); }

  /// The link dropped for a reason other than the watchdog — supervision
  /// timeout, pump power loss, the encryption-failure teardown. The watchdog
  /// was timing that interval against its budget right up to the drop, so
  /// discarding it censors the sample the same way dropping the recycle sample
  /// did, just at a threshold nobody configured: a link that routinely goes
  /// quiet for 45 s and then drops would report only its steady-state cadence.
  ///
  /// Disarms, so a disconnect with no open before it cannot sample the downtime
  /// since the previous session. A failed connection attempt that reports a
  /// disconnect without an open is the case that would otherwise record the
  /// entire gap between sessions as if the link had been up and silent for it —
  /// an inflated reading, which argues for a longer timeout and is exactly as
  /// wrong as the deflated one this class exists to fix.
  void on_disconnect(uint32_t now_ms) {
    if (!this->armed_)
      return;
    this->sample_(now_ms);
    this->armed_ = false;
  }

  uint32_t max_ms() const { return this->max_ms_; }

 private:
  void sample_(uint32_t now_ms) {
    // Unsigned subtraction, correct across the ~49-day millis() rollover for
    // the same reason link_data_timeout_expired() is.
    const uint32_t gap = static_cast<uint32_t>(now_ms - this->last_ms_);
    if (gap > this->max_ms_)
      this->max_ms_ = gap;
    this->last_ms_ = now_ms;
  }

  uint32_t last_ms_{0};
  uint32_t max_ms_{0};
  bool armed_{false};
};

}  // namespace alpha_hwr
}  // namespace esphome
