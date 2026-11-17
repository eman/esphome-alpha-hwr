// Host tests for type/version telemetry routing and serialized fault reads.
// Fixtures use the GENI profile 52_7 layouts and real CRCs. The motor v3
// fixture below is taken from an HWR capture; synthetic variants exercise
// same-length unrelated objects and different-length supported versions.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdint>
#include <iostream>
#include <vector>

#include "fixture_crc.h"
#include "../components/alpha_hwr/control_service.h"
#include "../components/alpha_hwr/frame_parser.h"
#include "../components/alpha_hwr/sensor_publisher.h"
#include "../components/alpha_hwr/session.h"
#include "../components/alpha_hwr/codec.h"
#include "../components/alpha_hwr/telemetry_service.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;
using namespace esphome::alpha_hwr;
int tests_passed = 0;
int tests_failed = 0;
#define TEST_ASSERT(condition, message) \
  do { \
    if (condition) { tests_passed++; std::cout << "[PASS] " << message << '\n'; } \
    else { tests_failed++; std::cout << "[FAIL] " << message << '\n'; } \
  } while (false)

namespace {
struct Rig {
  core::Transport transport;
  core::Session session;
  services::SensorPublisher publisher;
  services::ControlService control{transport, session};
  services::TelemetryService telemetry{transport};
  esphome::sensor::Sensor power, power_dc, rpm, flow, head, media, pcb, box;
  esphome::text_sensor::TextSensor alarms, warnings;
  std::vector<std::vector<uint8_t>> sent;

  bool answer_faults;
  size_t answered_fault_frames{0};

  explicit Rig(bool answer_faults = false) : answer_faults(answer_faults) {
    mock_millis = 0;
    publisher.set_power_sensor(&power);
    publisher.set_power_dc_sensor(&power_dc);
    publisher.set_rpm_sensor(&rpm);
    publisher.set_flow_sensor(&flow);
    publisher.set_head_sensor(&head);
    publisher.set_temp_media_sensor(&media);
    publisher.set_temp_pcb_sensor(&pcb);
    publisher.set_temp_control_box_sensor(&box);
    publisher.set_alarms_text_sensor(&alarms);
    publisher.set_warnings_text_sensor(&warnings);
    telemetry.set_sensor_publisher(&publisher);
    telemetry.set_control_service(&control);
    transport.set_packet_callback([this](const uint8_t* data, size_t len) {
      telemetry.on_packet(data, len);
    });
    transport.set_write_callback([this](const uint8_t* data, size_t len) {
      sent.emplace_back(data, data + len);
      return true;
    });
  }
  void drain(int rounds = 10) {
    for (int i = 0; i < rounds; i++) {
      mock_millis += 60;
      transport.loop();
      if (answer_faults && sent.size() > answered_fault_frames) {
        answered_fault_frames = sent.size();
        const auto &request = sent.back();
        if (request.size() == 11 && request[4] == 10 && request[6] == 88) {
          receive(with_crc({0x24, 13, 0xF8, 0xE7, 10, 9, 0, 2, 0x3A, 1,
                            0, 0, 2, 0, 0, 0, 0}));
        }
      }
    }
  }
  void receive(const std::vector<uint8_t>& frame) {
    transport.on_notification(frame.data(), frame.size());
  }
};
}

static std::vector<uint8_t> object_frame(uint16_t type, uint8_t version, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> frame = {0x24, 0, 0xF8, 0xE7, 0x0A, static_cast<uint8_t>(7 + body.size()),
                              0, static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), version,
                              0, 0, static_cast<uint8_t>(body.size())};
  frame.insert(frame.end(), body.begin(), body.end());
  frame.insert(frame.end(), {0, 0});
  frame[1] = static_cast<uint8_t>(frame.size() - 4);
  return with_crc(frame);
}

static std::vector<uint8_t> motor_frame(uint8_t version, size_t size) {
  std::vector<uint8_t> body(size, 0);
  protocol::encode_float_be(12.5f, body.data() + 12);
  protocol::encode_float_be(9.75f, body.data() + 16);
  protocol::encode_float_be(2400, body.data() + 20);
  return object_frame(256, version, body);
}

static std::vector<uint8_t> fault_frame(const std::vector<uint16_t>& codes) {
  std::vector<uint8_t> body = {0, static_cast<uint8_t>(codes.size())};
  for (uint16_t code : codes) {
    body.insert(body.end(), {static_cast<uint8_t>(code >> 8), static_cast<uint8_t>(code), 0, 99});
  }
  return object_frame(570, 1, body);
}

