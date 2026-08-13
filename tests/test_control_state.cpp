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
#include <functional>

// Test result tracking (same framework as test_protocol.cpp)
// NOTE: the notification-driven state tests that used to live here now drive
// the real ControlService in test_control_service.cpp. What remains asserts
// behaviour reachable only through ControlService's *private* wire primitives,
// which AGENTS §8.4 keeps private because WriteOperationService is the one
// write path. Those split two ways:
//
//   - send_control_request() / note_mode_commanded() are reached through the
//     operation layer, and test_write_operations.cpp already drives them
//     end-to-end against a pump simulator. The replicas here duplicate that.
//   - handle_remote_mode_ack() is not. There is no remote-mode WriteCommand --
//     ControlService::enable_remote_mode()/disable_remote_mode() call it
//     directly, one of the standalone write paths the audit flagged -- so the
//     remote-mode replicas below are the ONLY thing asserting it anywhere.
//     That is a real coverage gap, not a duplication: routing remote mode
//     through WriteOperationService would close both it and the architecture
//     violation.

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
 *
 * Updated for issue #51 / PR #57: the old single cached_setpoint field has
 * been replaced with four per-mode fields matching the production refactor.
 * Each mode reads and writes only its own slot, so a value set in one mode
 * can never influence start() under a different mode.
 */
struct PumpEnabledState {
  bool pump_enabled{false};
  bool pump_enabled_valid{false};
  ControlMode current_mode{ControlMode::NONE};
  bool mode_valid{false};

  // Mode-command coordination (issue #91): mirrors ControlService. A mode command
  // records the target as "pending"; readbacks keep the commanded mode until the
  // pump confirms it, so an in-flight/stale read can't overwrite it.
  ControlMode commanded_mode{ControlMode::NONE};
  bool mode_command_pending{false};
  int mode_confirm_attempts{0};
  static constexpr int MAX_MODE_CONFIRM_ATTEMPTS = 4;

  // Per-mode setpoint caches (issue #51): mirrors the four independent fields
  // in ControlService (cached_pressure_setpoint_, cached_proportional_setpoint_,
  // cached_speed_setpoint_, cached_flow_setpoint_). NAN = not yet written.
  float cached_pressure_setpoint{NAN};      // CONSTANT_PRESSURE: meters
  float cached_proportional_setpoint{NAN};  // PROPORTIONAL_PRESSURE: meters
  float cached_speed_setpoint{NAN};         // CONSTANT_SPEED: RPM
  float cached_flow_setpoint{NAN};          // CONSTANT_FLOW: m³/h

  // Records the setpoint actually sent to send_control_request() by the last
  // start() call, so tests can assert on it. NAN means "no setpoint was
  // passed" (i.e. send_control_request() fell back to the mode's default
  // suffix, reproducing the #43 hardcoded-3671 bug if left unfixed).
  float last_sent_setpoint{NAN};

  // Mock callback for schedule_callback_ to track if post-command readback
  // is scheduled (fixes #52)
  std::function<void(uint32_t)> mock_schedule_callback;

  // Mirrors ControlService::update_mode_from_notification() with the #91 guard:
  // a notification for the OLD mode does not overwrite an in-flight command.
  void update_from_notification(uint8_t mode, uint8_t operation_mode) {
    ControlMode reported = static_cast<ControlMode>(mode);
    if (!(mode_command_pending && reported != commanded_mode)) {
      mode_command_pending = false;
      current_mode = reported;
      mode_valid = true;
    }
    pump_enabled = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
    pump_enabled_valid = true;
  }

  // Mirrors ControlService::cache_setpoint_for_mode(): store into the mode's own
  // slot regardless of current_mode.
  void cache_setpoint_for_mode(ControlMode mode, float raw) {
    switch (mode) {
      case ControlMode::CONSTANT_PRESSURE:     cached_pressure_setpoint = raw / 9806.65f; break;
      case ControlMode::PROPORTIONAL_PRESSURE: cached_proportional_setpoint = raw / 9806.65f; break;
      case ControlMode::CONSTANT_FLOW:         cached_flow_setpoint = raw * 3600.0f; break;
      case ControlMode::CONSTANT_SPEED:        cached_speed_setpoint = raw; break;
      default: break;
    }
  }

  // Mirrors get_mode_async()'s response handler (issue #91): keep the commanded
  // mode while a command is pending and the pump still reports the old mode;
  // otherwise confirm/adopt the reported mode. Returns the mode the pump actually
  // reported (what get_mode_async passes to its callback, so sync can see it).
  ControlMode apply_readback(ControlMode reported, float raw_setpoint = NAN) {
    // The reported mode's setpoint is cached into its own slot regardless of the
    // pending guard, so bounded-retry recovery never lands on an unpopulated
    // setpoint (issue #91).
    if (!std::isnan(raw_setpoint)) {
      cache_setpoint_for_mode(reported, raw_setpoint);
    }
    if (mode_command_pending && reported != commanded_mode) {
      return reported;  // stale/in-flight: current_mode left untouched
    }
    mode_command_pending = false;
    mode_confirm_attempts = 0;
    current_mode = reported;
    mode_valid = true;
    return reported;
  }

