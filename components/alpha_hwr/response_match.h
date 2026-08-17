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
 * this set and is matched on its identifiers instead. Classes 2, 5 and 11 were
 * observed at zero frames across 373 frames of normal operation on one pump
 * (issue #174) -- they appear only as answers to these four reads.
 *
 * That is one specimen, so the guarantee is the queued-class term rather than
 * the census. The census is why the term is sufficient here and not merely
 * necessary.
 *
 * Note also what adding a class to this set cannot do: both predicates require
 * the *queued* command to be of a set class, and nothing outside the opening
 * sequence queues a Class 2, 5 or 11 command anywhere in this component. So no
 * existing traffic changes path -- the three new members are unreachable until
 * someone sends one.
 */
inline bool is_wildcard_matched_class(uint8_t class_byte) {
  if (class_byte == CLASS_2_MEASURED_DATA) return true;
  if (class_byte == CLASS_3_COMMAND_ACK) return true;
  if (class_byte == CLASS_5_REFERENCE_VALUES) return true;
  if (class_byte == CLASS_7_DEVICE_INFO) return true;
  return class_byte == CLASS_11_MEASURED_16BIT;
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