void test_poll_and_fault_identity() {
  Rig r;
  r.telemetry.poll(); r.drain();
  TEST_ASSERT(r.sent.empty(), "No polling before start");
  r.telemetry.start(); r.telemetry.poll(); r.drain();
  TEST_ASSERT(r.sent.size() == 4, "Motor/flow/temperature and alarm read sent; warning waits for alarm reply");
  r.receive(fault_frame({51})); r.drain();
  TEST_ASSERT(r.alarms.has_state() && !r.warnings.has_state(), "First type-570 reply belongs to alarm request");
  TEST_ASSERT(r.sent.size() == 5, "Warning request sent only after successful alarm reply");
  r.receive(fault_frame({77}));
  TEST_ASSERT(r.warnings.has_state() && r.warnings.state != r.alarms.state,
              "Identically typed warning reply is published into the correct entity");
  const std::vector<uint32_t> expected = {0x570045, 0x5D0122, 0x5D012C, 0x580000, 0x58000B};
  std::vector<uint32_t> actual;
  for (const auto& frame : r.sent) {
    actual.push_back((uint32_t(frame[6]) << 16) | (uint32_t(frame[7]) << 8) | frame[8]);
  }
  TEST_ASSERT(actual == expected, "Documented request set is preserved");
  r.telemetry.poll(); r.drain(); r.receive(fault_frame({})); r.drain(); r.receive(fault_frame({}));
  TEST_ASSERT(r.alarms.state == "None" && r.warnings.state == "None", "Valid empty arrays clear faults");
  r.telemetry.stop(); r.sent.clear(); r.telemetry.poll(); r.drain();
  TEST_ASSERT(r.sent.empty(), "No polling after stop");
}

void test_object_types_and_versions() {
  Rig r;
  for (uint8_t version : {1, 2, 3}) {
    auto frame = motor_frame(version, version == 1 ? 37 : 41);
    r.receive(frame);
    TEST_ASSERT(r.power.state == 12.5f && r.power_dc.state == 9.75f && r.rpm.state == 2400,
                "Known motor version preserves motor power, DC power and speed");
  }
  TEST_ASSERT(r.power.publish_count == 3, "Motor v1's different APDU length is accepted");
  auto extended = motor_frame(3, 45);
  r.receive(extended);
  TEST_ASSERT(r.power.publish_count == 4, "Appended fields of a known motor version do not change routing");
  for (uint8_t version : {0, 4}) r.receive(motor_frame(version, 41));
  auto unrelated = motor_frame(3, 41); unrelated[7] = 2; unrelated = with_crc(unrelated); r.receive(unrelated);
  TEST_ASSERT(r.power.publish_count == 4, "Unknown versions and same-length unrelated types cannot publish motor data");

  std::vector<uint8_t> flow(36, 0);
  protocol::encode_float_be(2.5f, flow.data() + 24);
  protocol::encode_float_be(3.2f, flow.data() + 28);
  r.receive(object_frame(565, 2, flow));
  TEST_ASSERT(r.flow.state == 2.5f && r.head.state == 3.2f, "Flow/head type 565 v2 decodes in existing units");
  r.receive(object_frame(565, 1, flow));
  TEST_ASSERT(r.flow.publish_count == 1, "Unknown flow version rejected");
  r.receive(object_frame(565, 2, std::vector<uint8_t>(35, 0)));
  TEST_ASSERT(r.flow.publish_count == 1, "Short flow object does not publish a partial reading");
  for (uint8_t version : {1, 2}) {
    std::vector<uint8_t> body(version == 1 ? 12 : 13, 0);
    protocol::encode_float_be(45, body.data());
    protocol::encode_float_be(50, body.data() + 4);
    protocol::encode_float_be(55, body.data() + 8);
    r.receive(object_frame(534, version, body));
    TEST_ASSERT(r.media.state == 45 && r.pcb.state == 50 && r.box.state == 55,
                "Temperature versions decode despite differing response lengths");
  }
  r.receive(object_frame(534, 3, std::vector<uint8_t>(13, 0)));
  TEST_ASSERT(r.media.publish_count == 2, "Unknown temperature version rejected");

  r.receive(object_frame(534, 1, std::vector<uint8_t>(11, 0)));
  TEST_ASSERT(r.media.publish_count == 2, "Short temperature object does not publish a partial reading");
  std::vector<uint8_t> status = {2, 0, 2, 0, 0, 0, 0};
  protocol::encode_float_be(2400, status.data() + 3);
  r.receive(object_frame(303, 1, status));
  TEST_ASSERT(r.control.get_current_mode() == services::ControlMode::CONSTANT_SPEED &&
              r.control.get_remote_enabled(), "Type 303 v1 updates control state and remote source");
  r.receive(object_frame(303, 1, {1, 1, 8, 0, 0, 0}));
  TEST_ASSERT(r.control.get_current_mode() == services::ControlMode::CONSTANT_SPEED,
              "Short control object does not update state");
  status[2] = 8;
  r.receive(object_frame(303, 2, status));
  TEST_ASSERT(r.control.get_current_mode() == services::ControlMode::CONSTANT_SPEED,
              "Unknown control-status version cannot update state");
}

