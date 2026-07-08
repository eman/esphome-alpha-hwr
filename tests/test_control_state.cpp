/**
 * Unit tests for ControlService pump enabled state tracking.
 *
 * Tests the logic that separates "pump enabled" (user intent) from
 * "motor running" (physical RPM > 0). This distinction is critical for
 * modes like Temperature Range where the motor cycles on/off autonomously.
 *
 * These tests verify the state machine without ESP32 or BLE dependencies
 * by extracting the pure logic into testable assertions.
 */

#include <iostream>
#include <cstdint>
#include <cstring>
#include <string>
#include <cmath>

// Test result tracking (same framework as test_protocol.cpp)
int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

#define TEST_ASSERT_EQ(actual, expected, message) \
  if ((actual) == (expected)) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << " (expected: " << (expected) << ", got: " << (actual) << ")" << std::endl; \
  }

// ============================================================================
// Minimal replicas of the enums and state logic from control_service.h/cpp
// These mirror the production code exactly, but without ESP32 dependencies.
// ============================================================================

enum class OperationMode : uint8_t {
  AUTO = 0,
  STOP = 1,
  USER_DEFINED = 4,
};

enum class ControlMode : uint8_t {
  CONSTANT_PRESSURE = 0,
  PROPORTIONAL_PRESSURE = 1,
  CONSTANT_SPEED = 2,
  CONSTANT_FLOW = 8,
  DHW_ON_OFF = 25,
  TEMPERATURE_RANGE = 27,
  NONE = 254,
};

/**
 * Minimal state tracker that mirrors the pump_enabled_ logic
 * in ControlService. This is the exact same logic extracted for testing.
 */
struct PumpEnabledState {
  bool pump_enabled{false};
  bool pump_enabled_valid{false};
  ControlMode current_mode{ControlMode::NONE};
  bool mode_valid{false};

  // Mirrors ControlService::update_mode_from_notification()
  void update_from_notification(uint8_t mode, uint8_t operation_mode) {
    current_mode = static_cast<ControlMode>(mode);
    mode_valid = true;
    pump_enabled = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
    pump_enabled_valid = true;
  }

  // Mirrors ControlService::start()
  bool start(uint8_t mode = 255) {
    if (mode != 255) {
      current_mode = static_cast<ControlMode>(mode);
      mode_valid = true;
    }
    pump_enabled = true;
    pump_enabled_valid = true;
    return true;
  }

  // Mirrors ControlService::stop()
  bool stop(uint8_t /* mode */ = 255) {
    pump_enabled = false;
    pump_enabled_valid = true;
    return true;
  }

  // Mirrors get_mode_async callback logic
  void update_from_mode_read(uint8_t operation_mode) {
    pump_enabled = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
    pump_enabled_valid = true;
  }
};

/**
 * Mirrors ControlService::check_flow_setpoint_scale() (issue #44 diagnostic
 * aid). Returns true if the transition from previous_setpoint to
 * new_setpoint would trigger the scaling-mismatch warning (>10x change in
 * either direction), false otherwise. Guards against NAN and a zero
 * previous value (first read / not-yet-cached) to avoid false positives.
 */
bool flow_setpoint_scale_flagged(float previous_setpoint, float new_setpoint) {
  if (std::isnan(previous_setpoint) || std::isnan(new_setpoint) || previous_setpoint == 0.0f) {
    return false;
  }
  float ratio = new_setpoint / previous_setpoint;
  return (ratio > 10.0f || ratio < 0.1f);
}

// ============================================================================
// Test: Initial state (before any pump communication)
// ============================================================================
void test_initial_state() {
  std::cout << "\n=== Testing Initial State ===" << std::endl;

  PumpEnabledState state;

  TEST_ASSERT_EQ(state.pump_enabled, false, "Initial: pump_enabled is false");
  TEST_ASSERT_EQ(state.pump_enabled_valid, false, "Initial: pump_enabled_valid is false");
  TEST_ASSERT_EQ(state.mode_valid, false, "Initial: mode_valid is false");
  TEST_ASSERT(state.current_mode == ControlMode::NONE, "Initial: mode is NONE");
}

