// Host tests for DeviceInfoService (device_info_service.cpp).
//
// Why this file exists: `esphome compile` was the only thing that compiled
// device_info_service.cpp. It reads the five identity strings the device
// entity is built from.
//
// It used to carry two hand-ported "repairs" for a leading character missing
// from the product name and from the serial. Issue #179 established that the
// pump was never dropping anything: the Class 7 header is six bytes, the
// parser assumed seven, and the repairs were patching over its own off-by-one
// on the two strings anyone had a known-good value for. The version strings,
// which had no such patch, reached Home Assistant a character short.
//
// The first version of this file could not have caught that, because
// make_class7_response() built its fixtures from the same seven-byte
// assumption the parser made. A fixture written to agree with the code under
// test proves only that they agree. The frames below are now transcribed from
// a capture instead — see the byte dumps in each fixture comment.
//
// The completion accounting is the other half. Five reads are queued at once
// and the callback must fire exactly once, after the last one lands, reporting
// failure if any of them did.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "fixture_crc.h"
#include "../components/alpha_hwr/device_info_service.h"
#include "../components/alpha_hwr/session.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::services::DeviceInfoService;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  if (condition) {                                                             \
    tests_passed++;                                                            \
    std::cout << "[PASS] " << message << std::endl;                            \
  } else {                                                                     \
    tests_failed++;                                                            \
    std::cout << "[FAIL] " << message << std::endl;                            \
  }

// A Class 7 ReadString response, built the way the pump builds one:
//
//   [0x24][LEN][DST][SRC][0x07][Count][...string...][CRC-H][CRC-L]
//      0    1     2    3     4     5      6 ..
//
// LEN counts everything after itself bar the CRC, so the total is LEN + 4.
// Count is the number of string bytes, so Count == total - 8. Both hold in
// every captured frame; `payload` is the string bytes exactly as they appear
// on the wire, trailing NUL included where the pump sends one.
//
// Note `string_id` is deliberately absent: the response does not echo it.
// Believing it did is what produced the seven-byte header.
static std::vector<uint8_t> make_class7_response(const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> f = {0x24, 0x00, 0xF8, 0xE7, 0x07,
                            static_cast<uint8_t>(payload.size())};
  f.insert(f.end(), payload.begin(), payload.end());
  f.push_back(0x00);  // CRC placeholder
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  return with_crc(f);
}

/// Convenience: a NUL-terminated string payload, as the pump sends for the
/// product name and the version strings.
static std::vector<uint8_t> nul_terminated(const std::string &value) {
  std::vector<uint8_t> p(value.begin(), value.end());
  p.push_back(0x00);
  return p;
}

/// Convenience: a bare string payload with no terminator. No captured frame
/// looks like this -- every Class 7 string the pump sends, the serial included,
/// carries its NUL. Kept only to prove the parser does not depend on finding
/// one, and labelled so nobody mistakes it for observed behaviour.
static std::vector<uint8_t> unterminated(const std::string &value) {
  return std::vector<uint8_t>(value.begin(), value.end());
}

// Anonymous namespace: several test files in this suite define their own
// Rig, and cppcheck's whole-program pass reports same-named structs across
// translation units as an ODR violation even though each test is its own
// binary. Same treatment as test_read_chain_lifetime.cpp.
namespace {

struct Rig {
  Transport transport;
  DeviceInfoService service{transport};

  /// Outgoing Class 7 read requests, in order, by the string ID they ask for.
  std::vector<uint8_t> requested_ids;

  Rig() {
    transport.set_write_callback([this](const uint8_t *data, size_t len) {
      // Request layout: [27][LEN][E7][F8][07][01][StringID][CRC-H][CRC-L]
      if (len >= 7 && data[0] == 0x27 && data[4] == 0x07)
        this->requested_ids.push_back(data[6]);
      return true;
    });
  }

  void step(int iters = 3) {
    for (int i = 0; i < iters; i++) {
      mock_millis += 51;
      transport.loop();
    }
  }

  /// Let the next queued read go out, then answer it with a raw payload.
  void answer_next_raw(const std::vector<uint8_t> &payload) {
    step();
    auto frame = make_class7_response(payload);
    transport.on_notification(frame.data(), frame.size());
    step();
  }

