// Telemetry units regression test.
//
// Pins the physical-unit interpretation of every live-telemetry field against
// the wire format confirmed in the reverse-engineering resources
// (reference/alpha-hwr/.../protocol/telemetry_decoder.py and the GENI unit
// tables). The pump sends these fields as raw IEEE-754 big-endian floats that
// are ALREADY in physical units — no GENI unit-index scaling, no ×3600, no
// temperature offset. A regression that reintroduces a scale factor on a
// telemetry path (the shape of the #88 m³/h-vs-m³/s class of bug) fails here.
//
// Fixtures are byte-exact to the captured frames' encoding: a known physical
// float is encoded big-endian at the documented offset, then decoded back.
//
// Compiled host-side against codec.cpp + telemetry_decoder.cpp.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "../components/alpha_hwr/codec.h"
#include "../components/alpha_hwr/telemetry_decoder.h"

using namespace esphome::alpha_hwr::protocol;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                 \
  if (condition) {                                      \
    tests_passed++;                                     \
    std::cout << "[PASS] " << message << std::endl;     \
  } else {                                              \
    tests_failed++;                                     \
    std::cout << "[FAIL] " << message << std::endl;     \
  }

#define TEST_ASSERT_FLOAT_EQ(actual, expected, tol, message)                 \
  if (std::abs((actual) - (expected)) < (tol)) {                             \
    tests_passed++;                                                          \
    std::cout << "[PASS] " << message << std::endl;                          \
  } else {                                                                   \
    tests_failed++;                                                          \
    std::cout << "[FAIL] " << message << " (expected: " << (expected)        \
              << ", got: " << (actual) << ")" << std::endl;                  \
  }

// Place a physical float on the wire exactly as the pump does: IEEE-754 BE at
// `offset`. This is the same primitive the decoder reads back (encode_float_be
// / decode_float_be), so a passing test pins the unit, not the codec.
static void put_float(uint8_t *buf, size_t offset, float value) {
  encode_float_be(value, buf + offset);
}

// ── Motor state: V / DC-V / A / W / RPM, all raw physical floats ──────────────
static void test_motor_state_units() {
  uint8_t f[41];
  std::memset(f, 0, sizeof(f));
  put_float(f, 13, 230.0f);   // AC voltage (V)
  put_float(f, 17, 311.0f);   // DC voltage (V)
  put_float(f, 21, 0.18f);    // current (A)
  put_float(f, 25, 12.5f);    // power (W)
  put_float(f, 33, 1650.0f);  // speed (RPM)

  MotorStateTelemetry m = decode_motor_state_response(f, sizeof(f));
  TEST_ASSERT(m.has_power && m.has_speed, "motor state decoded");
  TEST_ASSERT_FLOAT_EQ(m.voltage_ac_v, 230.0f, 0.01f, "AC voltage is raw V (no scaling)");
  TEST_ASSERT_FLOAT_EQ(m.voltage_dc_v, 311.0f, 0.01f, "DC voltage is raw V (no scaling)");
  TEST_ASSERT_FLOAT_EQ(m.current_a, 0.18f, 0.001f, "current is raw A (no scaling)");
  TEST_ASSERT_FLOAT_EQ(m.power_w, 12.5f, 0.01f, "power is raw W (no scaling)");
  // Anti-regression: RPM is a raw float, NOT a scaled 16-bit word (unit-index
  // 19 would be ×100). 1650 must stay 1650.
  TEST_ASSERT_FLOAT_EQ(m.speed_rpm, 1650.0f, 0.01f, "speed is raw RPM (not unit-index scaled)");
}

// ── Flow / head: the #88 axis. Telemetry flow is m³/h, head is meters. ────────
static void test_flow_head_units() {
  uint8_t f[45];
  std::memset(f, 0, sizeof(f));
  put_float(f, 37, 2.5f);   // flow (m³/h)
  put_float(f, 41, 3.2f);   // head (m)

  FlowPressureTelemetry fp = decode_flow_pressure_response(f, sizeof(f));
  TEST_ASSERT(fp.has_flow && fp.has_head, "flow/head decoded");

  // Telemetry flow is already m³/h — the extended-float response, unlike the
  // Object-86 setpoint register, is NOT SI m³/s. A commanded 2.5 must read
  // back 2.5, never 2.5×3600 = 9000 (the pre-#88 failure mode inverted).
  TEST_ASSERT_FLOAT_EQ(fp.flow_m3h, 2.5f, 0.001f, "telemetry flow is m³/h (no ×3600)");
  TEST_ASSERT(fp.flow_m3h < 100.0f, "telemetry flow not m³/s-scaled into out-of-range");

  // Head is meters of head — the pump's native unit, matching the setpoints.
  // A regression that re-applied the old ×9.80665 kPa conversion would read
  // 3.2 m as 31.4; pin meters.
  TEST_ASSERT_FLOAT_EQ(fp.head_m, 3.2f, 0.001f, "head is meters (no ×9.80665 kPa)");
  TEST_ASSERT(std::abs(fp.head_m - 3.2f * 9.80665f) > 1.0f, "head is NOT kPa");
}

