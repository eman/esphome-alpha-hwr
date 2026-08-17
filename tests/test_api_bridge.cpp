// Host tests for the Home Assistant surface: api_bridge.cpp.
//
// Why this file exists. api_bridge.cpp owns the entire public contract of the
// programmatic interface -- the service names Home Assistant lists, their
// argument lists, and every field of the `write_settled` event -- and until
// now NOTHING built it. It appeared in test_component_wiring's link line, but
// mocks/esphome/core/defines.h never defined USE_API, so api_bridge.h compiled
// the whole file out: the object it contributed had zero symbols. "The file is
// in a test target" was true and meaningless.
//
// setup()'s own comment named the consequence precisely:
//
//     "this file is compiled only against the real ESPHome API headers, so no
//      host test builds it and the mutation check has no target here. Pairing a
//      handler with the wrong enumerator below compiles, passes the whole suite
//      and passes the firmware build -- it surfaces only on a bench service
//      listing or in somebody's automation."
//
// That is the hazard these tests exist for, and the way to catch it is not to
// inspect the registration table but to CALL each service by the name it
// registered under and see which command comes back in the settle event. A
// handler paired with the wrong enumerator answers to the wrong name, and the
// event says so.
//
// The component is left unconnected on purpose. Every submitted operation then
// settles REJECTED at the ready check, which is a terminal event like any
// other -- enough to prove dispatch, argument parsing and the event contract
// without simulating a pump. What the pump does with an accepted write is
// test_write_operations.cpp's job, and it does not need this file's surface.

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../components/alpha_hwr/alpha_hwr.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/core/application.h"

uint32_t mock_millis = 0;

namespace esphome {
Application App;
}  // namespace esphome

using esphome::alpha_hwr::AlphaHwrComponent;
using esphome::api::mock_api_reset;
using esphome::api::mock_call_service;
using esphome::api::mock_fired_events;
using esphome::api::mock_registered_services;
using esphome::api::ServiceArg;
using esphome::ble_client::BLEClient;
using esphome::alpha_hwr::services::ControlMode;
using esphome::alpha_hwr::services::WriteCommand;
using esphome::alpha_hwr::services::WriteOrigin;
using esphome::alpha_hwr::services::WriteResult;
using esphome::alpha_hwr::services::WriteStatus;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  do {                                                                         \
    if (condition) {                                                           \
      tests_passed++;                                                          \
      std::cout << "[PASS] " << message << std::endl;                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      std::cout << "[FAIL] " << message << std::endl;                          \
    }                                                                          \
  } while (0)

static const char *const SETTLED = "esphome.alpha_hwr_write_settled";

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

/// A component with its bridge registered, and nothing connected. Constructed
/// once per test so the registration table and event log start empty.
struct BridgeHarness {
  BLEClient client;
  AlphaHwrComponent component{&client};

  BridgeHarness() {
    mock_api_reset();
    component.setup();
    // setup() itself may publish/settle nothing, but clear anything it did so
    // each test counts only its own events.
    mock_fired_events().clear();
  }

  size_t event_count() const { return mock_fired_events().size(); }

  /// The single settle event, or nullptr if there is not exactly one.
  const std::map<std::string, std::string> *only_event() const {
    if (mock_fired_events().size() != 1) return nullptr;
    if (mock_fired_events()[0].name != SETTLED) return nullptr;
    return &mock_fired_events()[0].data;
  }

  static std::string field(const std::map<std::string, std::string> &d, const std::string &k) {
    auto it = d.find(k);
    return it == d.end() ? std::string("<absent>") : it->second;
  }
  static bool has(const std::map<std::string, std::string> &d, const std::string &k) {
    return d.find(k) != d.end();
  }
};

