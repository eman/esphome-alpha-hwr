// Host tests for SensorPublisher (sensor_publisher.cpp).
//
// Why this file exists: `esphome compile` was the only thing that compiled
// sensor_publisher.cpp. It is the layer every telemetry value passes through on
// its way to Home Assistant, and it carries three separable jobs that had no
// host test between them — presence gating (`has_*` flags), range validation,
// and the issue #127 publish-on-change guards on the alarm/warning text.
//
// Built with -DUSE_TEXT_SENSOR, which matters: the #127 dedup guards live
// inside that ifdef, and the component's AUTO_LOAD pulls text_sensor in on
// every real build. Compiling without it would silently test a different
// program from the one that ships.

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/hal.h"

#include "../components/alpha_hwr/sensor_publisher.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::services::SensorPublisher;
using esphome::alpha_hwr::protocol::FlowPressureTelemetry;
using esphome::alpha_hwr::protocol::MotorStateTelemetry;
using esphome::alpha_hwr::protocol::TemperatureTelemetry;

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

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// Anonymous namespace: several test files in this suite define their own
// Rig, and cppcheck's whole-program pass reports same-named structs across
// translation units as an ODR violation even though each test is its own
// binary. Same treatment as test_read_chain_lifetime.cpp.
namespace {

struct Rig {
  esphome::sensor::Sensor flow, head, head_rate, power, rpm;
  esphome::sensor::Sensor temp_media, temp_pcb, temp_box;
  esphome::sensor::Sensor voltage, voltage_dc, current, inlet_pressure;
  esphome::text_sensor::TextSensor alarms, warnings;

  SensorPublisher pub;

  Rig() {
    pub.set_flow_sensor(&flow);
    pub.set_head_sensor(&head);
    pub.set_head_rate_sensor(&head_rate);
    pub.set_power_sensor(&power);
    pub.set_rpm_sensor(&rpm);
    pub.set_temp_media_sensor(&temp_media);
    pub.set_temp_pcb_sensor(&temp_pcb);
    pub.set_temp_control_box_sensor(&temp_box);
    pub.set_voltage_sensor(&voltage);
    pub.set_voltage_dc_sensor(&voltage_dc);
    pub.set_current_sensor(&current);
    pub.set_inlet_pressure_sensor(&inlet_pressure);
    pub.set_alarms_text_sensor(&alarms);
    pub.set_warnings_text_sensor(&warnings);
  }
};

}  // namespace


// ── 1. Presence gating on motor state ────────────────────────────────────────
// Every field carries its own has_* flag because the pump does not always
// report all of them. Publishing a field the pump did not send would put a
// fabricated 0 on the entity, which is worse than leaving it unknown.
void test_motor_state_publishes_only_present_fields() {
  std::cout << "\n=== Motor state publishes only the fields present ==="
            << std::endl;
  Rig r;
  MotorStateTelemetry m;
  m.has_power = true;
  m.power_w = 12.5f;
  m.has_speed = true;
  m.speed_rpm = 2400.0f;
  // voltage/current deliberately absent

  r.pub.publish_motor_state(m);

  TEST_ASSERT(near(r.power.state, 12.5f) && r.power.publish_count == 1,
              "Power published");
  TEST_ASSERT(near(r.rpm.state, 2400.0f) && r.rpm.publish_count == 1,
              "RPM published");
  TEST_ASSERT(r.voltage.publish_count == 0 && !r.voltage.has_state(),
              "Absent AC voltage stays unknown rather than publishing 0");
  TEST_ASSERT(r.voltage_dc.publish_count == 0,
              "Absent DC voltage stays unknown");
  TEST_ASSERT(r.current.publish_count == 0, "Absent current stays unknown");
}

void test_motor_state_with_no_data_publishes_nothing() {
  std::cout << "\n=== A motor frame with neither power nor speed is dropped ==="
            << std::endl;
  Rig r;
  MotorStateTelemetry m;
  m.has_voltage_ac = true;  // present, but the frame has no power and no speed
  m.voltage_ac_v = 232.0f;

  r.pub.publish_motor_state(m);

  TEST_ASSERT(r.voltage.publish_count == 0,
              "The whole frame is rejected when neither power nor speed is "
              "present — not partially published");
}