void test_validation_before_decoding() {
  Rig r;
  const auto good = motor_frame(3, 41);
  auto bad = good; bad.back() ^= 1; r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Bad CRC rejected");
  for (uint8_t head : {uint8_t(0x70), uint8_t(0xB0), uint8_t(0xF0), uint8_t(0x31), uint8_t(0x2F)}) {
    bad = good; bad[5] = head; bad = with_crc(bad); r.receive(bad);
    TEST_ASSERT(!r.power.has_state(), "Negative ACK or mismatched APDU length rejected");
  }
  for (uint8_t status : {2, 4}) {
    bad = good; bad[6] = status; bad = with_crc(bad); r.receive(bad);
    TEST_ASSERT(!r.power.has_state(), "Class 10 busy/operation-failed status rejected");
  }
  bad = good; bad[4] = 2; bad = with_crc(bad); r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Wrong GENI class rejected");
  bad = good; bad[0] = 0x27; bad = with_crc(bad); r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Request frame rejected");
  bad = good; bad[12] = 42; bad = with_crc(bad); r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Object size cannot extend into CRC");
  r.receive(motor_frame(3, 37));
  TEST_ASSERT(!r.power.has_state(), "Version-specific minimum body size checked separately");
  bad = good; bad[12] = 23; bad = with_crc(bad); r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Plausible trailing bytes outside the object cannot become sensor fields");
  bad = good; bad.insert(bad.end() - 2, {7, 0}); bad[1] += 2; bad = with_crc(bad); r.receive(bad);
  TEST_ASSERT(!r.power.has_state(), "Multiple APDUs are rejected, not decoded as one body");
  r.telemetry.on_packet(good.data(), good.size() - 1);
  TEST_ASSERT(!r.power.has_state(), "Truncated telegram rejected");
  const uint8_t runt[] = {0x24, 3, 0}; r.telemetry.on_packet(runt, sizeof(runt));
  TEST_ASSERT(!r.power.has_state(), "Runt telegram rejected safely");
  r.receive(good);
  TEST_ASSERT(r.power.state == 12.5f, "Valid frame still decodes after rejection tests");
  protocol::Class10ObjectView view;
  TEST_ASSERT(!protocol::parse_class10_object(nullptr, 50, view), "Null object frame rejected");
}

void test_captured_motor_and_fragmentation() {
  Rig r;
  const std::vector<uint8_t> captured = {
    0x24,0x34,0xF8,0xE7,0x0A,0x30,0x00,0x01,0x00,0x03,0x00,0x00,0x29,
    0x42,0xE2,0x91,0x24,0x43,0x20,0x35,0x00,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x7F,0xFF,0xFF,0xFF,0x7F,0xFF,0xFF,0xFF,0,0,0,0,0,0,0,0,0,0x39,0x4F};
  r.transport.on_notification(captured.data(), 20);
  TEST_ASSERT(!r.power.has_state(), "Fragment does not publish");
  r.transport.on_notification(captured.data() + 20, captured.size() - 20);
  TEST_ASSERT(r.power.has_state() && r.power.state == 0 && r.rpm.state == 0,
              "Captured motor v3 frame decodes through reassembly and type routing");
}

void test_fault_failures_do_not_cross_route() {
  for (int kind = 0; kind < 10; kind++) {
    Rig r;
    r.telemetry.start(); r.telemetry.poll(); r.drain();
    auto bad = fault_frame({51});
    if (kind == 0) bad[5] |= 0x80;
    if (kind == 1) bad[6] = 2;
    if (kind == 2) bad[6] = 4;
    if (kind == 3) bad[12] = 7;
    if (kind == 4) bad[14] = 2;
    if (kind == 5) bad[5]++;
    if (kind == 7) bad[9] = 2;
    if (kind == 8) bad[4] = 2;
    if (kind == 9) bad[7] = 0;
    if (kind != 6) { bad = with_crc(bad); r.receive(bad); }
    r.drain(70);  // Includes the no-response timeout case.
    TEST_ASSERT(!r.alarms.has_state() && !r.warnings.has_state(), "Fault failure cannot clear or fabricate an entity");
    TEST_ASSERT(r.sent.size() == 4, "Warning request is not sent after ambiguous alarm failure");
    r.receive(fault_frame({51}));
    r.telemetry.poll(); r.drain();
    TEST_ASSERT(!r.warnings.has_state() && r.sent.size() == 7,
                "Late fault reply is ignored; live telemetry continues without another ambiguous fault read");
    r.telemetry.stop(); r.transport.reset(); r.telemetry.start(); r.telemetry.poll(); r.drain();
    r.receive(fault_frame({})); r.drain(); r.receive(fault_frame({77}));
    TEST_ASSERT(r.warnings.has_state(), "Fresh session resumes fault reads");
  }
}

// Captured HWR 15-55 Object 87/1 reply: OverallEnergyConsumption, type 232 v1.
// system_energy = 176829746.81838083 J = 49.1193741162 kWh.
static std::vector<uint8_t> captured_energy_frame() {
  return {0x24, 0x25, 0xF8, 0xE7, 0x0A, 0x21, 0x00, 0x00, 0xE8, 0x01, 0x00, 0x00, 0x1A, 0x41,
          0xA5, 0x14, 0x6A, 0x65, 0xA3, 0x02, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
          0x00, 0x00, 0x00, 0x41, 0xA5, 0x14, 0x6A, 0x65, 0xA3, 0x02, 0xD0, 0x9C, 0x9E};
}

