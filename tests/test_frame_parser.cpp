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
// NOTE on `opspec`: it is the APDU head byte, whose low six bits declare the
// body length. Keep it consistent with `body.size()` unless the test is
// deliberately about an inconsistent header -- since issue #226 the parser
// bounds the payload by what the head declares, so a fixture that under-declares
// gets a truncated payload and one that over-declares is clamped to the frame.
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

// ── Multi-APDU telegrams (issue #226) ───────────────────────────────────────
// A telegram may carry several APDUs; App C.17 says errors are reported
// per-APDU, so an error reply substitutes for one answer inside a telegram
// carrying others. The parser used to return everything between the header and
// the CRC, so the second APDU arrived as part of the first one's payload with
// nothing to indicate it.
//
// Worth recording what this is NOT: the frame issue #226 cites as its example,
// `24 07 F8 E7 0A 81 00 40 40 5E BF`, does not actually reach the line it
// blames. Its class byte is 0x0A, so it routes to the Class 10 branch, whose
// default arm needs len >= 12; at 11 bytes it sets no payload at all. The
// exposure is real but lives in the other arms, which is what these pin.
static void test_a_second_apdu_is_not_reported_as_payload() {
  std::cout << "\n=== Multi-APDU: the payload stops at the first APDU ===" << std::endl;

  // Class 2 reply: APDU 1 = [02][01][34] (ok, one payload byte), then a
  // zero-length Unknown Class error for a second APDU: [40][40].
  auto f = build_frame(0x02, 0x01, {0x34, 0x40, 0x40});
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.valid && r.crc_valid, "the telegram is well formed");
  TEST_ASSERT(r.payload_len == 1,
              "payload is the one byte APDU 1 declares, not the three between "
              "the header and the CRC");
  TEST_ASSERT(r.payload && r.payload[0] == 0x34, "and it is APDU 1's byte");
  TEST_ASSERT(r.multi_apdu, "the frame reports that more followed");

  // Class 10 default arm: head 0x08 = 8 body bytes (4 ID + 4 payload), then a
  // second APDU.
  auto g = build_frame(0x0A, 0x08, {0x00, 0x01, 0x2F, 0x01, 0xDE, 0xAD, 0xBE, 0xEF, 0x40, 0x40});
  ParsedFrame rg = parse_frame(g.data(), g.size());
  TEST_ASSERT(rg.payload_len == 4,
              "Class 10 payload stops at APDU 1 too -- it used to run one byte "
              "into the next APDU");
  TEST_ASSERT(rg.payload && rg.payload[3] == 0xEF, "...ending on APDU 1's last byte");
  TEST_ASSERT(rg.multi_apdu, "and multi_apdu is set here as well");
}

// The flag must stay off for the single-APDU frames that are everything the
// component actually sees, or it is useless as a refusal signal.
static void test_single_apdu_frames_are_not_flagged() {
  std::cout << "\n=== Multi-APDU: ordinary frames are not flagged ===" << std::endl;

  auto f = build_frame(0x02, 0x03, {0x34, 0x07, 0x02});
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.payload_len == 3 && !r.multi_apdu,
              "a single-APDU Class 2 reply is unchanged and unflagged");

  auto g = build_frame(0x0A, 0x0E, {0x00, 0x01, 0x2F, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  ParsedFrame rg = parse_frame(g.data(), g.size());
  TEST_ASSERT(rg.payload_len == 10 && !rg.multi_apdu,
              "and so is a passive notification, whose head declares exactly its body");
}

// A head declaring more than the frame holds is trusted no further than the
// frame itself, and is not mistaken for a multi-APDU telegram.
static void test_an_overlong_declaration_is_clamped_not_flagged() {
  std::cout << "\n=== Multi-APDU: an over-declaring head is clamped ===" << std::endl;

  auto f = build_frame(0x02, 0x3F, {0x34, 0x07, 0x02});  // declares 63, carries 3
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(r.payload_len == 3, "payload is bounded by the frame, not the claim");
  TEST_ASSERT(!r.multi_apdu, "and nothing follows it, so the flag stays off");
}

static void test_class3_payload_len_underflow() {
  std::cout << "\n=== Regression: Class 2/3 payload_len underflow ==="
            << std::endl;

  // A Class 3 frame declaring a 7-byte total. Old code guarded `len > 6` then
  // computed `len - 8`, wrapping payload_len to SIZE_MAX while reporting
  // valid=true.
  //
  // These assertions were passing for the wrong reason, which is worth stating
  // rather than quietly repairing: a later guard rejects any frame declaring a
  // total below the protocol's own 8-byte minimum, so this frame now returns
  // valid=false and never reaches the payload code at all. payload_len was 0
  // from the initialiser, so "does not underflow" held vacuously.
  //
  // Assert the rejection itself, which is what actually protects the caller,
  // and keep the underflow assertions underneath it as a belt-and-braces check
  // on the fields a refused frame hands back.
  std::vector<uint8_t> f = {0x24, 0x03, 0x00, 0x07, 0x03, 0x01, 0xAA, 0xBB};
  ParsedFrame r = parse_frame(f.data(), f.size());

  TEST_ASSERT(!r.valid,
              "A frame declaring a total below the 8-byte minimum is refused "
              "outright -- which is what makes the underflow unreachable");
  TEST_ASSERT(r.payload_len < 1024,
              "Clamped 7-byte Class 3 frame does not underflow payload_len");
  TEST_ASSERT(!(r.payload != nullptr && r.payload_len == SIZE_MAX),
              "No payload pointer is published with a SIZE_MAX length");
}

static void test_short_length_field_does_not_read_past_window() {
  std::cout << "\n=== Regression: class byte read past the clamped window ==="
            << std::endl;

  // length_field 0 -> expected_total 4, below the 8-byte minimum, so this is
  // refused before the class byte is read. Same caveat as the test above: the
  // `class_byte == 0` assertion holds because the frame was rejected, not
  // because a read was bounded. Both are asserted so the reason is visible.
  std::vector<uint8_t> f = {0x24, 0x00, 0x00, 0x07, 0x0A, 0x02, 0x00, 0x00,
                            0x00, 0x00};
  ParsedFrame r = parse_frame(f.data(), f.size());
  TEST_ASSERT(!r.valid, "A frame declaring a 4-byte total is refused");
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
  // Head 0x08 because the body is 8 bytes. It used to be 0x03, which declared
  // three -- a shape no pump emits, and one the parser now believes (#226).
  auto f = build_frame(0x0A, 0x08, {0x01, 0x22, 0x00, 0x5D, 7, 7, 7, 7});
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
  test_a_second_apdu_is_not_reported_as_payload();
  test_single_apdu_frames_are_not_flagged();
  test_an_overlong_declaration_is_clamped_not_flagged();
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
