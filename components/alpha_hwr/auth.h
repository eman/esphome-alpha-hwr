#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <functional>

namespace esphome {
namespace alpha_hwr {
namespace core {

class Transport;

/**
 * @brief Sends the opening packet sequence a connection starts with.
 *
 * Ten packets in three stages:
 *
 *   Stage 1: the Class 2 identity read, sent 3x, each awaiting its reply
 *   Stage 2: the Class 10 operation-status read, sent 5x at 50 ms, then 200 ms
 *   Stage 3: the Class 5 then Class 11 INFO queries, each awaiting its reply
 *
 * **They are reads.** Not an authentication handshake, and not writes of any
 * kind: two GETs and two INFO queries, none of which addresses anything
 * writable. See the packet definitions at the bottom of this header for the
 * byte-level decode and for the in-tree evidence that fixes the encoding. The
 * class is still named Authentication and the packets still AUTH_*, which is
 * now a description of what they were believed to be rather than of what they
 * do.
 *
 * **The delays were transcriptions, not requirements.** Each was one of the
 * reference client's `asyncio.sleep()` values, carried across and then
 * documented as "timing requirements ... allows pump processing" without
 * anything having measured a pump. Worse, nothing here noticed whether a reply
 * ever arrived: send_packet() passed no callback, so all ten replies fell
 * through to the telemetry path, where the Class 10 ones were decoded as
 * passive notifications and the rest discarded for not being Class 10. A pump
 * that answered nothing and a pump that answered everything looked identical
 * from in here (issue #174).
 *
 * Stages 1 and 3 no longer work that way. Their packets go out as ordinary
 * matched reads and the next step is taken when the transport either matches
 * the reply or gives up waiting for it, so the pacing is the pump's rather than
 * a transcribed constant's -- a slower pump stretches the sequence instead of
 * being talked over, and a faster one is not waited on. Nothing is required to
 * answer: an unanswered read advances the sequence exactly as an answered one
 * does, and complete() says which happened.
 *
 * Stage 2 still runs on its timers, for a reason specific to its class that is
 * recorded at stage2_class10_burst().
 *
 * Reference: https://github.com/eman/alpha-hwr (Python implementation) and
 *   https://eman.github.io/alpha-hwr/reimplementation/ (protocol notes; the
 *   packet descriptions there carry the same misreading corrected below).
 */
class Authentication {
 public:
  /**
   * @brief Callback function type for completion notification
   * 
   * Called when authentication handshake completes successfully.
   * The session should transition to READY state after this callback.
   */
  using CompletionCallback = std::function<void()>;
  
  /**
   * @brief Callback function type for scheduling delayed tasks
   *
   * Still used by stage 2, and only by stage 2 -- see stage2_class10_burst().
   * Stages 1 and 3 are paced by the pump's replies and schedule nothing.
   *
   * @param delay_ms Delay in milliseconds
   * @param callback Function to call after delay
   */
  using SchedulerCallback = std::function<void(uint32_t delay_ms, std::function<void()> callback)>;

  /// How long a matched read waits for its reply before giving up on it.
  ///
  /// Round trips reported on one pump are 54-108 ms at normal log level, and up
  /// to 276 ms at VERBOSE, where roughly 4 ms per emitted log line dominates the
  /// interval (issue #174). A second pump averages ~175 ms at INFO, measured
  /// across the five reads of a bench handshake (2026-08-16). A second is ~5.7x
  /// that average and ~3.6x the worst VERBOSE figure.
  ///
  /// The spread between those two specimens -- 54 to 175 ms on the same
  /// operation at the same log level -- is the argument for this whole change,
  /// and it is why the number below is deliberately loose rather than tuned.
  ///
  /// The cost of it being too small is nil: a read that times out is treated
  /// exactly as one that was answered, so the sequence proceeds either way. The
  /// cost of it being too large is a silent pump stretching the sequence, which
  /// is why it is not simply the transport's 3000 ms default -- five unanswered
  /// reads at that default would spend 15 s of the link watchdog's 60 s budget
  /// before anything else got to run. At 1000 ms the same worst case is 5 s.
  static constexpr uint32_t REPLY_TIMEOUT_MS = 1000;