static esphome::alpha_hwr::protocol::EnergyTelemetry energy_sample(float kwh) {
  esphome::alpha_hwr::protocol::EnergyTelemetry energy;
  energy.has_energy = true;
  energy.energy_kwh = kwh;
  return energy;
}

static std::vector<uint8_t> object_frame(uint16_t type, uint8_t version, size_t body_size = 8) {
  std::vector<uint8_t> f = {
      0x24, 0, 0xF8, 0xE7, 0x0A, static_cast<uint8_t>(7 + body_size),
      0, static_cast<uint8_t>(type >> 8), static_cast<uint8_t>(type), version,
      0, 0, static_cast<uint8_t>(body_size)};
  f.insert(f.end(), body_size + 2, 0);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  return with_crc(f);
}

static size_t energy_requests(const Rig &r) {
  return std::count_if(r.sent.begin(), r.sent.end(), [](const std::vector<uint8_t> &f) {
    return f.size() == 11 && f[6] == 87 && f[7] == 0 && f[8] == 1;
  });
}

void test_energy_counter() {
  mock_millis = 0;
  Rig r(true);
  esphome::sensor::Sensor energy, power;
  r.publisher.set_energy_sensor(&energy);
  r.publisher.set_power_sensor(&power);
  power.publish_state(17);
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(5);
  TEST_ASSERT(r.sent.size() == 1,
              "Awaited energy request holds the queue before this poll's legacy reads");
  const std::vector<uint8_t> request = {0x27, 7, 0xE7, 0xF8, 0x0A, 3, 87, 0, 1, 0, 0};
  TEST_ASSERT(r.sent.front() == with_crc(request),
              "Energy request uses Object 87/1 and correct CRC");
  auto frame = captured_energy_frame();
  auto bad_crc = frame;
  bad_crc.at(bad_crc.size() - 1) ^= 1;
  r.transport.on_notification(bad_crc.data(), bad_crc.size());
  auto wrong_version = frame;
  wrong_version[9] = 2;
  wrong_version = with_crc(wrong_version);
  r.transport.on_notification(wrong_version.data(), wrong_version.size());
  auto wrong_type = frame;
  wrong_type[7] = 3;  // Type 1000 shares E801 with type 232.
  wrong_type = with_crc(wrong_type);
  r.transport.on_notification(wrong_type.data(), wrong_type.size());
  TEST_ASSERT(!energy.has_state(), "CRC, full type and version are checked before energy decoding");
  r.transport.on_notification(frame.data(), 20);
  TEST_ASSERT(!energy.has_state(), "A partial notification does not publish");
  r.transport.on_notification(frame.data() + 20, frame.size() - 20);
  r.drain(20);
  TEST_ASSERT(energy.publish_count == 1 && std::fabs(energy.state - 49.119374f) < 0.00001f,
              "Captured binary64 counter is published in kWh through the awaited callback");
  TEST_ASSERT(r.sent.size() == 6, "Legacy telemetry resumes after energy completes");
  TEST_ASSERT(power.state == 17 && power.publish_count == 1,
              "Motor power remains the only published power source");
  r.telemetry.on_packet(frame.data(), frame.size());
  TEST_ASSERT(energy.publish_count == 1, "There is no passive energy publication path");
  r.publisher.publish_energy(energy_sample(energy.raw_state));
  TEST_ASSERT(energy.publish_count == 1, "Unchanged raw counter is gated");
  energy.state *= 1000;  // Simulate a multiply filter after raw_state was recorded.
  r.publisher.publish_energy(energy_sample(energy.raw_state));
  TEST_ASSERT(energy.publish_count == 1, "Filtered state cannot defeat the raw-state gate");
  energy.set_force_update(true);
  r.publisher.publish_energy(energy_sample(energy.raw_state));
  TEST_ASSERT(energy.publish_count == 2, "force_update overrides the change gate");
  r.publisher.publish_energy(energy_sample(0));
  TEST_ASSERT(energy.state == 0, "A valid zero is trusted as a real pump counter value");
}

