/**
 * GENI Protocol Frame Parser Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/protocol/frame_parser.py
 */

#include "frame_parser.h"
#include "frame_builder.h"  // For FRAME_START and CLASS_10 constants
#include "codec.h"
#include "esphome/core/log.h"

namespace esphome {
namespace alpha_hwr {
namespace protocol {

static const char* TAG = "alpha_hwr.frame_parser";

ParsedFrame parse_frame(const uint8_t* data, size_t len) {
  // Initialize result with invalid state
  ParsedFrame result;
  result.valid = false;
  result.frame_type = FrameType::INVALID;
  result.class_byte = 0;
  result.opspec = 0;
  result.sub_id = 0;
  result.obj_id = 0;
  result.payload = nullptr;
  result.payload_len = 0;
  result.crc_valid = false;

  // Validate minimum length (need at least: Start + Len + SvcH + SvcL + Class + OpSpec + CRC-H + CRC-L = 8 bytes)
  if (len < 8) {
    ESP_LOGV(TAG, "Frame too short: %d bytes (need >= 8)", len);
    return result;
  }

  // Validate start byte
  uint8_t start_byte = data[0];
  if (start_byte == RESPONSE_START) {
    result.frame_type = FrameType::RESPONSE;
  } else if (start_byte == FRAME_START) {
    result.frame_type = FrameType::REQUEST;
  } else {
    ESP_LOGV(TAG, "Invalid start byte: 0x%02X (expected 0x24 or 0x27)", start_byte);
    return result;
  }

  // Frame is structurally valid
  result.valid = true;

  // Validate embedded length byte against actual data length.
  // Expected total = 1(Start) + 1(Length) + Length_field + 2(CRC) = Length_field + 4
  uint8_t length_field = data[1];
  size_t expected_total = static_cast<size_t>(length_field) + 4;
  if (len < expected_total) {
    // Insufficient bytes — the frame is truncated and cannot be trusted.
    ESP_LOGV(TAG, "Frame too short: header says %zu total, got %zu bytes", expected_total, len);
    result.valid = false;
    return result;
  }
  if (len > expected_total) {
    // Extra trailing bytes — clamp to expected length so CRC validation and
    // payload extraction use the correct window rather than the extra bytes.
    ESP_LOGV(TAG, "Frame has trailing bytes: header says %zu total, got %zu bytes; clamping", expected_total, len);
    len = expected_total;
  }

  // Validate CRC
  // CRC covers from Length byte (offset 1) to end of APDU (len - 3), excluding Start and CRC itself
  uint16_t calculated_crc = calc_crc16_read(data + 1, len - 3);
  uint16_t actual_crc = (data[len - 2] << 8) | data[len - 1];
  result.crc_valid = (calculated_crc == actual_crc);

  if (!result.crc_valid) {
    ESP_LOGV(TAG, "CRC mismatch: calculated=0x%04X, actual=0x%04X", calculated_crc, actual_crc);
  }

  // Extract class byte (offset 4 in frame)
  result.class_byte = data[4];

  // Extract OpSpec (offset 5) if present
  if (len > 5) {
    result.opspec = data[5];
  }

  // Parse based on class
  if (result.class_byte == CLASS_10 && len > 5) {
    uint8_t opspec = result.opspec;

    // OpSpecs for register-read responses: 0x30 (motor), 0x2B (flow), 0x14 (temp), 0x09 (alarms/warnings), etc.
    // Format: [Class][OpSpec][Seq(2)][Id(2)][Res(2)][DataLen][Data...]
    if (opspec == OPSPEC_MOTOR_STATE || 
        opspec == OPSPEC_FLOW_PRESSURE || 
        opspec == OPSPEC_TEMPERATURE || 
        opspec == OPSPEC_ALARMS_WARNINGS ||
        opspec == 0x2E ||  // Other register read responses
        opspec == 0x2D) {
      if (len >= 15) {
        // For register read responses, payload starts at offset 13
        result.payload = data + 13;
        result.payload_len = len - 15;  // Subtract header (13 bytes) + CRC (2 bytes)
        // Store sequence number as sub_id and register ID as obj_id for routing
        result.sub_id = (data[6] << 8) | data[7];  // Sequence number (big-endian)
        result.obj_id = (data[8] << 8) | data[9];  // Register ID (big-endian)
      }
    } else if (opspec == OPSPEC_PASSIVE_NOTIF || opspec == OPSPEC_ALARMS_READ) {
      // Class 10 Notification/Passive: [Class][OpSpec][SubH][SubL][ObjH][ObjL][Payload...][CRC]
      if (len >= 12) {
        result.sub_id = (data[6] << 8) | data[7];  // Big-endian uint16
        result.obj_id = (data[8] << 8) | data[9];  // Big-endian uint16
        result.payload = data + 10;  // From after ObjID
        result.payload_len = len - 12;  // Subtract header (10 bytes) + CRC (2 bytes)
      }
    } else {
      // Default Class 10 format: Sub-ID and Obj-ID at offsets 6-9
      if (len >= 12) {
        result.sub_id = (data[6] << 8) | data[7];  // Big-endian uint16
        result.obj_id = (data[8] << 8) | data[9];  // Big-endian uint16
        result.payload = data + 10;  // From after ObjID
        result.payload_len = len - 12;  // Subtract header (10 bytes) + CRC (2 bytes)
      }
    }
  } else {
    // Class 2/3: Payload starts after OpSpec
    // Format: [Start][Len][SvcH][SvcL][Class][OpSpec][Register...][Payload...][CRC]
    if (len > 6) {
      result.payload = data + 6;  // From after OpSpec
      result.payload_len = len - 8;  // Subtract header (6 bytes) + CRC (2 bytes)
    }
  }

  return result;
}

bool validate_frame_integrity(const ParsedFrame& frame) {
  if (!frame.valid) {
    ESP_LOGD(TAG, "Frame validation failed: Invalid frame structure (bad start byte or length)");
    return false;
  }

  if (!frame.crc_valid) {
    ESP_LOGD(TAG, "Frame validation failed: CRC checksum mismatch");
    return false;
  }

  if (frame.class_byte == 0) {
    ESP_LOGD(TAG, "Frame validation failed: Missing class byte");
    return false;
  }

  if (frame.class_byte == CLASS_10 && (frame.sub_id == 0 && frame.obj_id == 0)) {
    // Note: This check is lenient - some Class 10 frames might legitimately have zero IDs
    ESP_LOGV(TAG, "Class 10 frame has zero Sub-ID and Object ID");
  }

  return true;
}

bool is_telemetry_frame(const ParsedFrame& frame) {
  if (!frame.valid || frame.class_byte != CLASS_10) {
    return false;
  }

  // Known telemetry object IDs and sub IDs
  // Format: (obj_id, sub_id)
  struct TelemetryId {
    uint16_t obj_id;
    uint16_t sub_id;
  };

  static const TelemetryId TELEMETRY_OBJECTS[] = {
    {87, 69},      // Motor state (Obj 87, Sub 69 = 0x0057, 0x0045)
    {93, 290},     // Flow/Pressure (Obj 93, Sub 290 = 0x005D, 0x0122)
    {93, 300},     // Temperature (Obj 93, Sub 300 = 0x005D, 0x012C)
    {88, 0},       // Active alarms (Obj 88, Sub 0 = 0x0058, 0x0000)
    {88, 11},      // Active warnings (Obj 88, Sub 11 = 0x0058, 0x000B)
    {3, 1},        // Custom electrical
    {0x2D01, 1},   // Custom speed/power
    {0x1602, 2},   // Custom temperature
  };

  static const size_t NUM_TELEMETRY_OBJECTS = sizeof(TELEMETRY_OBJECTS) / sizeof(TELEMETRY_OBJECTS[0]);

  for (size_t i = 0; i < NUM_TELEMETRY_OBJECTS; i++) {
    if (frame.obj_id == TELEMETRY_OBJECTS[i].obj_id && 
        frame.sub_id == TELEMETRY_OBJECTS[i].sub_id) {
      return true;
    }
  }

  return false;
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