  // Mirrors one pass of sync_cache_async(): capture expected, run the readback
  // guard, then the bounded-retry mismatch handler. Returns true if settled
  // (match or accepted), false if it scheduled another 2s retry.
  bool sync_pass(ControlMode reported) {
    ControlMode expected = mode_valid ? current_mode : ControlMode::NONE;
    ControlMode got = apply_readback(reported);
    if (expected != ControlMode::NONE && got != expected) {
      if (mode_command_pending && mode_confirm_attempts < MAX_MODE_CONFIRM_ATTEMPTS) {
        mode_confirm_attempts++;
        return false;  // retry in 2s
      }
      mode_command_pending = false;
      mode_confirm_attempts = 0;
      current_mode = reported;
      mode_valid = true;
    }
    return true;
  }

  // Mirrors ControlService::invalidate_cache() (called on disconnect): drops any
  // in-flight mode command so a next-connection read can't false-confirm it.
  void invalidate_cache() {
    mode_valid = false;
    pump_enabled_valid = false;
    mode_command_pending = false;
    mode_confirm_attempts = 0;
  }

  // Helper: return the cached setpoint for `target`, or NAN if none.
  // Mirrors the mode→field resolver in ControlService::start().
  float get_cached_for_mode(ControlMode target) const {
    switch (target) {
      case ControlMode::CONSTANT_PRESSURE:     return cached_pressure_setpoint;
      case ControlMode::PROPORTIONAL_PRESSURE: return cached_proportional_setpoint;
      case ControlMode::CONSTANT_SPEED:        return cached_speed_setpoint;
      case ControlMode::CONSTANT_FLOW:         return cached_flow_setpoint;
      default:                                 return NAN;
    }
  }

  // Mirrors ControlService::start() after the #43 fix and the #51 refactor.
  // Per-mode fields mean no NAN-clearing is needed when mode != 255 —
  // the new mode's slot has its own storage and a previous mode's value
  // can never contaminate it. Also schedules post-command readback (#52).
  bool start(uint8_t mode = 255) {
    if (mode != 255) {
      current_mode = static_cast<ControlMode>(mode);
      mode_valid = true;
      // Per-mode fields (issue #51): no NAN-clearing needed — each mode has
      // its own storage, so a previous mode's value can never contaminate.
    }

    ControlMode target = current_mode;

    // Reuse the per-mode cached setpoint when no explicit mode override was
    // given and the target mode is one of the four scalar-setpoint modes.
    // Issue #51: read from the mode-specific field — cross-mode contamination
    // is structurally impossible with per-mode storage.
    float start_setpoint = NAN;
    if (mode == 255) {
      float cached = get_cached_for_mode(target);
      if (!std::isnan(cached)) {
        start_setpoint = cached;
        if (target == ControlMode::CONSTANT_PRESSURE ||
            target == ControlMode::PROPORTIONAL_PRESSURE) {
          start_setpoint *= 9806.65f;
        }
      }
    }
    last_sent_setpoint = start_setpoint;

    pump_enabled = true;
    pump_enabled_valid = true;

    // Schedule post-command readback after ~500ms (fixes #52)
    if (mock_schedule_callback) {
      mock_schedule_callback(500);
    }

    return true;
  }

  // Mirrors ControlService::stop()
  // Also schedules post-command readback (fixes #52).
  bool stop(uint8_t mode = 255) {
    ControlMode target = current_mode;
    if (mode != 255) {
      target = static_cast<ControlMode>(mode);
    }

    float stop_setpoint = NAN;
    if (mode == 255) {
      float cached = get_cached_for_mode(target);
      if (!std::isnan(cached)) {
        stop_setpoint = cached;
        if (target == ControlMode::CONSTANT_PRESSURE ||
            target == ControlMode::PROPORTIONAL_PRESSURE) {
          stop_setpoint *= 9806.65f;
        }
      }
    }
    last_sent_setpoint = stop_setpoint;

    pump_enabled = false;
    pump_enabled_valid = true;

    // Schedule post-command readback after ~500ms (fixes #52)
    if (mock_schedule_callback) {
      mock_schedule_callback(500);
    }

    return true;
  }

  // Mirrors get_mode_async callback logic
  void update_from_mode_read(uint8_t operation_mode) {
    pump_enabled = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
    pump_enabled_valid = true;
  }

  // Mirrors ControlService::with_resolved_enabled_state() after review
  // feedback on PR #49: when the enabled state is genuinely unknown (even
  // after a simulated read-back attempt), the caller must abort the control
  // request entirely -- neither "true" nor "false" is safe to guess, since
  // "true" could force-enable a stopped pump (the original #45 bug) and
  // "false" could force-disable a running pump (just as bad, in reverse).
  struct EnabledStateResolution {
    bool resolved;
    bool enabled;
  };
  EnabledStateResolution resolve_enabled_state_for_control_request() const {
    if (pump_enabled_valid) {
      return {true, pump_enabled};
    }
    return {false, false};  // Unknown: caller must abort, not guess
  }