/// Fire one event straight through the bridge, bypassing the operation layer.
/// Used for the field-mapping tests, where the question is purely what
/// fire_write_settled() puts in the map for a given WriteResult.
///
/// A standalone bridge is enough here: fire_write_settled() reads only the
/// result and App.get_name(), never component_, so no setup() is needed and
/// the test states exactly which WriteResult produced which map.
static const std::map<std::string, std::string> *fire(esphome::alpha_hwr::AlphaHwrApiBridge &bridge,
                                                      const WriteResult &r) {
  mock_fired_events().clear();
  bridge.fire_write_settled(r);
  if (mock_fired_events().size() != 1) return nullptr;
  return &mock_fired_events()[0].data;
}

// ---------------------------------------------------------------------------
// Registration surface
// ---------------------------------------------------------------------------

// The service list is public API twice over: the name Home Assistant shows,
// and the `command` its settle event reports. This pins the whole table --
// names AND argument lists -- so renaming a service or dropping an argument is
// a test failure rather than a surprise in somebody's automation.
static void test_the_registered_service_surface() {
  std::cout << "\n=== every service registers with the documented name and arguments ==="
            << std::endl;
  BridgeHarness h;

  struct Expected {
    const char *name;
    std::vector<std::string> args;
  };
  const std::vector<Expected> expected = {
      {"set_pump_enabled", {"enabled", "op_id"}},
      {"set_mode", {"mode", "op_id"}},
      {"set_setpoint", {"mode", "value", "op_id"}},
      {"set_temperature_range", {"min_c", "max_c", "autoadapt", "op_id"}},
      {"set_cycle_times", {"on_minutes", "off_minutes", "flow", "op_id"}},
      {"set_pump_state", {"state", "op_id"}},
      {"upload_schedule", {"data", "op_id"}},
      {"set_schedule_entry", {"data", "op_id"}},
      {"clear_schedule_entry", {"data", "op_id"}},
      {"set_schedule_enabled", {"data", "op_id"}},
      {"refresh_schedule", {"op_id"}},
      {"set_single_event", {"data", "op_id"}},
      {"clear_single_event", {"data", "op_id"}},
      {"refresh_single_events", {"op_id"}},
      {"set_vacation", {"data", "op_id"}},
      {"clear_vacation", {"op_id"}},
  };

  TEST_ASSERT(mock_registered_services().size() == expected.size(),
              "exactly 16 services are registered");

  for (const auto &want : expected) {
    const esphome::api::MockServiceRegistration *got = nullptr;
    for (const auto &reg : mock_registered_services()) {
      if (reg.name == want.name) { got = &reg; break; }
    }
    TEST_ASSERT(got != nullptr, std::string("service '") + want.name + "' is registered");
    if (got == nullptr) continue;
    TEST_ASSERT(got->args == want.args,
                std::string("service '") + want.name + "' takes the documented arguments");
  }

  // SET_CLOCK and SET_REMOTE_MODE are event-surface only. A service appearing
  // for either would be a new public API nobody documented.
  for (const auto &reg : mock_registered_services()) {
    TEST_ASSERT(reg.name != "set_clock" && reg.name != "set_remote_mode",
                std::string("'") + reg.name + "' is not one of the two event-only commands");
  }
}

