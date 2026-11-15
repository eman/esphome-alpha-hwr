// Host tests for type/version telemetry routing and serialized fault reads.
// Fixtures use the GENI profile 52_7 layouts and real CRCs. The motor v3
// fixture below is taken from an HWR capture; synthetic variants exercise
// same-length unrelated objects and different-length supported versions.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "fixture_crc.h"
#include "../components/alpha_hwr/control_service.h"
#include "../components/alpha_hwr/frame_parser.h"
#include "../components/alpha_hwr/sensor_publisher.h"
#include "../components/alpha_hwr/session.h"
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
  esphome::sensor::Sensor power, rpm, flow, head, media, pcb, box;
  esphome::text_sensor::TextSensor alarms, warnings;
  std::vector<std::vector<uint8_t>> sent;

  Rig() {
    mock_millis = 0;
    publisher.set_power_sensor(&power);
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
    TEST_ASSERT(r.power.state == 12.5f && r.rpm.state == 2400, "Known motor version decodes");
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

int main() {
  test_poll_and_fault_identity();
  test_object_types_and_versions();
  test_validation_before_decoding();
  test_captured_motor_and_fragmentation();
  test_fault_failures_do_not_cross_route();
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed\n";
  return tests_failed ? 1 : 0;
}