  /// Last resort: finish the sequence even if no callback ever comes back.
  ///
  /// Stages 1 and 3 are continued *only* by the transport's command callback,
  /// which is a single point of failure the old all-timers version did not
  /// have. Transport::reset() clears its command queue without invoking the
  /// pending callback, and on_notification() calls reset() on a live link when
  /// the reassembly buffer overruns -- reachable from one corrupt inbound
  /// fragment, since any frame header declaring >= 253 bytes overruns before
  /// the frame can complete or be CRC-checked. The continuation is then simply
  /// gone: no packet is ever sent again, complete() is never called, running_
  /// stays true so even a fresh start() is refused, and the session never
  /// leaves AUTHENTICATING.
  ///
  /// The link watchdog does not rescue that, because it is fed by *any*
  /// inbound notification regardless of session state, and this pump volunteers
  /// operation-status notifications throughout. A pump that keeps talking would
  /// hold the node connected, never ready, indefinitely.
  ///
  /// So one timer is armed for the whole sequence. 15 s is comfortably above
  /// the 5.45 s a fully silent pump needs (450 ms of stage 2 plus five
  /// REPLY_TIMEOUT_MS) and comfortably below the watchdog's 60 s, so it fires
  /// only when the chain is genuinely broken rather than merely slow.
  ///
  /// Fixing Transport::reset() to fire each queued callback with failure would
  /// be the better repair -- the dropped callback is not specific to this
  /// sequence -- but it changes the disconnect path for every service that
  /// queues a command, which is not this change's blast radius.
  static constexpr uint32_t SEQUENCE_BACKSTOP_MS = 15000;

  /**
   * @brief Construct an Authentication handler
   */
  explicit Authentication(Transport &transport);
  
  /**
   * @brief Set the scheduler callback
   * 
   * This callback will be used to schedule delayed tasks.
   * 
   * @param callback Function to schedule delayed tasks
   */
  void set_scheduler_callback(SchedulerCallback callback);
  
  /**
   * @brief Set the completion callback
   * 
   * This callback will be invoked when the authentication handshake
   * completes successfully.
   * 
   * @param callback Function to call on completion
   */
  void set_completion_callback(CompletionCallback callback);
  
  /**
   * @brief Start the authentication handshake
   * 
   * Initiates the 3-stage sequence. Non-blocking: stage 2 uses ESPHome's
   * scheduler, and stages 1 and 3 are driven by the transport's command
   * callbacks.
   *
   * How long it takes is now mostly the pump's answer. Measured end to end on
   * a real pump (2026-08-16): 1330 ms, of which 450 ms is stage 2's scheduled
   * delay (5 x 50 ms plus a 200 ms tail), leaving ~880 ms across the five
   * matched reads -- about 175 ms each, averaged, since the per-packet lines
   * are DEBUG and this was captured at INFO.
   *
   * So it is slightly *longer* than the 1200 ms the all-timers version always
   * took, not shorter, and that is the expected trade: the old number was a
   * constant that ignored the pump, and this one tracks it. Note the ~175 ms
   * is well above the 54-108 ms reported on another specimen at the same log
   * level, which is the variation between pumps and links that the transcribed
   * delays could never have accommodated.
   *
   * A pump that answers nothing takes that 450 ms plus five REPLY_TIMEOUT_MS,
   * about 5.5 s, and still completes.
   */
  void start();
  
  /**
   * @brief Cancel any in-progress authentication
   * 
   * Stops the authentication sequence if it's currently running.
   * This does not reset the pump's authentication state.
   */
  void cancel();
  
  /**
   * @brief Check if authentication is currently in progress
   * 
   * @return true if handshake is running
   */
  bool is_running() const { return running_; }

  /// Matched reads issued so far this connection, and how many were answered.
  ///
  /// Exposed because the sequence's defining property -- a read is not issued
  /// until the previous one has been resolved -- cannot be observed from
  /// outside without them. Counting BLE writes does not do it: the transport
  /// only writes one command per response cycle, so a version that queued all
  /// three of stage 1's reads at once would still be *written* one at a time
  /// and look identical. A test that counts packets is measuring the
  /// transport's queue, not this class. reads_sent() tells them apart.
  uint8_t reads_sent() const { return reads_sent_; }
  uint8_t replies_matched() const { return replies_matched_; }


 private:
  Transport &transport_;  ///< Transport layer
  SchedulerCallback scheduler_callback_;  ///< Callback to schedule delayed tasks
  CompletionCallback completion_callback_;  ///< Callback for completion
  bool running_ = false;  ///< True if authentication is in progress
  uint32_t auth_sequence_ = 0;  ///< Sequence counter to invalidate stale lambdas