// The hazard setup()'s comment names: a handler paired with the wrong
// enumerator. Calling by NAME and reading the command back off the event is
// what makes that visible -- the registration table alone cannot show it,
// because a mispaired entry still has a plausible name and argument list.
static void test_each_service_settles_as_the_command_it_is_named_for() {
  std::cout << "\n=== calling a service by name settles as that same command ===" << std::endl;

  struct Case {
    const char *service;
    const char *settles_as;
    std::vector<ServiceArg> args;
  };
  // Valid arguments throughout: the point is to reach the operation layer, not
  // to test parsing. Unconnected, each lands on the ready check.
  const std::vector<Case> cases = {
      {"set_pump_enabled", "set_pump_enabled", {true, std::string("t1")}},
      {"set_mode", "set_mode", {std::string("constant_speed"), std::string("t2")}},
      {"set_setpoint", "set_setpoint", {std::string("constant_speed"), 1500.0f, std::string("t3")}},
      {"set_temperature_range", "set_temperature_range", {35.0f, 45.0f, false, std::string("t4")}},
      {"set_cycle_times", "set_cycle_times", {5.0f, 15.0f, 1.0f, std::string("t5")}},
      {"upload_schedule", "upload_schedule", {std::string("v1,1;0,0,6,0,7,0"), std::string("t6")}},
      {"set_schedule_entry", "set_schedule_entry", {std::string("0,0,6,0,8,0"), std::string("t7")}},
      {"clear_schedule_entry", "clear_schedule_entry", {std::string("0,0"), std::string("t8")}},
      {"set_schedule_enabled", "set_schedule_enabled", {std::string("1"), std::string("t9")}},
      {"refresh_schedule", "refresh_schedule", {std::string("t10")}},
      {"set_single_event", "set_single_event", {std::string("2000000000,2000003600"), std::string("t11")}},
      {"clear_single_event", "clear_single_event", {std::string("0"), std::string("t12")}},
      {"refresh_single_events", "refresh_single_events", {std::string("t13")}},
      // The two documented exceptions: a vacation is a composition over the
      // single-event slots, not a command of its own, so it settles under the
      // single-event names. docs/programmatic-interface.md states this; here it
      // is pinned rather than trusted.
      {"set_vacation", "set_single_event", {std::string("2000000000,2000086400"), std::string("t14")}},
      {"clear_vacation", "clear_single_event", {std::string("t15")}},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    bool called = mock_call_service(c.service, c.args);
    TEST_ASSERT(called, std::string("service '") + c.service + "' exists and takes those arguments");
    if (!called) continue;

    const auto *ev = h.only_event();
    TEST_ASSERT(ev != nullptr,
                std::string("'") + c.service + "' fired exactly one terminal event");
    if (ev == nullptr) continue;
    TEST_ASSERT(BridgeHarness::field(*ev, "command") == c.settles_as,
                std::string("'") + c.service + "' settles as command '" + c.settles_as + "'");
    TEST_ASSERT(BridgeHarness::field(*ev, "origin") == "service",
                std::string("'") + c.service + "' reports origin service");
  }
}

// set_pump_state is composed at the bridge from two flag writes, so it is the
// one service whose aggregate event the operation layer never builds. Its
// op_id must still get exactly one terminal event.
static void test_set_pump_state_aggregates_to_one_event() {
  std::cout << "\n=== set_pump_state: one aggregate event for the caller's op_id ===" << std::endl;
  BridgeHarness h;

  TEST_ASSERT(mock_call_service("set_pump_state", {std::string("engaged"), std::string("ps1")}),
              "the service accepts state + op_id");

  int for_op = 0;
  for (const auto &e : mock_fired_events()) {
    if (e.name != SETTLED) continue;
    auto it = e.data.find("op_id");
    if (it != e.data.end() && it->second == "ps1") for_op++;
  }
  TEST_ASSERT(for_op == 1, "exactly one terminal event carries the caller's op_id");
}

// ---------------------------------------------------------------------------
// Bridge-level rejections
// ---------------------------------------------------------------------------