  // Mirrors ControlService::set_mode() after the #97/#83 fix: a mode change writes
  // overall_control_mode_local_request (GENI obj 86 / sub-id 10, wire Obj 0x0A01)
  // with operation_mode=NoCmd and set_point=NaN, so the pump changes ONLY the
  // control mode. It therefore:
  //   - never resolves or writes the enabled state (run state is preserved), so
  //     it never aborts on an unknown enabled state and never force-enables (#45);
  //   - never writes a setpoint, so each mode's stored setpoint is preserved and
  //     can no longer be clobbered by a default suffix (#83).
  // These flags let tests assert the mode change touched nothing but the mode.
  bool set_mode_wrote_setpoint{false};
  bool set_mode_wrote_enabled{false};
  bool set_mode(ControlMode mode) {
    // Intentionally does NOT touch pump_enabled or any stored setpoint.
    set_mode_wrote_setpoint = false;
    set_mode_wrote_enabled = false;
    // Issue #91: record the commanded mode as pending.
    commanded_mode = mode;
    mode_command_pending = true;
    mode_confirm_attempts = 0;
    current_mode = mode;
    mode_valid = true;
    return true;
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

  // Mirrors the control_source → is_remote_mode_enabled_ logic added in
  // ControlService::update_mode_from_notification() and the get_mode_async
  // callback (fix #53). Only updates on known values (2=Remote, 1=Local);
  // unknown values leave the current state unchanged.
  void update_remote_from_notification(uint8_t control_source) {
    if (control_source == 2) {
      is_remote_mode_enabled = true;   // Remote/Digital
    } else if (control_source == 1) {
      is_remote_mode_enabled = false;  // Local/Panel
    }
    // 0 or any other byte: leave state unchanged (conservative guard)
  }
};

/**
 * Mirrors the setpoint-caching branch of
 * ControlService::update_mode_from_notification() / get_mode_async() after
 * the #88 fix: CONSTANT_FLOW natively uses m³/s, so we must multiply by 3600.
 */
float resolve_cached_setpoint_from_pump_read(ControlMode mode, ControlMode /*previous_mode*/,
                                              float /*previous_cached*/, float raw_from_pump) {
  if (mode == ControlMode::CONSTANT_PRESSURE || mode == ControlMode::PROPORTIONAL_PRESSURE) {
    return raw_from_pump / 9806.65f;
  } else if (mode == ControlMode::CONSTANT_FLOW) {
    return raw_from_pump * 3600.0f;
  } else {
    return raw_from_pump;
  }
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
// Test: notification control_source supersedes previous ACK-based state (#53)
// A confirmed-remote state (via ACK) must be overwritten when the pump later
// reports control_source=1 (e.g. user presses local panel button).
// ============================================================================
void test_notification_control_source_overrides_ack_state() {
  std::cout << "\n=== Testing Notification control_source Supersedes ACK State (#53) ===" << std::endl;

  RemoteModeState state;
  // Simulate a successful enable-remote ACK.
  state.handle_ack(/*enabling=*/true, /*got_response=*/true, /*ack_byte=*/0x00);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, true,
                 "Precondition: ACK confirmed remote mode enabled");

  // Pump then sends a notification with control_source=1 (user pressed panel).
  state.update_remote_from_notification(/*control_source=*/1);
  TEST_ASSERT_EQ(state.is_remote_mode_enabled, false,
                 "#53: panel button press (control_source=1) correctly clears remote state");
}

// ============================================================================
// Test: setpoint/mode writes no longer force-enable the pump when off (#45)
// ============================================================================
void test_set_mode_does_not_force_enable_when_off() {
  std::cout << "\n=== Testing set_mode() Doesn't Force-Enable When Off (#45) ===" << std::endl;

  PumpEnabledState state;
  // Pump is known to be off.
  state.pump_enabled = false;
  state.pump_enabled_valid = true;

  state.set_mode(ControlMode::CONSTANT_PRESSURE);

  TEST_ASSERT_EQ(state.set_mode_wrote_enabled, false,
                 "#45: set_mode() does not write the enabled state (mode change is not start/stop)");
  TEST_ASSERT_EQ(state.pump_enabled, false,
                 "#45: pump_enabled stays false -- a mode change never force-enables the pump");
}

// ============================================================================
// Test: setpoint/mode writes preserve the enabled state when pump is on (#45)
// ============================================================================
void test_set_mode_preserves_enabled_when_on() {
  std::cout << "\n=== Testing set_mode() Preserves Enabled State When On (#45) ===" << std::endl;

  PumpEnabledState state;
  state.pump_enabled = true;
  state.pump_enabled_valid = true;

  state.set_mode(ControlMode::CONSTANT_SPEED);

  TEST_ASSERT_EQ(state.pump_enabled, true,
                 "#45: pump_enabled stays true -- a mode change preserves the running state");
  TEST_ASSERT_EQ(state.set_mode_wrote_enabled, false,
                 "#45: set_mode() leaves the enabled state untouched (operation_mode=NoCmd)");
}

// ============================================================================
// Test: unknown enabled state aborts the control request rather than
// guessing true or false (#45, hardened per review feedback on PR #49)
// ============================================================================
void test_resolve_enabled_state_aborts_when_unknown() {
  std::cout << "\n=== Testing Unknown Enabled State Aborts Rather Than Guessing (#45) ===" << std::endl;

  PumpEnabledState state;
  // pump_enabled_valid is false (never determined yet) -- simulates the
  // rare case where with_resolved_enabled_state()'s get_mode_async()
  // read-back also failed.
  TEST_ASSERT_EQ(state.pump_enabled_valid, false, "Precondition: enabled state is unknown");

  PumpEnabledState::EnabledStateResolution resolution = state.resolve_enabled_state_for_control_request();

  TEST_ASSERT_EQ(resolution.resolved, false,
                 "#45: unknown enabled state is reported as unresolved, not guessed as false");
}

// ============================================================================
// Test: set_mode() no longer depends on the enabled state. The new mode-change
// command (Obj 0x0A01, operation_mode=NoCmd) leaves the run state untouched, so
// it succeeds and changes the mode even when the enabled state is unknown --
// there is nothing to guess and nothing to force-enable/disable (#97/#45).
// ============================================================================
void test_set_mode_succeeds_when_enabled_state_unknown() {
  std::cout << "\n=== Testing set_mode() Succeeds When Enabled State Unknown (#97) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::TEMPERATURE_RANGE;
  state.mode_valid = true;
  // pump_enabled_valid left false: enabled state genuinely unknown.

  bool result = state.set_mode(ControlMode::CONSTANT_SPEED);

  TEST_ASSERT_EQ(result, true, "set_mode() succeeds without needing the enabled state");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_SPEED,
              "set_mode() changes the mode even when the enabled state is unknown");
  TEST_ASSERT_EQ(state.set_mode_wrote_enabled, false,
                 "set_mode() does not write the enabled state");
}

