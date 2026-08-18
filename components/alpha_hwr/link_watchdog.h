#pragma once

#include <cstddef>
#include <cstdint>

// Inbound-data watchdog for the GENI link.
//
// The session FSM tracks the *handshake*, not whether the pump is answering.
// READY is still reached whether or not the pump replies -- but the reason is
// no longer that nothing looks.
//
// This used to read "a pure scheduler chain ... 1200 ms later (150 + 100 + 250
// + 200 + 500) without ever inspecting a reply". That stopped being true when
// issue #174 made the opening sequence reply-driven. Stages 1 and 3 now send
// matched reads and advance when the transport either matches the reply or
// gives up on it, and complete() reports how many of the five were answered.
// What survives is the *policy*: an unanswered read advances the sequence
// exactly as an answered one does, deliberately, because two logs from one
// specimen justify waiting for a reply and not requiring one. So a deaf pump
// still reaches READY, and this watchdog is still the thing that notices.
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
// handshake does not synchronise with. The fixed part is 500 ms post-connect +
// 3 x 1000 ms discovery retries + 2000 ms stabilize = 5.5 s, then up to 10 s to
// the next poll and 500 ms to its schedule read = 10.5 s after READY.
//
// The opening sequence is what varies, and since issue #174 it varies with the
// pump rather than with a constant:
//
//   answered      450 ms of stage-2 timers + 5 round trips.  Measured 1.33 s on
//                 the bench specimen, whose replies average ~175 ms.
//   unanswered    450 ms + 5 x REPLY_TIMEOUT_MS (1000 ms) = 5.45 s. This is the
//                 case the watchdog exists for, so it is the one that sizes it.
//   callback lost 450 ms + SEQUENCE_BACKSTOP_MS (15 s) = 15.45 s. Only reachable
//                 if the transport drops a queued callback -- see auth.h -- and
//                 quoted because it is the true ceiling, not because it is
//                 expected.
//
// So: 5.5 + 5.45 + 10.5 = 21.5 s worst case to first inbound data against the
// 60 s default, leaving ~38 s of slack; and 5.5 + 15.45 + 10.5 = 31.5 s even
// with the backstop firing, leaving ~28 s. Both fit, which is the property this
// note exists to establish -- the previous arithmetic reached 17.2 s from an
// auth chain that no longer takes 1200 ms.
//
// Measured on hardware: the opening sequence itself takes 1.33 s on the bench
// specimen (2026-08-17), start of handshake to completion, with all five reads
// answered. That is the only segment issue #174 changed, and it is the segment
// quoted above.
//
// The open-to-READY figures previously recorded here -- 5.90/6.17/5.94 s across
// three reconnects -- predate the change and are NOT re-measured. They remain
// indicative rather than current: the fixed 5.5 s before the handshake is
// unaffected, so the expected shift is roughly the difference between the old
// 1.2 s constant and whatever the pump now takes to answer. Re-measure before
// relying on them.
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

