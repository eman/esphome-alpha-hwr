/**
 * GENI Protocol Frame Parser
 * 
 * This module parses raw GENI protocol frames received from the pump:
 * - Response validation (start byte, length, CRC)
 * - Class 2/3 register responses
 * - Class 10 DataObject notifications
 * - Error handling and validation
 * 
 * Frame Structure
 * ---------------
 * All GENI frames follow this structure:
 * 
 * [Start] [Length] [ServiceID-H] [ServiceID-L/Source] [APDU...] [CRC-H] [CRC-L]
 * 
 * Where:
 * - Start: 0x24 (RESPONSE_START for responses) or 0x27 (FRAME_START for requests)
 * - Length: Number of bytes from ServiceID to end of APDU (not including CRC)
 * - ServiceID-H: 0xE7 (GENI service)
 * - ServiceID-L/Source: 0xF8 (standard) or 0x0A (alternative)
 * - APDU: Application Protocol Data Unit (class, opspec, data)
 * - CRC: CRC-16-CCITT checksum
 * 
 * APDU Formats
 * ------------
 * 
 * Class 2/3 (Register-based):
 * [Class] [OpSpec] [Register...] [Data...]
 * 
 * Class 10 (DataObject):
 * [0x0A] [OpSpec] [SubID-H] [SubID-L] [ObjID-H] [ObjID-L] [Data...]
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/protocol/frame_parser.py
 */

#pragma once

#include "esphome/core/component.h"
#include <cstdint>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace protocol {

// Frame start byte constants (note: FRAME_START and CLASS_10 are also defined in frame_builder.h)
static constexpr uint8_t RESPONSE_START = 0x24;   // Response frames (0x27 = request is FRAME_START)

// Protocol class constants
static constexpr uint8_t CLASS_2 = 0x02;
static constexpr uint8_t CLASS_3 = 0x03;

// Class 10 OpSpec constants (operation types)
static constexpr uint8_t OPSPEC_MOTOR_STATE = 0x30;      // Motor telemetry response
static constexpr uint8_t OPSPEC_FLOW_PRESSURE = 0x2B;    // Flow/pressure response
static constexpr uint8_t OPSPEC_TEMPERATURE = 0x14;      // Temperature response
static constexpr uint8_t OPSPEC_ALARMS_WARNINGS = 0x09;  // Alarms/warnings response
static constexpr uint8_t OPSPEC_PASSIVE_NOTIF = 0x0E;    // Passive notification (streaming)
static constexpr uint8_t OPSPEC_ALARMS_READ = 0x13;      // Alarms read response

// Frame type enumeration
enum class FrameType {
  INVALID,
  REQUEST,
  RESPONSE
};

/**
 * Parsed GENI protocol frame.
 * 
 * This structure represents a fully parsed GENI frame with all fields extracted
 * and validated. It mirrors the ParsedFrame dataclass from the Python reference.
 * 
 * Fields:
 * - valid: True if the frame structure is valid (correct start byte, length)
 * - frame_type: REQUEST (0x27) or RESPONSE (0x24) or INVALID
 * - class_byte: GENI class byte (2, 3, 10, etc.)
 * - opspec: Operation specification byte (Class 10 only)
 * - sub_id: Sub-ID for Class 10 frames (0 for other classes)
 * - obj_id: Object ID for Class 10 frames (0 for other classes)
 * - payload: Raw payload bytes of the FIRST APDU (excluding header and CRC)
 * - payload_len: Length of that payload, bounded by what the APDU declares
 * - multi_apdu: True if the telegram carries bytes past the first APDU
 * - crc_valid: True if CRC checksum is correct
 *
 * On `payload_len` and `multi_apdu`, because the two together are the contract:
 *
 * A GENIbus telegram may carry several APDUs. App C.17 says errors are reported
 * per-APDU -- "errors in one APDU will in no way influence the reply to sound
 * APDU's" -- so an error reply substitutes for one APDU's answer inside a
 * telegram carrying others. Every frame in the 24,233-frame reference corpus
 * happens to hold exactly one, which is why `byte5 == total_len - 8` looked like
 * a property of the protocol; it is a property of that capture.
 *
 * This parser reports the FIRST APDU only, bounded by the length that APDU
 * declares in the low six bits of its head byte. Before issue #226 it returned
 * everything between the header and the CRC, so on a two-APDU telegram the
 * payload ran on into the next APDU with nothing to say so -- a Class 2 reply
 * declaring one payload byte reported three, the extra two being the whole of
 * APDU 2.
 *
 * Reporting the first APDU rather than exposing a list is deliberate: no caller
 * in this component wants a list, every one of them asks a question about a
 * single answer, and a list would push the single-APDU assumption into all of
 * them instead of resolving it here. `multi_apdu` is the escape hatch -- it says
 * "there was more in this telegram than you are being shown", so a caller that
 * cares can refuse rather than silently seeing a truncated view.
 */