// ── Inlet pressure: raw bar float ────────────────────────────────────────────
static void test_inlet_pressure_units() {
  uint8_t f[49];
  std::memset(f, 0, sizeof(f));
  put_float(f, 37, 1.0f);
  put_float(f, 41, 2.0f);
  put_float(f, 45, 1.6f);  // inlet pressure (bar)

  FlowPressureTelemetry fp = decode_flow_pressure_response(f, sizeof(f));
  TEST_ASSERT(fp.has_inlet_pressure, "inlet pressure decoded");
  TEST_ASSERT_FLOAT_EQ(fp.inlet_pressure_bar, 1.6f, 0.001f, "inlet pressure is raw bar");
}

// ── Temperatures: raw °C floats, no deci-K / centi-degree offset ─────────────
static void test_temperature_units() {
  uint8_t f[25];
  std::memset(f, 0, sizeof(f));
  put_float(f, 13, 37.0f);  // media (°C)
  put_float(f, 17, 40.5f);  // PCB (°C)
  put_float(f, 21, 42.0f);  // control box (°C)

  TemperatureTelemetry t = decode_temperature_response(f, sizeof(f));
  TEST_ASSERT(t.has_media_temp, "media temperature decoded");
  // A raw float 37.0 stays 37.0 — not divided by 10 (deci-K ≈ 3.7) or offset by
  // 273.15. Pins the "temperature is a plain °C float" assumption.
  TEST_ASSERT_FLOAT_EQ(t.media_temperature_c, 37.0f, 0.01f, "media temp is raw °C (no offset/scale)");
  TEST_ASSERT_FLOAT_EQ(t.pcb_temperature_c, 40.5f, 0.01f, "PCB temp is raw °C");
  TEST_ASSERT_FLOAT_EQ(t.control_box_temperature_c, 42.0f, 0.01f, "control-box temp is raw °C");
}

static void test_dc_power_from_captured_frame() {
  const uint8_t motor[] = {0x24, 0x34, 0xf8, 0xe7, 0x0a, 0x30, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00,
                           0x29, 0x42, 0xe5, 0xa6, 0x46, 0x43, 0x21, 0x4c, 0x00, 0x3e, 0x00, 0x32,
                           0x3e, 0x41, 0x1f, 0xda, 0xc0, 0x41, 0x21, 0x36, 0xa0, 0x45, 0x52, 0x51,
                           0xcc, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8a, 0xb0};
  auto motor_state = decode_motor_state_response(motor, sizeof(motor));
  TEST_ASSERT(motor_state.has_dc_power, "Captured motor reply includes DC power");
  TEST_ASSERT_FLOAT_EQ(motor_state.dc_power_w, 10.075836f, 0.0001f, "DC power remains raw watts");

  uint8_t synthetic[56] = {};
  put_float(synthetic, 29, 18.5f);
  motor_state = decode_motor_state_response(synthetic, sizeof(synthetic));
  TEST_ASSERT(motor_state.has_dc_power && motor_state.dc_power_w == 18.5f,
              "DC power is decoded independently of motor power");
  TEST_ASSERT(!decode_motor_state_response(synthetic, 34).has_dc_power,
              "Truncated DC power cannot overlap trailing CRC bytes");
  TEST_ASSERT(decode_motor_state_response(synthetic, 35).has_dc_power,
              "Complete DC power is accepted before later motor fields");
  for (float invalid : {INFINITY, NAN, -1.0f, 1001.0f}) {
    put_float(synthetic, 29, invalid);
    motor_state = decode_motor_state_response(synthetic, sizeof(synthetic));
    TEST_ASSERT(!motor_state.has_dc_power && motor_state.has_power,
                "Invalid DC power does not invalidate motor power");
  }
}

int main() {
  std::cout << "Running Telemetry Units Tests..." << std::endl;
  test_motor_state_units();
  test_dc_power_from_captured_frame();
  test_flow_head_units();
  test_inlet_pressure_units();
  test_temperature_units();

  std::cout << "\nPassed: " << tests_passed << "  Failed: " << tests_failed << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
