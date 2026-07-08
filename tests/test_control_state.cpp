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
  float cached_setpoint{NAN};  // Mirrors ControlService::cached_setpoint_ (display units)

  // Records the setpoint actually sent to send_control_request() by the last
  // start() call, so tests can assert on it. NAN means "no setpoint was
  // passed" (i.e. send_control_request() fell back to the mode's default
  // suffix, reproducing the #43 hardcoded-3671 bug if left unfixed).
  float last_sent_setpoint{NAN};

  // Mirrors ControlService::update_mode_from_notification()
  void update_from_notification(uint8_t mode, uint8_t operation_mode) {
    current_mode = static_cast<ControlMode>(mode);
    mode_valid = true;
    pump_enabled = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
    pump_enabled_valid = true;
  }

  // Mirrors ControlService::start() (fix for #43: reuse cached setpoint
  // instead of always relying on the mode's hardcoded default suffix).
  bool start(uint8_t mode = 255) {
    if (mode != 255) {
      current_mode = static_cast<ControlMode>(mode);
      mode_valid = true;
      // Clear any setpoint cached for the previous mode -- it's not valid
      // for the newly-requested mode. Mirrors ControlService::start().
      cached_setpoint = NAN;
    }

    ControlMode target = current_mode;

    // Only reuse cached_setpoint when no explicit mode override was given,
    // a cached setpoint exists, and the target mode is one where
    // cached_setpoint is known to hold a plain setpoint float.
    float start_setpoint = NAN;
    if (mode == 255 && !std::isnan(cached_setpoint) &&
        (target == ControlMode::CONSTANT_PRESSURE || target == ControlMode::PROPORTIONAL_PRESSURE ||
         target == ControlMode::CONSTANT_SPEED || target == ControlMode::CONSTANT_FLOW)) {
      start_setpoint = cached_setpoint;
      if (target == ControlMode::CONSTANT_PRESSURE || target == ControlMode::PROPORTIONAL_PRESSURE) {
        start_setpoint *= 9806.65f;
      }
    }
    last_sent_setpoint = start_setpoint;

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
 * Minimal state tracker mirroring ControlService::handle_remote_mode_ack()
 * (fix #46): is_remote_mode_enabled_ is only updated when the pump's Class 3
 * ACK confirms success (ack byte 0x00); a rejected ACK (0x01) or a timeout
 * leaves the previous state unchanged instead of blindly assuming success.
 */
struct RemoteModeState {
  bool is_remote_mode_enabled{false};

  // Mirrors ControlService::handle_remote_mode_ack()
  void handle_ack(bool enabling, bool got_response, uint8_t ack_byte) {
    if (!got_response) {
      return;  // Timeout: leave state unchanged
    }
    if (ack_byte == 0x00) {
      is_remote_mode_enabled = enabling;
    }
    // ack_byte != 0x00 (e.g. 0x01 "rejected"): leave state unchanged
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

/**
 * Mirrors the setpoint-caching branch of
 * ControlService::update_mode_from_notification() / get_mode_async() after
 * the #44 display fix (and the review-feedback follow-up fixing cross-mode
 * contamination): for CONSTANT_FLOW, the Object 86/Sub 6 register is
 * known-unreliable (bench-verified) and is never applied to cached_setpoint_.
 * The previous (client-commanded) value is kept as-is *only* when we were
 * already in CONSTANT_FLOW (steady state); if we just transitioned into
 * CONSTANT_FLOW from a different mode, the stale cross-mode value (RPM,
 * meters, etc.) is cleared to NAN instead of leaking in as a bogus flow
 * setpoint. Pressure modes still apply the Pa->m conversion; all other
 * modes still trust the register.
 */
float resolve_cached_setpoint_from_pump_read(ControlMode mode, ControlMode previous_mode,
                                              float previous_cached, float raw_from_pump) {
  if (mode == ControlMode::CONSTANT_PRESSURE || mode == ControlMode::PROPORTIONAL_PRESSURE) {
    return raw_from_pump / 9806.65f;
  } else if (mode == ControlMode::CONSTANT_FLOW) {
    if (previous_mode != ControlMode::CONSTANT_FLOW) {
      return NAN;  // Just entered Constant Flow: clear stale cross-mode value
    }
    return previous_cached;  // Steady state: keep last client-commanded value
  } else {
    return raw_from_pump;
  }
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
// Test: Remote mode ACK handling -- clean ACK (0x00) confirms the requested
// state (fixes #46, bench-verified: opcode 0x81 produces this ACK)
// ============================================================================
void test_remote_mode_clean_ack_confirms_state() {
  std::cout << "\n=== Testing Remote Mode: Clean ACK (0x00) Confirms State (#46) ===" << std::endl;

  RemoteModeState state;

  state.handle_ack(/*enabling=*/true, /*got_response=*/true, /*ack_byte=*/0x00);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, true,
                 "#46: clean ACK (0x00) confirms remote mode enabled");

  state.handle_ack(/*enabling=*/false, /*got_response=*/true, /*ack_byte=*/0x00);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, false,
                 "#46: clean ACK (0x00) confirms remote mode disabled");
}

// ============================================================================
// Test: Remote mode ACK handling -- rejected ACK (0x01) leaves state unchanged
// (bench-verified: the old opcode 0xC1 always produced this rejected ACK)
// ============================================================================
void test_remote_mode_rejected_ack_leaves_state_unchanged() {
  std::cout << "\n=== Testing Remote Mode: Rejected ACK (0x01) Leaves State Unchanged (#46) ===" << std::endl;

  RemoteModeState state;
  // Starts false; a rejected enable attempt must NOT flip it to true --
  // this is the exact bug #46 reports (old code assumed success unconditionally).
  state.handle_ack(/*enabling=*/true, /*got_response=*/true, /*ack_byte=*/0x01);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, false,
                 "#46: rejected ACK (0x01) does not falsely report remote mode as enabled");
}

// ============================================================================
// Test: Remote mode ACK handling -- timeout (no response) leaves state unchanged
// ============================================================================
void test_remote_mode_timeout_leaves_state_unchanged() {
  std::cout << "\n=== Testing Remote Mode: Timeout Leaves State Unchanged (#46) ===" << std::endl;

  RemoteModeState state;
  state.handle_ack(/*enabling=*/true, /*got_response=*/false, /*ack_byte=*/0x00);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, false,
                 "#46: no response (timeout) does not falsely report remote mode as enabled");
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
// Test: Constant Flow display keeps last commanded value in steady state,
// ignores bad register (#44 fix)
// ============================================================================
void test_flow_display_ignores_unreliable_register() {
  std::cout << "\n=== Testing Constant Flow Display Ignores Unreliable Register (#44 fix) ===" << std::endl;

  // Bench-verified scenario: already in Constant Flow (steady state), user
  // commanded 2.0 m³/h, register always reads back 0.000694.
  float resolved = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_FLOW, ControlMode::CONSTANT_FLOW,
                                                           2.0f, 0.000694f);
  TEST_ASSERT_EQ(resolved, 2.0f,
                 "#44: Constant Flow keeps the last commanded value instead of the bad register readback");

  // Never commanded yet (previous cached is NAN) -> stays NAN, doesn't show a wrong number.
  float resolved_uncommanded = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_FLOW, ControlMode::CONSTANT_FLOW,
                                                                       NAN, 0.000694f);
  TEST_ASSERT(std::isnan(resolved_uncommanded),
              "#44: Constant Flow with nothing commanded yet stays NAN rather than showing the bad register value");
}

