#include "frame_builder.h"
#include "codec.h"

namespace esphome {
namespace alpha_hwr {
namespace protocol {

// Build Class 10 register READ request for telemetry
// Reference: alpha_hwr/protocol/frame_builder.py::build_class10_read()
void build_class10_read(uint32_t register_addr, uint8_t *packet_out, uint8_t source) {
  // Encode register as 3 bytes (big-endian)
  uint8_t reg_high = (register_addr >> 16) & 0xFF;
  uint8_t reg_mid = (register_addr >> 8) & 0xFF;
  uint8_t reg_low = register_addr & 0xFF;
  
  // Frame structure: [27] [07] [E7] [F8] [0A] [03] [Reg-H] [Reg-M] [Reg-L] [CRC-H] [CRC-L]
  packet_out[0] = FRAME_START;      // 0x27
  packet_out[1] = 0x07;             // Length (7 bytes: E7 F8 0A 03 + 3 register bytes)
  packet_out[2] = SERVICE_ID_HIGH;  // 0xE7
  packet_out[3] = source;           // 0xF8 (default)
  packet_out[4] = CLASS_10;         // 0x0A
  packet_out[5] = 0x03;             // OpSpec (READ)
  packet_out[6] = reg_high;         // Register high byte
  packet_out[7] = reg_mid;          // Register middle byte
  packet_out[8] = reg_low;          // Register low byte
  
  // Calculate CRC over bytes 1-8 (everything after start byte, before CRC)
  uint16_t crc = calc_crc16_read(&packet_out[1], 8);
  packet_out[9] = (crc >> 8) & 0xFF;   // CRC high byte
  packet_out[10] = crc & 0xFF;         // CRC low byte
}

// Build Class 10 DataObject SET operation
// Reference: alpha_hwr/protocol/frame_builder.py::build_data_object_set()
size_t build_data_object_set(uint16_t sub_id, uint16_t obj_id,
                              const uint8_t *data, size_t data_len,
                              uint8_t *packet_out, uint8_t source) {
  // Calculate frame length
  // Length = ServiceID + Source + Class + OpSpec + SubID (2) + ObjID (2) + Data
  size_t length = 1 + 1 + 1 + 1 + 2 + 2 + data_len;
  
  // Validate: OpSpec bits 5-0 encode (length - 4), which must fit in 6 bits (max 63).
  // That limits data_len to 59 bytes (63 - 4 for SubID + ObjID).
  if (data_len > 59) {
    return 0;  // Payload too large for single-frame SET
  }
  
  // Calculate OpSpec (bit 7 set = SET, bits 6-0 = data length including IDs)
  // op_bits = length - 4 (subtract ServiceID, Source, Class, OpSpec)
  uint8_t op_bits = (length - 4) & 0x3F;
  uint8_t op_spec = 0x80 | op_bits;
  
  // Build frame header
  packet_out[0] = FRAME_START;      // 0x27
  packet_out[1] = length & 0xFF;    // Length byte
  packet_out[2] = SERVICE_ID_HIGH;  // 0xE7
  packet_out[3] = source;           // 0xF8 (default)
  
  // Build APDU
  packet_out[4] = CLASS_10;                  // 0x0A
  packet_out[5] = op_spec;                   // OpSpec (SET + length)
  packet_out[6] = (sub_id >> 8) & 0xFF;      // Sub ID high byte
  packet_out[7] = sub_id & 0xFF;             // Sub ID low byte
  packet_out[8] = (obj_id >> 8) & 0xFF;      // Obj ID high byte
  packet_out[9] = obj_id & 0xFF;             // Obj ID low byte
  
  // Copy data payload (if any)
  if (data != nullptr && data_len > 0) {
    for (size_t i = 0; i < data_len; i++) {
      packet_out[10 + i] = data[i];
    }
  }
  
  // Calculate CRC over everything after start byte, before CRC
  size_t crc_data_len = 1 + length;  // Length byte + all data
  uint16_t crc = calc_crc16_read(&packet_out[1], crc_data_len);
  
  // Append CRC
  size_t crc_offset = 1 + crc_data_len;
  packet_out[crc_offset] = (crc >> 8) & 0xFF;     // CRC high byte
  packet_out[crc_offset + 1] = crc & 0xFF;        // CRC low byte
  
  // Return total packet length (frame + CRC)
  return crc_offset + 2;
}

// Build INFO command for reading register value
// Reference: alpha_hwr/protocol/frame_builder.py::build_command_info()
size_t build_command_info(uint8_t class_byte, uint32_t register_addr,
                           uint8_t *packet_out, uint8_t source) {
  // Encode register address (variable length based on value)
  uint8_t reg_bytes[3];
  size_t reg_len;
  
  if (register_addr > 0xFFFF) {
    // 3-byte register
    reg_bytes[0] = (register_addr >> 16) & 0xFF;
    reg_bytes[1] = (register_addr >> 8) & 0xFF;
    reg_bytes[2] = register_addr & 0xFF;
    reg_len = 3;
  } else if (register_addr > 0xFF) {
    // 2-byte register
    reg_bytes[0] = (register_addr >> 8) & 0xFF;
    reg_bytes[1] = register_addr & 0xFF;
    reg_len = 2;
  } else {
    // 1-byte register
    reg_bytes[0] = register_addr & 0xFF;
    reg_len = 1;
  }
  
  // OpSpec: INFO operation (bits 7-6 = 00), length = register bytes
  // OS_INFO = 0x00, so OpSpec byte is just the register length
  uint8_t op_length_byte = reg_len;
  
  // Calculate frame length
  size_t length = 1 + 1 + 1 + 1 + reg_len;  // ServiceID + Source + Class + OpSpec + Register
  
  // Build frame
  packet_out[0] = FRAME_START;      // 0x27
  packet_out[1] = length & 0xFF;    // Length byte
  packet_out[2] = SERVICE_ID_HIGH;  // 0xE7
  packet_out[3] = source;           // 0xF8 (default)
  packet_out[4] = class_byte;       // Class (2 or 3)
  packet_out[5] = op_length_byte;   // OpSpec
  
  // Copy register bytes
  for (size_t i = 0; i < reg_len; i++) {
    packet_out[6 + i] = reg_bytes[i];
  }
  
  // Calculate CRC over everything after start byte, before CRC
  size_t crc_data_len = 1 + length;  // Length byte + all data
  uint16_t crc = calc_crc16_read(&packet_out[1], crc_data_len);
  
  // Append CRC
  size_t crc_offset = 1 + crc_data_len;
  packet_out[crc_offset] = (crc >> 8) & 0xFF;     // CRC high byte
  packet_out[crc_offset + 1] = crc & 0xFF;        // CRC low byte
  
  // Return total packet length (frame + CRC)
  return crc_offset + 2;
}

// Build generic GENI packet with APDU
// Reference: alpha_hwr/services/base.py::_build_geni_packet()
size_t build_geni_packet(uint8_t service_id, uint8_t source,
                          const uint8_t *apdu, size_t apdu_len,
                          uint8_t *packet_out) {
  // Calculate frame length: ServiceID (1) + Source (1) + APDU
  size_t length = 1 + 1 + apdu_len;

  // Validate: length field must fit in a single byte (max 255)
  if (length > 255) {
    return 0;  // APDU too large for single GENI frame
  }
  
  // Build frame header
  packet_out[0] = FRAME_START;  // 0x27
  packet_out[1] = length & 0xFF;
  packet_out[2] = service_id;
  packet_out[3] = source;
  
  // Copy APDU
  for (size_t i = 0; i < apdu_len; i++) {
    packet_out[4 + i] = apdu[i];
  }
  
  // Calculate CRC over everything after start byte, before CRC
  size_t crc_data_len = 1 + length;  // Length byte + ServiceID + Source + APDU
  uint16_t crc = calc_crc16_read(&packet_out[1], crc_data_len);
  
  // Append CRC
  size_t crc_offset = 1 + crc_data_len;
  packet_out[crc_offset] = (crc >> 8) & 0xFF;     // CRC high byte
  packet_out[crc_offset + 1] = crc & 0xFF;        // CRC low byte
  
  // Return total packet length (frame + CRC)
  return crc_offset + 2;
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