/// Thresholds the tail histogram in LinkGapSampler counts against, in ms.
///
/// A running maximum is one extreme value. It cannot answer the question the
/// `data_timeout` default actually turns on -- "how many times a day would a
/// budget of T have fired?" -- and one freak interval pins it for the rest of
/// the boot. These counters answer it directly, one point on the survival curve
/// each, because "intervals longer than T" IS the number of times a budget of T
/// would have expired (see the strict `>` in record_(), which mirrors
/// link_data_timeout_expired()).
///
/// The ladder is chosen against the sizing note at the top of this file rather
/// than by round numbers:
///
///   15 s, 20 s   One missed poll cycle. Steady state is bounded by our own
///                fixed 10 s poll -- update_interval is deliberately not in the
///                component's schema -- so anything past ~20 s means a poll
///                response did not arrive, not that the pump reports slowly.
///                15 s is also the instrument's own liveness check: after a day
///                an all-zero histogram is indistinguishable from one that was
///                never wired up, and a nonzero 15 s counter is the cheapest
///                proof the whole path works.
///   30 s, 45 s   Straddle the handshake worst cases: 21.5 s to first inbound
///                data, 31.5 s with the sequence backstop firing. A default
///                below these would recycle links that were merely connecting.
///   60 s         The value under test. Its counter is what says whether the
///                shipped default was ever close to firing.
///   90 s         Above the default deliberately. Without a rung up here the
///                data can say "60 s would have fired N times" but cannot say
///                whether those excursions were 61-89 s -- in which case a
///                longer default covers them -- or minutes long, in which case
///                they are genuine link deaths that SHOULD recycle and the
///                default is not the problem. Those two readings argue in
///                opposite directions, so the rung is load-bearing rather than
///                decorative. It needs `data_timeout` raised past it to be
///                observable at all; see the censoring note on LinkGapSampler.
///
/// Keep in step with __init__.py, which names the entities after these values.
/// The setters in alpha_hwr.h static_assert each index against the value its
/// name claims, so reordering this array is a compile error and a Python key
/// with no matching setter fails the build -- neither can end up silently
/// labelling a counter with the wrong threshold, which is a mistake nothing in
/// a reading would reveal.
static constexpr uint32_t LINK_GAP_THRESHOLDS_MS[] = {15000u, 20000u, 30000u,
                                                      45000u, 60000u, 90000u};
static constexpr size_t LINK_GAP_BUCKETS =
    sizeof(LINK_GAP_THRESHOLDS_MS) / sizeof(LINK_GAP_THRESHOLDS_MS[0]);

/// True when `data_timeout` is small enough that the top thresholds above
/// cannot be observed at all.
///
/// An interval is closed by the watchdog at whatever window is in force, on the
/// first 1 s tick strictly past it, so with budget B no sample can exceed
/// B + ~1 s unless a drop ended it instead. Two different failures follow, and
/// the comparison is `<=` because both of them matter:
///
///   - A threshold ABOVE B reads a structural zero however badly the pump
///     behaves, which reads exactly like "the budget was never close" — the
///     conclusion this histogram exists to stop anyone reaching by accident.
///   - A threshold AT B is not zero, it is worse: the only samples that can
///     reach it are the ones the watchdog itself cut off, so the counter
///     silently changes meaning from "quiet intervals this long" to "recycles".
///     Those are different numbers and only one of them answers the question.
///
/// @param top_rung_ms The largest threshold actually configured. Every rung is
///                    independently optional, so this is NOT always the top of
///                    the ladder: a config declaring only `link_gaps_over_15s`
///                    is perfectly well served by a 60 s budget and must not be
///                    warned about the 90 s rung it never asked for. 0 means no
///                    rung is configured, which nothing can censor.
///
/// A disabled watchdog (0) is not censored either: with nothing recycling,
/// nothing truncates an interval.
inline bool link_gap_thresholds_censored(uint32_t data_timeout_ms,
                                         uint32_t top_rung_ms) {
  if (data_timeout_ms == 0 || top_rung_ms == 0)
    return false;
  return data_timeout_ms <= top_rung_ms;
}

/// How often the watched-time total may be published, in ms.
///
/// Unlike the counters, watched time advances on every notification — so a
/// plain change gate would emit a frame per API subscriber every 10 s forever,
/// which is the load shape that OOMs this node (issue #127). 300 s is Home
/// Assistant's short-term statistics bucket and its long-term statistics are
/// hourly, so nothing that consumes this value can resolve a finer cadence
/// anyway: the throttle costs no information at all.
static const uint32_t LINK_GAP_WATCH_PUBLISH_MS = 300000u;

