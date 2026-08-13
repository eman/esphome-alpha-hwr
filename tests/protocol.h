#pragma once

/**
 * Thin forwarding shim onto the production protocol code.
 *
 * This header used to contain its own copy of the CRC-16 table, the Class 10
 * read-packet builder and the big-endian readers, and tests/README.md told
 * contributors to keep mirroring production into it. That made
 * test_protocol.cpp a test of the copy rather than of the firmware: corrupting
 * codec.cpp's CRC initial value and swapping the CRC bytes in
 * frame_builder.cpp -- a total protocol break where every frame the device
 * emits carries a wrong checksum -- left the suite passing with byte-identical
 * output.
 *
 * The names below are kept so test_protocol.cpp reads unchanged; each one now
 * calls the shipped implementation. Anything added here must forward, never
 * reimplement.
 */

#include <cstddef>
#include <cstdint>

#include "../components/alpha_hwr/codec.h"
#include "../components/alpha_hwr/frame_builder.h"

namespace geni_protocol {

namespace proto = esphome::alpha_hwr::protocol;

/// CRC-16-CCITT over `len` bytes. Production takes an `init` with a default.
inline uint16_t calc_crc16(const uint8_t *data, size_t len) {
  return proto::calc_crc16(data, len);
}

/// CRC-16-CCITT with the final XOR used for READ operations.
inline uint16_t calc_crc16_read(const uint8_t *data, size_t len) {
  return proto::calc_crc16_read(data, len);
}

/// Class 10 READ request. Production names this build_class10_read() and takes
/// the source address as a defaulted third argument.
inline void build_class10_read_packet(uint32_t register_addr,
                                      uint8_t *packet_out) {
  proto::build_class10_read(register_addr, packet_out);
}

/// Big-endian IEEE-754 float. Production names this decode_float_be().
inline float read_float_be(const uint8_t *data, size_t offset) {
  return proto::decode_float_be(data, offset);
}

}  // namespace geni_protocol