// ============================================================================
// Test: Constant Flow display keeps last commanded value in steady state,
// ignores bad register (#44 fix)
// ============================================================================
void test_flow_display_scales_correctly() {
  std::cout << "\n=== Testing Constant Flow Unit Scaling (#88 fix) ===" << std::endl;

  // The pump stores m³/s (0.000694), we should display 2.5 m³/h
  float resolved = resolve_cached_setpoint_from_pump_read(ControlMode::CONSTANT_FLOW, ControlMode::CONSTANT_FLOW,
                                                           NAN, 0.000694444f);
  // Allow a small epsilon for float precision
  TEST_ASSERT(std::abs(resolved - 2.5f) < 0.01f,
                 "#88: Constant Flow correctly scales from m³/s up to m³/h (0.000694 m³/s -> 2.5 m³/h)");
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
// Test: stop() reuses cached setpoint to prevent default overwrite (#67)
// ============================================================================
void test_stop_reuses_cached_setpoint() {
  std::cout << "\n=== Testing stop() Reuses Cached Setpoint (#67) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_PRESSURE;
  state.cached_pressure_setpoint = 4.5f; // 4.5m
  
  state.stop();
  // 4.5 * 9806.65 = 44129.925
  TEST_ASSERT(std::abs(state.last_sent_setpoint - 44129.925f) < 0.1f,
              "#67: stop() correctly passes the cached setpoint (in Pascals) for pressure modes");

  // Speed mode
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.cached_speed_setpoint = 2500.0f;
  state.stop();
  TEST_ASSERT_EQ(state.last_sent_setpoint, 2500.0f,
                 "#67: stop() correctly passes the cached setpoint for speed mode");
}

// ============================================================================
// Test: set_mode() does not write a setpoint at all -- the mode change goes to
// overall_control_mode_local_request (set_point ignored), so the pump keeps each
// mode's stored setpoint. This is the fix for #83: a mode change can no longer
// overwrite the stored value with a default (#67/#70/#83).
// ============================================================================
void test_set_mode_does_not_write_setpoint() {
  std::cout << "\n=== Testing set_mode() Does Not Write a Setpoint (#83) ===" << std::endl;

  PumpEnabledState state;
  state.pump_enabled_valid = true;
  state.pump_enabled = true;
  state.cached_flow_setpoint = 1.2f;

  state.set_mode(ControlMode::CONSTANT_FLOW);

  TEST_ASSERT_EQ(state.set_mode_wrote_setpoint, false,
                 "#83: set_mode() writes no setpoint (mode-change object ignores set_point)");
  TEST_ASSERT_EQ(state.cached_flow_setpoint, 1.2f,
                 "#83: set_mode() leaves the stored setpoint untouched (no clobber)");
}

// ============================================================================
// Test: set_mode() into a scalar mode with a cold (NaN) cache still succeeds and
// never clobbers. Regression guard for #97: v0.10.3 tried to read the setpoint
// from the pump first (Obj 86 Sub 13/15/39), which always timed out and aborted,
// making Constant Pressure/Proportional Pressure/Constant Flow (and Constant
// Speed when not current) unreachable. The mode change now goes to the
// control-mode object (set_point ignored), so it neither aborts nor clobbers.
// ============================================================================
void test_set_mode_uncached_scalar_mode_sends_not_aborts() {
  std::cout << "\n=== Testing set_mode() Un-cached Scalar Mode Succeeds, No Clobber (#97) ===" << std::endl;

  for (ControlMode mode : {ControlMode::CONSTANT_PRESSURE,
                           ControlMode::PROPORTIONAL_PRESSURE,
                           ControlMode::CONSTANT_SPEED,
                           ControlMode::CONSTANT_FLOW}) {
    PumpEnabledState state;
    state.pump_enabled_valid = true;
    state.pump_enabled = true;
    // All per-mode setpoint caches remain NAN (cold cache — the normal state
    // for any mode the pump is not currently in).

    bool queued = state.set_mode(mode);

    TEST_ASSERT_EQ(queued, true,
                   "#97: set_mode() into an un-cached scalar mode is queued (not rejected)");
    TEST_ASSERT_EQ(state.mode_valid, true,
                   "#97: set_mode() applies the new mode");
    TEST_ASSERT_EQ(state.set_mode_wrote_setpoint, false,
                   "#97: set_mode() writes no setpoint on a cold cache (no clobber, no default suffix)");
  }
}

// ============================================================================
// Issue #91: mode-command coordination — a readback that lands before the pump
// applies a mode command must not overwrite the commanded mode.
// ============================================================================
void test_mode_readback_stale_during_pending_is_ignored() {
  std::cout << "\n=== Testing Stale Readback During Pending Command Is Ignored (#91) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;

  // User switches to Constant Flow; the command is now pending confirmation.
  state.set_mode(ControlMode::CONSTANT_FLOW);
  TEST_ASSERT(state.mode_command_pending, "#91: set_mode marks the command pending");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_FLOW,
              "#91: set_mode optimistically shows the commanded mode");

  // The #54 poll (or a reconcile) reads the pump before it applied the command,
  // so it reports the OLD mode. This must NOT overwrite the commanded mode.
  ControlMode reported = state.apply_readback(ControlMode::CONSTANT_SPEED);
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_FLOW,
              "#91: a stale readback does not overwrite the commanded mode");
  TEST_ASSERT(state.mode_command_pending,
              "#91: the command stays pending until the pump confirms it");
  TEST_ASSERT(reported == ControlMode::CONSTANT_SPEED,
              "#91: the readback still reports the pump's actual mode to callers");
}

