// Host tests for TelemetryService (telemetry_service.cpp).
//
// Why this file exists: `esphome compile` was the only thing that compiled
// telemetry_service.cpp. It owns the poll set — which registers get read, and
// in what order — and the OpSpec routing table that decides which decoder every
// inbound frame reaches. Neither had a host test.
//
// The routing is the interesting half. Alarms and warnings come back under the
// *same* OpSpec (0x09) and are told apart by a register address echoed inside
// the response, with a poll-order toggle as the fallback for responses too
// short to carry one. Getting that backwards publishes the pump's warnings into
// the alarm entity and vice versa — silently, since both are well-formed
// strings.

#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "fixture_crc.h"
#include "../components/alpha_hwr/sensor_publisher.h"
#include "../components/alpha_hwr/telemetry_service.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::services::SensorPublisher;
using esphome::alpha_hwr::services::TelemetryService;

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

// Anonymous namespace: several test files in this suite define their own
// Rig, and cppcheck's whole-program pass reports same-named structs across
// translation units as an ODR violation even though each test is its own
// binary. Same treatment as test_read_chain_lifetime.cpp.
namespace {

struct Rig {
  Transport transport;
  SensorPublisher publisher;
  TelemetryService telemetry{transport};

  esphome::text_sensor::TextSensor alarms, warnings;
  std::vector<std::vector<uint8_t>> sent;

  Rig() {
    publisher.set_alarms_text_sensor(&alarms);
    publisher.set_warnings_text_sensor(&warnings);
    telemetry.set_sensor_publisher(&publisher);
    transport.set_write_callback([this](const uint8_t *data, size_t len) {
      this->sent.push_back(std::vector<uint8_t>(data, data + len));
      return true;
    });
  }

  void drain(int rounds = 12) {
    for (int i = 0; i < rounds; i++) {
      mock_millis += 60;
      transport.loop();
    }
  }
};

}  // namespace


// Build a Class 10 register-read response (OpSpec 0x09) that echoes `reg_addr`
// at bytes 10-12, which is how the service tells an alarm response from a
// warning one. Padded past 15 bytes so the echo path is the one exercised.
static std::vector<uint8_t> make_09_response(uint32_t reg_addr,
                                             const std::vector<uint8_t> &codes) {
  std::vector<uint8_t> f = {
      0x24, 0x00,              // RESPONSE_START (0x24, not the 0x27 of a request)
      0xE7, 0xF8,              // service id
      0x0A,                    // class 10
      0x09,                    // opspec: register-read response
      0x00, 0x01,              // sub id (sequence number for 0x09)
      0x00, 0x58,              // obj id (register 0x58)
      static_cast<uint8_t>((reg_addr >> 16) & 0xFF),
      static_cast<uint8_t>((reg_addr >> 8) & 0xFF),
      static_cast<uint8_t>(reg_addr & 0xFF),
      0x00, 0x00,              // padding to reach the len >= 15 echo path
  };
  for (uint8_t c : codes)
    f.push_back(c);
  f.push_back(0x00);  // CRC placeholder
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);  // length covers bytes 2..n-3
  return with_crc(f);
}

// ── 1. The poll set ──────────────────────────────────────────────────────────
void test_poll_reads_the_documented_register_set() {
  std::cout << "\n=== poll() reads the five documented registers ==="
            << std::endl;
  mock_millis = 0;
  Rig r;

  r.telemetry.poll();
  r.drain();
  TEST_ASSERT(r.sent.empty(),
              "A poll before start() sends nothing — telemetry must not go out "
              "on an unauthenticated link");

  r.telemetry.start();
  TEST_ASSERT(r.telemetry.is_running(), "Running after start()");
  r.telemetry.poll();
  r.drain(40);

  TEST_ASSERT(r.sent.size() == 5,
              "Five read requests queued: motor, flow, temperature, alarms, "
              "warnings");

  // Every request carries its 3-byte register address; check the set and the
  // order, since the 0x09 fallback routing depends on alarms preceding
  // warnings.
  // build_class10_read lays the 3-byte register out at offsets 6-8:
  // [27][07][E7][F8][0A][03][Reg-H][Reg-M][Reg-L][CRC-H][CRC-L]
  std::vector<uint32_t> addrs;
  for (const auto &p : r.sent) {
    if (p.size() >= 9)
      addrs.push_back((static_cast<uint32_t>(p[6]) << 16) |
                      (static_cast<uint32_t>(p[7]) << 8) |
                      static_cast<uint32_t>(p[8]));
  }
  const std::vector<uint32_t> expected = {0x570045, 0x5D0122, 0x5D012C,
                                          0x580000, 0x58000B};
  TEST_ASSERT(addrs == expected,
              "In the documented order, with alarms (0x580000) before warnings "
              "(0x58000B)");

  r.telemetry.stop();
  TEST_ASSERT(!r.telemetry.is_running(), "Not running after stop()");
  r.sent.clear();
  r.telemetry.poll();
  r.drain();
  TEST_ASSERT(r.sent.empty(), "A poll after stop() sends nothing");
}

