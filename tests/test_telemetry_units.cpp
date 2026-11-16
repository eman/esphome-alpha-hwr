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

static void test_additional_diagnostics_from_captured_frames() {
  // CRC-valid HWR motor and process replies reassembled from Android snoops.
  const uint8_t motor[] = {0x24, 0x34, 0xf8, 0xe7, 0x0a, 0x30, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00,
                           0x29, 0x42, 0xe5, 0xa6, 0x46, 0x43, 0x21, 0x4c, 0x00, 0x3e, 0x00, 0x32,
                           0x3e, 0x41, 0x1f, 0xda, 0xc0, 0x41, 0x21, 0x36, 0xa0, 0x45, 0x52, 0x51,
                           0xcc, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8a, 0xb0};
  const uint8_t process[] = {0x24, 0x2f, 0xf8, 0xe7, 0x0a, 0x2b, 0x00, 0x02, 0x35, 0x02, 0x00,
                             0x00, 0x24, 0x38, 0xc6, 0xc4, 0x24, 0x46, 0xc7, 0x70, 0xdd, 0x7f,
                             0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff,
                             0x7f, 0xff, 0xff, 0xff, 0x3e, 0xae, 0xb2, 0x64, 0x40, 0x26, 0xa5,
                             0xf5, 0x3f, 0x2c, 0xe3, 0xad, 0xd4, 0xa8};
  auto m = decode_motor_state_response(motor, sizeof(motor));
  TEST_ASSERT(m.has_dc_power, "Captured motor reply includes DC power");
  TEST_ASSERT_FLOAT_EQ(m.dc_power_w, 10.075836f, 0.0001f, "DC power remains raw watts");
  TEST_ASSERT(m.has_converter_temp && std::isnan(m.converter_temperature_c),
              "Captured converter temperature is unavailable, not zero");
  TEST_ASSERT(m.has_inverter_temp && std::isnan(m.inverter_temperature_c),
              "Captured inverter temperature is unavailable");
  TEST_ASSERT(m.has_aux_temp && m.aux_temperature_c == 0,
              "Captured auxiliary temperature is exactly zero");
  auto f = decode_flow_pressure_response(process, sizeof(process));
  TEST_ASSERT(f.has_differential_temp && std::isnan(f.differential_temperature_c),
              "Captured differential temperature is unavailable");

  uint8_t synthetic[56] = {};
  put_float(synthetic, 29, 18.5f);
  put_float(synthetic, 37, 42.5f);
  put_float(synthetic, 41, 43.5f);
  put_float(synthetic, 45, -5.5f);
  m = decode_motor_state_response(synthetic, sizeof(synthetic));
  TEST_ASSERT(m.has_dc_power && m.dc_power_w == 18.5f && m.converter_temperature_c == 42.5f &&
                  m.inverter_temperature_c == 43.5f && m.aux_temperature_c == -5.5f,
              "Available motor temperatures retain independent raw Celsius values");
  put_float(synthetic, 33, -4.25f);
  f = decode_flow_pressure_response(synthetic, sizeof(synthetic));
  TEST_ASSERT(f.has_differential_temp && f.differential_temperature_c == -4.25f,
              "Differential temperature is signed degrees, with no absolute-temperature offset");
  TEST_ASSERT(!decode_motor_state_response(synthetic, 34).has_dc_power &&
                  !decode_motor_state_response(synthetic, 42).has_converter_temp &&
                  !decode_motor_state_response(synthetic, 46).has_inverter_temp &&
                  !decode_motor_state_response(synthetic, 50).has_aux_temp &&
                  !decode_flow_pressure_response(synthetic, 38).has_differential_temp,
              "Truncated fields cannot overlap trailing CRC bytes");
  put_float(synthetic, 29, INFINITY);
  put_float(synthetic, 41, 999.0f);
  put_float(synthetic, 33, -999.0f);
  m = decode_motor_state_response(synthetic, sizeof(synthetic));
  f = decode_flow_pressure_response(synthetic, sizeof(synthetic));
  TEST_ASSERT(!m.has_dc_power && !m.has_inverter_temp && m.has_aux_temp && !f.has_differential_temp,
              "Invalid diagnostics are rejected independently");
}

int main() {
  std::cout << "Running Telemetry Units Tests..." << std::endl;
  test_motor_state_units();
  test_additional_diagnostics_from_captured_frames();
  test_flow_head_units();
  test_inlet_pressure_units();
  test_temperature_units();

  std::cout << "\nPassed: " << tests_passed << "  Failed: " << tests_failed << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
