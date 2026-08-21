/**
 * ControlService state tests, driving the shipped service.
 *
 * These replace the equivalents in test_control_state.cpp, which asserted a
 * hand-written replica of ControlService's state machine. A replica passes no
 * matter what the firmware does, and the audit found this one had already
 * drifted: production caches a setpoint per mode and maintains the issue-#46
 * remote-mode state, and the replica did neither.
 *
 * Where the replica reached into its own fields, these assert the observable
 * behaviour instead:
 *
 * Scope: the notification-driven state surface, which ControlService exposes
 * publicly. The command primitives are private because AGENTS §8.4 makes
 * WriteOperationService the one write path and a `friend` of this class, and
 * they divide unevenly:
 *
 *   - send_control_request() and note_mode_commanded() are reached through the
 *     operation layer, and test_write_operations.cpp drives them end-to-end
 *     against a pump simulator.
 *   - send_remote_mode_command() likewise, since remote mode became a
 *     SET_REMOTE_MODE WriteCommand. It used to be the exception: two standalone
 *     enable_remote_mode()/disable_remote_mode() entry points talking to the
 *     transport directly, confirming from the command ACK, with no
 *     production-linked coverage anywhere.
 */

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../components/alpha_hwr/control_service.h"
#include "../components/alpha_hwr/session.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                    \
  do {                                            \
    if (cond) {                                   \
      tests_passed++;                             \
      std::cout << "[PASS] " << msg << std::endl; \
    } else {                                      \
      tests_failed++;                             \
      std::cout << "[FAIL] " << msg << std::endl; \
    }                                             \
  } while (0)

using esphome::alpha_hwr::core::Session;
using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::services::ControlMode;
using esphome::alpha_hwr::services::ControlService;
using esphome::alpha_hwr::services::control_readback_changed;

namespace {

// Operation-mode byte as the pump reports it in a passive notification.
// Derived from the production enum, not restated. Writing the numbers here
// would reintroduce the replica problem in miniature: renumber USER_DEFINED and
// this test would keep sending a stale 4 and still pass, because the
// implementation only special-cases STOP.
using esphome::alpha_hwr::services::OperationMode;
constexpr uint8_t OP_AUTO = static_cast<uint8_t>(OperationMode::AUTO);
constexpr uint8_t OP_STOP = static_cast<uint8_t>(OperationMode::STOP);
constexpr uint8_t OP_USER_DEFINED = static_cast<uint8_t>(OperationMode::USER_DEFINED);

/// Control-source byte: 2 = Remote/Digital, 1 = Local/Panel, 0 = unknown.
constexpr uint8_t SRC_REMOTE = 2;
constexpr uint8_t SRC_LOCAL = 1;
constexpr uint8_t SRC_UNKNOWN = 0;

struct Rig {
  Transport transport;
  Session session;
  ControlService control;
  Rig() : control(transport, session) {
    transport.set_write_callback([](const uint8_t *, size_t) { return true; });
    session.on_ready();
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Initial state
// ---------------------------------------------------------------------------
static void test_initial_state() {
  std::cout << "\n=== Initial state ===" << std::endl;
  Rig rig;
  TEST_ASSERT(!rig.control.is_pump_enabled(), "Initial: pump not enabled");
  TEST_ASSERT(!rig.control.is_pump_enabled_valid(),
              "Initial: enabled state not yet known");
  TEST_ASSERT(!rig.control.is_mode_valid(), "Initial: mode not valid");
  TEST_ASSERT(rig.control.get_current_mode() == ControlMode::NONE,
              "Initial: mode is NONE");
  TEST_ASSERT(!rig.control.get_remote_enabled(),
              "Initial: remote mode disabled");
}

// ---------------------------------------------------------------------------
// Passive notifications drive mode and enabled state
// ---------------------------------------------------------------------------
static void test_notification_operation_modes() {
  std::cout << "\n=== Notification: operation modes ===" << std::endl;

  {
    Rig rig;
    rig.control.update_mode_from_notification(
        static_cast<uint8_t>(ControlMode::TEMPERATURE_RANGE), OP_AUTO, NAN);
    TEST_ASSERT(rig.control.is_pump_enabled(), "AUTO: pump enabled");
    TEST_ASSERT(rig.control.is_pump_enabled_valid(), "AUTO: state valid");
    TEST_ASSERT(rig.control.get_current_mode() == ControlMode::TEMPERATURE_RANGE,
                "AUTO: mode is TEMPERATURE_RANGE");
    TEST_ASSERT(rig.control.is_mode_valid(), "AUTO: mode valid");
  }
  {
    Rig rig;
    rig.control.update_mode_from_notification(
        static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_STOP, NAN);
    TEST_ASSERT(!rig.control.is_pump_enabled(), "STOP: pump disabled");
    TEST_ASSERT(rig.control.is_pump_enabled_valid(), "STOP: state valid");
  }
  {
    Rig rig;
    rig.control.update_mode_from_notification(
        static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE), OP_USER_DEFINED,
        NAN);
    TEST_ASSERT(rig.control.is_pump_enabled(), "USER_DEFINED: pump enabled");
  }
}

// ---------------------------------------------------------------------------
// control_source in a notification drives the issue-#46 remote-mode state.
// This is the authoritative path: the ACK-derived one is a private primitive
// and is covered through the operation layer (see the header comment).
// ---------------------------------------------------------------------------
static void test_control_source_drives_remote_state() {
  std::cout << "\n=== Remote mode from control_source ===" << std::endl;
  Rig rig;

  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_AUTO, NAN, SRC_REMOTE);
  TEST_ASSERT(rig.control.get_remote_enabled(),
              "control_source 2 (Remote) sets remote enabled");

  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_AUTO, NAN, SRC_LOCAL);
  TEST_ASSERT(!rig.control.get_remote_enabled(),
              "control_source 1 (Local) clears remote enabled");