  /// Let the next queued read go out, then answer it with a NUL-terminated
  /// string -- what the pump sends for every string except the serial.
  void answer_next(const std::string &value) {
    answer_next_raw(nul_terminated(value));
  }
};

}  // namespace


// ── 1. The five reads ────────────────────────────────────────────────────────
void test_reads_the_five_documented_string_ids() {
  std::cout << "\n=== Five Class 7 string reads are queued ===" << std::endl;
  mock_millis = 0;
  Rig r;

  int completions = 0;
  bool reported_ok = false;
  r.service.read_device_info_async([&](bool ok) {
    completions++;
    reported_ok = ok;
  });

  r.answer_next("ALPHA HWR");
  r.answer_next("10000479");
  r.answer_next("V04.02.01");
  r.answer_next("V01.03.00");
  r.answer_next("V06.00.01");

  const std::vector<uint8_t> expected = {1, 9, 50, 52, 58};
  TEST_ASSERT(r.requested_ids == expected,
              "String IDs 1, 9, 50, 52 and 58 are read, in that order");
  TEST_ASSERT(completions == 1,
              "The completion callback fires exactly once, after the last read");
  TEST_ASSERT(reported_ok, "...and reports success when all five landed");

  TEST_ASSERT(r.service.get_product_name() == "ALPHA HWR", "Product name kept");
  TEST_ASSERT(r.service.get_serial_number() == "10000479", "Serial kept");
  TEST_ASSERT(r.service.get_software_version() == "V04.02.01",
              "Software version kept");
  TEST_ASSERT(r.service.get_hardware_version() == "V01.03.00",
              "Hardware version kept");
  TEST_ASSERT(r.service.get_ble_version() == "V06.00.01", "BLE version kept");
}

// ── 2. The captured frames, byte for byte ────────────────────────────────────
// These are the frames from issue #179, transcribed rather than generated.
// Each one is a whole frame as the pump emitted it, so nothing here can agree
// with the parser by construction: if the header length is wrong, the expected
// string does not come out.
//
// The capture is twelve bytes wide, so the tails are reconstructed from the
// frame length (total = LEN + 4) and the count byte, both of which the visible
// prefix pins. The two version strings are the ones with an independent
// witness: the Grundfos GO app displays them in full.
void test_captured_frames_decode_to_their_full_strings() {
  std::cout << "\n=== Captured Class 7 frames keep their first character ==="
            << std::endl;

  // Each case answers the *first* queued read, so the decoded value lands in
  // product_name_ regardless of which string the frame was captured for.
  // read_class7_string_async() is one function; the five callers differ only
  // in which member they assign.
  struct Case {
    const char *what;
    std::vector<uint8_t> prefix;  // the twelve bytes the capture shows
    std::vector<uint8_t> payload;
    const char *expected;  // "" when the capture does not pin the whole string
    const char *expected_prefix;
  };

  const std::vector<Case> cases = {
      // 24 0E F8 E7 07 0A 41 4C 50 48 41 20   |$.....ALPHA |
      // LEN 0x0E → 18 bytes total; count 0x0A → 10 string bytes.
      {"product name",
       {0x24, 0x0E, 0xF8, 0xE7, 0x07, 0x0A, 0x41, 0x4C, 0x50, 0x48, 0x41, 0x20},
       nul_terminated("ALPHA HWR"),
       "ALPHA HWR",
       "ALPHA"},
      // 24 0D F8 E7 07 09 31 30 30 30 30 34 37 39 00 C3 47
      // LEN 0x0D → 17 bytes total; count 0x09 → 9 string bytes, which is the
      // 8-character serial plus its terminator. This is the whole frame, not a
      // 12-byte prefix -- it is short enough to have been captured entire.
      {"serial number",
       {0x24, 0x0D, 0xF8, 0xE7, 0x07, 0x09, 0x31, 0x30, 0x30, 0x30, 0x30, 0x34},
       nul_terminated("10000479"),
       "10000479",
       "10000479"},
      // 24 1C F8 E7 07 18 39 32 36 30 31 36   |$.....926016|
      // LEN 0x1C → 32 bytes total; count 0x18 → 24 string bytes, which is
      // exactly the GO app's 23-character value plus a terminator.
      {"software version",
       {0x24, 0x1C, 0xF8, 0xE7, 0x07, 0x18, 0x39, 0x32, 0x36, 0x30, 0x31, 0x36},
       nul_terminated("92601618V04.02.01.02539"),
       "92601618V04.02.01.02539",
       "926016"},
      // 24 1C F8 E7 07 18 39 32 38 31 31 34   |$.....928114|
      {"BLE version",
       {0x24, 0x1C, 0xF8, 0xE7, 0x07, 0x18, 0x39, 0x32, 0x38, 0x31, 0x31, 0x34},
       nul_terminated("92811431V06.00.01.00001"),
       "92811431V06.00.01.00001",
       "928114"},
  };

  for (const auto &c : cases) {
    auto frame = make_class7_response(c.payload);

    // The generated frame must reproduce the capture's visible prefix. This is
    // what keeps make_class7_response() honest: it is only a fixture builder
    // for as long as it agrees with bytes nobody in this repo chose.
    bool prefix_ok = frame.size() >= c.prefix.size();
    for (size_t i = 0; prefix_ok && i < c.prefix.size(); i++)
      prefix_ok = (frame[i] == c.prefix[i]);
    TEST_ASSERT(prefix_ok, std::string("The ") + c.what +
                               " fixture reproduces the captured 12 bytes");

    mock_millis = 0;
    Rig r;
    r.service.read_device_info_async([](bool) {});
    r.answer_next_raw(c.payload);
    const std::string got = r.service.get_product_name();

    TEST_ASSERT(got.rfind(c.expected_prefix, 0) == 0,
                std::string("The ") + c.what + " decodes starting \"" +
                    c.expected_prefix + "\" — its first character is not lost");
    if (c.expected[0] != '\0') {
      TEST_ASSERT(got == c.expected,
                  std::string("The ") + c.what + " decodes to \"" +
                      c.expected + "\" in full");
    }
  }
}

