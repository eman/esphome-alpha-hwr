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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "fixture_crc.h"
#include "../components/alpha_hwr/sensor_publisher.h"
#include "../components/alpha_hwr/codec.h"
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
    transport.set_packet_callback([this](const uint8_t *data, size_t len) {
      this->telemetry.on_packet(data, len);
    });
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

// Captured HWR 15-55 Object 87/1 reply: OverallEnergyConsumption, type 232 v1.
// system_energy = 176829746.81838083 J = 49.1193741162 kWh.
static std::vector<uint8_t> captured_energy_frame() {
  return {0x24, 0x25, 0xF8, 0xE7, 0x0A, 0x21, 0x00, 0x00, 0xE8, 0x01,
          0x00, 0x00, 0x1A, 0x41, 0xA5, 0x14, 0x6A, 0x65, 0xA3, 0x02, 0xD0,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
          0x41, 0xA5, 0x14, 0x6A, 0x65, 0xA3, 0x02, 0xD0, 0x9C, 0x9E};
}

static esphome::alpha_hwr::protocol::EnergyTelemetry energy_sample(float kwh) {
  esphome::alpha_hwr::protocol::EnergyTelemetry energy;
  energy.has_energy = true;
  energy.energy_kwh = kwh;
  return energy;
}

void test_energy_counter() {
  mock_millis = 0;
  Rig r;
  esphome::sensor::Sensor energy, power;
  r.publisher.set_power_sensor(&power);
  power.publish_state(17.0f);
  r.publisher.set_energy_sensor(&energy);
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(40);
  TEST_ASSERT(r.sent.size() == 6, "Energy adds one read only when configured");
  const std::vector<uint8_t> request = {0x27, 0x07, 0xE7, 0xF8, 0x0A, 0x03, 87, 0, 1, 0, 0};
  TEST_ASSERT(r.sent.back() == with_crc(request), "Energy request reads Object 87/1 with correct CRC");

  auto frame = captured_energy_frame();
  auto wrong_type = frame;
  wrong_type[8] = 0xE9;
  wrong_type = with_crc(wrong_type);
  r.transport.on_notification(wrong_type.data(), wrong_type.size());
  TEST_ASSERT(!energy.has_state(), "A different object type cannot become an energy sample");
  auto wrong_version = frame;
  wrong_version[9] = 2;
  wrong_version = with_crc(wrong_version);
  r.transport.on_notification(wrong_version.data(), wrong_version.size());
  TEST_ASSERT(!energy.has_state(), "An unknown energy object version is not decoded");
  auto bad_crc = frame;
  bad_crc.back() ^= 1;
  r.transport.on_notification(bad_crc.data(), bad_crc.size());
  TEST_ASSERT(!energy.has_state(), "Bad CRC does not publish energy");
  r.transport.on_notification(frame.data(), 20);
  TEST_ASSERT(!energy.has_state(), "A partial BLE notification does not publish energy");
  r.transport.on_notification(frame.data() + 20, frame.size() - 20);
  TEST_ASSERT(energy.publish_count == 1 && std::fabs(energy.state - 49.119374f) < 0.00001f,
              "Captured binary64 joule counter is published in kWh through the transport");
  TEST_ASSERT(power.state == 17.0f && power.publish_count == 1,
              "Energy-object power does not overwrite the motor-state power sensor");
  esphome::alpha_hwr::protocol::EnergyTelemetry unavailable;
  unavailable.energy_kwh = 123.0f;
  unavailable.has_power = true;
  unavailable.power_w = 12.5f;
  r.publisher.publish_energy(unavailable);
  TEST_ASSERT(energy.publish_count == 1 && std::fabs(energy.state - 49.119374f) < 0.00001f,
              "The publisher honors has_energy independently of has_power");
  r.publisher.publish_energy(energy_sample(energy.state));
  TEST_ASSERT(energy.publish_count == 1, "An unchanged counter does not republish");
  energy.set_force_update(true);
  r.publisher.publish_energy(energy_sample(energy.state));
  TEST_ASSERT(energy.publish_count == 2, "force_update requests repeated counter publications");
  r.publisher.publish_energy(energy_sample(0));
  TEST_ASSERT(energy.state == 0, "A pump counter reset is passed through for total_increasing handling");
}

void test_energy_unsupported_and_silence_preserve_state() {
  mock_millis = 0;
  Rig r;
  esphome::sensor::Sensor energy;
  r.publisher.set_energy_sensor(&energy);
  r.publisher.publish_energy(energy_sample(12.5f));
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(40);
  r.telemetry.poll();
  r.drain(40);
  TEST_ASSERT(r.sent.size() == 12, "A missing energy reply does not hold the next telemetry poll");
  auto unsupported = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x04, 0, 0});
  r.transport.on_notification(unsupported.data(), unsupported.size());
  r.drain(60);
  TEST_ASSERT(energy.state == 12.5f && energy.publish_count == 1,
              "An unsupported read or missing reply preserves the last counter without inventing zero");
}