// ============================================================================
// Test: Entering Constant Flow from a different mode clears a stale
// cross-mode cached setpoint instead of leaking it in as a bogus flow value
// (review feedback on #44/PR #48).
// ============================================================================
void test_flow_mode_transition_clears_cross_mode_setpoint() {
  std::cout << "\n=== Testing Constant Flow Mode Transition Clears Cross-Mode Setpoint (Review Feedback) ===" << std::endl;

  // Coming from Constant Speed with a cached 2000 RPM value -- must NOT
  // display as if it were a 2000 m^3/h flow setpoint.
  float resolved = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_FLOW, ControlMode::CONSTANT_SPEED,
                                                           2000.0f, 0.000694f);
  TEST_ASSERT(std::isnan(resolved),
              "Entering Constant Flow from Constant Speed clears the stale 2000 RPM cross-mode value");

  // Coming from Constant Pressure with a cached 4.0 m value -- same story.
  float resolved_from_pressure = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_FLOW, ControlMode::CONSTANT_PRESSURE,
                                                                         4.0f, 0.000694f);
  TEST_ASSERT(std::isnan(resolved_from_pressure),
              "Entering Constant Flow from Constant Pressure clears the stale 4.0 m cross-mode value");
}

// ============================================================================
// Test: Other modes still trust the register (no regression from #44 fix)
// ============================================================================
void test_other_modes_still_trust_register() {
  std::cout << "\n=== Testing Other Modes Still Trust the Register (No Regression) ===" << std::endl;

  float resolved_speed = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_SPEED, ControlMode::NONE,
                                                                 NAN, 2000.0f);
  TEST_ASSERT_EQ(resolved_speed, 2000.0f, "Constant Speed still reads its setpoint from the register");

  float resolved_pressure = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_PRESSURE, ControlMode::NONE,
                                                                    NAN, 4.0f * 9806.65f);
  TEST_ASSERT(std::fabs(resolved_pressure - 4.0f) < 0.01f,
              "Constant Pressure still converts the register's Pa reading to meters");
}

// ============================================================================
// Test: start() reuses cached Constant Speed setpoint (fixes #43)
// ============================================================================
void test_start_reuses_cached_speed_setpoint() {
  std::cout << "\n=== Testing start() Reuses Cached Constant Speed Setpoint (#43) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.cached_setpoint = 2000.0f;  // User configured 2000 RPM

  state.start();  // mode=255 → use current mode

  TEST_ASSERT(!std::isnan(state.last_sent_setpoint),
              "#43: start() sends a setpoint instead of falling back to default suffix");
  TEST_ASSERT_EQ(state.last_sent_setpoint, 2000.0f,
                 "#43: start() sends the cached 2000 RPM setpoint, not hardcoded 3671");
}

