#pragma once

#include <cstdint>
#include <cstddef>

namespace esphome {
namespace alpha_hwr {
namespace protocol {

/**
 * GENI protocol frame builder.
 * 
 * This module constructs GENI protocol frames for all operations:
 * - Class 2/3: Register-based operations (legacy)
 * - Class 10: DataObject operations (modern)
 * - Command types: INFO, SET, READ, WRITE, EXECUTE
 * 
 * Frame Structure:
 * [Start] [Length] [ServiceID-H] [ServiceID-L/Source] [APDU...] [CRC-H] [CRC-L]
 * 
 * Where:
 * - Start: 0x27 (FRAME_START for requests; replies start 0x24)
 * - Length: Number of bytes from the destination address to the end of the
 *   APDU (not including CRC). Frame total is Length + 4.
 * - Destination: 0xE7, the pump
 * - Source: 0xF8 (standard) or 0x0A (alternative), us
 * - APDU: Application Protocol Data Unit (class, opspec, data)
 * - CRC: CRC-16-CCITT checksum
 *
 * The two address bytes were described here as one 16-bit "Service ID (GENI)".
 * They are a destination and a source, and the pump's replies show it: they
 * come back with the pair reversed, `24 .. F8 E7 ..` (issue #174).
 * 
 * Reference: alpha_hwr/protocol/frame_builder.py
 */

// GENI Protocol Constants
static const uint8_t FRAME_START = 0x27;
/// Telegram destination and source addresses (GENIbus `DA` and `SA`, the two
/// bytes after the start delimiter and the length).
///
/// SERVICE_ID_HIGH is a misnomer kept for now because it is spelled out at
/// every call site: byte 2 is the DESTINATION ADDRESS, the pump's unit address
/// (0xE7 = 231), and byte 3 is ours (0xF8 = 248). There is no "service ID" in
/// the telegram format. Both values are what the Grundfos GO app uses, taken
/// from the captures.
static const uint8_t SERVICE_ID_HIGH = 0xE7;
static const uint8_t SOURCE_ADDRESS = 0xF8;

/// The protocol's own size ceilings (GENIbus Protocol Specification fig. 2).
///
/// The length field is "the number of following bytes excluding the Check
/// Value", and the PDU bracket in fig. 2 covers the APDUs and the optional RFS
/// byte ONLY -- DA and SA sit in the head of the telegram, outside it. So
///
///     LENGTH   = DA + SA + PDU  <=  2 + 253 = 255
///     TELEGRAM = LENGTH + 4     <=  259
///
/// which is the one reading under which the specification's own two numbers are
/// consistent. The Grundfos GO app agrees, in the only place it could:
/// GeniBuilder.calculateAndAppendCRC() computes the length as `top - 2` -- DA,
/// SA and the APDUs -- and rejects it above 255, not above 253.
///
/// This file previously glossed the PDU as "DA + SA + APDUs", which made its own
/// MAX_TELEGRAM_LEN unreachable and would put the largest legal telegram at 257.
/// Issue #278 briefly promoted that misreading into a named constant before the
/// specification settled it. There is no gap: the largest legal telegram is
/// MAX_TELEGRAM_LEN.
static const size_t MAX_TELEGRAM_LEN = 259;
static const size_t MAX_PDU_LEN = 253;

/// The floor on the same field, derived the same way: DA + SA + the shortest
/// APDU, and an APDU header is two bytes (class, then `0booLLLLLL`).
///
///     24 04 F8 E7 0A 40 CRC CRC     an Unknown Class refusal, 8 bytes
///     24 05 F8 E7 0A 01 00 AE A2    a Class 10 acknowledge,   9 bytes
///
/// **4, not 5, and the difference is a lesson worth keeping.** 5 is what the
/// capture corpus says: the smallest length byte in either direction across all
/// 44,200 frames. But no frame in that corpus is refused AT THE APDU HEAD --
/// all 22,062 inbound frames carry acknowledge OK there, whatever the Class 10
/// status byte below it says -- and the zero-payload shape occurs only in a
/// head-level refusal. So the corpus minimum is a minimum over head-OK traffic,
/// and a floor set from it rejects `0x40` Unknown Class, whose format App C.17
/// gives and which this project handles deliberately (issue #208). That mistake
/// was made here and caught by the suite.
///
/// Not the protocol's absolute floor: fig. 2 says a PDU may consist of ZERO
/// APDUs, so LENGTH 2 is legal and describes a 6-byte telegram carrying nothing.
/// 4 is the floor for a telegram that carries something, which is the useful
/// bound for a receiver -- refusing an empty telegram loses nothing dispatchable.
static const uint8_t MIN_LENGTH_FIELD = 4;

static const uint8_t CLASS_10 = 0x0A;

/**
 * Build Class 10 register READ request for telemetry.
 * 
 * This builds the format that the ALPHA HWR pump actually uses for
 * telemetry queries: Class 10, OpSpec 0x03, with a 3-byte register address.
 * 
 * Frame Structure:
 * [27] [07] [E7] [F8] [0A] [03] [Reg-H] [Reg-M] [Reg-L] [CRC-H] [CRC-L]
 * 
 * @param register_addr 3-byte register address (e.g., 0x570045 for motor state)
 * @param packet_out Output buffer (must be at least 11 bytes)
 * @param source Source address (default: 0xF8)
 * 
 * Common registers:
 * - 0x570045: Motor state (voltage, current, power, RPM, temp)
 * - 0x5D0122: Flow rate and head pressure
 * - 0x5D012C: Temperatures (media, PCB, control box)
 * - 0x580000: Alarms (Obj 88, Sub 0)
 * - 0x58000B: Warnings (Obj 88, Sub 11)
 * 
 * Reference: alpha_hwr/protocol/frame_builder.py::build_class10_read()
 */
void build_class10_read(uint32_t register_addr, uint8_t *packet_out, uint8_t source = SOURCE_ADDRESS);

/**
 * Build Class 10 DataObject SET operation.
 * 
 * Class 10 operations are the modern GENI protocol used for:
 * - Control mode changes (Sub 0x5600)
 * - Schedule management
 * - Configuration updates
 * - Device information queries
 * 
 * Frame Structure:
 * [27] [Len] [E7] [F8] [0A] [OpSpec] [Sub-H] [Sub-L] [Obj-H] [Obj-L] [Data...] [CRC]
 * 
 * @param sub_id Sub-system ID (e.g., 0x5600 for control)
 * @param obj_id Object ID within subsystem
 * @param data Payload data (can be NULL for trigger operations)
 * @param data_len Length of payload data
 * @param packet_out Output buffer (must be large enough for frame + data)
 * @param source Source address (default: 0xF8)
 * @return Total packet length (including CRC)
 * 
 * Notes:
 * OpSpec for Class 10 SET:
 * - Bits 7-6: the operation; 0b10 is SET
 * - Bits 5-0: Length of SubID + ObjID + Data (minimum 4), which is why the
 *   payload cap below is 59 rather than 123
 *
 * "Bits 6-0 = length" was the wording here, and the code has always disagreed
 * with it -- build_data_object_set() masks 0x3F, six bits, and ORs in 0x80,
 * leaving bit 6 clear. Six bits is also what the caps in the .cpp assume.
 *
 * **The width of the operation field is not settled, and the tree holds both
 * readings.** This paragraph moved here from auth.h when the opening sequence
 * was removed; it is the one part of that file's decode that is still live.
 * Every length this component sends bar one is under 32, so a three-bit
 * operation with a five-bit length reads them identically -- and the rival
 * reading is in the tree, at schedule_service.h, which labels 0x93 "OpSpec 4"
 * and 0xB3 "OpSpec 5". The two readings diverge on exactly two frames, in
 * opposite directions: the 53-byte layer write (51 body bytes) is right under
 * the two-bit reading and wrong under the three-bit one, and the 21-byte
 * single-event write (19 body bytes) is the reverse. Both send 0xB3. So one of
 * those two frames is malformed and the tree cannot say which. Nothing here
 * leans on the answer; it is recorded so the next person does not rediscover
 * the conflict from scratch (issue #174).
 * 
 * Reference: alpha_hwr/protocol/frame_builder.py::build_data_object_set()
 */
size_t build_data_object_set(uint16_t sub_id, uint16_t obj_id, 
                              const uint8_t *data, size_t data_len,
                              uint8_t *packet_out, uint8_t source = SOURCE_ADDRESS);

/**
 * Build a register-read command for Class 2/3 (legacy telemetry reads).
 *
 * **The name is wrong, and the function is uncalled.** It builds a GET: the
 * OpSpec it emits is `0b00` in bits 7-6 with the register length in bits 5-0.
 * INFO is `0b11` -- a different operation, asking for an item's scaling
 * metadata rather than its value. Issue #46 is what confusing the two costs: a
 * Class 3 command sent as `0xC1` (INFO) instead of `0x81` (SET) was answered
 * correctly as an INFO query and so never took effect.
 *
 * The comments are corrected rather than the identifier because nothing in the
 * repo calls this function; renaming or deleting it is a separate change
 * (issue #174).
 *
 * Frame Structure:
 * [27] [Length] [E7] [F8] [Class] [OpSpec] [Register...] [CRC-H] [CRC-L]
 * 
 * @param class_byte Protocol class (2 or 3 for register operations)
 * @param register_addr Register address (1, 2, or 3 bytes depending on value)
 * @param packet_out Output buffer (must be large enough)
 * @param source Source address (default: 0xF8)
 * @return Total packet length (including CRC)
 * 
 * Notes:
 * Register length is auto-detected:
 * - register <= 0xFF: 1 byte
 * - register <= 0xFFFF: 2 bytes
 * - register > 0xFFFF: 3 bytes
 * 
 * Reference: alpha_hwr/protocol/frame_builder.py::build_command_info()
 */
size_t build_command_info(uint8_t class_byte, uint32_t register_addr,
                           uint8_t *packet_out, uint8_t source = SOURCE_ADDRESS);

/**
 * Build generic GENI packet with APDU.
 * 
 * This is a general-purpose packet builder used for any GENI command
 * that doesn't have a specialized builder function. It's particularly
 * useful for Class 7 (device info) string reads.
 * 
 * Frame Structure:
 * [27] [Length] [ServiceID] [Source] [APDU...] [CRC-H] [CRC-L]
 * 
 * @param service_id Service ID (typically 0xE7 for GENI commands)
 * @param source Source address (typically 0xF8)
 * @param apdu Application Protocol Data Unit (command payload)
 * @param apdu_len Length of APDU
 * @param packet_out Output buffer (must be large enough for header + APDU + CRC)
 * @return Total packet length (including CRC)
 * 
 * Example - Class 7 string read:
 *   uint8_t apdu[] = {0x07, 0x01, string_id};
 *   build_geni_packet(0xE7, 0xF8, apdu, 3, packet_out);
 * 
 * Reference: alpha_hwr/services/base.py::_build_geni_packet()
 */
size_t build_geni_packet(uint8_t service_id, uint8_t source,
                          const uint8_t *apdu, size_t apdu_len,
                          uint8_t *packet_out);

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