  // An unrecognised source carries no information and must not clobber state:
  // re-assert Remote, then send an unknown source and require it to hold.
  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_AUTO, NAN, SRC_REMOTE);
  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_AUTO, NAN, SRC_UNKNOWN);
  TEST_ASSERT(rig.control.get_remote_enabled(),
              "control_source 0 (unknown) leaves remote state unchanged");
}

// ---------------------------------------------------------------------------
// Issue #51: each mode keeps its own setpoint
// ---------------------------------------------------------------------------
static void test_setpoint_cached_per_mode() {
  std::cout << "\n=== Per-mode setpoint cache (#51) ===" << std::endl;
  Rig rig;

  // A notification carries the setpoint for the mode it reports. Pressure is
  // reported in Pa on the wire and cached in metres.
  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_SPEED), OP_AUTO, 2400.0f);
  TEST_ASSERT(std::fabs(rig.control.get_cached_speed_setpoint() - 2400.0f) < 0.5f,
              "#51: speed setpoint cached for CONSTANT_SPEED");

  rig.control.update_mode_from_notification(
      static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE), OP_AUTO, 29419.95f);
  TEST_ASSERT(std::fabs(rig.control.get_cached_pressure_setpoint() - 3.0f) < 0.01f,
              "#51: pressure setpoint cached in metres for CONSTANT_PRESSURE");

  // Switching modes must not disturb the other mode's slot.
  TEST_ASSERT(std::fabs(rig.control.get_cached_speed_setpoint() - 2400.0f) < 0.5f,
              "#51: the speed slot survives a mode change");
}

// ---------------------------------------------------------------------------
// Issue #43: the setpoint on the wire is the one asked for, not a default

// ---------------------------------------------------------------------------
// Issue #265: the readback log fires at INFO only when something moved
// ---------------------------------------------------------------------------
//
// The predicate rather than the log line, because the mock ESP_LOGx macros
// compile to nothing on the host -- there is no log level to observe. What is
// testable, and what the defect actually was, is the decision behind it.
static void test_readback_change_gate() {
  std::cout << "\n=== Readback change gate (#265) ===" << std::endl;

  // The steady state the issue describes: the #54 poll re-reading the same mode
  // and the same setpoint at 30 s intervals, indefinitely. This is the case that
  // produced ~2,880 identical INFO lines a day.
  TEST_ASSERT(!control_readback_changed(true, ControlMode::CONSTANT_SPEED,
                                        ControlMode::CONSTANT_SPEED, 1650.0f, 1650.0f),
              "#265: an unchanged poll does not count as a change");

  // A real transition still does.
  TEST_ASSERT(control_readback_changed(true, ControlMode::CONSTANT_SPEED,
                                       ControlMode::CONSTANT_PRESSURE, 1650.0f, 3.0f),
              "#265: a mode change counts");
  TEST_ASSERT(control_readback_changed(true, ControlMode::CONSTANT_SPEED,
                                       ControlMode::CONSTANT_SPEED, 1650.0f, 1800.0f),
              "#265: a setpoint change in the same mode counts");

  // The first reading after a connect. mode_valid_ is false until the pump has
  // been read once, so this is what keeps the state visible at INFO once per
  // link instead of being gated away against a cache nothing populated.
  TEST_ASSERT(control_readback_changed(false, ControlMode::NONE,
                                       ControlMode::CONSTANT_SPEED, NAN, 1650.0f),
              "#265: the first reading after a connect counts as a change");

  // NAN is a value here, not a missing one. get_setpoint_for_mode() returns it
  // for every mode with no scalar setpoint, and NAN != NAN -- so a plain `!=`
  // would report a change on every poll of exactly those modes and reinstate the
  // defect for a pump sitting in AutoAdapt or temperature-range control.
  TEST_ASSERT(!control_readback_changed(true, ControlMode::AUTO_ADAPT,
                                        ControlMode::AUTO_ADAPT, NAN, NAN),
              "#265: NAN == NAN for this comparison; a setpointless mode is quiet");
  TEST_ASSERT(control_readback_changed(true, ControlMode::TEMPERATURE_RANGE,
                                       ControlMode::TEMPERATURE_RANGE, NAN, 3.0f),
              "#265: NAN -> a value is a change");
  TEST_ASSERT(control_readback_changed(true, ControlMode::CONSTANT_SPEED,
                                       ControlMode::CONSTANT_SPEED, 1650.0f, NAN),
              "#265: a value -> NAN is a change");

  // Same mode, same setpoint, but no prior reading: the had_prior_mode term has
  // to win over the two equalities, or a reconnect onto an unchanged pump goes
  // unreported.
  TEST_ASSERT(control_readback_changed(false, ControlMode::CONSTANT_SPEED,
                                       ControlMode::CONSTANT_SPEED, 1650.0f, 1650.0f),
              "#265: no prior reading outranks an unchanged comparison");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "ControlService State Tests (real service)" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_initial_state();
  test_notification_operation_modes();
  test_control_source_drives_remote_state();
  test_setpoint_cached_per_mode();
  test_readback_change_gate();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