void test_mode_readback_caches_reported_setpoint_while_pending() {
  std::cout << "\n=== Testing Readback Caches Reported Mode's Setpoint While Pending (#91) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.set_mode(ControlMode::CONSTANT_FLOW);  // pending; pump still in Constant Speed

  // A stale readback reports the pump still in Constant Speed at 1800 RPM. Even
  // though current_mode is held at the commanded Constant Flow, the reported
  // mode's setpoint must be cached so that if the command is never applied and
  // sync recovery accepts Constant Speed, its setpoint is already populated.
  state.apply_readback(ControlMode::CONSTANT_SPEED, 1800.0f);

  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_FLOW,
              "#91: current mode stays commanded during pending");
  TEST_ASSERT_EQ(state.cached_speed_setpoint, 1800.0f,
                 "#91: the reported mode's setpoint is cached even while pending (recovery-safe)");
}

void test_mode_readback_confirms_command() {
  std::cout << "\n=== Testing Readback Confirms Commanded Mode (#91) ===" << std::endl;

  PumpEnabledState state;
  state.set_mode(ControlMode::CONSTANT_FLOW);

  // The pump now reports the commanded mode -> confirmed, pending cleared.
  state.apply_readback(ControlMode::CONSTANT_FLOW);
  TEST_ASSERT(!state.mode_command_pending, "#91: a matching readback clears pending");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_FLOW,
              "#91: current mode is the confirmed mode");
}

void test_mode_out_of_band_readback_is_adopted() {
  std::cout << "\n=== Testing Out-of-Band Readback Is Adopted (#54/#91) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  // No command pending: someone changed the mode on the pump directly.

  state.apply_readback(ControlMode::TEMPERATURE_RANGE);
  TEST_ASSERT(state.current_mode == ControlMode::TEMPERATURE_RANGE,
              "#54: with no command pending, a readback adopts the pump's mode");
  TEST_ASSERT(!state.mode_command_pending, "#54: no command was pending");
}