void test_energy_cadence_and_generation() {
  mock_millis = 0;
  Rig r(true);
  esphome::sensor::Sensor energy;
  r.publisher.set_energy_sensor(&energy);
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(2);
  r.telemetry.poll();
  r.drain(2);
  auto frame = captured_energy_frame();
  r.transport.on_notification(frame.data(), frame.size());
  r.drain(40);
  TEST_ASSERT(energy_requests(r) == 1,
              "A queued/outstanding read is not duplicated by another poll");
  r.telemetry.poll();
  r.drain(20);
  TEST_ASSERT(energy_requests(r) == 1,
              "Default 60-second cadence does not follow every telemetry poll");
  mock_millis += 60000;
  r.telemetry.poll();
  r.drain(2);
  TEST_ASSERT(energy_requests(r) == 2, "Energy becomes due after the interval");
  r.telemetry.stop();
  r.transport.reset();  // Retires callback belonging to the previous connection.
  r.transport.on_notification(frame.data(), frame.size());
  TEST_ASSERT(energy.publish_count == 1, "A stale reply after disconnect cannot publish");
  r.telemetry.set_energy_update_interval(1000);
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(2);
  TEST_ASSERT(energy_requests(r) == 3, "First energy attempt after reconnect is immediate");
  r.transport.on_notification(frame.data(), frame.size());
  r.drain(20);
  mock_millis += 1000;
  r.telemetry.poll();
  r.drain(2);
  TEST_ASSERT(energy_requests(r) == 4, "Configured energy cadence is honored");
}

void test_energy_short_refusals() {
  const uint8_t heads[] = {0x40, 0x81, 0xC1, 1, 1, 1, 0x41, 0x80};
  const uint8_t statuses[] = {0, 0, 0, 2, 4, 7, 0, 0};
  for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
    const uint8_t head = heads[i];
    mock_millis = 0;
    Rig r(true);
    esphome::sensor::Sensor energy;
    r.publisher.set_energy_sensor(&energy);
    r.publisher.publish_energy(energy_sample(12.5f));
    r.telemetry.start();
    r.telemetry.poll();
    r.drain(2);
    // Unknown Class has no payload; other errors carry an item ID or status.
    const uint8_t byte = statuses[i];
    auto refusal = (head == 0x40 || head == 0x80)
                       ? with_crc({0x24, 4, 0xF8, 0xE7, 0x0A, head, 0, 0})
                       : with_crc({0x24, 5, 0xF8, 0xE7, 0x0A, head, byte, 0, 0});
    r.transport.on_notification(refusal.data(), refusal.size());
    r.drain(20);
    if (head == 0x41 || head == 0x80) {
      TEST_ASSERT(r.sent.size() == 1 && energy.state == 12.5f && energy.publish_count == 1,
                  "Malformed refusal leaves the energy request waiting and preserves its counter");
      mock_millis += 3000;
      r.drain(20);
    }
    TEST_ASSERT(r.sent.size() == 6 && energy.state == 12.5f && energy.publish_count == 1,
                "Valid refusal or timeout releases the queue without inventing a zero counter");
    mock_millis += 60000;
    r.telemetry.poll();
    r.drain(20);
    const bool unsupported = head == 0x40 || head == 0x81;
    TEST_ASSERT(
        energy_requests(r) == (unsupported ? 1 : 2),
        "Unsupported counter is suppressed; transient or malformed refusals can retry");
    r.telemetry.stop();
    r.transport.reset();
    r.telemetry.start();
    r.telemetry.poll();
    r.drain(2);
    TEST_ASSERT(energy_requests(r) == (unsupported ? 2 : 3), "Reconnect re-probes counter support");
  }
}

void test_energy_object_validation() {
  for (int kind = 0; kind < 8; kind++) {
    mock_millis = 0;
    Rig r(true);
    esphome::sensor::Sensor energy;
    r.publisher.set_energy_sensor(&energy);
    r.telemetry.start();
    r.telemetry.poll();
    r.drain(2);
    auto bad = captured_energy_frame();
    if (kind == 0) bad[5] |= 0x80;
    if (kind == 1) bad[6] = 2;
    if (kind == 2) bad[6] = 4;
    if (kind == 3) bad[12] = 27;
    if (kind == 4) bad[5]++;
    if (kind == 5) {
      bad.insert(bad.end() - 2, {7, 0});
      bad[1] += 2;
    }
    if (kind == 6) bad[0] = 0x27;
    if (kind == 7) {
      bad[13] = 0x7F;
      bad[14] = 0xF8;
    }  // NaN energy, valid power.
    bad = with_crc(bad);
    r.transport.on_notification(bad.data(), bad.size());
    TEST_ASSERT(!energy.has_state(), "Malformed/refused object cannot become an energy sample");
  }
}