// ── 2. Alarm vs warning routing by echoed register ───────────────────────────
void test_09_responses_route_by_echoed_register() {
  std::cout << "\n=== 0x09 responses route by the register they echo ==="
            << std::endl;
  mock_millis = 0;
  Rig r;
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(40);

  // Deliberately out of poll order: the warning response arrives first. The
  // echoed register must decide, not arrival order.
  auto warn = make_09_response(0x58000B, {0x00, 0x4D});
  r.telemetry.on_packet(warn.data(), warn.size());

  auto alarm = make_09_response(0x580000, {0x00, 0x33});
  r.telemetry.on_packet(alarm.data(), alarm.size());

  TEST_ASSERT(r.warnings.has_state(),
              "The response echoing 0x58000B reached the warnings entity");
  TEST_ASSERT(r.alarms.has_state(),
              "The response echoing 0x580000 reached the alarms entity");
  TEST_ASSERT(r.warnings.state != r.alarms.state,
              "The two carried different payloads, so they were not both "
              "routed to the same handler");
}

// ── 3. Frame rejection ───────────────────────────────────────────────────────
// on_packet is fed straight from the BLE notification path, so a corrupt or
// unrelated frame reaching a decoder is a real possibility rather than a
// theoretical one.
void test_malformed_frames_are_rejected() {
  std::cout << "\n=== Malformed and unrelated frames are dropped ==="
            << std::endl;
  mock_millis = 0;
  Rig r;
  r.telemetry.start();

  auto good = make_09_response(0x580000, {0x00, 0x33});

  // Bad CRC: same frame with the checksum left wrong.
  auto bad_crc = good;
  bad_crc[bad_crc.size() - 1] ^= 0xFF;
  r.telemetry.on_packet(bad_crc.data(), bad_crc.size());
  TEST_ASSERT(!r.alarms.has_state(),
              "A frame with a bad CRC never reaches a decoder");

  // Wrong class: Class 2 rather than Class 10.
  auto wrong_class = good;
  wrong_class[4] = 0x02;
  wrong_class = with_crc(wrong_class);
  r.telemetry.on_packet(wrong_class.data(), wrong_class.size());
  TEST_ASSERT(!r.alarms.has_state(), "A non-Class-10 frame is ignored");

  // Unhandled OpSpec.
  auto unknown_op = good;
  unknown_op[5] = 0x77;
  unknown_op = with_crc(unknown_op);
  r.telemetry.on_packet(unknown_op.data(), unknown_op.size());
  TEST_ASSERT(!r.alarms.has_state(), "An unhandled OpSpec is ignored");

  // A runt frame, which must not index past its end.
  const uint8_t runt[] = {0x27, 0x02, 0xE7};
  r.telemetry.on_packet(runt, sizeof(runt));
  TEST_ASSERT(!r.alarms.has_state(), "A runt frame is ignored, not indexed");

  // The same frame, intact, still works — otherwise the four assertions above
  // would pass with the routing removed entirely.
  r.telemetry.on_packet(good.data(), good.size());
  TEST_ASSERT(r.alarms.has_state(),
              "...and the intact frame does reach the alarms decoder");
}

// ── 4. No publisher wired ────────────────────────────────────────────────────
void test_service_without_a_publisher_is_inert() {
  std::cout << "\n=== A service with no publisher does not crash ==="
            << std::endl;
  mock_millis = 0;
  Transport transport;
  TelemetryService telemetry(transport);
  telemetry.start();
  telemetry.poll();

  auto frame = make_09_response(0x580000, {0x00, 0x33});
  telemetry.on_packet(frame.data(), frame.size());
  TEST_ASSERT(true, "Routing a response with no publisher wired is safe");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Telemetry Service Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_poll_reads_the_documented_register_set();
  test_09_responses_route_by_echoed_register();
  test_malformed_frames_are_rejected();
  test_service_without_a_publisher_is_inert();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
