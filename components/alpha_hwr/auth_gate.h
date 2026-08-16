#pragma once

#include <cstddef>
#include <cstdint>

// Whether the GENI handshake may leave a stage, decided from the pump's
// replies rather than from a stopwatch alone.
//
// The defect this exists for (issue #174). auth.cpp sends ten packets on
// timers and calls its completion callback 1200 ms later. It passes no command
// callback to Transport::send_command(), so the transport takes the "no
// response expected" branch, pops each command immediately, and every reply
// the pump sends reaches the telemetry parser instead of the handshake -- the
// Class 10 ones decoded as passive telemetry, the Class 2, 5 and 11 ones
// discarded as "Not Class 10". The Authentication object sees none of it.
//
// The pump is not silent. Two independent captures on the reporter's specimen
// show a class-matched reply behind every one of the ten packets: three
// 11-byte Class 2 replies to stage 1, five 22-byte Class 10 replies to
// stage 2, and a 9-byte Class 5 and 9-byte Class 11 reply to stage 3. Ten
// packets, ten replies, in both logs.
//
// The four latencies the capture actually yields, each from a send to the
// first log line reporting its reply: 51 ms (stage 1 packet 2), 188 ms
// (stage 2 packet 1), and 150 and 204 ms (stage 3). The issue's "about 40 ms
// behind each" characterises stage 1 and does not generalise. Read them as a
// spread, not as measurements -- AUTH_GATE_MAX_WAITS below records why they
// are looser than they look, and why the largest latency in the capture is in
// fact unknown.
//
// **What the capture does NOT show is a stage being walked past.** The issue
// reads it as one -- "stage 2 was declared complete at 14.108 while the reply
// to its fifth packet arrived at 14.189" -- and that inference is wrong, which
// matters enough to record because this change was very nearly justified by
// it. `14.108` is the log line, not the transition:
//
//     ESP_LOGD(TAG, "Stage 2 complete, waiting 200ms before Stage 3");
//     scheduler_callback_(200, [...]{ this->stage3_extensions(); });
//
// The transition is 200 ms behind that line, and the capture confirms it --
// `[16:24:14.312] Stage 3: Sending extension packets`, 204 ms later. So the
// stage-2 boundary was at 14.312 and the fifth reply landed at 14.189, i.e.
// **123 ms inside it**. Run against that capture the gate below changes
// nothing: every stage evaluates fully answered at its floor and the handshake
// is timing-identical to the code it replaces.
//
// The case for it is therefore the issue's first argument rather than its
// second, and that argument does not need the race: the delays trace to the
// reference client's sleeps (`Python uses 0.1s` / `0.2s` / `0.5s`), so what
// they encode is one specimen's timing on one firmware. The measured margin at
// the tightest boundary is 123 ms. A firmware update, a different pump
// generation, a busier pump or a congested link all move it, nothing today
// reports it if it goes negative, and advancing on a matched reply is
// self-correcting at whatever speed the pump actually runs. This is
// prospective robustness, bought at no cost to the answered case -- not the
// repair of an observed failure.
//
// ---------------------------------------------------------------------------
// Why a gate rather than a rewrite
//
// The obvious fix -- pass a command callback and let the transport match the
// replies -- does not work, and the reason is worth recording so it is not
// attempted again. Of the four reply classes, the transport's matcher can
// match exactly one, and matching that one causes a regression:
//
//   - Class 2 (stage 1) and Class 5 / Class 11 (stage 3): the awaiting-command
//     path in try_dispatch_response() only matches Class 10 (`data[4] == 0x0A`)
//     plus the Class 3/7 wildcard; Class 2 falls out of the first, and the
//     9-byte stage-3 replies are refused earlier still by the `len < 11` guard.
//     Both would time out with their answers in hand.
//   - Class 10 (stage 2) DOES match, via the wildcard path -- and that is the
//     problem. A matched command response is consumed: try_dispatch_response()
//     returns true and packet_callback_ never runs, so the control-mode
//     notification the pump sends during stage 2 would stop being decoded and
//     published. That notification is why the component publishes no default
//     control mode at setup (link_watchdog.h).
//
// So the replies are observed, not consumed: Transport::set_frame_observer()
// hands every CRC-valid frame to the handshake on its way past, and dispatch
// is unaffected.
//
// ---------------------------------------------------------------------------
// What the gate does and does not do
//
// It is a floor-and-ceiling, not a replacement clock:
//
//   - **The existing delay is a floor.** A stage transition never happens
//     earlier than it does today. The delays exist to give the pump time to
//     process; sending sooner because a reply happened to be quick would be a
//     new risk taken for no benefit.
//   - **A reply is what releases the floor.** At the floor the gate asks
//     whether the stage's packets have been answered. If they have, it
//     proceeds; if not, it waits in short ticks. That is the self-correcting
//     half: on a slower pump, a busier link or a later firmware, the handshake
//     stretches to fit instead of walking past an unanswered stage.
//   - **The ceiling fails open.** After the ceiling the gate proceeds anyway,
//     with the shortfall logged. A pump variant that answers nothing still
//     authenticates exactly as it does today, only later. This is the same
//     trade link_watchdog.h made when it declined to gate READY on received
//     data: the evidence that this pump answers is two logs from one specimen,
//     which is enough to justify *waiting* for a reply and not enough to
//     justify *requiring* one.
//
// Only stage transitions are gated, not the repeats inside a burst. The burst
// is a burst on purpose, its repeats are the same packet, and the race in the
// capture is at the boundary -- gating each repeat would add three states and
// address nothing observed.
//
// What it deliberately does not do is decide whether a step was *rejected*.
// The 9- and 11-byte replies sat below the packet-dump threshold, so the
// capture establishes their framing and class but not their payload, and
// whether this protocol can even express a refusal at those lengths is
// unsettled. Counting a reply proves the pump is listening on that class; it
// is not read as an ACK. (The dump threshold in transport.cpp is lowered in
// the same change, so the next capture can settle it.)