// The removed repairs must stay removed, and in particular must not come back
// as "helpful" normalisation. A serial genuinely beginning with 0 has to
// survive: the old rule would have turned it into a different pump's serial.
void test_no_string_is_rewritten_after_decoding() {
  std::cout << "\n=== Decoded strings are published verbatim ===" << std::endl;

  mock_millis = 0;
  Rig r;
  r.service.read_device_info_async([](bool) {});
  r.answer_next("LPHA HWR");
  TEST_ASSERT(r.service.get_product_name() == "LPHA HWR",
              "A name that really does arrive as \"LPHA HWR\" is not "
              "back-filled to \"ALPHA HWR\" — that repair patched an "
              "off-by-one that no longer exists");

  mock_millis = 0;
  Rig r2;
  r2.service.read_device_info_async([](bool) {});
  r2.answer_next("ALPHA HWR");
  r2.answer_next_raw(unterminated("0123456"));
  TEST_ASSERT(r2.service.get_serial_number() == "0123456",
              "A serial beginning with 0 keeps its 0 — prepending a 1 was "
              "correct only for serials beginning \"10\"");
}

// ── 2b. The length guard, which is the only thing between a runt and an
//        unsigned underflow ───────────────────────────────────────────────────
//
// string_len is `len - HEADER_LEN - CRC_LEN` in size_t arithmetic, so a frame
// shorter than 8 bytes would wrap it to ~1.8e19 and the copy loop would read
// ~127 bytes past the frame. A skeptic pass found the guard load-bearing but
// untested: relaxing it to `len < 5` left the whole suite green.
//
// What this case proves has since MOVED, and the comment is corrected rather
// than left to rot. It used to say "transport.cpp dispatches Class 3/7 on
// `len >= 5`, so 5-, 6- and 7-byte frames really do reach this callback". Since
// issue #278 they do not: on_notification() refuses a frame start whose length
// byte is below 4 -- the structural floor, since the field counts DA + SA +
// APDU -- so nothing under 8 bytes reaches any service at all.
//
// So these three frames are now stopped one layer out, and this case asserts the
// OUTCOME rather than which guard produced it. That is still worth having: it is
// the regression test for anyone who lowers the transport's floor, and it is why
// the service's own guard stays even though no input can now distinguish it.
void test_runt_frames_are_rejected_before_the_length_arithmetic() {
  std::cout << "\n=== Frames too short to hold a string are rejected ==="
            << std::endl;

  for (size_t total : {5u, 6u, 7u}) {
    mock_millis = 0;
    Rig r;
    r.service.read_device_info_async([](bool) {});
    r.step();

    // [STX][LEN][DST][SRC][0x07] then CRC, sized so total == LEN + 4.
    std::vector<uint8_t> f = {0x24, static_cast<uint8_t>(total - 4), 0xF8, 0xE7,
                              0x07};
    while (f.size() < total)
      f.push_back(0x00);
    auto frame = with_crc(f);
    r.transport.on_notification(frame.data(), frame.size());
    r.step();

    TEST_ASSERT(r.service.get_product_name().empty(),
                std::string("A ") + std::to_string(total) +
                    "-byte Class 7 frame yields no string rather than "
                    "underflowing the length");
  }
}