// ── 2. Range validation on temperatures ──────────────────────────────────────
// A decode error or a corrupt frame surfaces as an absurd temperature. These
// bounds are the last thing between that and a Home Assistant history graph
// with a permanent spike in it.
void test_temperature_range_validation() {
  std::cout << "\n=== Out-of-range temperatures are rejected ===" << std::endl;
  Rig r;

  TemperatureTelemetry good;
  good.has_media_temp = true;
  good.media_temperature_c = 45.0f;
  good.has_pcb_temp = true;
  good.pcb_temperature_c = 60.0f;
  good.has_control_box_temp = true;
  good.control_box_temperature_c = 55.0f;
  r.pub.publish_temperature(good);

  TEST_ASSERT(near(r.temp_media.state, 45.0f), "In-range media temp published");
  TEST_ASSERT(near(r.temp_pcb.state, 60.0f), "In-range PCB temp published");
  TEST_ASSERT(near(r.temp_box.state, 55.0f), "In-range box temp published");

  TemperatureTelemetry bad;
  bad.has_media_temp = true;
  bad.media_temperature_c = 150.0f;  // media bound is 100 °C
  bad.has_pcb_temp = true;
  bad.pcb_temperature_c = -40.0f;  // below the -20 °C floor
  bad.has_control_box_temp = true;
  bad.control_box_temperature_c = 200.0f;  // above the 150 °C ceiling
  r.pub.publish_temperature(bad);

  TEST_ASSERT(r.temp_media.publish_count == 1 && near(r.temp_media.state, 45.0f),
              "150 °C media reading rejected; the entity keeps its last good "
              "value");
  TEST_ASSERT(r.temp_pcb.publish_count == 1,
              "-40 °C PCB reading rejected at the lower bound");
  TEST_ASSERT(r.temp_box.publish_count == 1,
              "200 °C control box reading rejected at the upper bound");

  // The media channel has a tighter ceiling than the other two, which is easy
  // to lose in a refactor: 120 °C is valid for the PCB and not for the media.
  TemperatureTelemetry edge;
  edge.has_media_temp = true;
  edge.media_temperature_c = 120.0f;
  edge.has_pcb_temp = true;
  edge.pcb_temperature_c = 120.0f;
  r.pub.publish_temperature(edge);

  TEST_ASSERT(r.temp_media.publish_count == 1,
              "120 °C is out of range for media (100 °C ceiling)");
  TEST_ASSERT(r.temp_pcb.publish_count == 2 && near(r.temp_pcb.state, 120.0f),
              "...and in range for the PCB (150 °C ceiling)");
}

// ── 3. Flow and pressure ─────────────────────────────────────────────────────
void test_flow_pressure_publishing() {
  std::cout << "\n=== Flow and head publish; a NaN inlet pressure does not ==="
            << std::endl;
  Rig r;
  FlowPressureTelemetry f;
  f.has_flow = true;
  f.flow_m3h = 1.25f;
  f.has_head = true;
  f.head_m = 3.4f;
  f.has_inlet_pressure = true;
  f.inlet_pressure_bar = NAN;  // the HWR routinely reports this as NaN

  r.pub.publish_flow_pressure(f);

  TEST_ASSERT(near(r.flow.state, 1.25f), "Flow published in m³/h");
  TEST_ASSERT(near(r.head.state, 3.4f),
              "Head published in metres — the pump's native unit");
  TEST_ASSERT(r.inlet_pressure.publish_count == 0,
              "A NaN inlet pressure is dropped even though has_inlet_pressure "
              "is set");

  FlowPressureTelemetry empty;  // neither flow nor head
  empty.has_inlet_pressure = true;
  empty.inlet_pressure_bar = 1.1f;
  r.pub.publish_flow_pressure(empty);
  TEST_ASSERT(r.inlet_pressure.publish_count == 0,
              "A frame with neither flow nor head is dropped whole");
}

// ── 4. Head rate of change ───────────────────────────────────────────────────
// The derivative is computed in a callback on head_sensor_, not in the publish
// path, so it fires however the value arrives. Its two guards — a 30 s
// reconnect-gap reset and a 0.1 s minimum dt — are only reachable from here.
void test_head_rate_derivative() {
  std::cout << "\n=== Head rate derivative and its two guards ===" << std::endl;
  Rig r;
  r.pub.setup_head_rate_callback();

  mock_millis = 100000;
  r.head.publish_state(3.0f);
  TEST_ASSERT(r.head_rate.publish_count == 0,
              "The first sample establishes a baseline and publishes no rate");

  mock_millis = 101000;  // +1.0 s, +0.5 m
  r.head.publish_state(3.5f);
  TEST_ASSERT(r.head_rate.publish_count == 1 && near(r.head_rate.state, 0.5f),
              "0.5 m over 1 s is 0.5 m/s");

  mock_millis = 101050;  // +50 ms: below the 0.1 s floor
  r.head.publish_state(4.0f);
  TEST_ASSERT(r.head_rate.publish_count == 1,
              "A sub-100 ms dt is skipped rather than dividing by a tiny "
              "interval and reporting a huge rate");

  // ...but the baseline still advanced, so the next real sample differentiates
  // from the skipped value, not the one before it.
  mock_millis = 102050;  // +1.0 s from the skipped sample, +0.5 m from 4.0
  r.head.publish_state(4.5f);
  TEST_ASSERT(r.head_rate.publish_count == 2 && near(r.head_rate.state, 0.5f),
              "The skipped sample still advanced the baseline");

  mock_millis = 152050;  // +50 s: a BLE reconnect gap
  r.head.publish_state(1.0f);
  TEST_ASSERT(r.head_rate.publish_count == 2,
              "A gap over 30 s resets the baseline instead of reporting a "
              "3.5 m step as a rate");

  mock_millis = 153050;
  r.head.publish_state(1.2f);
  TEST_ASSERT(r.head_rate.publish_count == 3 && near(r.head_rate.state, 0.2f),
              "And the sample after the reset differentiates normally");
}

