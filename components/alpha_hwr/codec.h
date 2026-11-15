#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace protocol {

/**
 * Codec module for GENI protocol primitive encoding/decoding.
 * 
 * This module provides functions for encoding and decoding basic data types
 * used in the GENI protocol. All multi-byte values use Big-Endian byte order.
 * 
 * Data Types:
 * - Float (32-bit IEEE 754): Used for telemetry values (flow, pressure, power, etc.)
 * - Uint16 (16-bit unsigned): Used for integer values, alarms, etc.)
 * - Uint32 (32-bit unsigned): Used for timestamps, counters, etc.
 * 
 * Byte Order: Big-Endian
 * All GENI protocol values use network byte order (big-endian).
 * 
 * Reference: alpha_hwr/protocol/codec.py
 */

// CRC-16-CCITT lookup table for GENI protocol
// This is the same table used in alpha_hwr.h
extern const uint16_t CRC_TABLE[256];

/**
 * Calculate CRC-16-CCITT checksum (No Final XOR).
 * Used for WRITE operations.
 * 
 * @param data Pointer to data buffer
 * @param len Length of data in bytes
 * @param init Initial CRC value (default: 0xFFFF)
 * @return Calculated CRC value
 * 
 * Reference: alpha_hwr/utils.py::calc_crc16()
 */
uint16_t calc_crc16(const uint8_t *data, size_t len, uint16_t init = 0xFFFF);

/**
 * Calculate CRC-16-CCITT checksum (With Final XOR).
 * Used for READ/INFO/EXECUTE operations.
 * 
 * @param data Pointer to data buffer
 * @param len Length of data in bytes
 * @param init Initial CRC value (default: 0xFFFF)
 * @return Calculated CRC value (with final XOR)
 * 
 * Reference: alpha_hwr/utils.py::calc_crc16_read()
 */
uint16_t calc_crc16_read(const uint8_t *data, size_t len, uint16_t init = 0xFFFF);

/**
 * Encode a float as 4-byte big-endian IEEE 754.
 * 
 * @param value Float value to encode
 * @param bytes Output buffer (must be at least 4 bytes)
 * 
 * Reference: alpha_hwr/protocol/codec.py::encode_float_be()
 */
void encode_float_be(float value, uint8_t *bytes);

/**
 * Decode a big-endian float from bytes.
 * 
 * @param data Input byte buffer
 * @param offset Starting position in byte array
 * @return Decoded float value
 * 
 * Reference: alpha_hwr/protocol/codec.py::decode_float_be()
 */
float decode_float_be(const uint8_t *data, size_t offset = 0);

/**
 * Decode a big-endian IEEE-754 double.
 *
 * @param data Buffer containing at least eight bytes after offset
 * @param offset Offset of the double (default 0)
 * @return Decoded value, including IEEE-754 NaN/infinity when encoded
 */
double decode_double_be(const uint8_t *data, size_t offset = 0);

/**
 * Decode a big-endian unsigned 24-bit integer.
 *
 * @param data Buffer containing at least three bytes after offset
 * @param offset Offset of the integer (default 0)
 * @return Decoded value in a uint32_t
 */
uint32_t decode_uint24_be(const uint8_t *data, size_t offset = 0);

/**
 * Encode a 16-bit unsigned integer as big-endian bytes.
 * 
 * @param value Integer value to encode (0-65535)
 * @param bytes Output buffer (must be at least 2 bytes)
 * 
 * Reference: alpha_hwr/protocol/codec.py::encode_uint16_be()
 */
void encode_uint16_be(uint16_t value, uint8_t *bytes);

/**
 * Decode a big-endian 16-bit unsigned integer.
 * 
 * @param data Input byte buffer
 * @param offset Starting position in byte array
 * @return Decoded integer value (0-65535)
 * 
 * Reference: alpha_hwr/protocol/codec.py::decode_uint16_be()
 */
uint16_t decode_uint16_be(const uint8_t *data, size_t offset = 0);

/**
 * Encode a 32-bit unsigned integer as big-endian bytes.
 * 
 * @param value Integer value to encode (0-4294967295)
 * @param bytes Output buffer (must be at least 4 bytes)
 * 
 * Reference: alpha_hwr/protocol/codec.py::encode_uint32_be()
 */
void encode_uint32_be(uint32_t value, uint8_t *bytes);

/**
 * Decode a big-endian 32-bit unsigned integer.
 * 
 * @param data Input byte buffer
 * @param offset Starting position in byte array
 * @return Decoded integer value (0-4294967295)
 * 
 * Reference: alpha_hwr/protocol/codec.py::decode_uint32_be()
 */
uint32_t decode_uint32_be(const uint8_t *data, size_t offset = 0);

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