// A real captured frame: 24 05 F8 E7 07 01 00 EC F3 -- count 1, payload one
// NUL, i.e. a genuinely empty string. The old parser could never return this
// (its `len < 10` reject ran before the empty-string branch, making that branch
// dead code), so the verdict is new behaviour and worth pinning: empty is a
// successful read of an empty string, not a failure.
void test_an_empty_string_reads_as_success() {
  std::cout << "\n=== An empty Class 7 string is a success, not a failure ==="
            << std::endl;
  mock_millis = 0;
  Rig r;
  r.service.read_device_info_async([](bool) {});
  r.answer_next_raw(std::vector<uint8_t>{0x00});
  TEST_ASSERT(r.service.get_product_name().empty(),
              "A one-NUL payload decodes to the empty string");

  // ...and the read still counts as answered, so the retry machinery is not
  // left waiting on a string the pump has already said is empty.
  mock_millis = 0;
  Rig r2;
  int completions = 0;
  bool ok = false;
  r2.service.read_device_info_async([&](bool o) { completions++; ok = o; });
  r2.answer_next_raw(std::vector<uint8_t>{0x00});
  r2.answer_next("10000479");
  r2.answer_next("V04.02.01");
  r2.answer_next("V01.03.00");
  r2.answer_next("V06.00.01");
  TEST_ASSERT(completions == 1 && ok,
              "An empty string does not fail the device-info read");
}

// ── 3. Failure accounting ────────────────────────────────────────────────────
void test_a_failed_read_is_reported() {
  std::cout << "\n=== A read that never answers reports failure ==="
            << std::endl;
  mock_millis = 0;
  Rig r;

  int completions = 0;
  bool reported_ok = true;
  r.service.read_device_info_async([&](bool ok) {
    completions++;
    reported_ok = ok;
  });

  r.answer_next("ALPHA HWR");
  r.answer_next("10000479");
  r.answer_next("V04.02.01");
  r.answer_next("V01.03.00");
  // The fifth read is never answered; let the transport time it out.
  for (int i = 0; i < 4000; i++) {
    mock_millis += 51;
    r.transport.loop();
  }

  TEST_ASSERT(completions == 1,
              "The callback still fires once — a silent pump must not leave the "
              "read hanging forever");
  TEST_ASSERT(!reported_ok, "...and reports failure");
  TEST_ASSERT(r.service.get_ble_version().empty(),
              "The unanswered field stays empty rather than holding a guess");
  TEST_ASSERT(r.service.get_product_name() == "ALPHA HWR",
              "The four that did land are kept");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Device Info Service Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_reads_the_five_documented_string_ids();
  test_captured_frames_decode_to_their_full_strings();
  test_no_string_is_rewritten_after_decoding();
  test_runt_frames_are_rejected_before_the_length_arithmetic();
  test_an_empty_string_reads_as_success();
  test_a_failed_read_is_reported();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