void test_mode_stuck_command_recovers_after_max_retries() {
  std::cout << "\n=== Testing Stuck Command Recovers After Max Retries (#91) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.set_mode(ControlMode::CONSTANT_FLOW);  // pending; pump will never apply it

  // Each sync pass reads the pump still in the old mode. The first
  // MAX_MODE_CONFIRM_ATTEMPTS passes retry (keeping the commanded mode); the next
  // one accepts the pump-reported mode so state reflects reality.
  for (int i = 0; i < PumpEnabledState::MAX_MODE_CONFIRM_ATTEMPTS; i++) {
    bool settled = state.sync_pass(ControlMode::CONSTANT_SPEED);
    TEST_ASSERT_EQ(settled, false, "#91: mismatch schedules a retry while under the cap");
    TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_FLOW,
                "#91: the commanded mode is held during retries");
  }
  bool settled = state.sync_pass(ControlMode::CONSTANT_SPEED);
  TEST_ASSERT_EQ(settled, true, "#91: retries are bounded — it stops after the cap");
  TEST_ASSERT(!state.mode_command_pending, "#91: pending is cleared on recovery");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_SPEED,
              "#91: after the cap, the pump-reported mode is accepted (no endless forcing)");
}

void test_mode_pending_cleared_on_disconnect() {
  std::cout << "\n=== Testing Disconnect Clears Pending Command (#91) ===" << std::endl;

  PumpEnabledState state;
  state.set_mode(ControlMode::CONSTANT_FLOW);
  TEST_ASSERT(state.mode_command_pending, "precondition: command pending");

  state.invalidate_cache();  // disconnect
  TEST_ASSERT(!state.mode_command_pending,
              "#91: a disconnect drops the in-flight command so a next-connection read can't false-confirm it");
}

// ============================================================================
// Issue #94: cycle-time config parsing and readiness independence.
// ============================================================================

// Mirrors ControlService::parse_cycle_time_minutes() (control_service.h): valid
// range 1-60 -> value; anything else (0, 0xFF "unset", out of range) -> -1.
static int8_t parse_cycle_time_minutes(uint8_t raw) {
  return (raw >= 1 && raw <= 60) ? static_cast<int8_t>(raw) : -1;
}

// Mirrors the post-#94 ControlService::is_cache_valid(): cycle-time fields are
// NOT part of the readiness gate.
static bool is_cache_valid_model(bool mode_valid, bool pump_enabled_valid,
                                 int8_t autoadapt, float temp_min, float temp_max) {
  return mode_valid && pump_enabled_valid && autoadapt != -1 &&
         !std::isnan(temp_min) && !std::isnan(temp_max);
}

void test_parse_cycle_time_minutes_range() {
  std::cout << "\n=== Testing Cycle-Time Byte Range Validation (#94) ===" << std::endl;

  // Valid values (1-60) are stored as-is.
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(1), 1, "#94: byte 1 -> 1");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(5), 5, "#94: byte 5 -> 5 (real pump ON)");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(15), 15, "#94: byte 15 -> 15 (real pump OFF)");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(60), 60, "#94: byte 60 -> 60");

  // Out-of-range bytes are explicitly treated as unknown (-1), not stored raw.
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(0), -1, "#94: byte 0 (unconfigured) -> unknown, not a bogus valid value");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(61), -1, "#94: byte 61 (above max) -> unknown");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(128), -1, "#94: byte 128 -> unknown (raw store would have been a negative minute)");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(254), -1, "#94: byte 254 -> unknown (raw store would have been negative)");
  TEST_ASSERT_EQ((int)parse_cycle_time_minutes(0xFF), -1, "#94: byte 0xFF -> explicitly unknown via range check (not by relying on signed truncation)");
}

void test_readiness_independent_of_cycle_time() {
  std::cout << "\n=== Testing Readiness No Longer Requires Cycle-Time Config (#94) ===" << std::endl;

  // The exact state that used to brick the component: everything the pump needs
  // is synced, but the cycle-time fields are still -1 (e.g. a short Object 91
  // payload). Readiness must now be TRUE.
  TEST_ASSERT(is_cache_valid_model(/*mode*/true, /*enabled*/true, /*autoadapt*/1, 35.0f, 39.7f),
              "#94: cache is valid with mode/enabled/autoadapt/temps set, regardless of cycle times");

  // Still gated on the fields that ARE required (autoadapt here).
  TEST_ASSERT(!is_cache_valid_model(true, true, /*autoadapt unknown*/-1, 35.0f, 39.7f),
              "#94: cache is still invalid when a required field (autoadapt) is unknown");
  TEST_ASSERT(!is_cache_valid_model(true, true, 1, NAN, 39.7f),
              "#94: cache is still invalid when temp_min is unknown");
}

// ============================================================================
// Test: start() reuses cached Constant Speed setpoint (fixes #43)
// ============================================================================
void test_start_reuses_cached_speed_setpoint() {
  std::cout << "\n=== Testing start() Reuses Cached Constant Speed Setpoint (#43) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.cached_speed_setpoint = 2000.0f;  // User configured 2000 RPM

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
  state.cached_pressure_setpoint = 4.0f;  // User configured 4.0 m

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
  // cached_speed_setpoint left as NAN (nothing read from pump yet)

  state.start();

  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "Fallback: no cached setpoint -> no setpoint override sent (unchanged behavior)");
}