// Parse failures settle INVALID at the bridge, before the operation layer is
// involved -- so a client waiting on its op_id can never hang on a malformed
// call. These also pin the `seq: "0"` that identifies a bridge-built event.
static void test_unparsable_arguments_settle_invalid_at_the_bridge() {
  std::cout << "\n=== malformed arguments settle invalid, with seq 0 ===" << std::endl;

  struct Case {
    const char *service;
    const char *settles_as;
    std::vector<ServiceArg> args;
  };
  const std::vector<Case> cases = {
      {"set_schedule_entry", "set_schedule_entry", {std::string("not,a,schedule"), std::string("b1")}},
      {"clear_schedule_entry", "clear_schedule_entry", {std::string("garbage"), std::string("b2")}},
      {"set_single_event", "set_single_event", {std::string("nope"), std::string("b3")}},
      {"clear_single_event", "clear_single_event", {std::string("xyz"), std::string("b4")}},
      {"set_mode", "set_mode", {std::string("no_such_mode"), std::string("b5")}},
      {"set_pump_state", "set_pump_state", {std::string("sideways"), std::string("b6")}},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    TEST_ASSERT(mock_call_service(c.service, c.args),
                std::string("'") + c.service + "' accepts the call");
    const auto *ev = h.only_event();
    TEST_ASSERT(ev != nullptr,
                std::string("'") + c.service + "' fired exactly one terminal event");
    if (ev == nullptr) continue;
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "invalid",
                std::string("'") + c.service + "' settles invalid, not rejected");
    TEST_ASSERT(BridgeHarness::field(*ev, "command") == c.settles_as,
                std::string("'") + c.service + "' names its own command in the failure");
    // Documented in docs/programmatic-interface.md: real sequence numbers start
    // at 1, so "0" is what marks an event the bridge built itself.
    TEST_ASSERT(BridgeHarness::field(*ev, "seq") == "0",
                std::string("'") + c.service + "' reports seq 0 -- never queued");
  }
}

// ---------------------------------------------------------------------------
// Event field mapping
// ---------------------------------------------------------------------------

static void test_every_event_carries_the_common_fields() {
  std::cout << "\n=== op_id, command, status, origin, node and seq are always present ==="
            << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;
  WriteResult r;
  r.op_id = "c1";
  r.command = WriteCommand::SET_MODE;
  r.status = WriteStatus::ACCEPTED;
  r.origin = WriteOrigin::ENTITY;
  r.seq = 42;
  r.mode = ControlMode::CONSTANT_SPEED;

  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "op_id") == "c1", "op_id echoed");
  TEST_ASSERT(BridgeHarness::field(*ev, "command") == "set_mode", "command matches the enum string");
  TEST_ASSERT(BridgeHarness::field(*ev, "status") == "accepted", "status stringified");
  TEST_ASSERT(BridgeHarness::field(*ev, "origin") == "entity", "origin stringified");
  TEST_ASSERT(BridgeHarness::field(*ev, "seq") == "42", "seq echoed");
  TEST_ASSERT(BridgeHarness::has(*ev, "node"), "node is present on every event (issue #113)");
}

// `enabled` means the pump's RUN STATE on the control commands and the
// SCHEDULE flag on the schedule commands. The docs asserted the opposite for
// years ("on every other command it is the pump's run state"), and so did the
// comment in api_bridge.cpp. Pinned here so the two meanings stay deliberate.
static void test_enabled_carries_two_different_meanings() {
  std::cout << "\n=== `enabled` is run state on control commands, schedule flag on schedule ones ==="
            << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult run;
  run.command = WriteCommand::SET_PUMP_ENABLED;
  run.status = WriteStatus::ACCEPTED;
  run.enabled = 1;
  run.sched_enabled = 0;
  const auto *a = fire(bridge, run);
  TEST_ASSERT(a && BridgeHarness::field(*a, "enabled") == "true",
              "set_pump_enabled reports the run state in `enabled`");

  WriteResult sched;
  sched.command = WriteCommand::SET_SCHEDULE_ENTRY;
  sched.status = WriteStatus::ACCEPTED;
  sched.enabled = 0;         // run state -- deliberately the opposite value
  sched.sched_enabled = 1;   // schedule flag
  sched.layer = 2;
  sched.day = 3;
  const auto *b = fire(bridge, sched);
  TEST_ASSERT(b && BridgeHarness::field(*b, "enabled") == "true",
              "a schedule command reports the SCHEDULE flag in `enabled`, not the run state");
  TEST_ASSERT(b && BridgeHarness::field(*b, "day_name") == "Thursday", "day_name resolves from day");

  // And remote mode uses a third key rather than overloading `enabled` again.
  WriteResult remote;
  remote.command = WriteCommand::SET_REMOTE_MODE;
  remote.status = WriteStatus::ACCEPTED;
  remote.enabled = 1;
  const auto *c = fire(bridge, remote);
  TEST_ASSERT(c && BridgeHarness::field(*c, "remote_enabled") == "true",
              "set_remote_mode reports remote_enabled");
  TEST_ASSERT(c && !BridgeHarness::has(*c, "enabled"),
              "...and does NOT also emit `enabled`, which would mean two things at once");
}

