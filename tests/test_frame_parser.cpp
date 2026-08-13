/**
 * Host tests for protocol::parse_frame().
 *
 * These compile the REAL frame_parser.cpp and codec.cpp -- not a replica --
 * because the point is to pin the behaviour of the shipped parser against
 * malformed input arriving from the radio. Nothing else in the suite feeds
 * parse_frame() a truncated, over-long or bad-CRC frame.
 */

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <vector>

#include "../components/alpha_hwr/codec.h"
#include "../components/alpha_hwr/frame_parser.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::protocol::FrameType;
using esphome::alpha_hwr::protocol::ParsedFrame;
using esphome::alpha_hwr::protocol::parse_frame;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                       \
  do {                                               \
    if (cond) {                                      \
      tests_passed++;                                \
      std::cout << "[PASS] " << msg << std::endl;    \
    } else {                                         \
      tests_failed++;                                \
      std::cout << "[FAIL] " << msg << std::endl;    \
    }                                                \
  } while (0)

/// Build a frame with a correct CRC over [Length .. end of APDU].
/// Layout: [0x24][len_field][SvcH][SvcL][Class][OpSpec][body...][CRC-H][CRC-L]
static std::vector<uint8_t> build_frame(uint8_t class_byte, uint8_t opspec,
                                        const std::vector<uint8_t> &body) {
  std::vector<uint8_t> f;
  f.push_back(0x24);
  f.push_back(0);  // placeholder length_field
  f.push_back(0x00);
  f.push_back(0x07);
  f.push_back(class_byte);
  f.push_back(opspec);
  f.insert(f.end(), body.begin(), body.end());
  // total = f.size() + 2 CRC bytes; length_field = total - 4
  f[1] = static_cast<uint8_t>(f.size() + 2 - 4);
  uint16_t crc = esphome::alpha_hwr::protocol::calc_crc16_read(f.data() + 1,
                                                               f.size() - 1);
  f.push_back((crc >> 8) & 0xFF);
  f.push_back(crc & 0xFF);
  return f;
}

static void test_rejects_runts() {
  std::cout << "\n=== Malformed: runts and bad start bytes ===" << std::endl;

  uint8_t empty[1] = {0x24};
  ParsedFrame r = parse_frame(empty, 0);
  TEST_ASSERT(!r.valid, "Zero-length input is rejected");

  uint8_t seven[7] = {0x24, 0x03, 0x00, 0x07, 0x03, 0x01, 0x00};
  r = parse_frame(seven, 7);
  TEST_ASSERT(!r.valid, "Frame shorter than the 8-byte minimum is rejected");

  uint8_t bad_start[10] = {0x99, 0x06, 0x00, 0x07, 0x0A, 0x02, 0, 0, 0, 0};
  r = parse_frame(bad_start, 10);
  TEST_ASSERT(!r.valid, "Unknown start byte is rejected");
}

static void test_truncated_against_length_field() {
  std::cout << "\n=== Malformed: declared length exceeds bytes supplied ==="
            << std::endl;

  auto f = build_frame(0x0A, 0x02, {1, 2, 3, 4, 5, 6, 7, 8});
  // Claim far more than we hand over.
  f[1] = 0x40;
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(!r.valid,
              "Frame whose length_field overruns the buffer is rejected");
}

static void test_class3_payload_len_underflow() {
  std::cout << "\n=== Regression: Class 2/3 payload_len underflow ==="
            << std::endl;

  // A Class 3 frame whose declared length clamps the window to exactly 7 bytes.
  // Old code guarded `len > 6` then computed `len - 8`, wrapping payload_len to
  // SIZE_MAX while reporting valid=true.
  std::vector<uint8_t> f = {0x24, 0x03, 0x00, 0x07, 0x03, 0x01, 0xAA, 0xBB};
  // length_field 3 -> expected_total 7, so the 8-byte buffer is clamped to 7.
  ParsedFrame r = parse_frame(f.data(), f.size());

  TEST_ASSERT(r.payload_len < 1024,
              "Clamped 7-byte Class 3 frame does not underflow payload_len");
  TEST_ASSERT(!(r.payload != nullptr && r.payload_len == SIZE_MAX),
              "No payload pointer is published with a SIZE_MAX length");
}

static void test_short_length_field_does_not_read_past_window() {
  std::cout << "\n=== Regression: class byte read past the clamped window ==="
            << std::endl;

  // length_field 0 -> expected_total 4, so the window is shorter than offset 4.
  std::vector<uint8_t> f = {0x24, 0x00, 0x00, 0x07, 0x0A, 0x02, 0x00, 0x00,
                            0x00, 0x00};
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.class_byte == 0,
              "class_byte is not read from beyond the clamped frame window");
}

static void test_trailing_bytes_are_clamped_for_crc() {
  std::cout << "\n=== Trailing bytes ===" << std::endl;

  auto f = build_frame(0x0A, 0x0E, {0x00, 0x00, 0xDE, 0x01, 1, 2, 3, 4});
  size_t real_len = f.size();
  for (int i = 0; i < 12; i++)
    f.push_back(0xEE);  // junk after a complete frame

  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.valid, "Frame with trailing bytes still parses");
  TEST_ASSERT(r.crc_valid,
              "CRC is computed over the declared frame, not the trailing junk");

  ParsedFrame clean = parse_frame(f.data(), real_len);
  TEST_ASSERT(clean.crc_valid == r.crc_valid,
              "Clamped and exact parses agree on CRC validity");
}

static void test_crc_detection() {
  std::cout << "\n=== CRC ===" << std::endl;

  auto good = build_frame(0x0A, 0x0E, {0x00, 0x00, 0xDE, 0x01, 9, 9, 9, 9});
  ParsedFrame r = parse_frame(good.data(), good.size());
  TEST_ASSERT(r.valid && r.crc_valid, "Well-formed frame reports crc_valid");

  auto bad = good;
  bad[bad.size() - 1] ^= 0xFF;
  r = parse_frame(bad.data(), bad.size());
  TEST_ASSERT(!r.crc_valid, "Corrupted CRC byte is detected");

  auto flipped = good;
  flipped[7] ^= 0x01;  // mutate a payload byte, leave the CRC alone
  r = parse_frame(flipped.data(), flipped.size());
  TEST_ASSERT(!r.crc_valid, "Corrupted payload byte is detected");
}

static void test_class10_id_extraction() {
  std::cout << "\n=== Class 10 ID extraction (big-endian) ===" << std::endl;

  // Default Class 10 layout: [SubH][SubL][ObjH][ObjL][payload...]
  auto f = build_frame(0x0A, 0x03, {0x01, 0x22, 0x00, 0x5D, 7, 7, 7, 7});
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.sub_id == 0x0122, "Sub-ID decoded big-endian");
  TEST_ASSERT(r.obj_id == 0x005D, "Object ID decoded big-endian");
  TEST_ASSERT(r.payload_len == f.size() - 12,
              "Class 10 payload_len excludes header and CRC");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Frame Parser Tests (malformed input)" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_rejects_runts();
  test_truncated_against_length_field();
  test_class3_payload_len_underflow();
  test_short_length_field_does_not_read_past_window();
  test_trailing_bytes_are_clamped_for_crc();
  test_crc_detection();
  test_class10_id_extraction();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