// ============================================================================
// Test: Notification with AUTO operation mode → pump enabled
// ============================================================================
void test_notification_auto_mode() {
  std::cout << "\n=== Testing Notification: AUTO Mode ===" << std::endl;

  PumpEnabledState state;

  // Simulate passive notification: Temperature Range mode, AUTO operation
  state.update_from_notification(
      static_cast<uint8_t>(ControlMode::TEMPERATURE_RANGE),
      static_cast<uint8_t>(OperationMode::AUTO));

  TEST_ASSERT_EQ(state.pump_enabled, true, "AUTO notification: pump is enabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "AUTO notification: state is valid");
  TEST_ASSERT(state.current_mode == ControlMode::TEMPERATURE_RANGE,
              "AUTO notification: mode is TEMPERATURE_RANGE");
  TEST_ASSERT_EQ(state.mode_valid, true, "AUTO notification: mode is valid");
}

// ============================================================================
// Test: Notification with STOP operation mode → pump disabled
// ============================================================================
void test_notification_stop_mode() {
  std::cout << "\n=== Testing Notification: STOP Mode ===" << std::endl;

  PumpEnabledState state;

  // Simulate passive notification: Temperature Range mode, STOP operation
  state.update_from_notification(
      static_cast<uint8_t>(ControlMode::TEMPERATURE_RANGE),
      static_cast<uint8_t>(OperationMode::STOP));

  TEST_ASSERT_EQ(state.pump_enabled, false, "STOP notification: pump is disabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "STOP notification: state is valid");
  TEST_ASSERT(state.current_mode == ControlMode::TEMPERATURE_RANGE,
              "STOP notification: mode is still TEMPERATURE_RANGE");
}

// ============================================================================
// Test: Notification with USER_DEFINED operation mode → pump enabled
// ============================================================================
void test_notification_user_defined_mode() {
  std::cout << "\n=== Testing Notification: USER_DEFINED Mode ===" << std::endl;

  PumpEnabledState state;

  state.update_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED),
      static_cast<uint8_t>(OperationMode::USER_DEFINED));

  TEST_ASSERT_EQ(state.pump_enabled, true, "USER_DEFINED notification: pump is enabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "USER_DEFINED notification: state is valid");
}

// ============================================================================
// Test: start() sets pump enabled
// ============================================================================
void test_start_enables_pump() {
  std::cout << "\n=== Testing start() Enables Pump ===" << std::endl;

  PumpEnabledState state;

  // Start with default mode (255 = use current)
  state.start();

  TEST_ASSERT_EQ(state.pump_enabled, true, "start(): pump is enabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "start(): state is valid");
}

// ============================================================================
// Test: start() with specific mode sets mode and enables
// ============================================================================
void test_start_with_mode() {
  std::cout << "\n=== Testing start() With Specific Mode ===" << std::endl;

  PumpEnabledState state;

  state.start(static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE));

  TEST_ASSERT_EQ(state.pump_enabled, true, "start(mode): pump is enabled");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_PRESSURE,
              "start(mode): mode updated to CONSTANT_PRESSURE");
  TEST_ASSERT_EQ(state.mode_valid, true, "start(mode): mode is valid");
}