// ============================================================================
// Test: explicit mode override uses only the new mode's own cache slot
// (issue #51: per-mode storage; no stale cross-mode contamination possible)
// ============================================================================
void test_start_with_mode_override_ignores_stale_setpoint() {
  std::cout << "\n=== Testing start(mode) Uses New Mode's Own Cache Slot (#51) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.mode_valid = true;
  state.cached_speed_setpoint = 2000.0f;     // Cached for CONSTANT_SPEED
  // Pressure slot is still NAN — no setpoint stored for this mode yet.

  // Switch to Constant Pressure via explicit override.
  state.start(static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE));

  // start(mode) goes through the mode == 255 branch? No: the explicit
  // override sets mode != 255, so start_setpoint stays NAN (we only
  // resolve the cache when restarting the *current* mode with mode=255).
  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "#51: mode override with empty pressure slot → no setpoint sent (no cross-mode leak)");
  TEST_ASSERT(state.current_mode == ControlMode::CONSTANT_PRESSURE,
              "Mode override: mode updated to CONSTANT_PRESSURE");
}

// ============================================================================
// Test: per-mode isolation — a later start(255) in Mode B does not resend
// any setpoint cached for Mode A (issue #51 core correctness assertion).
//
// This replaces the old NAN-clearing test: with per-mode storage the
// isolation is structural — Mode A's slot is a different field from Mode B's
// slot, so there is nothing to clear and nothing to leak.
// ============================================================================
void test_start_after_mode_override_does_not_resend_stale_setpoint() {
  std::cout << "\n=== Testing start() After Mode Override Uses Correct Per-Mode Cache (#51) ===" << std::endl;

  PumpEnabledState state;
  state.current_mode = ControlMode::CONSTANT_PRESSURE;
  state.mode_valid = true;
  state.cached_pressure_setpoint = 4.0f;   // Cached for CONSTANT_PRESSURE
  // Speed slot deliberately left NAN — nothing configured for Constant Speed.

  // Explicit mode override to Constant Speed.
  state.start(static_cast<uint8_t>(ControlMode::CONSTANT_SPEED));
  // start(mode != 255) skips the cache-reuse branch entirely.
  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "#51: mode override start() sends no setpoint when the new mode's slot is empty");

  // A subsequent start(255) must use only the Constant Speed slot, which is
  // still NAN — the 4.0 m pressure value must never appear here.
  state.start();
  TEST_ASSERT(std::isnan(state.last_sent_setpoint),
              "#51: subsequent start(255) does not reuse the old pressure setpoint under Constant Speed");
}

// ============================================================================
// Test: cross-mode isolation — Constant Speed setpoint does not affect
// Constant Pressure start() and vice versa (issue #51)
// ============================================================================
void test_start_per_mode_isolation_speed_vs_pressure() {
  std::cout << "\n=== Testing Per-Mode Cache Isolation: Speed vs Pressure (#51) ===" << std::endl;

  PumpEnabledState state;
  state.mode_valid = true;
  // Populate both per-mode slots independently.
  state.cached_speed_setpoint = 2000.0f;       // CONSTANT_SPEED: 2000 RPM
  state.cached_pressure_setpoint = 4.0f;       // CONSTANT_PRESSURE: 4.0 m

  // Restart as Constant Speed: should send exactly 2000 RPM.
  state.current_mode = ControlMode::CONSTANT_SPEED;
  state.start();
  TEST_ASSERT_EQ(state.last_sent_setpoint, 2000.0f,
                 "#51: CONSTANT_SPEED start() sends its own cached 2000 RPM, not pressure meters");

  // Restart as Constant Pressure: should send 4.0 m converted to Pa.
  state.current_mode = ControlMode::CONSTANT_PRESSURE;
  state.start();
  float expected_pa = 4.0f * 9806.65f;
  TEST_ASSERT(std::fabs(state.last_sent_setpoint - expected_pa) < 0.01f,
              "#51: CONSTANT_PRESSURE start() sends its own cached 4.0 m → Pa, not RPM");
}

// ============================================================================
// Test: cross-mode isolation — Proportional Pressure and Constant Flow each
// have their own independent slot (issue #51)
// ============================================================================
void test_start_per_mode_isolation_proportional_and_flow() {
  std::cout << "\n=== Testing Per-Mode Cache Isolation: Proportional Pressure and Constant Flow (#51) ===" << std::endl;

  PumpEnabledState state;
  state.mode_valid = true;
  state.cached_proportional_setpoint = 3.0f;  // PROPORTIONAL_PRESSURE: 3.0 m
  state.cached_flow_setpoint = 1.5f;           // CONSTANT_FLOW: 1.5 m³/h

  // Proportional Pressure should send 3.0 m converted to Pa.
  state.current_mode = ControlMode::PROPORTIONAL_PRESSURE;
  state.start();
  float expected_prop_pa = 3.0f * 9806.65f;
  TEST_ASSERT(std::fabs(state.last_sent_setpoint - expected_prop_pa) < 0.01f,
              "#51: PROPORTIONAL_PRESSURE start() sends its own 3.0 m → Pa, not flow value");

  // Constant Flow should send 1.5 m³/h (no Pa conversion).
  state.current_mode = ControlMode::CONSTANT_FLOW;
  state.start();
  TEST_ASSERT(std::fabs(state.last_sent_setpoint - 1.5f) < 0.001f,
              "#51: CONSTANT_FLOW start() sends its own 1.5 m³/h, not pressure meters");
}