struct ParsedFrame {
  bool valid;
  FrameType frame_type;
  uint8_t class_byte;
  uint8_t opspec;
  uint16_t sub_id;
  uint16_t obj_id;
  const uint8_t* payload;
  size_t payload_len;
  bool crc_valid;
  bool multi_apdu;

  // Default constructor
  ParsedFrame() 
    : valid(false), 
      frame_type(FrameType::INVALID),
      class_byte(0),
      opspec(0),
      sub_id(0),
      obj_id(0),
      payload(nullptr),
      payload_len(0),
      crc_valid(false),
      multi_apdu(false) {}
};

/**
 * Parse raw GENI frame into structured data.
 * 
 * This function validates the frame structure and extracts all fields.
 * It performs the following validations:
 * 1. Minimum length (8 bytes)
 * 2. Valid start byte (0x27 or 0x24)
 * 3. CRC checksum
 * 
 * @param data Raw frame bytes from BLE notification or response
 * @param len Length of data in bytes
 * @return ParsedFrame with extracted fields and validation flags
 * 
 * Usage:
 *   ParsedFrame frame = protocol::parse_frame(data, len);
 *   if (frame.valid && frame.crc_valid) {
 *     // Process frame based on class_byte and opspec
 *   }
 * 
 * Reference: FrameParser.parse_frame() in frame_parser.py
 */
ParsedFrame parse_frame(const uint8_t* data, size_t len);

/**
 * Validate frame integrity.
 * 
 * Checks all validation flags and returns detailed error message if invalid.
 * 
 * @param frame Parsed frame from parse_frame()
 * @return true if all checks pass, false otherwise
 * 
 * Reference: FrameParser.validate_frame_integrity() in frame_parser.py
 */
bool validate_frame_integrity(const ParsedFrame& frame);

/**
 * Check if frame is a telemetry notification.
 * 
 * Telemetry frames are Class 10 responses with known Sub-ID/Object ID pairs.
 * 
 * @param frame Parsed frame from parse_frame()
 * @return true if frame is a known telemetry type
 * 
 * Reference: FrameParser.is_telemetry_frame() in frame_parser.py
 */
bool is_telemetry_frame(const ParsedFrame& frame);

/**
 * Does a complete GENI frame's trailing CRC-16 match its contents?
 *
 * `[Start][Length][...Length_field bytes...][CRC-H][CRC-L]`, with the CRC
 * covering offset 1 through the last APDU byte -- Start and the CRC itself are
 * excluded. Callers must pass a frame already trimmed to its declared length;
 * trailing bytes beyond it are not part of what the CRC covers.
 *
 * Split out of parse_frame() so the command-response path can reject a
 * corrupt frame without paying for a full parse, and so both paths compute the
 * same thing. Everything a pump write is confirmed against arrives this way,
 * so accepting an unverified frame means deciding write verdicts from bytes
 * the radio may have mangled.
 */
bool frame_crc_valid(const uint8_t* data, size_t len);

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