namespace esphome {
namespace alpha_hwr {
namespace core {

/// Start byte of a pump-to-client frame. 0x27 is the client-to-pump direction
/// and is echoed on this link, so counting it would let our own request stand
/// in for the answer to itself.
constexpr uint8_t AUTH_REPLY_FRAME_START = 0x24;

/// How long a gate waits past the floor, expressed as a tick count rather than
/// a duration so the decision needs no clock (the caller owns the scheduler,
/// and the host test drives it). Ten ticks of AUTH_GATE_POLL_MS.
///
/// Sizing, and the honest limits of it. The four latencies quoted above are
/// the only ones the capture yields, and 500 ms is about 2.5x the largest of
/// them. Three caveats, because a bare "2.45x the worst observed" would be
/// three different kinds of wrong:
///
///   - **The reply that motivates the change is not among them.** The 123 ms
///     margin comes from the fifth stage-2 packet's reply at 14.189, and its
///     latency cannot be derived: the capture timestamps the first of the five
///     sends, not the fifth. The floor puts it at >= 131 ms; distributing the
///     observed 13.300 -> 14.108 span evenly over the five hops puts it near
///     243 ms, i.e. plausibly worse than any figure above. So the largest
///     latency in this capture is unknown, and 204 ms is a lower bound on it.
///   - **The four are not commensurable.** They are measured to whichever
///     log line the issue happened to quote for each stage: two transport
///     lines that are consecutive statements in the same block, and a
///     telemetry line further downstream still -- the capture shows 8-12 ms
///     between the transport and telemetry lines for one frame. The spread
///     across stages is real; the precision is not.
///   - **The ratio is against the wrong quantity anyway.** The ceiling bounds
///     waiting *past the floor*, not latency *from the send*, so what a stage
///     actually tolerates is floor + 500 ms -- 700 ms at stage 2.
///
/// What the number rests on is therefore the shape and not the decimal: the
/// observed latencies run from ~50 ms to ~200 ms, the unobserved one is
/// bounded well above that, the ceiling is a few times the largest of them,
/// and exceeding it costs nothing but the fail-open path.
///
/// Three gates puts the handshake's worst case at 1200 + 1500 = 2700 ms of
/// scheduled delay against the 1200 ms it takes today, which the inbound-data
/// watchdog's budget absorbs -- see the sizing note in link_watchdog.h,
/// updated for this.
///
/// **Scheduled delay, not wall clock**, and the capture says these run long:
///
///   - Stage 2's five 50 ms hops: 250 ms scheduled, 13.300 -> 14.108
///     measured. **3.2x.**
///   - Stage 1's last hop plus the 100 ms stage-1 tail: 200 ms scheduled,
///     13.024 -> 13.300 measured. **1.4x.**
///   - The 200 ms stage-2 tail: 14.108 -> 14.312 measured. **1.02x.**
///
/// Three measurements spanning 1.02x to 3.2x, which is not a distribution
/// anything can be sized from, and this note does not pretend otherwise. Take
/// the worst: a 500 ms ceiling is then ~1.6 s of real time and the 2700 ms
/// worst case ~8.6 s -- still an order of magnitude inside the 60 s watchdog
/// budget, which is the property that has to hold. The host tests measure
/// summed scheduled delay and never elapsed time, so neither figure is a
/// wall-clock promise.
///
/// Two corrections are folded into the above, both from adversarial review.
/// The 3.2x figure was retracted once as "misadded" and is restored: 250 ms is
/// the correct scheduled delay for that span (the 200 ms tail lies outside
/// it), so the arithmetic was right and only the prose describing it was
/// wrong. Its replacement -- that the 808 ms is mostly the transport's 50 ms
/// send pacing -- was wrong on mechanism: send_command() only queues, all
/// pacing lives in Transport::loop(), and the span is bracketed by two
/// auth.cpp log lines on a set_timeout chain that does not wait for writes. It
/// is scheduler slop, and the 1.02x tail is the outlier rather than the rule.
constexpr uint8_t AUTH_GATE_MAX_WAITS = 10;

/// Tick length for the wait above.
constexpr uint32_t AUTH_GATE_POLL_MS = 50;

/// What a gate decides. PROCEED_UNANSWERED is separated from PROCEED_ANSWERED
/// because the two mean opposite things about the link even though both
/// advance: one is a pump that answered, the other is the ceiling expiring on
/// a pump that did not, which is worth a warning. The first of those fires
/// 750 ms into the handshake, the earliest anything here reports a stage going
/// unanswered; the summary in Authentication::complete() follows at 2700 ms.
/// Both are measured from handshake start, while the watchdog's 60 s runs from
/// connection-open -- see complete() for the head start that results.
enum class AuthGate : uint8_t {
  PROCEED_ANSWERED,
  WAIT,
  PROCEED_UNANSWERED,
};

/// Decide whether a stage may be left.
///
/// @param replies_seen  Replies counted for this stage so far.
/// @param packets_sent  Packets the stage sent.
/// @param waits_used    Ticks already spent waiting past the floor.
/// @param max_waits     Ceiling, in ticks.
///
/// `>=` rather than `==` on the reply count: a duplicate or an unrelated frame
/// of the same class inflates the count, and treating that as "not yet
/// answered" would hold the gate open to its ceiling for no reason. The
/// consequence of an inflated count is bounded by the floor -- the gate cannot
/// advance a stage early, only decline to wait longer than it does today.
inline AuthGate auth_stage_gate(uint8_t replies_seen, uint8_t packets_sent,
                                uint8_t waits_used, uint8_t max_waits) {
  if (replies_seen >= packets_sent)
    return AuthGate::PROCEED_ANSWERED;
  if (waits_used < max_waits)
    return AuthGate::WAIT;
  return AuthGate::PROCEED_UNANSWERED;
}

/// Which handshake stage a reply of this class answers, or 0 for none.
///
/// Stages are 1-based so 0 can mean "not a handshake reply". Stage 3 sends two
/// packets of different classes (EXT_1 is Class 0x05, EXT_2 is Class 0x0B) and
/// both count toward the same stage.
inline uint8_t auth_stage_for_reply_class(uint8_t class_byte) {
  switch (class_byte) {
    case 0x02:
      return 1;  // legacy magic burst
    case 0x0A:
      return 2;  // Class 10 unlock burst
    case 0x05:
    case 0x0B:
      return 3;  // extension packets
    default:
      return 0;
  }
}

/// Does this frame count as a reply to stage @p current_stage's packets?
///
/// Two conditions beyond the class match, both of which exist to stop a frame
/// being credited to a stage it cannot belong to:
///
///   - It must be a response frame (0x24). See AUTH_REPLY_FRAME_START.
///   - Its stage must not be ahead of the stage in flight. Class 0x0A is also
///     the class of ordinary telemetry, and this pump pushes control-mode
///     notifications unprompted -- link_watchdog.h records that they arrive
///     *during* the handshake, which is why the component publishes no default
///     control mode at setup. One of those landing during stage 1 is not an
///     answer to a stage-2 packet that has not been sent yet.
///
///     (An earlier version of this note justified the test with "on a
///     re-authentication over a live session". There is no such path:
///     `authenticate()` has exactly one call site, the post-subscribe timer,
///     and `start()` refuses re-entry while running. The unsolicited
///     notification is the real reason and it is enough on its own.)
///
///     A telemetry frame arriving *during* stage 2 is still miscountable, and
///     is left so knowingly: the floor bounds the damage to today's behaviour,
///     and no field in the reply distinguishes them. That argument covers the
///     gate; where it does NOT reach is the zero-replies deaf-link report --
///     see Authentication::complete().
///
/// @param len is checked here rather than at the call site so that a runt can
/// never index past the class byte.
inline bool auth_frame_answers_stage(const uint8_t *data, size_t len,
                                     uint8_t current_stage) {
  if (data == nullptr || len < 5)
    return false;
  if (data[0] != AUTH_REPLY_FRAME_START)
    return false;
  const uint8_t stage = auth_stage_for_reply_class(data[4]);
  return stage != 0 && stage <= current_stage;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