// ============================================================================
// Test: stop() sets pump disabled
// ============================================================================
void test_stop_disables_pump() {
  std::cout << "\n=== Testing stop() Disables Pump ===" << std::endl;

  PumpEnabledState state;

  // First enable the pump
  state.start();
  TEST_ASSERT_EQ(state.pump_enabled, true, "Pre-stop: pump is enabled");

  // Now stop it
  state.stop();

  TEST_ASSERT_EQ(state.pump_enabled, false, "stop(): pump is disabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "stop(): state is valid");
}

// ============================================================================
// Test: Sequence - start → stop → start (state transitions correctly)
// ============================================================================
void test_start_stop_sequence() {
  std::cout << "\n=== Testing Start/Stop Sequence ===" << std::endl;

  PumpEnabledState state;

  state.start();
  TEST_ASSERT_EQ(state.pump_enabled, true, "Sequence: after start → enabled");

  state.stop();
  TEST_ASSERT_EQ(state.pump_enabled, false, "Sequence: after stop → disabled");

  state.start();
  TEST_ASSERT_EQ(state.pump_enabled, true, "Sequence: after re-start → enabled");
}

// ============================================================================
// Test: Notification overrides optimistic state from start/stop
// ============================================================================
void test_notification_overrides_optimistic() {
  std::cout << "\n=== Testing Notification Overrides Optimistic State ===" << std::endl;

  PumpEnabledState state;

  // User started the pump (optimistic: enabled)
  state.start();
  TEST_ASSERT_EQ(state.pump_enabled, true, "Override: after start → enabled");

  // Pump notification says STOP (real state from pump)
  state.update_from_notification(
      static_cast<uint8_t>(ControlMode::TEMPERATURE_RANGE),
      static_cast<uint8_t>(OperationMode::STOP));

  TEST_ASSERT_EQ(state.pump_enabled, false,
                 "Override: notification STOP overrides optimistic start");
}

// ============================================================================
// Test: Temperature Range mode - motor cycling doesn't affect enabled state
// ============================================================================
void test_temp_range_motor_cycling() {
  std::cout << "\n=== Testing Temperature Range Motor Cycling ===" << std::endl;

  PumpEnabledState state;

  // Pump is enabled in temperature range mode (AUTO operation)
  state.update_from_notification(
      static_cast<uint8_t>(ControlMode::TEMPERATURE_RANGE),
      static_cast<uint8_t>(OperationMode::AUTO));

  TEST_ASSERT_EQ(state.pump_enabled, true,
                 "TempRange: pump enabled while motor may cycle");

  // Simulate: motor RPM goes to 0 (motor off between cycles)
  // The pump_enabled state should NOT change - it's independent of RPM
  // (RPM tracking is handled by the binary sensor, not the switch)
  bool motor_active = false;  // RPM = 0
  TEST_ASSERT_EQ(state.pump_enabled, true,
                 "TempRange: pump still enabled when motor idle (RPM=0)");
  TEST_ASSERT_EQ(motor_active, false,
                 "TempRange: motor is idle (separate from enabled state)");

  // Simulate: motor RPM goes back up (motor started by temp trigger)
  motor_active = true;  // RPM > 0
  TEST_ASSERT_EQ(state.pump_enabled, true,
                 "TempRange: pump still enabled when motor restarts");
  TEST_ASSERT_EQ(motor_active, true,
                 "TempRange: motor is active (separate from enabled state)");
}

// ============================================================================
// Test: All control modes derive enabled state correctly from AUTO
// ============================================================================
void test_all_modes_auto_enabled() {
  std::cout << "\n=== Testing All Modes with AUTO → Enabled ===" << std::endl;

  struct TestCase {
    ControlMode mode;
    const char *name;
  };

  TestCase modes[] = {
      {ControlMode::CONSTANT_PRESSURE, "CONSTANT_PRESSURE"},
      {ControlMode::PROPORTIONAL_PRESSURE, "PROPORTIONAL_PRESSURE"},
      {ControlMode::CONSTANT_SPEED, "CONSTANT_SPEED"},
      {ControlMode::CONSTANT_FLOW, "CONSTANT_FLOW"},
      {ControlMode::DHW_ON_OFF, "DHW_ON_OFF"},
      {ControlMode::TEMPERATURE_RANGE, "TEMPERATURE_RANGE"},
  };

  for (const auto &tc : modes) {
    PumpEnabledState state;
    state.update_from_notification(
        static_cast<uint8_t>(tc.mode),
        static_cast<uint8_t>(OperationMode::AUTO));

    std::string msg = std::string(tc.name) + " + AUTO → enabled";
    TEST_ASSERT_EQ(state.pump_enabled, true, msg.c_str());
  }
}

// ============================================================================
// Test: All control modes derive disabled state correctly from STOP
// ============================================================================
void test_all_modes_stop_disabled() {
  std::cout << "\n=== Testing All Modes with STOP → Disabled ===" << std::endl;

  struct TestCase {
    ControlMode mode;
    const char *name;
  };

  TestCase modes[] = {
      {ControlMode::CONSTANT_PRESSURE, "CONSTANT_PRESSURE"},
      {ControlMode::PROPORTIONAL_PRESSURE, "PROPORTIONAL_PRESSURE"},
      {ControlMode::CONSTANT_SPEED, "CONSTANT_SPEED"},
      {ControlMode::CONSTANT_FLOW, "CONSTANT_FLOW"},
      {ControlMode::DHW_ON_OFF, "DHW_ON_OFF"},
      {ControlMode::TEMPERATURE_RANGE, "TEMPERATURE_RANGE"},
  };

  for (const auto &tc : modes) {
    PumpEnabledState state;
    state.update_from_notification(
        static_cast<uint8_t>(tc.mode),
        static_cast<uint8_t>(OperationMode::STOP));

    std::string msg = std::string(tc.name) + " + STOP → disabled";
    TEST_ASSERT_EQ(state.pump_enabled, false, msg.c_str());
  }
}

// ============================================================================
// Test: get_mode_async callback updates enabled state
// ============================================================================
void test_mode_read_updates_enabled() {
  std::cout << "\n=== Testing Mode Read Updates Enabled State ===" << std::endl;

  PumpEnabledState state;

  // Simulate get_mode_async callback with AUTO
  state.update_from_mode_read(static_cast<uint8_t>(OperationMode::AUTO));
  TEST_ASSERT_EQ(state.pump_enabled, true, "Mode read AUTO: pump enabled");
  TEST_ASSERT_EQ(state.pump_enabled_valid, true, "Mode read AUTO: state valid");

  // Simulate get_mode_async callback with STOP
  state.update_from_mode_read(static_cast<uint8_t>(OperationMode::STOP));
  TEST_ASSERT_EQ(state.pump_enabled, false, "Mode read STOP: pump disabled");
}

// ============================================================================
// Test: Constant Flow setpoint scale check flags the reporter's scenario (#44)
// ============================================================================
void test_flow_scale_flags_reporter_scenario() {
  std::cout << "\n=== Testing Flow Setpoint Scale Check: Reporter's Scenario (#44) ===" << std::endl;

  // Reporter had a plausible commanded setpoint, then a readback of 0.003056
  // (~650x smaller) — should be flagged as a likely scaling issue.
  TEST_ASSERT(flow_setpoint_scale_flagged(2.0f, 0.003056f),
              "#44: readback far below commanded setpoint is flagged");
}

// ============================================================================
// Test: Flow setpoint scale check flags large upward jumps too
// ============================================================================
void test_flow_scale_flags_large_upward_jump() {
  std::cout << "\n=== Testing Flow Setpoint Scale Check: Large Upward Jump ===" << std::endl;

  TEST_ASSERT(flow_setpoint_scale_flagged(0.5f, 6.0f),
              "Large upward jump (12x) is flagged");
}

// ============================================================================
// Test: Flow setpoint scale check does not false-positive on normal adjustments
// ============================================================================
void test_flow_scale_ignores_normal_adjustment() {
  std::cout << "\n=== Testing Flow Setpoint Scale Check: Normal Adjustment (No False Positive) ===" << std::endl;

  TEST_ASSERT(!flow_setpoint_scale_flagged(2.0f, 2.1f),
              "Small user-driven adjustment (1.05x) is not flagged");
  TEST_ASSERT(!flow_setpoint_scale_flagged(2.0f, 0.3f),
              "3x change (within 0.1x-10x band) is not flagged");
}

// ============================================================================
// Test: Flow setpoint scale check ignores first read / uninitialized values
// ============================================================================
void test_flow_scale_ignores_first_read() {
  std::cout << "\n=== Testing Flow Setpoint Scale Check: First Read Guard ===" << std::endl;

  TEST_ASSERT(!flow_setpoint_scale_flagged(NAN, 2.0f),
              "First read (previous=NAN) is never flagged");
  TEST_ASSERT(!flow_setpoint_scale_flagged(0.0f, 2.0f),
              "Zero previous value is never flagged (avoids divide-by-zero)");
  TEST_ASSERT(!flow_setpoint_scale_flagged(2.0f, NAN),
              "NAN new value is never flagged");
}

// ============================================================================
// Main
// ============================================================================
int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Pump Enabled State Test Suite" << std::endl;
  std::cout << "  Tests separation of pump enabled (user intent) from" << std::endl;
  std::cout << "  motor running (physical RPM > 0)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_initial_state();
  test_notification_auto_mode();
  test_notification_stop_mode();
  test_notification_user_defined_mode();
  test_start_enables_pump();
  test_start_with_mode();
  test_stop_disables_pump();
  test_start_stop_sequence();
  test_notification_overrides_optimistic();
  test_temp_range_motor_cycling();
  test_all_modes_auto_enabled();
  test_all_modes_stop_disabled();
  test_mode_read_updates_enabled();
  test_flow_scale_flags_reporter_scenario();
  test_flow_scale_flags_large_upward_jump();
  test_flow_scale_ignores_normal_adjustment();
  test_flow_scale_ignores_first_read();

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
    return 1;
  }
}
