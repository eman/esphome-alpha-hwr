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
 * Ten packets in three stages, on a fixed timeline:
 *
 *   Stage 1: the Class 2 identity read, sent 3x at 50 ms, then 100 ms
 *   Stage 2: the Class 10 operation-status read, sent 5x at 50 ms, then 200 ms
 *   Stage 3: the Class 5 and Class 11 INFO queries, back to back, then 500 ms
 *
 * Two things about that description are deliberate, because both were wrong
 * here until issue #174 decoded the frames.
 *
 * **They are reads.** Not an authentication handshake, and not writes of any
 * kind: two GETs and two INFO queries, none of which addresses anything
 * writable. See the packet definitions at the bottom of this header for the
 * byte-level decode and for the in-tree evidence that fixes the encoding. The
 * class is still named Authentication and the packets still AUTH_*, which is
 * now a description of what they were believed to be rather than of what they
 * do.
 *
 * **The delays are transcriptions, not requirements.** Each is one of the
 * reference client's `asyncio.sleep()` values, and they were carried across as
 * "timing requirements ... allows pump processing" without anything measuring
 * a pump. What has since been measured, on one specimen: the pump answers all
 * ten packets, class-matched, in 54-108 ms at normal log level. So the delays
 * are not tracking anything the pump does, and nothing in this class notices
 * whether a reply arrived -- send_packet() passes no callback, so every reply
 * falls through to the telemetry path, where the Class 10 ones are decoded as
 * passive notifications and the rest are discarded for not being Class 10.
 *
 * Issue #174 covers both, and the fix for the second is to stop sending these
 * as timed blind writes and let the transport match their replies like any
 * other read.
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
   * @param delay_ms Delay in milliseconds
   * @param callback Function to call after delay
   */
  using SchedulerCallback = std::function<void(uint32_t delay_ms, std::function<void()> callback)>;
  
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
   * Initiates the 3-stage authentication sequence. This is a non-blocking
   * operation that uses ESPHome's scheduler to manage timing.
   * 
   * The handshake will complete in approximately 1 second, after which
   * the completion callback will be invoked.
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
  
 private:
  Transport &transport_;  ///< Transport layer
  SchedulerCallback scheduler_callback_;  ///< Callback to schedule delayed tasks
  CompletionCallback completion_callback_;  ///< Callback for completion
  bool running_ = false;  ///< True if authentication is in progress
  uint32_t auth_sequence_ = 0;  ///< Sequence counter to invalidate stale lambdas
  
  // Authentication stage functions
  void stage1_legacy_burst(int repeat_count);
  void stage2_class10_burst(int repeat_count);
  void stage3_extensions();
  void complete();
  
  // Helper function to send a packet
  bool send_packet(const uint8_t* data, size_t len);
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
// This is the rule the packets below are decoded under, and it is worth being
// explicit that it does not rest on the third-party documentation alone. Three
// independent places in this repo, all bench-derived, fix the same bits:
//
//   - TimeService::send_set_clock_command() sends OpSpec 0x94 = 0b10_010100
//     and carries 20 body bytes, confirmed against a frame the pump accepted.
//     That pins SET = 0b10 and the low six bits as a length.
//   - frame_builder.cpp::build_data_object_set() builds every Class 10 SET as
//     "bit 7 set, bits 6-0 = length" -- the same encoding, arrived at
//     separately.
//   - Issue #46 established on the bench that 0xC1 (0b11) and 0x81 (0b10) are
//     different operations to this pump: the Class 3 remote-mode command sent
//     as 0xC1 was refused every time and only took effect as 0x81.
//
// With SET fixed at 0b10, an OpSpec of 0x03 is 0b00 -- a GET -- and the three
// bytes after it are a payload length of three, not an address plus a value.
// That single misreading is where "register 0x9495, unlock code 0x96" came
// from, and everything downstream of it.
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
 * Observed reply: `24 07 F8 E7 02 03 34 07 02 89 7A` -- ack ok, three values,
 * 52 / 7 / 2. Family 52 type 7 is the ALPHA HWR, which agrees with what the
 * Class 7 device-info strings report later in the connection.
 *
 * This was described here as a SET of register 0x9495 carrying unlock code
 * 0x96. It is a read of three identification items, and the pump answers it
 * with its own identity.
 *
 * Nothing reads that answer today. It is the earliest point in a connection at
 * which the component could confirm it is talking to an ALPHA HWR and not to
 * some other Grundfos product with a different object map -- earlier than the
 * Class 7 product name, and already paid for.
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
 * Object ID 0x0006". Object first is the order frame_builder.h already uses
 * for its telemetry registers (0x580000 = Obj 88 Sub 0), and the reply settles
 * it independently: it comes back identified as 0x0001 / 0x2F01, which
 * RESPONSE_IDENTIFIERS names as Obj 86 Sub 5-10, operation status.
 *
 * Observed reply: `24 12 F8 E7 0A 0E 00 01 2F 01 00 00 07 00 01 02 44 CE 40 00
 * EE 74` -- ack ok, 14 payload bytes. TelemetryService already decodes this
 * exact frame as a control-mode notification and publishes it; the reporter's
 * capture read mode=2, op_mode=1, setpoint=1650.00, and 44 CE 40 00 is
 * IEEE-754 1650.0, their actual constant-speed setpoint. Four bytes
 * (00 00 07 00) are still unaccounted for.
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