/// The distribution of the quiet intervals the watchdog above measures: a
/// running maximum, cumulative counts of the intervals that exceeded each
/// LINK_GAP_THRESHOLDS_MS rung, and the time those intervals cover.
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
///     12 s and 60 s ended on its own" — the opposite conclusion. Measured, by
///     flashing the pre-fix build and this one against the same pump with
///     data_timeout forced to 5 s: five recycles, and the old build's maximum
///     read 2.6 s — apparent double headroom over a budget it had breached five
///     times. This one reads 6.0 s.
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
///
/// The counters and the watched-time total are fed from the same sample_(), so
/// they see exactly the intervals the maximum sees. That is why they live in
/// this class rather than beside it: what the two statistics share is not
/// arithmetic but the sampling *policy* argued above — which intervals count —
/// and a second class holding a second copy of that policy would diverge from
/// this one silently, in a direction no reading reveals. One sampling site
/// makes the divergence impossible rather than merely tested for.
///
/// Two things the counters add that a maximum cannot express:
///
///   - **`over_count(i)` is the number of times a `data_timeout` of
///     LINK_GAP_THRESHOLDS_MS[i] would have fired**, exactly, because the
///     comparison is the same strict `>` that link_data_timeout_expired() uses.
///     That equivalence is the whole point; it is what makes the counter a
///     decision input rather than a curiosity, and it is what the boundary test
///     pins from both sides.
///   - **`truncated()` is how far the reading can be trusted.** An interval
///     closed by a recycle or a drop did not end on its own, so it is a lower
///     bound, and a run with many of them has a tail that was cut off rather
///     than observed. Counting them turns that from an assumption into a
///     measurement — which matters because the failure this whole statistic
///     already suffered once was a censored reading that looked clean (the
///     2.6 s maximum against five breaches, above).
///
/// The counters are censored the same way, and worse, because the cutoff moves:
/// with budget B in force no sample can exceed B by more than one 1 s tick, and
/// the backoff widens B after each recycle. So every rung at or above the
/// configured budget reads a structural zero. A measurement run has to raise
/// `data_timeout` past the top rung — see link_gap_thresholds_censored(), which
/// is what the component warns from at setup.
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
  ///
  /// Arms rather than samples if no open preceded it. A notification with no
  /// live link behind it is not the end of a quiet interval — there was no link
  /// to be quiet — and measuring across the downtime would inflate the reading,
  /// which is the same error on_disconnect()'s armed_ guard exists to prevent.
  /// It matters more here than it did for the maximum alone: an inflated
  /// maximum is one number a reader already knows is a floor, while an inflated
  /// sample permanently increments the top counters and reads afterwards as a
  /// genuine multi-minute excursion.
  ///
  /// Self-arming rather than a bare early return, deliberately. No path today
  /// delivers a notification without a connection callback first; if one ever
  /// did, a bare guard would silently discard *every* sample of that session,
  /// which is worse than the inflation it fixes. Arming loses at most the
  /// session's first interval.
  ///
  /// on_recycle() below needs no such guard: the watchdog only fires while the
  /// session is connected, so it cannot run unarmed.
  void on_inbound(uint32_t now_ms) {
    if (!this->armed_) {
      this->last_ms_ = now_ms;
      this->armed_ = true;
      return;
    }
    this->sample_(now_ms, true);
  }

  /// The watchdog fired: record the interval it gave up on, then stop sampling
  /// this session.
  ///
  /// Disarming is what keeps one recycle from counting as two truncated
  /// intervals. check_link_liveness_() samples here and then calls
  /// force_disconnect(), which is asynchronous -- the DISCONNECT event lands a
  /// tick or two later and the disconnection callback calls on_disconnect().
  /// Left armed, that second call samples the ~1 s between the re-arm and the
  /// event and counts it as another truncated interval, so `link_gaps_truncated`
  /// reads about twice the number of recycles. Measured: open, one notification,
  /// one recycle, one disconnect gave truncated = 2.
  ///
  /// The interval that would be lost is the tail end of a link already being
  /// torn down, which is an artifact of the asynchronous close rather than a
  /// quiet period on a live link. Once the watchdog has given up, this session
  /// is over as far as the statistic is concerned.
  ///
  /// Not guarded on armed_ itself: the watchdog only fires while the session is
  /// connected, and if force_disconnect() produces no DISCONNECT event (the
  /// documented case where another client holds the ACL) it re-fires once per
  /// window, and those intervals are real and should be recorded.
  void on_recycle(uint32_t now_ms) {
    this->sample_(now_ms, false);
    this->armed_ = false;
  }

  /// The link dropped for a reason other than the watchdog — supervision
  /// timeout, pump power loss, the encryption-failure teardown. The watchdog
  /// was timing that interval against its budget right up to the drop, so
  /// discarding it censors the sample the same way dropping the recycle sample
  /// did, just at a threshold nobody configured: a link that routinely goes
  /// quiet for 45 s and then drops would report only its steady-state cadence.
  ///
  /// Measured the same way as the recycle case: polling suspended for 45 s on a
  /// live link, then the link dropped. The pre-fix build published nothing and
  /// stayed at its steady-state 9.5 s; this one recorded 53.0 s — the interval
  /// from the last notification to the drop, with the 60 s watchdog never
  /// involved.
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
    this->sample_(now_ms, false);
    this->armed_ = false;
  }

  uint32_t max_ms() const { return this->max_ms_; }

  /// Intervals longer than LINK_GAP_THRESHOLDS_MS[index], since boot.
  ///
  /// Out of range reads 0 rather than asserting: the only caller iterates to
  /// bucket_count(), and there is nowhere useful for an ESP32 to send an
  /// assertion. Pinned by a test so it does not read as dead code.
  uint32_t over_count(size_t index) const {
    return index < LINK_GAP_BUCKETS ? this->over_counts_[index] : 0;
  }

  /// Intervals closed by a recycle or a drop rather than by data, since boot.
  /// The trust check on everything above — see the class comment.
  uint32_t truncated() const { return this->truncated_; }

  /// Total length of every interval sampled, since boot.
  ///
  /// The sum of the intervals IS the time the watchdog was armed, so this is
  /// the denominator that turns a count into a rate, obtained without a second
  /// clock or a call site of its own. 64-bit because 32 bits of milliseconds
  /// wraps at 49.7 days and a measurement run is weeks: a wrap would read to
  /// Home Assistant as a counter reset and quietly discard the run.
  uint64_t watched_ms() const { return this->watched_ms_; }

  static constexpr size_t bucket_count() { return LINK_GAP_BUCKETS; }

  static constexpr uint32_t threshold_ms(size_t index) {
    return index < LINK_GAP_BUCKETS ? LINK_GAP_THRESHOLDS_MS[index] : 0;
  }

 private:
  /// @param closed_by_data True when a notification ended the interval; false
  ///                       when a recycle or a drop cut it short.
  void sample_(uint32_t now_ms, bool closed_by_data) {
    // Unsigned subtraction, correct across the ~49-day millis() rollover for
    // the same reason link_data_timeout_expired() is.
    const uint32_t gap = static_cast<uint32_t>(now_ms - this->last_ms_);
    if (gap > this->max_ms_)
      this->max_ms_ = gap;
    this->watched_ms_ += gap;
    if (!closed_by_data)
      this->truncated_++;
    // Strict `>`, matching link_data_timeout_expired(). A gap of exactly T did
    // not expire a budget of T, so it must not count as one — see the class
    // comment on why that equivalence is the point.
    //
    // The whole array every time, with no break on the first threshold the gap
    // does not reach. Six comparisons per ~10 s is free, while the break
    // version is silently wrong the day someone writes the thresholds out of
    // order — a bug traded for an optimisation nothing needs.
    for (size_t i = 0; i < LINK_GAP_BUCKETS; i++) {
      if (gap > LINK_GAP_THRESHOLDS_MS[i])
        this->over_counts_[i]++;
    }
    this->last_ms_ = now_ms;
  }

  uint32_t last_ms_{0};
  uint32_t max_ms_{0};
  bool armed_{false};
  // Since-boot totals: a reconnect must not clear them, or a flapping link
  // reports near-zero counts over a denominator of nothing.
  uint32_t over_counts_[LINK_GAP_BUCKETS]{};
  uint32_t truncated_{0};
  uint64_t watched_ms_{0};
};

}  // namespace alpha_hwr
}  // namespace esphome