// ============================================================================
// Test: DHW On/Off and Temperature Range never have cached setpoints
// ============================================================================
void test_start_dhw_and_temp_range_unaffected() {
  std::cout << "\n=== Testing DHW On/Off and Temperature Range Unaffected by Fix ===" << std::endl;

  ControlMode special_modes[] = {ControlMode::DHW_ON_OFF, ControlMode::TEMPERATURE_RANGE};

  for (auto mode : special_modes) {
    PumpEnabledState state;
    state.current_mode = mode;
    state.mode_valid = true;
    // Populate all scalar slots — none should ever surface for these modes.
    state.cached_pressure_setpoint = 1234.5f;
    state.cached_proportional_setpoint = 1234.5f;
    state.cached_speed_setpoint = 1234.5f;
    state.cached_flow_setpoint = 1234.5f;

    state.start();

    TEST_ASSERT(std::isnan(state.last_sent_setpoint),
                "Special mode: no scalar setpoint cache is reused (default suffix still used)");
  }
}

// ============================================================================
// Test: Post-command readback after start (fixes #52)
// ============================================================================
void test_start_schedules_readback() {
  std::cout << "\n=== Testing start() Schedules Post-Command Readback (#52) ===" << std::endl;

  PumpEnabledState state;
  
  // Mock the schedule callback tracking
  bool readback_scheduled = false;
  uint32_t readback_delay = 0;
  state.mock_schedule_callback = [&readback_scheduled, &readback_delay](uint32_t delay) {
    readback_scheduled = true;
    readback_delay = delay;
  };

  state.start();

  TEST_ASSERT(readback_scheduled, "start() schedules a post-command readback");
  TEST_ASSERT_EQ(readback_delay, 500,
                 "Post-command readback scheduled with ~500ms delay (per reporter's bench testing)");
}

// ============================================================================
// Test: Post-command readback after stop (fixes #52)
// ============================================================================
void test_stop_schedules_readback() {
  std::cout << "\n=== Testing stop() Schedules Post-Command Readback (#52) ===" << std::endl;

  PumpEnabledState state;
  
  // Mock the schedule callback tracking
  bool readback_scheduled = false;
  uint32_t readback_delay = 0;
  state.mock_schedule_callback = [&readback_scheduled, &readback_delay](uint32_t delay) {
    readback_scheduled = true;
    readback_delay = delay;
  };

  state.stop();

  TEST_ASSERT(readback_scheduled, "stop() schedules a post-command readback");
  TEST_ASSERT_EQ(readback_delay, 500,
                 "Post-command readback scheduled with ~500ms delay");
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
  // Issue #53: control_source-based remote state tracking
  test_notification_control_source_overrides_ack_state();
  test_set_mode_does_not_force_enable_when_off();
  test_set_mode_preserves_enabled_when_on();
  test_resolve_enabled_state_aborts_when_unknown();
  test_set_mode_succeeds_when_enabled_state_unknown();

  test_flow_display_scales_correctly();
  test_other_modes_still_trust_register();
  test_start_reuses_cached_speed_setpoint();
  test_start_converts_pressure_setpoint();
  test_start_falls_back_without_cached_setpoint();
  test_start_with_mode_override_ignores_stale_setpoint();
  test_start_after_mode_override_does_not_resend_stale_setpoint();
  // Issue #51: per-mode isolation assertions
  test_start_per_mode_isolation_speed_vs_pressure();
  test_start_per_mode_isolation_proportional_and_flow();
  test_start_dhw_and_temp_range_unaffected();
  test_stop_reuses_cached_setpoint();
  test_set_mode_does_not_write_setpoint();
  test_set_mode_uncached_scalar_mode_sends_not_aborts();
  // Issue #91: mode-command coordination
  test_mode_readback_stale_during_pending_is_ignored();
  test_mode_readback_caches_reported_setpoint_while_pending();
  test_mode_readback_confirms_command();
  test_mode_out_of_band_readback_is_adopted();
  test_mode_stuck_command_recovers_after_max_retries();
  test_mode_pending_cleared_on_disconnect();
  // Issue #94: cycle-time parsing + readiness independence
  test_parse_cycle_time_minutes_range();
  test_readiness_independent_of_cycle_time();
  test_start_schedules_readback();
  test_stop_schedules_readback();

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