void test_head_rate_without_a_rate_sensor_is_inert() {
  std::cout << "\n=== No head-rate sensor wired: no callback registered ==="
            << std::endl;
  esphome::sensor::Sensor head;
  SensorPublisher pub;
  pub.set_head_sensor(&head);
  pub.setup_head_rate_callback();  // head_rate_sensor_ is null

  mock_millis = 200000;
  head.publish_state(1.0f);
  mock_millis = 201000;
  head.publish_state(2.0f);
  TEST_ASSERT(true, "Publishing head with no rate sensor does not crash");
}

// ── 5. Alarm and warning text ────────────────────────────────────────────────
// These are re-read on every telemetry poll and are virtually always "None".
// TextSensor does not dedup, so an unguarded republish is an API state frame
// per subscriber per poll — that is issue #127, and the guard is in this file.
void test_alarm_text_is_published_on_transitions_only() {
  std::cout << "\n=== Alarm text publishes on transitions only (issue #127) ==="
            << std::endl;
  Rig r;

  r.pub.publish_alarms({});
  TEST_ASSERT(r.alarms.publish_count == 1 && r.alarms.state == "None",
              "An empty code list publishes \"None\" once");

  for (int i = 0; i < 10; i++)
    r.pub.publish_alarms({});
  TEST_ASSERT(r.alarms.publish_count == 1,
              "Ten more identical polls publish nothing further");

  r.pub.publish_alarms({51});
  TEST_ASSERT(r.alarms.publish_count == 2,
              "A real alarm code publishes");
  TEST_ASSERT(r.alarms.state.find("(51)") != std::string::npos,
              "...and the text carries the numeric code");

  r.pub.publish_alarms({51});
  TEST_ASSERT(r.alarms.publish_count == 2,
              "Repeating the same alarm stays quiet");

  r.pub.publish_alarms({});
  TEST_ASSERT(r.alarms.publish_count == 3 && r.alarms.state == "None",
              "Clearing back to None publishes the transition");
}

void test_warning_text_uses_the_same_guard() {
  std::cout << "\n=== Warning text uses the same guard ===" << std::endl;
  Rig r;

  r.pub.publish_warnings({});
  for (int i = 0; i < 5; i++)
    r.pub.publish_warnings({});
  TEST_ASSERT(r.warnings.publish_count == 1,
              "Six identical warning polls cost one publish");

  r.pub.publish_warnings({77, 32});
  TEST_ASSERT(r.warnings.publish_count == 2,
              "A two-code list publishes");
  TEST_ASSERT(r.warnings.state.find(", ") != std::string::npos,
              "Multiple codes are comma-separated");
  TEST_ASSERT(r.warnings.state.find("(77)") != std::string::npos &&
                  r.warnings.state.find("(32)") != std::string::npos,
              "Both codes appear");

  // Order is part of the value: the same codes reversed is a different string
  // and must not be suppressed as unchanged.
  r.pub.publish_warnings({32, 77});
  TEST_ASSERT(r.warnings.publish_count == 3,
              "The same codes in a different order is a different state");
}

void test_unknown_codes_are_labelled_not_dropped() {
  std::cout << "\n=== An unrecognised code is labelled, not swallowed ==="
            << std::endl;
  Rig r;
  r.pub.publish_alarms({60000});  // not in the GENI table
  TEST_ASSERT(r.alarms.publish_count == 1,
              "An unknown code still publishes");
  TEST_ASSERT(r.alarms.state.find("60000") != std::string::npos,
              "...carrying the raw number, so it can be looked up");
  TEST_ASSERT(r.alarms.state != "None",
              "...and is never conflated with no alarms at all");
}

// ── 6. Nothing wired ─────────────────────────────────────────────────────────
void test_unwired_publisher_is_inert() {
  std::cout << "\n=== A publisher with no sensors wired is inert ==="
            << std::endl;
  SensorPublisher pub;
  MotorStateTelemetry m;
  m.has_power = true;
  m.power_w = 1.0f;
  m.has_speed = true;
  m.speed_rpm = 1.0f;
  FlowPressureTelemetry f;
  f.has_flow = true;
  f.flow_m3h = 1.0f;
  TemperatureTelemetry t;
  t.has_media_temp = true;
  t.media_temperature_c = 20.0f;

  pub.publish_motor_state(m);
  pub.publish_flow_pressure(f);
  pub.publish_temperature(t);
  pub.publish_alarms({1});
  pub.publish_warnings({1});
  pub.setup_head_rate_callback();

  TEST_ASSERT(true, "Every publish path survives null sensors");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Sensor Publisher Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_motor_state_publishes_only_present_fields();
  test_motor_state_with_no_data_publishes_nothing();
  test_temperature_range_validation();
  test_flow_pressure_publishing();
  test_head_rate_derivative();
  test_head_rate_without_a_rate_sensor_is_inert();
  test_alarm_text_is_published_on_transitions_only();
  test_warning_text_uses_the_same_guard();
  test_unknown_codes_are_labelled_not_dropped();
  test_unwired_publisher_is_inert();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