void test_owned_replies_do_not_answer_other_commands() {
  for (bool wildcard : {true, false}) {
    mock_millis = 0;
    Rig r(true);
    int energy_callbacks = 0, other_callbacks = 0;
    r.transport.send_class10_object_read(87, 1, 232, 1, [&](bool ok, const uint8_t *, size_t) {
      if (ok) energy_callbacks++;
    });
    const uint8_t other[] = {0x0A, 3, 94, 0, 102};
    r.transport.send_apdu_command(other, sizeof(other), wildcard ? 0 : 0xE801, wildcard ? 0 : 3,
                                  [&](bool ok, const uint8_t *, size_t) {
                                    if (ok) other_callbacks++;
                                  });
    r.drain(2);
    TEST_ASSERT(r.sent.size() == 1,
                "Awaited energy owns the transaction before later DST/history reads");
    auto energy = captured_energy_frame();
    r.transport.on_notification(energy.data(), energy.size());
    r.drain(2);
    r.transport.on_notification(energy.data(), energy.size());
    TEST_ASSERT(
        energy_callbacks == 1 && other_callbacks == 0,
        "Duplicate/late type-232 reply cannot answer wildcard DST or typed cycle timestamps");
    auto real = object_frame(wildcard ? 999 : 1000, 1);
    r.transport.on_notification(real.data(), real.size());
    TEST_ASSERT(other_callbacks == 1, "Other command still accepts its actual response");
    r.transport.send_class10_object_read(
        87, 1, 232, 1, [&](bool ok, const uint8_t *payload, size_t len) {
          if (ok && len == 41 &&
              esphome::alpha_hwr::protocol::decode_energy_payload(payload, len).has_energy) {
            energy_callbacks++;
          }
        });
    r.drain(2);
    auto two_pumps = energy;
    two_pumps.insert(two_pumps.end() - 2, 12, 0);
    two_pumps[1] += 12;
    two_pumps[5] = 0x2D;
    two_pumps[12] = 38;
    two_pumps[26] = 2;
    two_pumps = with_crc(two_pumps);
    r.transport.on_notification(two_pumps.data(), two_pumps.size());
    TEST_ASSERT(
        energy_callbacks == 2,
        "Typed read accepts variable object length, including 0x2D telemetry-filter collision");
  }
  // Strict full-type matching also applies without an owned-type registration.
  mock_millis = 0;
  Rig r(true);
  bool history = false;
  const uint8_t other[] = {0x0A, 3, 88, 0, 1};
  r.transport.send_apdu_command(other, sizeof(other), 0xE801, 3,
                                [&](bool ok, const uint8_t *, size_t) { history = ok; });
  r.drain(2);
  auto energy = captured_energy_frame();
  r.transport.on_notification(energy.data(), energy.size());
  TEST_ASSERT(!history, "Type-high zero is not a wildcard in an exact type expectation");
  auto real = object_frame(1000, 1);
  r.transport.on_notification(real.data(), real.size());
  TEST_ASSERT(history, "Full type 1000 still matches");
}

void test_energy_timeout_and_late_reply_guards() {
  for (bool wildcard : {true, false}) {
    mock_millis = 0;
    Rig r(true);
    int energy_callbacks = 0, other_callbacks = 0;
    r.transport.send_class10_object_read(87, 1, 232, 1, [&](bool ok, const uint8_t *p, size_t) {
      if (!ok && p == nullptr) energy_callbacks++;
    });
    r.drain(52);  // Read sent at 60 ms, timed out at 3120 ms.
    const uint8_t other[] = {0x0A, 3, 94, 0, 102};
    r.transport.send_apdu_command(other, sizeof(other), wildcard ? 0 : 0xE801, wildcard ? 0 : 3,
                                  [&](bool ok, const uint8_t *, size_t) {
                                    if (ok) other_callbacks++;
                                  });
    r.drain(1);
    auto late = captured_energy_frame();
    r.transport.on_notification(late.data(), late.size());
    TEST_ASSERT(energy_callbacks == 1 && other_callbacks == 0,
                "After timeout, late typed energy cannot answer another command");
    auto real = object_frame(wildcard ? 999 : 1000, 1);
    r.transport.on_notification(real.data(), real.size());
    TEST_ASSERT(other_callbacks == 1, "Queue continues after energy timeout");
  }
  for (bool guard_expired : {false, true}) {
    mock_millis = 0;
    Rig r(true);
    bool set_completed = false, set_ok = false;
    r.transport.send_class10_object_read(87, 1, 232, 1, [](bool, const uint8_t *, size_t) {});
    r.drain(52);
    if (guard_expired) mock_millis += 501;
    const uint8_t set[] = {0x0A, 0x80};
    r.transport.send_apdu_command(
        set, sizeof(set), 0, 0,
        [&](bool ok, const uint8_t *, size_t) {
          set_completed = true;
          set_ok = ok;
        },
        3000, false, true);
    r.drain(1);
    if (!guard_expired) {
      auto late_error = with_crc({0x24, 5, 0xF8, 0xE7, 0x0A, 1, 4, 0, 0});
      r.transport.on_notification(late_error.data(), late_error.size());
      TEST_ASSERT(!set_completed,
                  "Owed-reply guard consumes a late refusal instead of failing the SET");
    }
    auto ack = with_crc({0x24, 5, 0xF8, 0xE7, 0x0A, 1, 0, 0, 0});
    r.transport.on_notification(ack.data(), ack.size());
    TEST_ASSERT(set_completed && set_ok,
                "A real SET ACK succeeds after debt consumption or guard expiry");
  }
}

