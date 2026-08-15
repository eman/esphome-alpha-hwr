// Host tests for DeviceInfoService (device_info_service.cpp).
//
// Why this file exists: `esphome compile` was the only thing that compiled
// device_info_service.cpp. It reads the five identity strings the device
// entity is built from, and it carries two hand-ported repairs for quirks in
// what the pump returns — a leading letter missing from the product name, and a
// leading digit missing from the serial. Both were transcribed from the Python
// client with a comment and no test. Both fire on real hardware: a bench run
// shows the pump returning a serial the service publishes as "10000479".
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

// A Class 7 ReadString response:
//   [0x24][LEN][DST][SRC][0x07][Cmd][ID][...string...][CRC-H][CRC-L]
// The service takes the string as bytes 7 .. len-3.
static std::vector<uint8_t> make_class7_response(uint8_t string_id,
                                                 const std::string &value) {
  std::vector<uint8_t> f = {0x24, 0x00, 0xE7, 0xF8, 0x07, 0x01, string_id};
  for (char c : value)
    f.push_back(static_cast<uint8_t>(c));
  f.push_back(0x00);  // CRC placeholder
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  return with_crc(f);
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

  /// Let the next queued read go out, then answer it.
  void answer_next(uint8_t string_id, const std::string &value) {
    step();
    auto frame = make_class7_response(string_id, value);
    transport.on_notification(frame.data(), frame.size());
    step();
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

  r.answer_next(1, "ALPHA HWR");
  r.answer_next(9, "10000479");
  r.answer_next(50, "V04.02.01");
  r.answer_next(52, "V01.03.00");
  r.answer_next(58, "V06.00.01");

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

// ── 2. The two transcribed repairs ───────────────────────────────────────────
// Both of these exist because the pump drops the first character. They are
// narrow string rewrites with no test behind them until now, and each is one
// careless edit away from either not firing or firing on the wrong value.
void test_product_name_missing_letter_is_repaired() {
  std::cout << "\n=== A truncated product name is repaired ===" << std::endl;
  mock_millis = 0;
  Rig r;
  r.service.read_device_info_async([](bool) {});

  r.answer_next(1, "LPHA HWR");
  TEST_ASSERT(r.service.get_product_name() == "ALPHA HWR",
              "\"LPHA HWR\" becomes \"ALPHA HWR\"");

  // The repair must be exact-match, not a prefix guess: a correct name must
  // survive untouched.
  mock_millis = 0;
  Rig r2;
  r2.service.read_device_info_async([](bool) {});
  r2.answer_next(1, "ALPHA HWR");
  TEST_ASSERT(r2.service.get_product_name() == "ALPHA HWR",
              "An already-correct name is left alone, not double-prefixed");

  mock_millis = 0;
  Rig r3;
  r3.service.read_device_info_async([](bool) {});
  r3.answer_next(1, "LPHA3 XYZ");
  TEST_ASSERT(r3.service.get_product_name() == "LPHA3 XYZ",
              "A different model's name is not rewritten by the repair");
}

void test_serial_leading_zero_is_repaired() {
  std::cout << "\n=== A serial that starts with 0 gains its leading 1 ==="
            << std::endl;
  mock_millis = 0;
  Rig r;
  r.service.read_device_info_async([](bool) {});
  r.answer_next(1, "ALPHA HWR");
  r.answer_next(9, "0000479");
  TEST_ASSERT(r.service.get_serial_number() == "10000479",
              "\"0000479\" becomes \"10000479\" — the value the bench shows");

  mock_millis = 0;
  Rig r2;
  r2.service.read_device_info_async([](bool) {});
  r2.answer_next(1, "ALPHA HWR");
  r2.answer_next(9, "8123456");
  TEST_ASSERT(r2.service.get_serial_number() == "8123456",
              "A serial not starting with 0 is left alone");
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

  r.answer_next(1, "ALPHA HWR");
  r.answer_next(9, "10000479");
  r.answer_next(50, "V04.02.01");
  r.answer_next(52, "V01.03.00");
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
  test_product_name_missing_letter_is_repaired();
  test_serial_leading_zero_is_repaired();
  test_a_failed_read_is_reported();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