void test_energy_on_packet_validation() {
  Rig r;
  esphome::sensor::Sensor energy;
  r.publisher.set_energy_sensor(&energy);
  const auto captured = captured_energy_frame();

  auto bad = captured;
  bad.back() ^= 1;
  r.telemetry.on_packet(bad.data(), bad.size());
  TEST_ASSERT(!energy.has_state(), "on_packet rejects energy with bad CRC directly");
  for (size_t index : {size_t(0), size_t(4), size_t(7), size_t(8), size_t(9)}) {
    bad = captured;
    bad[index] ^= 1;
    bad = with_crc(bad);
    r.telemetry.on_packet(bad.data(), bad.size());
    TEST_ASSERT(!energy.has_state(), "on_packet rejects wrong frame kind, class, type, or version");
  }
  bad = captured;
  bad[0] = 0x27;
  bad = with_crc(bad);
  r.telemetry.on_packet(bad.data(), bad.size());
  TEST_ASSERT(!energy.has_state(), "on_packet rejects a request frame containing energy-shaped data");
  for (uint8_t head : {uint8_t(0x61), uint8_t(0xA1), uint8_t(0xE1), uint8_t(0x22)}) {
    bad = captured;
    bad[5] = head;
    bad = with_crc(bad);
    r.telemetry.on_packet(bad.data(), bad.size());
    TEST_ASSERT(!energy.has_state(), "on_packet rejects negative ACKs and an overstated APDU length");
  }
  bad = captured;
  bad[5] = 0x20;
  bad = with_crc(bad);
  r.telemetry.on_packet(bad.data(), bad.size());
  TEST_ASSERT(!energy.has_state(), "on_packet rejects trailing APDUs instead of decoding beyond APDU one");
  r.telemetry.on_packet(captured.data(), captured.size() - 1);
  TEST_ASSERT(!energy.has_state(), "on_packet rejects a truncated energy frame");
  r.telemetry.on_packet(captured.data(), captured.size());
  TEST_ASSERT(energy.publish_count == 1, "A valid energy reply is routed without a pending request");

  // Two per-pump records give an APDU length of 0x2D, which the legacy parser
  // treats as a register-read response. The type-routed energy path must still
  // decode the size-prefixed object from offset 10.
  auto two_pumps = captured;
  two_pumps.insert(two_pumps.end() - 2, 12, 0);
  two_pumps[1] = static_cast<uint8_t>(two_pumps.size() - 4);
  two_pumps[5] = 0x2D;
  two_pumps[12] = 38;
  two_pumps[26] = 2;
  esphome::alpha_hwr::protocol::encode_float_be(12.5f, two_pumps.data() + 21);
  two_pumps[13] = 0x41;
  two_pumps[14] = 0xB5;  // Double the captured counter, keeping the remaining bits
  two_pumps = with_crc(two_pumps);
  r.telemetry.on_packet(two_pumps.data(), two_pumps.size());
  TEST_ASSERT(energy.publish_count == 2 && std::fabs(energy.state - 98.238748f) < 0.00002f,
              "Energy routing uses object type across different APDU lengths");
}

void test_energy_payload_validation() {
  using esphome::alpha_hwr::protocol::decode_energy_payload;
  auto frame = captured_energy_frame();
  std::vector<uint8_t> payload(frame.begin() + 10, frame.end() - 2);
  auto decoded = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(decoded.has_energy && std::fabs(decoded.energy_kwh - 49.119374f) < 0.00001f,
              "Captured energy is decoded in kWh");
  TEST_ASSERT(decoded.has_power && decoded.power_w == 0, "Captured system power is decoded in watts");
  for (size_t len = 0; len < payload.size(); len++) {
    auto truncated = decode_energy_payload(payload.data(), len);
    TEST_ASSERT(!truncated.has_energy && !truncated.has_power, "Truncated object rejected");
  }
  auto absent = decode_energy_payload(nullptr, 11);
  TEST_ASSERT(!absent.has_energy && !absent.has_power, "Null payload rejected");
  auto short_size = payload;
  short_size[2] = 7;
  auto short_result = decode_energy_payload(short_size.data(), short_size.size());
  TEST_ASSERT(!short_result.has_energy && !short_result.has_power, "Object too small for either field rejected");
  short_size[2] = 8;
  auto energy_only = decode_energy_payload(short_size.data(), short_size.size());
  TEST_ASSERT(energy_only.has_energy && !energy_only.has_power, "Object size bounds power independently of trailing bytes");
  for (double invalid : {-1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max()}) {
    uint64_t bits;
    std::memcpy(&bits, &invalid, sizeof(bits));
    auto bad = payload;
    for (size_t i = 0; i < 8; i++) bad[3 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    auto result = decode_energy_payload(bad.data(), bad.size());
    TEST_ASSERT(!result.has_energy && result.has_power && result.power_w == 0,
                "Invalid energy does not discard valid system power");
  }
  for (float invalid : {-1.0f, 1001.0f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
    auto bad = payload;
    esphome::alpha_hwr::protocol::encode_float_be(invalid, bad.data() + 11);
    auto result = decode_energy_payload(bad.data(), bad.size());
    TEST_ASSERT(result.has_energy && !result.has_power && std::fabs(result.energy_kwh - 49.119374f) < 0.00001f,
                "Invalid system power does not discard valid energy");
  }
  esphome::alpha_hwr::protocol::encode_float_be(12.5f, payload.data() + 11);
  auto powered = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(powered.has_power && powered.power_w == 12.5f, "System power is a big-endian float in watts");
  std::fill(payload.begin() + 3, payload.begin() + 11, 0);
  auto zero = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(zero.has_energy && zero.energy_kwh == 0 && zero.has_power,
              "Zero is a valid cumulative counter");
}

int main() {
  test_energy_counter();
  test_energy_on_packet_validation();
  test_energy_payload_validation();
  test_energy_unsupported_and_silence_preserve_state();
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