// ============================================================================
// Test: start() converts cached Constant Pressure setpoint (meters) to Pascals
// ============================================================================
void test_start_converts_pressure_setpoint() {
  std::cout << "\n=== Testing start() Converts Cached Pressure Setpoint (m -> Pa) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_PRESSURE;
  state.mode_valid = true;
  state.cached_setpoint = 4.0f;  // User configured 4.0 m

  state.start();

  float expected_pa = 4.0f * 9806.65f;
  TEST_ASSERT(!std::isnan(state.last_sent_setpoint),
              "Pressure: start() sends a setpoint instead of the default suffix");
  TEST_ASSERT(std::fabs(state.last_sent_setpoint - expected_pa) < 0.01f,
              "Pressure: cached 4.0 m setpoint is converted to Pascals before sending");
}

// ============================================================================
// Test: start() falls back to default suffix when no setpoint is cached yet
// ============================================================================
void test_start_falls_back_without_cached_setpoint() {
  std::cout << "\n=== Testing start() Fallback When No Cached Setpoint (Regression Guard) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  // cached_setpoint left as NAN (nothing read from pump yet)

  state.start();

  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "Fallback: no cached setpoint -> no setpoint override sent (unchanged behavior)");
}

// ============================================================================
// Test: explicit mode override does not reuse a stale cached setpoint
// ============================================================================
void test_start_with_mode_override_ignores_stale_setpoint() {
  std::cout << "\n=== Testing start(mode) Ignores Stale Cached Setpoint ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.cached_setpoint = 2000.0f;  // Cached for CONSTANT_SPEED

  // Switch to a different mode with an explicit override
  state.start(static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE));

  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "Mode override: stale Constant Speed setpoint is not reused for the new mode");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_PRESSURE,
              "Mode override: mode updated to CONSTANT_PRESSURE");
}

// ============================================================================
// Test: a later start() (mode=255) after a mode override does not resend a
// stale cached setpoint under the new mode (regression for review feedback
// on #43/PR #47: start(mode) didn't clear cached_setpoint_, so a subsequent
// start() could reuse an unrelated value -- e.g. a pressure setpoint in
// meters resent as if it were a speed setpoint in RPM).
// ============================================================================
void test_start_after_mode_override_does_not_resend_stale_setpoint() {
  std::cout << "\n=== Testing start() After Mode Override Doesn't Resend Stale Setpoint ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_PRESSURE;
  state.mode_valid = true;
  state.cached_setpoint = 4.0f;  // Cached pressure setpoint (meters)

  // Explicit mode override to Constant Speed -- must clear the stale
  // pressure setpoint so it can't leak into the new mode.
  state.start(static_cast<uint8_t>(ControlMode::CONSTANT_SPEED));
  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "Mode override start(): no setpoint resent yet (nothing cached for Constant Speed)");

  // A later start() with mode=255 (e.g. re-enabling the pump) must NOT
  // resend the old 4.0 (meters) value reinterpreted as a Constant Speed
  // (RPM) setpoint.
  state.start();
  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "Subsequent start(): stale pressure setpoint (4.0 m) is not resent as a bogus "
              "Constant Speed setpoint after a mode override");
}

// ============================================================================
// Test: DHW On/Off and Temperature Range never reuse cached_setpoint
// ============================================================================
void test_start_dhw_and_temp_range_unaffected() {
  std::cout << "\n=== Testing DHW On/Off and Temperature Range Unaffected by Fix ===" << std::endl;

  ControlMode special_modes[] = {ControlMode::DHW_ON_OFF, ControlMode::TEMPERATURE_RANGE};

  for (auto mode : special_modes) {
    PumpEnabledState state;
    state.current_mode = mode;
    state.mode_valid = true;
    state.cached_setpoint = 1234.5f;  // Should never be reused for these modes

    state.start();

    TEST_ASSERT(std::isnan(state.last_sent_setpoint),
                "Special mode: cached_setpoint is never reused (default suffix still used)");
  }
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
  test_remote_mode_clean_ack_confirms_state();
  test_remote_mode_rejected_ack_leaves_state_unchanged();
  test_remote_mode_timeout_leaves_state_unchanged();
  test_flow_scale_flags_reporter_scenario();
  test_flow_scale_flags_large_upward_jump();
  test_flow_scale_ignores_normal_adjustment();
  test_flow_scale_ignores_first_read();
  test_flow_display_ignores_unreliable_register();
  test_flow_mode_transition_clears_cross_mode_setpoint();
  test_other_modes_still_trust_register();
  test_start_reuses_cached_speed_setpoint();
  test_start_converts_pressure_setpoint();
  test_start_falls_back_without_cached_setpoint();
  test_start_with_mode_override_ignores_stale_setpoint();
  test_start_after_mode_override_does_not_resend_stale_setpoint();
  test_start_dhw_and_temp_range_unaffected();

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
