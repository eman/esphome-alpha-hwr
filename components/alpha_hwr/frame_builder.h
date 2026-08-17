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
static const uint8_t SERVICE_ID_HIGH = 0xE7;
static const uint8_t SOURCE_ADDRESS = 0xF8;
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
