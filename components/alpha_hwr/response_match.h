#pragma once

#include <cstdint>

/**
 * Pure response-matching predicates for core::Transport.
 *
 * These live in their own header, free of ESPHome and BLE dependencies, for the
 * same reason pump_schedule_ux.h and dhw_demand_logic.h do: a host test can
 * assert the shipped decision directly instead of re-deriving it. The Class 3/7
 * gating below previously existed only inline inside try_dispatch_response(),
 * and tests/test_transport_matching.cpp asserted a hand-written copy of it --
 * which meant the test could keep passing while the real predicate changed.
 */

namespace esphome {
namespace alpha_hwr {
namespace protocol {

/// GENI class bytes that use a packet structure distinct from Class 10
/// DataObjects, and are therefore matched by class byte alone under a wildcard.
constexpr uint8_t CLASS_2_MEASURED_DATA = 0x02;
constexpr uint8_t CLASS_3_COMMAND_ACK = 0x03;
constexpr uint8_t CLASS_5_REFERENCE_VALUES = 0x05;
constexpr uint8_t CLASS_7_DEVICE_INFO = 0x07;
constexpr uint8_t CLASS_11_MEASURED_16BIT = 0x0B;

/**
 * Is this a class whose replies are matched by class byte alone?
 *
 * Class 3 and 7 were here first. Classes 2, 5 and 11 joined them for the
 * opening packet sequence (issue #174), whose four reads are one of each: a
 * Class 2 GET, a Class 10 GET, and two INFO queries on Classes 5 and 11. Their
 * replies could not be matched at all before -- Class 2's failed the Class 10
 * path's `data[4] == 0x0A` test, and the 9-byte Class 5 and 11 replies were
 * shorter than its `len < 11` floor -- so the sequence sent them blind and
 * advanced on timers instead.
 *
 * Matching by class byte alone is weaker than matching a Class 10 DataObject
 * reply on its type identifiers, and it is worth being clear about what carries
 * the weight here. It is not that these classes are inherently unambiguous: it
 * is that the predicates below only ever fire against a command *we queued as
 * that same class*, and the ambiguity that would matter -- an unsolicited frame
 * arriving mid-command and being taken for the answer -- is a property of what
 * the pump volunteers. Class 10 is what it volunteers: telemetry notifications
 * arrive continuously and unbidden, which is exactly why Class 10 is not in
 * this set and is matched on its identifiers instead.
 *
 * Classes 2, 5 and 11 appeared at zero frames across 373 frames of *this
 * component's* normal operation on one pump (issue #174). Read that narrowly.
 * It is not a claim that the pump does not speak Class 2 -- it plainly does,
 * and in bulk: the reference captures behind transport.cpp's statistics were
 * taken from the phone app, which reads telemetry as Class 2, and roughly
 * 2,500 of those ~24,000 inbound frames are non-Class-10 as a result. What the
 * census says is only that *this* component never asks for Class 2, 5 or 11
 * outside the opening sequence, because it reads telemetry as Class 10 -- so
 * nothing of those classes is in flight to be confused with anything.
 *
 * The guarantee is therefore the queued-class term, not the census; one
 * specimen could not carry a guarantee anyway. The census only explains why
 * that term is sufficient here rather than merely necessary.
 *
 * Note also what adding a class to this set cannot do: both predicates require
 * the *queued* command to be of a set class. When the three were added, nothing
 * outside the opening sequence queued a Class 2, 5 or 11 command; since that
 * sequence was removed (issue #174) nothing queues one at all, so they are now
 * unreachable outright rather than merely unreachable by existing traffic.
 *
 * They are kept anyway, and deliberately. This set is a statement about which
 * classes CAN be matched by class byte alone, which is a property of the
 * protocol rather than of what this component currently happens to send; the
 * cost of an unreachable entry in two branch-free predicates is nothing, and
 * the analysis behind admitting them is above and would have to be redone.
 * That is a different case from the ERROR session state or `on_authenticating()`,
 * both of which were deleted for being unreachable -- those were live code paths
 * promising behaviour they could not deliver, where this is a lookup table with
 * a row nobody reads. If a Class 2 identity read is ever added (reading
 * `unit_family`/`unit_type` as a guard against driving a non-HWR pump is the
 * obvious candidate), it works with no change here.
 */
inline bool is_wildcard_matched_class(uint8_t class_byte) {
  if (class_byte == CLASS_2_MEASURED_DATA) return true;
  if (class_byte == CLASS_3_COMMAND_ACK) return true;
  if (class_byte == CLASS_5_REFERENCE_VALUES) return true;
  if (class_byte == CLASS_7_DEVICE_INFO) return true;
  return class_byte == CLASS_11_MEASURED_16BIT;
}

/**
 * The acknowledge field of a Data Reply APDU, and its payload length.
 *
 * Byte 1 of an APDU is `0booLLLLLL`. In a *request* the top two bits are the
 * operation (00 GET, 10 SET, 11 INFO); in a *reply* they are the acknowledge.
 * The low six are the payload byte count either way. App C.17 of the GENIbus
 * documentation gives each acknowledge kind its own reply format:
 *
 *     ACK   meaning              reply payload
 *     ---   ------------------   -----------------------------------------
 *     00    ok                   the normal payload, LLLLLL bytes of it
 *     01    Unknown Class        none (LLLLLL == 0)
 *     10    Unknown Data Item    the ID of the first unknown Data Item
 *     11    Illegal Operation    the ID of the first inaccessible Data Item
 *
 * Note what the payload of an *error* reply is: an item ID. It is not an error
 * code, and reading it as one is where issue #208 came from -- `transport.cpp`
 * treated a Class 10 `0x81` head as a short ACK carrying an error code and
 * called the write successful when that byte happened to be zero. `0x81` is
 * `10 000001`: Unknown Data Item, one payload byte, and that byte is the ID of
 * the item the pump did not recognise. So the write failed, and it was reported
 * as succeeding precisely when the unknown item's ID was 0x00.
 *
 * Note the asymmetry in those payloads, because it is easy to lose: only the
 * two item-related errors carry a byte. Unknown Class declares length 0, so its
 * reply head is `0x40` and its frame is one byte shorter. A matcher keyed on
 * "declares exactly one payload byte" therefore admits `0x41` -- a length-1
 * Unknown Class, which this table says does not occur -- while rejecting the
 * `0x40` that does. The second APDU of the frame captured in #208 is exactly
 * that: `40 40`, an Unknown Class error with no payload.
 *
 * Evidence that this pump populates the field at all, since #174 declined to
 * build on the documentation alone: a CRC-verified `0x81` reply captured during
 * a setpoint write, and a deliberate probe returning `0xC1` (Illegal Operation)
 * that was predicted byte-for-byte before the build. Two observations, one of
 * them chosen rather than stumbled into (issue #208).
 */
enum class ApduAck : uint8_t {
  OK = 0,
  UNKNOWN_CLASS = 1,
  UNKNOWN_DATA_ITEM = 2,
  ILLEGAL_OPERATION = 3,
};

/// The two acknowledge bits of an APDU head byte.
inline ApduAck apdu_ack(uint8_t apdu_head) {
  return static_cast<ApduAck>((apdu_head >> 6) & 0x03);
}

/// The payload byte count an APDU head declares (its low six bits).
inline uint8_t apdu_payload_len(uint8_t apdu_head) { return apdu_head & 0x3F; }

/// Did the pump accept the operation this APDU answers?
inline bool apdu_ack_is_ok(uint8_t apdu_head) { return apdu_ack(apdu_head) == ApduAck::OK; }

/// The SECOND acknowledge a Class 10 reply carries, after the APDU head's.
///
/// Class 10 answers a request with the generic APDU acknowledge in the head and
/// then a status byte of its own in the payload. The Grundfos GO app names all
/// three (`GeniAPDU.CLASS10_ACK_OK/_BUSY/_OPERATION_FAILED`, read from
/// `raw[apdu_offset + 2]` by `getAcknowledgeCodeForClass10()`), and the captures
/// contain exactly those three values and no others: across
/// resources/traffic_capture, 459 short Class 10 replies split 420 OK, 26 BUSY,
/// 13 OPERATION_FAILED, and every one of them has head acknowledge OK.
/// (`tools/geni_capture_scan.py acks`. An earlier revision of this note said
/// 136/100/24/12 -- a pre-reassembly count, the trap the corpus README
/// documents: writes exceed the 20-byte ATT payload and fragment, so a scanner
/// reading packets individually sees only part of the traffic.)
///
/// It is request-consistent in a way that rules out coincidence: Obj 202 Sub 100
/// answers BUSY every time (24/24) and Obj 202 Sub 200 answers OPERATION_FAILED
/// every time (12/12), while every other object answers OK.
///
/// Reading only the head's acknowledge therefore reports success for a pump that
/// said "busy" or "that failed" -- the same defect issue #208 fixed one layer up,
/// one layer further down. 39 of those 459 replies are not OK.
///
/// Only meaningful when the head's acknowledge IS OK. A refusal (Unknown Data
/// Item, Illegal Operation) puts the offending item's ID in that byte instead,
/// which is what #208 established; the two readings are told apart by the head,
/// never by the byte itself.
enum class Class10Ack : uint8_t {
  OK = 0,
  BUSY = 2,
  OPERATION_FAILED = 4,
};

/// Human-readable Class 10 acknowledge, for logs.
inline const char *class10_ack_name(uint8_t code) {
  switch (code) {
    case 0:
      return "ok";
    case 2:
      return "busy";
    case 4:
      return "operation failed";
    default:
      return "unknown";
  }
}

/// Did a Class 10 reply accept the request, reading BOTH acknowledges?
///
/// `head` is the APDU head byte and `first_payload` the byte after it, which is
/// only present when the head declares a payload. A zero-length Class 10 reply
/// carries no status byte and is taken at the head's word.
inline bool class10_reply_is_ok(uint8_t head, bool has_payload, uint8_t first_payload) {
  if (!apdu_ack_is_ok(head)) return false;
  if (!has_payload) return true;
  return first_payload == static_cast<uint8_t>(Class10Ack::OK);
}

/// The operation an APDU head requests, in the REQUEST direction.
///
/// Same two bits as the acknowledge, read the other way round (App. Prog.
/// Manual fig C.2): 00 GET, 10 SET, 11 INFO. There is no operation 01. The
/// direction decides which reading applies -- a head is an operation in a
/// request and an acknowledge in a reply, and nothing in the byte says which,
/// so only the caller's knowledge of what it is holding disambiguates.
enum class ApduOp : uint8_t {
  GET = 0,
  SET = 2,
  INFO = 3,
};

/// The operation bits of a request's APDU head.
inline ApduOp apdu_op(uint8_t apdu_head) {
  return static_cast<ApduOp>((apdu_head >> 6) & 0x03);
}

/// Is this request APDU head a SET?
///
/// Used to gate the short-ACK match (issue #248) on the one operation whose
/// reply is acknowledgement-only: "the SET operation never returns anything but
/// the APDU Head" (fig 3.5 note 1). A GET's reply carries data, and a one-byte
/// data reply is byte-identical to a short ACK -- which is why the queued
/// command's operation, not the reply's shape, is what may be tested here.
inline bool apdu_is_set(uint8_t apdu_head) { return apdu_op(apdu_head) == ApduOp::SET; }

/// Human-readable acknowledge, for logs. Kept beside the enum so a new kind
/// cannot be added without a name.
inline const char *apdu_ack_name(ApduAck ack) {
  switch (ack) {
    case ApduAck::OK:
      return "ok";
    case ApduAck::UNKNOWN_CLASS:
      return "Unknown Class";
    case ApduAck::UNKNOWN_DATA_ITEM:
      return "Unknown Data Item";
    case ApduAck::ILLEGAL_OPERATION:
      return "Illegal Operation";
  }
  return "unknown";
}

/**
 * Does an incoming Class 3/7 packet answer the queued command?
 *
 * A wildcard expectation (`expect_type_low_ver == 0 && expect_type_high == 0`) carries no
 * Object/Sub ID to match on, so these classes are matched by class byte alone --
 * but only against a command that was actually *sent* as that class. Without the
 * `incoming_class == queued_class` term, an unrelated Class 10 telemetry
 * notification arriving while a Class 3 command is in flight would be taken for
 * that command's ACK (PR #50 review).
 *
 * @param queued_class   Class byte of the command awaiting a response
 * @param incoming_class Class byte of the packet that just arrived
 * @param wildcard_expect expect_type_low_ver == 0 && expect_type_high == 0
 */
inline bool class_wildcard_matches(uint8_t queued_class,
                                   uint8_t incoming_class,
                                   bool wildcard_expect) {
  if (!is_wildcard_matched_class(incoming_class)) return false;
  if (incoming_class != queued_class) return false;
  return wildcard_expect;
}

/**
 * Should an incoming packet be ignored outright because the queued command is
 * Class 3/7 and this response is neither?
 *
 * Such a packet is definitely not the answer, and letting it fall through risks
 * the Class 10 wildcard path matching it by accident.
 */
inline bool ignore_unrelated_while_awaiting_wildcard_class(uint8_t queued_class,
                                                           uint8_t incoming_class) {
  if (!is_wildcard_matched_class(queued_class)) return false;
  return !is_wildcard_matched_class(incoming_class);
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
