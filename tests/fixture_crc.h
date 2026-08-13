#pragma once

/**
 * Stamp a real CRC into a fixture frame.
 *
 * Every response fixture in this suite used to end in a literal `0xAA 0xBB` --
 * a garbage CRC -- and was accepted, because the command-response path did not
 * check it. Once Transport rejects a bad CRC those fixtures stop being
 * responses at all, so they have to carry the real thing.
 *
 * The CRC is computed with the PRODUCTION `calc_crc16_read`, not a copy of it.
 * That is deliberate and load-bearing: a private implementation here would be
 * the same defect the mutation check exists to catch -- corrupt the shipped CRC
 * and the fixtures would keep agreeing with themselves while every frame the
 * device emits carried a wrong checksum. Sharing the function means the suite
 * can only pass when production and fixtures agree.
 *
 * Deliberately-corrupt frames bypass this and are injected raw; see the
 * bad-CRC tests in test_transport_fsm.cpp.
 */

#include <cstdint>
#include <vector>

#include "../components/alpha_hwr/codec.h"

/**
 * Overwrite the last two bytes of `frame` with its correct GENI CRC-16.
 *
 * Frames shorter than 4 bytes are returned untouched: there is no room for
 * Start + Length + a covered byte + the CRC, so there is nothing to stamp, and
 * such a runt is exactly what the transport is supposed to reject.
 */
inline std::vector<uint8_t> with_crc(std::vector<uint8_t> frame) {
  if (frame.size() >= 4) {
    uint16_t crc = esphome::alpha_hwr::protocol::calc_crc16_read(
        frame.data() + 1, frame.size() - 3);
    frame[frame.size() - 2] = static_cast<uint8_t>(crc >> 8);
    frame[frame.size() - 1] = static_cast<uint8_t>(crc & 0xFF);
  }
  return frame;
}