// schedule_hash was emitted on every upload settle and documented nowhere as
// an event field -- the step-7 gap. Pinned so it cannot silently disappear
// again, together with the two layer masks and their format.
static void test_upload_carries_its_three_specific_fields() {
  std::cout << "\n=== upload_schedule emits layers_written, layers_skipped and schedule_hash ==="
            << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;
  WriteResult r;
  r.command = WriteCommand::UPLOAD_SCHEDULE;
  r.status = WriteStatus::PARTIAL;
  r.layers_written = "0,2";
  r.layers_skipped = "1";
  r.schedule_hash = "v1:0123456789abcdef";
  r.sched_enabled = 1;

  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "status") == "partial", "partial is a reportable status");
  TEST_ASSERT(BridgeHarness::field(*ev, "layers_written") == "0,2",
              "layers_written is a comma-separated index list");
  TEST_ASSERT(BridgeHarness::field(*ev, "layers_skipped") == "1", "layers_skipped likewise");
  TEST_ASSERT(BridgeHarness::field(*ev, "schedule_hash") == "v1:0123456789abcdef",
              "schedule_hash reaches the event, not only the sensor");
}

static void test_clock_offset_is_omitted_when_nothing_was_measured() {
  std::cout << "\n=== clock_offset_s is present when measured and absent when not ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult measured;
  measured.command = WriteCommand::SET_CLOCK;
  measured.status = WriteStatus::REJECTED;
  measured.clock_offset_s = -12.0f;
  const auto *a = fire(bridge, measured);
  TEST_ASSERT(a && BridgeHarness::field(*a, "clock_offset_s") == "-12",
              "a measured offset is reported");

  // NAN is the timeout case -- and, since a decoded readback settles the
  // operation on the spot, it is the ONLY thing a timeout can carry.
  WriteResult unmeasured;
  unmeasured.command = WriteCommand::SET_CLOCK;
  unmeasured.status = WriteStatus::TIMEOUT;
  unmeasured.clock_offset_s = NAN;
  const auto *b = fire(bridge, unmeasured);
  TEST_ASSERT(b && !BridgeHarness::has(*b, "clock_offset_s"),
              "an unmeasured offset emits no key rather than the string 'nan'");
}

static void test_pump_state_reports_both_flags_and_the_derived_name() {
  std::cout << "\n=== set_pump_state reports enabled, schedule_enabled and state ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;
  WriteResult r;
  r.command = WriteCommand::SET_PUMP_STATE;
  r.status = WriteStatus::ACCEPTED;
  r.enabled = 1;
  r.sched_enabled = 1;

  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "enabled") == "true", "run-state flag reported");
  TEST_ASSERT(BridgeHarness::field(*ev, "schedule_enabled") == "true", "schedule flag reported");
  TEST_ASSERT(BridgeHarness::field(*ev, "state") == "scheduled",
              "and the derived three-state name, so the event is self-contained");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  API Bridge Test Suite (issue #92 public surface)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_registered_service_surface();
  test_each_service_settles_as_the_command_it_is_named_for();
  test_set_pump_state_aggregates_to_one_event();
  test_unparsable_arguments_settle_invalid_at_the_bridge();
  test_every_event_carries_the_common_fields();
  test_enabled_carries_two_different_meanings();
  test_upload_carries_its_three_specific_fields();
  test_clock_offset_is_omitted_when_nothing_was_measured();
  test_pump_state_reports_both_flags_and_the_derived_name();

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
