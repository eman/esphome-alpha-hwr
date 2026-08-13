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
constexpr uint8_t CLASS_3_COMMAND_ACK = 0x03;
constexpr uint8_t CLASS_7_DEVICE_INFO = 0x07;

inline bool is_class3_or_7(uint8_t class_byte) {
  return class_byte == CLASS_3_COMMAND_ACK || class_byte == CLASS_7_DEVICE_INFO;
}

/**
 * Does an incoming Class 3/7 packet answer the queued command?
 *
 * A wildcard expectation (`expect_obj_id == 0 && expect_sub_id == 0`) carries no
 * Object/Sub ID to match on, so these classes are matched by class byte alone --
 * but only against a command that was actually *sent* as that class. Without the
 * `incoming_class == queued_class` term, an unrelated Class 10 telemetry
 * notification arriving while a Class 3 command is in flight would be taken for
 * that command's ACK (PR #50 review).
 *
 * @param queued_class   Class byte of the command awaiting a response
 * @param incoming_class Class byte of the packet that just arrived
 * @param wildcard_expect expect_obj_id == 0 && expect_sub_id == 0
 */
inline bool class3_or_7_wildcard_matches(uint8_t queued_class,
                                         uint8_t incoming_class,
                                         bool wildcard_expect) {
  return is_class3_or_7(incoming_class) && incoming_class == queued_class &&
         wildcard_expect;
}

/**
 * Should an incoming packet be ignored outright because the queued command is
 * Class 3/7 and this response is neither?
 *
 * Such a packet is definitely not the answer, and letting it fall through risks
 * the Class 10 wildcard path matching it by accident.
 */
inline bool ignore_unrelated_while_awaiting_class3_or_7(uint8_t queued_class,
                                                        uint8_t incoming_class) {
  return is_class3_or_7(queued_class) && !is_class3_or_7(incoming_class);
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