void test_energy_timeout_retries_on_cadence() {
  mock_millis = 0;
  Rig r(true);
  esphome::sensor::Sensor energy;
  r.publisher.set_energy_sensor(&energy);
  r.publisher.publish_energy(energy_sample(12.5f));
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(60);
  TEST_ASSERT(r.sent.size() == 6 && energy.publish_count == 1,
              "Energy timeout releases legacy telemetry and preserves the last sample");
  mock_millis += 60000;
  r.telemetry.poll();
  r.drain(2);
  TEST_ASSERT(energy_requests(r) == 2,
              "Timeout retries on the normal cadence without reconnecting");
}

void test_energy_codec_vectors() {
  using namespace esphome::alpha_hwr::protocol;
  const uint8_t bytes[] = {0xAA, 0x3F, 0xF8, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT(decode_double_be(bytes, 1) == 1.5, "Codec decodes binary64 with an offset");
  const uint8_t neg[] = {0xBF, 0xF8, 0, 0, 0, 0, 0, 0};
  const uint8_t inf[] = {0x7F, 0xF0, 0, 0, 0, 0, 0, 0};
  const uint8_t nan[] = {0x7F, 0xF8, 0, 0, 0, 0, 0, 0};
  const uint8_t zero[8] = {};
  TEST_ASSERT(decode_double_be(neg) == -1.5 && decode_double_be(zero) == 0,
              "Codec preserves signed and zero values");
  TEST_ASSERT(std::isinf(decode_double_be(inf)) && std::isnan(decode_double_be(nan)),
              "Codec preserves IEEE special values for validation");
  const uint8_t u24[] = {0xAA, 0x12, 0x34, 0x56};
  TEST_ASSERT(decode_uint24_be(u24, 1) == 0x123456, "Codec decodes uint24 with an offset");
}

void test_energy_payload_validation() {
  using esphome::alpha_hwr::protocol::decode_energy_payload;
  auto frame = captured_energy_frame();
  std::vector<uint8_t> payload(frame.begin() + 10, frame.end() - 2);
  auto decoded = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(decoded.has_energy && std::fabs(decoded.energy_kwh - 49.119374f) < 0.00001f,
              "Captured energy is decoded in kWh");
  TEST_ASSERT(decoded.has_power && decoded.power_w == 0,
              "Captured system power is decoded in watts");
  for (size_t len = 0; len < payload.size(); len++) {
    auto truncated = decode_energy_payload(payload.data(), len);
    TEST_ASSERT(!truncated.has_energy && !truncated.has_power, "Truncated object rejected");
  }
  auto absent = decode_energy_payload(nullptr, 11);
  TEST_ASSERT(!absent.has_energy && !absent.has_power, "Null payload rejected");
  auto short_size = payload;
  short_size[2] = 7;
  auto short_result = decode_energy_payload(short_size.data(), short_size.size());
  TEST_ASSERT(!short_result.has_energy && !short_result.has_power,
              "Object too small for either field rejected");
  short_size[2] = 8;
  auto energy_only = decode_energy_payload(short_size.data(), short_size.size());
  TEST_ASSERT(energy_only.has_energy && !energy_only.has_power,
              "Object size bounds power independently of trailing bytes");
  for (double invalid :
       {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::max()}) {
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
    TEST_ASSERT(result.has_energy && !result.has_power &&
                    std::fabs(result.energy_kwh - 49.119374f) < 0.00001f,
                "Invalid system power does not discard valid energy");
  }
  esphome::alpha_hwr::protocol::encode_float_be(12.5f, payload.data() + 11);
  auto powered = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(powered.has_power && powered.power_w == 12.5f,
              "System power is a big-endian float in watts");
  std::fill(payload.begin() + 3, payload.begin() + 11, 0);
  auto zero = decode_energy_payload(payload.data(), payload.size());
  TEST_ASSERT(zero.has_energy && zero.energy_kwh == 0 && zero.has_power,
              "Zero is a valid cumulative counter");
}


// A timed-out owned read records reply debt (issue #248). When its late reply
// then arrives, the owned-type guard discards it -- and must pay that debt, or
// the next SET's real acknowledgement is consumed in its place and the write
// stalls to its timeout. It must pay ONLY debt an owned read recorded: a stray
// owned-type frame must not cancel a SET's own debt.
void test_late_owned_reply_pays_its_own_debt() {
  const uint8_t set[] = {0x0A, 0x80};
  const auto ack = with_crc({0x24, 5, 0xF8, 0xE7, 0x0A, 1, 0, 0, 0});
  {
    mock_millis = 0;
    Rig r(true);
    bool done = false, ok = false;
    r.transport.send_class10_object_read(87, 1, 232, 1, [](bool, const uint8_t *, size_t) {});
    r.drain(52);  // Energy read times out: one owed reply, recorded by an owned read.
    r.transport.send_apdu_command(
        set, sizeof(set), 0, 0,
        [&](bool k, const uint8_t *, size_t) {
          done = true;
          ok = k;
        },
        3000, false, true);
    r.drain(1);
    auto late = captured_energy_frame();
    r.transport.on_notification(late.data(), late.size());
    r.transport.on_notification(ack.data(), ack.size());
    TEST_ASSERT(done && ok, "A late owned reply pays its own debt; the SET keeps its ACK");
  }
  {
    mock_millis = 0;
    Rig r(true);
    bool a_done = false, b_done = false, c_done = false, c_ok = false;
    r.transport.send_class10_object_read(87, 1, 232, 1, [](bool, const uint8_t *, size_t) {});
    r.drain(52);
    r.transport.send_apdu_command(
        set, sizeof(set), 0, 0, [&](bool, const uint8_t *, size_t) { a_done = true; }, 3000,
        false, true);
    r.drain(1);
    // The energy read's late REFUSAL pays its debt through the short-frame path.
    auto refusal = with_crc({0x24, 5, 0xF8, 0xE7, 0x0A, 1, 4, 0, 0});
    r.transport.on_notification(refusal.data(), refusal.size());
    r.transport.on_notification(ack.data(), ack.size());
    TEST_ASSERT(a_done, "Late owned refusal settles the owned debt; the SET then completes");
    // Now a SET times out: debt an owned read did NOT record.
    r.transport.send_apdu_command(
        set, sizeof(set), 0, 0, [&](bool, const uint8_t *, size_t) { b_done = true; }, 3000,
        false, true);
    r.drain(52);
    TEST_ASSERT(b_done, "Second SET timed out and left a SET-owned debt");
    r.transport.send_apdu_command(
        set, sizeof(set), 0, 0,
        [&](bool k, const uint8_t *, size_t) {
          c_done = true;
          c_ok = k;
        },
        3000, false, true);
    r.drain(1);
    auto stray = captured_energy_frame();
    r.transport.on_notification(stray.data(), stray.size());
    r.transport.on_notification(ack.data(), ack.size());  // The timed-out SET's late ACK.
    TEST_ASSERT(!c_done, "A stray owned reply cannot pay a SET's debt; the late ACK is absorbed");
    r.transport.on_notification(ack.data(), ack.size());
    TEST_ASSERT(c_done && c_ok, "The next SET still receives its own ACK");
  }
}

void test_energy_and_fault_reads_share_the_transport() {
  Rig r;
  esphome::sensor::Sensor energy;
  r.publisher.set_energy_sensor(&energy);
  r.telemetry.set_energy_update_interval(1000);
  r.telemetry.start();
  r.telemetry.poll();
  r.drain(5);
  r.receive(captured_energy_frame());
  r.drain();
  TEST_ASSERT(energy.has_state(), "Energy completes before the awaited fault read");
  r.receive(motor_frame(3, 41));
  TEST_ASSERT(r.power_dc.state == 9.75f, "Motor DC power routes while fault read is pending");
  r.receive(fault_frame({51}));
  r.drain();
  r.receive(fault_frame({77}));
  TEST_ASSERT(r.alarms.state != "None" && r.warnings.state != "None",
              "Energy polling preserves serialized alarm and warning reads");

  r.telemetry.poll();
  r.drain(5);
  r.receive(captured_energy_frame());
  r.drain(70);  // The next alarm read times out.
  int wildcard_calls = 0;
  const uint8_t request[] = {10, 3, 94, 0, 102};
  r.transport.send_apdu_command(request, sizeof(request), 0, 0,
      [&](bool ok, const uint8_t *, size_t) { if (ok) wildcard_calls++; });
  r.drain(5);
  r.receive(fault_frame({51}));
  TEST_ASSERT(wildcard_calls == 0, "A late owned fault reply cannot answer a DST wildcard");
  auto dst = object_frame(323, 1, 10);
  r.receive(dst);
  TEST_ASSERT(wildcard_calls == 1, "The DST wildcard still accepts its own object reply");

  r.telemetry.poll();
  r.drain(5);
  TEST_ASSERT(energy_requests(r) == 3, "Energy polling continues after a failed fault read");
  energy.publish_state(0);
  r.receive(fault_frame({51}));
  TEST_ASSERT(energy.state == 0, "Late alarm cannot update the energy sensor");
  r.receive(captured_energy_frame());
  TEST_ASSERT(std::fabs(energy.state - 49.119374f) < 0.0001f,
              "The matching energy response still publishes after a late alarm");
}

int main() {
  test_energy_and_fault_reads_share_the_transport();
  test_poll_and_fault_identity();
  test_object_types_and_versions();
  test_validation_before_decoding();
  test_captured_motor_and_fragmentation();
  test_fault_failures_do_not_cross_route();
  test_energy_counter();
  test_energy_cadence_and_generation();
  test_energy_short_refusals();
  test_energy_object_validation();
  test_owned_replies_do_not_answer_other_commands();
  test_energy_timeout_and_late_reply_guards();
  test_late_owned_reply_pays_its_own_debt();
  test_energy_timeout_retries_on_cadence();
  test_energy_codec_vectors();
  test_energy_payload_validation();
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
  return tests_failed ? 1 : 0;
}