  /// Replies matched to a packet this sequence sent, counted per connection.
  uint8_t replies_matched_ = 0;
  /// Packets sent with a callback -- i.e. the ones a reply is expected for.
  uint8_t reads_sent_ = 0;

  // Sequence stage functions
  void stage1_legacy_burst(int repeat_count);
  void stage2_class10_burst(int repeat_count);
  void stage3_extensions();
  void complete();

  /// Send a packet blind: no callback, nothing waits for a reply.
  bool send_packet(const uint8_t* data, size_t len);

  /// Send a packet as a matched read and run @p on_reply when the transport
  /// either matches its reply or gives up waiting. The bool it receives says
  /// which, and every caller proceeds regardless -- see complete().
  bool send_read(const uint8_t* data, size_t len,
                 std::function<void(bool answered, const uint8_t *frame, size_t frame_len)> on_reply);
};

// ============================================================================
// HANDSHAKE PACKETS
// ============================================================================
//
// Four packets, byte-for-byte as the Python reference sends them
// (https://github.com/eman/alpha-hwr). Do not change the bytes here without
// changing them there: two clients sending different frames to the same pump
// is how a difference in behaviour becomes impossible to attribute.
//
// The comments, unlike the bytes, have changed. They used to describe these as
// register writes carrying unlock codes. They are not: all four are reads.
// See "How the second APDU byte decodes" below for why, and issue #174 for the
// decode and the captures behind it (jfriend00, cross-checked against the
// GENIbus documentation carried by https://github.com/christoph2/GENIBus).
//
// ---------------------------------------------------------------------------
// Frame layout, common to all four
// ---------------------------------------------------------------------------
//
//   [Start] [Length] [DA] [SA] [APDU...] [CRC-H] [CRC-L]
//
//   Start   0x27 in a request, 0x24 in a reply.
//   Length  bytes from DA to the end of the APDU; frame total is Length + 4.
//   DA/SA   destination and source addresses -- 0xE7 is the pump, 0xF8 is us.
//           These were long labelled "Service ID (GENI)", as though 0xE7F8
//           were one 16-bit constant. The replies disprove that on their own:
//           every one of them comes back `24 .. F8 E7 ..`, with the pair
//           reversed, which is a destination and a source swapping places and
//           is not something a constant does.
//
// ---------------------------------------------------------------------------
// How the second APDU byte decodes
// ---------------------------------------------------------------------------
//
//   byte 1 = 0booLLLLLL
//     oo     in a request: 00 GET, 10 SET, 11 INFO
//            in a reply:   00 ok, 01 class unknown, 10 id unknown,
//                          11 operation illegal
//     LLLLLL payload byte count
//
// The request half is what this file relies on. The reply half is documented
// rather than observed, and transport.cpp reaches a flatter conclusion from the
// captures: byte 5 of a reply is simply the APDU body length, `byte5 ==
// total_len - 8` holding for all 24,233 CRC-valid inbound frames without
// exception. The two are not in conflict -- they agree on every frame anyone
// here has seen -- but only because an acknowledge of `00` and no acknowledge
// field at all look identical. Nothing in this repo has ever observed a reply
// whose top two bits were set, so every "ack ok" noted below is the reading the
// documentation gives those bits and not a measurement. If the pump does
// populate the field, one refused operation would settle it; none has been
// captured.
//
// The rule itself comes from the GENIbus documentation. What this repo adds is
// bench evidence constraining it, and the two legs below are worth separating
// because they establish different halves and neither establishes both.
//
//   - The low bits are a payload length, and bit 7 distinguishes SET.
//     TimeService::send_set_clock_command() sends OpSpec 0x94 with exactly 20
//     body bytes after it (1 obj + 2 sub + 2 type + 1 version + 3 size + 11
//     data), and 0x94 & 0x3F == 20. That frame is bench-confirmed in
//     time_service.h: written at 20:39:07 and read back as 20:39:08.
//
//   - The top bits select an operation, and 0b11 is not 0b10. Issue #46 sent
//     the Class 3 remote-mode command as 0xC1 and as 0x81 back to back against
//     a real pump. Both carry a 1-byte payload -- the APDU is 3 bytes either
//     way -- so the length field is identical and the top bits are the only
//     difference. 0x81 took effect; 0xC1 never did, and came back `[03 01 AC]`.
//
// Note what that reply is, because it was recorded as a rejection and is not
// one: byte 5 of 0x01 is acknowledge 0b00, ok, with one payload byte. The pump
// answered the INFO query correctly and INFO simply does not change anything.
// It is the same one-byte shape as the two INFO replies below (0xA1, 0x80),
// which makes it a third instance of it, and evidence for 0b11 = INFO rather
// than against.
//
// What neither leg establishes is the *width* of the operation field. Every
// length this component sends bar one is under 32, so a three-bit operation
// with a five-bit length reads them identically -- and that rival reading is
// live in the tree, at schedule_service.h, which labels 0x93 "OpSpec 4" and
// 0xB3 "OpSpec 5". The two readings diverge on exactly two frames, in opposite
// directions: the 53-byte layer write (51 body bytes) is right under the
// two-bit reading and wrong under the three-bit one, and the 21-byte
// single-event write (19 body bytes) is the reverse. Both send 0xB3. So one of
// those two frames is malformed and the tree cannot say which; it is not
// settled here, and nothing below leans on it.
//
// None of that ambiguity touches this file's conclusion. It is bit 7 that
// separates 0x03 from a SET, and an OpSpec of 0x03 has no bits set at all
// under either reading: a GET, with a payload length of three. The three bytes
// after it are that payload -- not an address plus a value. That single
// misreading is where "register 0x9495, unlock code 0x96" came from, and
// everything downstream of it.
//
// ---------------------------------------------------------------------------
// What is still unknown
// ---------------------------------------------------------------------------
//
// Why these four reads are sent at all. Knowing that they are reads makes the
// question sharper rather than answering it: a client that already knows it is
// talking to an ALPHA HWR does not need to ask what it is talking to, and this
// component discards all four replies. The documented claim that the pump
// otherwise ignores control commands is not sourced to a capture anywhere in
// either repo, and one specimen has been reported reaching Pump Ready and
// accepting writes with the sequence removed entirely (issue #174) -- on a
// pump that had never had its bond cleared, which is exactly the state that
// would mask a first-pairing requirement. Not enough to drop them. Enough that
// nobody should assume the reason is understood.
// ============================================================================

/**
 * @brief Class 2 GET: read the pump's identity.
 *
 * Frame: 27 07 E7 F8 02 03 94 95 96 EB 47
 *
 *   27       Request start
 *   07       Length; frame total 11
 *   E7 F8    Destination (pump), source (us)
 *   02       Class 2, Measured Data
 *   03       GET, 3 payload bytes
 *   94 95 96 Item IDs 148, 149, 150 -- unit_family, unit_type, unit_version
 *   EB 47    CRC-16-CCITT
 *
 * Observed reply: `24 07 F8 E7 02 03 34 07 02 89 7A` -- three values, 52 / 7 / 2.
 *
 * Those first two are values this component already knows: alpha_hwr.h defines
 * PRODUCT_FAMILY_ALPHA = 0x34 (52) and PRODUCT_TYPE_HWR = 0x07, and the BLE
 * scan filter matches on them in the manufacturer advertisement before a
 * connection is made at all. So the pump's answer here agrees with the
 * advertisement that selected it. (The Class 7 device-info strings are not the
 * corroboration -- they carry a product name, serial and versions, no numeric
 * family or type.)
 *
 * This was described here as a SET of register 0x9495 carrying unlock code
 * 0x96. It is a read of three identification items, and the pump answers it
 * with its own identity.
 *
 * Nothing reads that answer today. Doing so would be a cross-check of the
 * connected device against the advertisement the scan filter matched, rather
 * than a first line of defence -- the filter is that -- which is why it is
 * noted here and not built.
 */
static const uint8_t AUTH_LEGACY[] = {0x27, 0x07, 0xE7, 0xF8, 0x02, 0x03, 0x94, 0x95, 0x96, 0xEB, 0x47};

/**
 * @brief Class 10 GET: read the operation status object.
 *
 * Frame: 27 07 E7 F8 0A 03 56 00 06 C5 5A
 *
 *   27       Request start
 *   07       Length; frame total 11
 *   E7 F8    Destination (pump), source (us)
 *   0A       Class 10, Data Objects
 *   03       GET, 3 payload bytes
 *   56 00 06 Data item: object 0x56 = 86, sub 0x0006 = 6
 *   C5 5A    CRC-16-CCITT
 *
 * The object and sub were previously read backwards here, as "Sub-ID 0x5600,
 * Object ID 0x0006". Object first is what every Class 10 address this component
 * actually puts on the wire does: control_service.cpp builds the same shape
 * with `apdu[2] = 0x56` commented "Object 86 (1 byte!)", and the clock and
 * schedule writes are object-then-2-byte-sub as well. Be warned that the
 * repo's *labels* are not consistent about it -- several call sites and
 * parameter names say "Sub" for the byte that carries the object -- but the
 * addresses are.
 *
 * The reply cannot settle the ordering, and was cited here as though it could.
 * Its bytes 6-9 are a type header, not an echo: transport.cpp establishes from
 * 21,236 captured responses that a reply carries no object and no sub-id at
 * all, only the type of the object the request asked for. 0x0001 / 0x2F01 is
 * that type, and the Obj 86 Sub 7 read answers with the identical header, so
 * it does not even distinguish which sub was asked for.
 *
 * Observed reply: `24 12 F8 E7 0A 0E 00 01 2F 01 00 00 07 00 01 02 44 CE 40 00
 * EE 74` -- ack ok, 14 payload bytes. TelemetryService already decodes this
 * exact frame as a control-mode notification and publishes it; the reporter's
 * capture read mode=2, op_mode=1, setpoint=1650.00, and 44 CE 40 00 is
 * IEEE-754 1650.0, their actual constant-speed setpoint.
 *
 * The `00 00 07 00` that looks unaccounted for is not: `00 00 07` is the
 * ordinary three-byte size header and 7 is exactly the number of object-data
 * bytes after it, and the fourth byte is control_source, which
 * ControlService::update_mode_from_notification() reads (2 = remote/digital,
 * 1 = local/panel). The captured 0 is outside that set, which is the only part
 * still open.
 *
 * So the pump's reply to this one is already understood and already useful --
 * it is the same object the mode and setpoint controls write to. It arrives
 * during the handshake and is decoded only because it reaches the telemetry
 * path by accident, nothing having claimed it.
 */
static const uint8_t AUTH_CLASS10[] = {0x27, 0x07, 0xE7, 0xF8, 0x0A, 0x03, 0x56, 0x00, 0x06, 0xC5, 0x5A};

/**
 * @brief Class 5 INFO: ask for scaling metadata on Reference Values item 0x4B.
 *
 * Frame: 27 05 E7 F8 05 C1 4B C3 82
 *
 *   27       Request start
 *   05       Length; frame total 9
 *   E7 F8    Destination (pump), source (us)
 *   05       Class 5, Reference Values
 *   C1       INFO (0b11), 1 payload byte
 *   4B       Item 75
 *   C3 82    CRC-16-CCITT
 *
 * Observed reply: `24 05 F8 E7 05 01 A1 27 58` -- ack ok, one byte, 0xA1. That
 * byte is an INFO head: the documentation gives the full four-byte unit / zero
 * / range structure only for scaled items and the head alone otherwise, and
 * 0xA1's scale-information field says unscaled. So a one-byte answer is the
 * documented shape here, not a truncated one.
 *
 * What item 75 is remains unknown; it does not appear in the device databases
 * of the other Grundfos pumps that GENIbus tooling covers. Sent once, unlike
 * the two above, and described here for a long time as extending an
 * authentication timeout -- which is not something an INFO query does.
 */
static const uint8_t AUTH_EXT_1[] = {0x27, 0x05, 0xE7, 0xF8, 0x05, 0xC1, 0x4B, 0xC3, 0x82};

/**
 * @brief Class 11 INFO: ask for scaling metadata on 16-bit Measured item 0x0F.
 *
 * Frame: 27 05 E7 F8 0B C1 0F D0 C3
 *
 *   27       Request start
 *   05       Length; frame total 9
 *   E7 F8    Destination (pump), source (us)
 *   0B       Class 11, 16-bit Measured Data
 *   C1       INFO (0b11), 1 payload byte
 *   0F       Item 15
 *   D0 C3    CRC-16-CCITT
 *
 * Observed reply: `24 05 F8 E7 0B 01 80 08 1A` -- ack ok, one byte, 0x80,
 * another unscaled INFO head. Item 15 is likewise unidentified.
 *
 * Sent immediately after the Class 5 query with no delay between them. The
 * ordering was documented as mattering; nothing observed requires it, and two
 * INFO queries on different classes have no evident dependency.
 */
static const uint8_t AUTH_EXT_2[] = {0x27, 0x05, 0xE7, 0xF8, 0x0B, 0xC1, 0x0F, 0xD0, 0xC3};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
