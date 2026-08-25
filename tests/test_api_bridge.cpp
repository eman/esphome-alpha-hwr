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
// An adversarial pass applied all 98 same-arity handler mispairings by hand.
// 96 died; the two survivors were `set_single_event` <-> `set_vacation`, which
// settle under the SAME command string, so `command` could not tell them
// apart. That pair was never a technicality: a vacation is submitted as a STOP
// single event, so a `set_single_event` call bound to `on_set_vacation` turns
// the pump OFF across the caller's window instead of running it.
//
// Closed by giving the event an `event_type` ("run" / "stop"), which a client
// needed anyway -- until then nothing watching write_settled could tell "run
// the pump once at 6am" from "hold the pump off for a week". All 98 now die.
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
      {"set_flow_limiter", {"limiter", "enabled", "limit_gpm", "op_id"}},
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
              "exactly 17 services are registered");

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
    // "" when the command carries no event_type. For the two single-event
    // services this is what tells them apart: they settle under the SAME
    // command string, so `command` alone cannot see one bound to the other's
    // handler -- and the difference is not cosmetic, a vacation is a Stop
    // event that holds the pump OFF across the window.
    const char *event_type;
    std::vector<ServiceArg> args;
  };
  // Valid arguments throughout: the point is to reach the operation layer, not
  // to test parsing. Unconnected, each lands on the ready check.
  const std::vector<Case> cases = {
      {"set_pump_enabled", "set_pump_enabled", "", {true, std::string("t1")}},
      {"set_mode", "set_mode", "", {std::string("constant_speed"), std::string("t2")}},
      {"set_setpoint", "set_setpoint", "", {std::string("constant_speed"), 1500.0f, std::string("t3")}},
      {"set_temperature_range", "set_temperature_range", "", {35.0f, 45.0f, false, std::string("t4")}},
      {"set_cycle_times", "set_cycle_times", "", {5.0f, 15.0f, 1.0f, std::string("t5")}},
      {"set_flow_limiter", "set_flow_limiter", "",
       {std::string("maxflow"), true, 1.6f, std::string("t5b")}},
      {"upload_schedule", "upload_schedule", "", {std::string("v1,1;0,0,6,0,7,0"), std::string("t6")}},
      {"set_schedule_entry", "set_schedule_entry", "", {std::string("0,0,6,0,8,0"), std::string("t7")}},
      {"clear_schedule_entry", "clear_schedule_entry", "", {std::string("0,0"), std::string("t8")}},
      {"set_schedule_enabled", "set_schedule_enabled", "", {std::string("1"), std::string("t9")}},
      {"refresh_schedule", "refresh_schedule", "", {std::string("t10")}},
      {"set_single_event", "set_single_event", "run", {std::string("2000000000,2000003600"), std::string("t11")}},
      {"clear_single_event", "clear_single_event", "", {std::string("0"), std::string("t12")}},
      {"refresh_single_events", "refresh_single_events", "", {std::string("t13")}},
      // The two documented exceptions: a vacation is a composition over the
      // single-event slots, not a command of its own, so it settles under the
      // single-event names. docs/programmatic-interface.md states this; here it
      // is pinned rather than trusted.
      {"set_vacation", "set_single_event", "stop", {std::string("2000000000,2000086400"), std::string("t14")}},
      {"clear_vacation", "clear_single_event", "", {std::string("t15")}},
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
    // The comment above this test claims these arguments "reach the operation
    // layer". Without this assertion that was unverified, and three handlers
    // could be made to reject every valid input at the bridge with the suite
    // still green -- reject_() reports the same `command`. The ready check is
    // the operation layer's, so seeing it proves the bridge parsed and
    // submitted rather than refusing.
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "rejected",
                std::string("'") + c.service + "' reached the write layer's ready check");
    TEST_ASSERT(BridgeHarness::field(*ev, "detail") == "pump not connected/synchronized",
                std::string("'") + c.service + "' reports why, and `detail` is not empty");
    // The discriminator that closes the set_single_event <-> set_vacation
    // blind spot: both settle under one command, and only this says which.
    if (std::string(c.event_type).empty()) {
      TEST_ASSERT(!BridgeHarness::has(*ev, "event_type"),
                  std::string("'") + c.service + "' carries no event_type");
    } else {
      TEST_ASSERT(BridgeHarness::field(*ev, "event_type") == c.event_type,
                  std::string("'") + c.service + "' reports event_type '" + c.event_type + "'");
    }
  }
}

// set_pump_state is composed at the bridge from two flag writes, so it is the
// one service whose terminal event the operation layer never builds. Its op_id
// must still get exactly one.
//
// Scope, stated because an earlier name overclaimed it: unconnected, this
// never reaches the aggregation. submit_set_pump_state bails at check_ready and
// calls on_complete directly, so the Agg fan-in -- worst-severity selection,
// the pending counter, the leg events -- is NOT exercised here and a mutation
// of it survives. Covering that needs a ready component, which belongs with
// the write-operation tests.
static void test_set_pump_state_fires_one_event_when_it_never_reaches_the_pump() {
  std::cout << "\n=== set_pump_state: one terminal event for the caller's op_id ===" << std::endl;
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

// Everything below was found by a review pass that could only run once this
// file existed: api_bridge.cpp had never been compiled by a test, so its
// argument parsing had never had a machine over it.
//
// sscanf ignored trailing characters and wrapped on overflow, which did not
// merely lose the request -- it turned malformed input into a SUCCESSFUL write
// to a plausible-looking target. `clear_single_event` with "4294967296"
// wrapped to 0 and cleared slot 0. That is strictly worse than failing.
static void test_partially_numeric_arguments_are_rejected() {
  std::cout << "\n=== partially numeric arguments are rejected, not silently rounded ==="
            << std::endl;

  struct Case {
    const char *service;
    const char *data;
    const char *why;
  };
  const std::vector<Case> cases = {
      // Trailing garbage: sscanf stopped at the first bad character and kept
      // what it had.
      {"clear_single_event", "3.9", "a fractional slot is not slot 3"},
      {"clear_single_event", "1abc", "a number with a tail is not that number"},
      {"clear_single_event", " 7", "leading whitespace is not a valid slot"},
      {"clear_single_event", "0xJUNK", "hex-looking junk is not slot 0"},
      {"set_schedule_entry", "0,0,6,0,8,0GARBAGE", "a trailing tail invalidates the entry"},
      {"set_schedule_entry", "0,0,6,0,8,0,99,88", "extra fields invalidate the entry"},
      {"clear_schedule_entry", "0,0extra", "a trailing tail invalidates the target"},
      // Overflow: undefined behaviour that wrapped INTO the valid range, so
      // the range guard passed. "4294967296" cleared slot 0.
      {"clear_single_event", "4294967296", "an overflowing slot must not wrap to 0"},
      // Wide enough to set ERANGE even where long is 64-bit, so the overflow
      // guard itself is exercised rather than the range check standing in for it.
      {"clear_single_event", "99999999999999999999", "a value past LONG_MAX is refused"},
      {"set_schedule_entry", "99999999999999999999,0,6,0,8,0", "...in a csv field too"},
      {"set_schedule_entry", "4294967296,0,6,0,8,0", "an overflowing layer must not wrap to 0"},
      {"clear_schedule_entry", "4294967296,0", "an overflowing layer must not wrap to 0"},
      // "%lu" accepted a leading minus and negated it into a huge value.
      {"set_single_event", "-2,-1", "negative timestamps must not negate into 2106"},
      {"set_vacation", "-2,-1", "...for a vacation either"},
      // Ordered at 64-bit precision, reversed once narrowed to the wire's
      // 32 bits. The comparison now happens after narrowing.
      {"set_single_event", "4294967295,4294967296", "an out-of-range end must not truncate to 0"},
      // BOTH out of range. These narrow to 0 and 1 -- an ordered, plausible
      // pair -- so only the explicit width check refuses them; the
      // post-narrowing order comparison cannot.
      {"set_single_event", "4294967296,4294967297", "a pair past the wire's width is refused"},
      {"set_vacation", "4294967296,4294967297", "...for a vacation too"},
      // Both in range and reversed, so the width check passes them and only
      // the ordering rule can refuse. Without this the ordering comparison was
      // never reached by any case in this table.
      {"set_single_event", "2000,1000", "an end before its begin is refused"},
      {"set_single_event", "1000,1000", "a zero-length window is refused"},
      {"set_vacation", "2000,1000", "...for a vacation too"},
      // Empty and separator-only fields.
      {"clear_schedule_entry", "0,", "an empty field is not a zero"},
      {"clear_schedule_entry", ",0", "...in either position"},
      {"set_single_event", "1000,", "an empty end timestamp is not a zero"},
      // 0 is the wire's disabled/cleared sentinel, never shifted in either
      // direction, so an enabled event beginning at 0 confirmed clean while
      // describing a slot that says "cleared" (issue #263). This case used to
      // sit in the accepted table.
      {"set_single_event", "0,4294967295", "0 is the cleared sentinel, not a begin timestamp"},
      {"set_single_event", "1000,0", "...and not an end timestamp either"},
      {"set_vacation", "0,86400", "...for a vacation either"},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    TEST_ASSERT(mock_call_service(c.service, {std::string(c.data), std::string("p1")}),
                std::string("'") + c.service + "' accepts the call");
    const auto *ev = h.only_event();
    TEST_ASSERT(ev != nullptr, std::string("'") + c.data + "' fired exactly one terminal event");
    if (ev == nullptr) continue;
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "invalid",
                std::string(c.service) + " '" + c.data + "': " + c.why);
  }
}

// The counterpart: well-formed input must still be accepted, or the strictness
// above would be indistinguishable from rejecting everything.
//
// Read the epoch cases below in the light of HOW this file is built, because on
// a 64-bit host most of them are documentation and it took issue #255 to notice.
//
// `long` is 64 bits here and 32 on the ESP32-C3. The bound that broke #255
// narrowed only at the target's width, so every case below passed while the
// firmware refused all of them -- `0,4294967295` sat in this accepted list the
// whole time, both compiler legs green. The bug was not uncovered. It was
// covered by an assertion that could not fail.
//
// `make test-ilp32` is the answer to that, and CI runs it as "Unit tests
// (32-bit long)": this same file rebuilt with -m32, so `long` is 32 bits and
// these assertions mean at the target's width what they claim at the host's.
// Nothing here is written twice and nothing is a replica.
//
// Which leg earns which case is worth being exact about:
//
//   - At 64 bits, the three epoch cases added with #255 are documentation.
//     They pass identically against the broken code -- verified by restoring it
//     and running them. They are kept for what they say, not for what they
//     catch, and the pre-existing `0,4294967295` is what kills a mutation on
//     the epoch ceiling.
//   - At 32 bits they are load-bearing, and they divide the defect in two:
//     `0,4294967295` fails on the narrowed BOUND, and `2147483648,2147483649`
//     fails on the narrowed PARSE, where a 32-bit std::strtol saturates at
//     2147483647 and reports ERANGE. Both halves shipped; each needs its own
//     case.
static void test_well_formed_arguments_still_reach_the_operation_layer() {
  std::cout << "\n=== well-formed arguments are still accepted ===" << std::endl;

  struct Case {
    const char *service;
    const char *data;
  };
  const std::vector<Case> cases = {
      {"clear_single_event", "0"},
      {"clear_single_event", "99"},
      {"set_schedule_entry", "4,6,23,59,23,59"},
      {"set_schedule_entry", "0,0,0,0,0,0"},
      {"clear_schedule_entry", "4,6"},
      {"set_vacation", "1000,2000"},
      // Issue #255. On the device every one of these was refused, because the
      // parser's upper bound travelled as a `long` and 4294967295 narrowed to
      // -1 in the ESP32-C3's 32 bits, making `v > hi` reject every value >= 0.
      // A plausible present-day window is the case a user actually hits; the
      // two above 2147483647 are the ones a 32-bit `std::strtol` saturates on
      // and reports as ERANGE, which this parser treats as a rejection. The
      // wire holds a `uint32_t` (ClockProgramSingleEvent, object type 220), so
      // its last instant is in 2106 and none of these is out of range.
      {"set_single_event", "1798761600,1798761900"},
      {"set_single_event", "2147483648,2147483649"},
      {"set_vacation", "2200000000,2200086400"},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    TEST_ASSERT(mock_call_service(c.service, {std::string(c.data), std::string("p2")}),
                std::string("'") + c.service + "' accepts the call");
    const auto *ev = h.only_event();
    if (ev == nullptr) {
      TEST_ASSERT(false, std::string(c.data) + " fired exactly one terminal event");
      continue;
    }
    // Unconnected, so the operation layer refuses it -- but at the READY check,
    // which means the bridge parsed it and submitted it.
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "rejected",
                std::string(c.service) + " '" + c.data + "' parsed and reached the write layer");
  }
}

// Every reject_ caller echoes its argument into `detail`, and the event map is
// copied into an API message on a device with tens of KB of usable heap. An
// unbounded echo made an oversized service call the bridge's problem.
static void test_an_oversized_argument_is_not_echoed_whole() {
  std::cout << "\n=== a huge argument is truncated in `detail`, not echoed back ===" << std::endl;
  BridgeHarness h;
  const std::string huge(200000, 'x');

  TEST_ASSERT(mock_call_service("set_schedule_entry", {huge, std::string("p3")}),
              "the service accepts the call");
  const auto *ev = h.only_event();
  TEST_ASSERT(ev != nullptr, "exactly one terminal event");
  if (ev == nullptr) return;
  const std::string detail = BridgeHarness::field(*ev, "detail");
  TEST_ASSERT(BridgeHarness::field(*ev, "status") == "invalid", "still settles invalid");
  TEST_ASSERT(detail.size() < 200, "detail is bounded rather than 200 KB");
  TEST_ASSERT(detail.find("200000 chars") != std::string::npos,
              "and says how much was elided, so the caller can still diagnose it");
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
  // By value, not merely present: an empty node defeats the purpose (issue
  // #113 -- attributing an event to a controller in a multi-node install).
  TEST_ASSERT(BridgeHarness::field(*ev, "node") == std::string(esphome::App.get_name()),
              "node carries this controller's name, not an empty string");
  TEST_ASSERT(std::string(esphome::App.get_name()) != std::string(),
              "...and that name is non-empty to begin with");
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

// state_name(pump_auto, schedule_on) is ASYMMETRIC -- (true,false) is
// "engaged" and (false,true) is "off" -- so a fixture with both flags set to
// the same value cannot see its arguments swapped. An earlier version of this
// test used enabled=1, sched_enabled=1, and swapping the two arguments at the
// call site (a running unscheduled pump reporting "off") left the suite green.
// Both asymmetric combinations are covered here, plus the unknown case.
static void test_pump_state_reports_both_flags_and_the_derived_name() {
  std::cout << "\n=== set_pump_state reports enabled, schedule_enabled and state ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult engaged;
  engaged.command = WriteCommand::SET_PUMP_STATE;
  engaged.status = WriteStatus::ACCEPTED;
  engaged.enabled = 1;
  engaged.sched_enabled = 0;
  const auto *a = fire(bridge, engaged);
  TEST_ASSERT(a && BridgeHarness::field(*a, "enabled") == "true", "run-state flag reported");
  TEST_ASSERT(a && BridgeHarness::field(*a, "schedule_enabled") == "false",
              "schedule flag reported");
  TEST_ASSERT(a && BridgeHarness::field(*a, "state") == "engaged",
              "AUTO with the schedule off is 'engaged'");

  // The mirror image. If the two arguments were swapped at the call site this
  // one reports "engaged" and the one above reports "off".
  WriteResult off;
  off.command = WriteCommand::SET_PUMP_STATE;
  off.status = WriteStatus::ACCEPTED;
  off.enabled = 0;
  off.sched_enabled = 1;
  const auto *b = fire(bridge, off);
  TEST_ASSERT(b && BridgeHarness::field(*b, "state") == "off",
              "STOP with the schedule on is 'off', not 'scheduled'");

  WriteResult both;
  both.command = WriteCommand::SET_PUMP_STATE;
  both.status = WriteStatus::ACCEPTED;
  both.enabled = 1;
  both.sched_enabled = 1;
  const auto *c = fire(bridge, both);
  TEST_ASSERT(c && BridgeHarness::field(*c, "state") == "scheduled", "AUTO + schedule is 'scheduled'");

  // -1 is "not known". The guard must drop `state` entirely rather than let
  // -1 != 0 read as true and fabricate one.
  WriteResult unknown;
  unknown.command = WriteCommand::SET_PUMP_STATE;
  unknown.status = WriteStatus::REJECTED;
  unknown.enabled = -1;
  unknown.sched_enabled = -1;
  const auto *d = fire(bridge, unknown);
  TEST_ASSERT(d && !BridgeHarness::has(*d, "state"),
              "unknown flags emit no state rather than a fabricated one");

  // One known, one not: still not enough to name a state.
  WriteResult half;
  half.command = WriteCommand::SET_PUMP_STATE;
  half.status = WriteStatus::REJECTED;
  half.enabled = 1;
  half.sched_enabled = -1;
  const auto *e = fire(bridge, half);
  TEST_ASSERT(e && BridgeHarness::field(*e, "enabled") == "true", "the known half is reported");
  TEST_ASSERT(e && !BridgeHarness::has(*e, "state"), "but a half-known state is no state");
}

// An infinity is not a number a client can parse, and the suite already pins
// the contract for NaN: a key carries a real value or is absent. Neither
// submit_set_setpoint nor submit_set_temperature_range validates its floats, so
// `inf` reaches the event straight from a service call.
static void test_infinities_are_omitted_like_nan() {
  std::cout << "\n=== an infinite value emits no key rather than the word 'inf' ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult r;
  r.command = WriteCommand::SET_SETPOINT;
  r.status = WriteStatus::ACCEPTED;
  r.value = INFINITY;
  r.requested_value = -INFINITY;
  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(!BridgeHarness::has(*ev, "value"), "an infinite settled value emits no key");
  TEST_ASSERT(!BridgeHarness::has(*ev, "requested_value"),
              "nor does an infinite requested value");
}

// The two temperature echoes shared one guard, so a valid min with a NaN max
// emitted requested_temp_max: "nan".
static void test_paired_echo_keys_are_guarded_independently() {
  std::cout << "\n=== each requested_* key is guarded on its own value ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult r;
  r.command = WriteCommand::SET_TEMPERATURE_RANGE;
  r.status = WriteStatus::INVALID;
  r.requested_temp_min = 35.0f;
  r.requested_temp_max = NAN;
  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "requested_temp_min") == "35.0",
              "the valid half is reported");
  TEST_ASSERT(!BridgeHarness::has(*ev, "requested_temp_max"),
              "the NaN half emits no key rather than the string 'nan'");
}

// upload_schedule's three keys are omitted when unset, like every other key in
// that branch. An empty schedule_hash on a payload rejected before any wire
// work reads as "the upload ran and the grid hashes to nothing".
static void test_upload_keys_are_omitted_when_nothing_ran() {
  std::cout << "\n=== a rejected upload emits no empty layer/hash keys ===" << std::endl;
  esphome::alpha_hwr::AlphaHwrApiBridge bridge;

  WriteResult r;
  r.command = WriteCommand::UPLOAD_SCHEDULE;
  r.status = WriteStatus::INVALID;
  const auto *ev = fire(bridge, r);
  TEST_ASSERT(ev != nullptr, "one event fired");
  if (ev == nullptr) return;
  TEST_ASSERT(!BridgeHarness::has(*ev, "schedule_hash"), "no empty schedule_hash");
  TEST_ASSERT(!BridgeHarness::has(*ev, "layers_written"), "no empty layers_written");
  TEST_ASSERT(!BridgeHarness::has(*ev, "layers_skipped"), "no empty layers_skipped");
}

// The one that matters most. A set_pump_state call that never reached the pump
// used to report a CONCRETE state -- enabled: false, schedule_enabled: false,
// state: "off" -- for a pump nothing had been written to and whose caches the
// disconnect had invalidated. An automation reading `state` concluded the pump
// was off. The tri-state encoding and the `>= 0` guard both already existed;
// the callback just could not produce -1, so the guard was dead.
static void test_a_rejected_pump_state_does_not_assert_a_state() {
  std::cout << "\n=== set_pump_state on an unsynchronized pump reports no state at all ==="
            << std::endl;
  BridgeHarness h;

  TEST_ASSERT(mock_call_service("set_pump_state", {std::string("scheduled"), std::string("ps2")}),
              "the service accepts the call");
  const std::map<std::string, std::string> *ev = nullptr;
  for (const auto &e : mock_fired_events()) {
    if (e.name != SETTLED) continue;
    auto it = e.data.find("op_id");
    if (it != e.data.end() && it->second == "ps2") ev = &e.data;
  }
  TEST_ASSERT(ev != nullptr, "the caller's op_id gets its terminal event");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "status") == "rejected", "it settles rejected");
  TEST_ASSERT(!BridgeHarness::has(*ev, "state"),
              "and asserts NO state -- nothing was written and nothing was read");
  TEST_ASSERT(!BridgeHarness::has(*ev, "enabled"), "no run-state flag either");
  TEST_ASSERT(!BridgeHarness::has(*ev, "schedule_enabled"), "nor a schedule flag");
}

// ---------------------------------------------------------------------------
// The entity path settles too (issue #302)
// ---------------------------------------------------------------------------
//
// The two coupled switches used to bare-`return` on the readiness check, where
// the same write submitted through `set_pump_state` settled REJECTED. Nothing
// was written and nothing was said, so a client watching write_settled -- which
// the docs invite, since entity writes are described as going through the same
// verified path -- waited for an event that was never coming.
//
// Asserted through the ENTITY entry points rather than the service, because
// that is the path that was silent; the service arm above already covers the
// same refusal under an op_id.
static void test_a_switch_toggled_before_the_pump_is_ready_still_settles() {
  std::cout << "\n=== an entity toggle on an unsynchronized pump settles rejected ==="
            << std::endl;

  struct Case {
    const char *label;
    bool schedule_switch;  // false = Engage Pump, true = Schedule Enabled
    bool on;
  };
  const std::vector<Case> cases = {
      {"Engage Pump on", false, true},
      {"Engage Pump off", false, false},
      {"Schedule Enabled on", true, true},
      {"Schedule Enabled off", true, false},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    if (c.schedule_switch) {
      h.component.set_schedule(c.on);
    } else {
      h.component.set_engage_pump(c.on);
    }

    const auto *ev = h.only_event();
    TEST_ASSERT(ev != nullptr,
                std::string(c.label) + ": fires exactly one settle event");
    if (ev == nullptr) continue;
    TEST_ASSERT(BridgeHarness::field(*ev, "command") == "set_pump_state",
                std::string(c.label) + ": ...named set_pump_state, not one of the flag writes");
    TEST_ASSERT(BridgeHarness::field(*ev, "origin") == "entity",
                std::string(c.label) + ": ...reported as an entity write");
    TEST_ASSERT(BridgeHarness::field(*ev, "op_id").empty(),
                std::string(c.label) + ": ...with the empty op_id entity writes carry");
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "rejected",
                std::string(c.label) + ": ...settling rejected");
    // Same rule the service path is held to: nothing was written and the
    // caches are invalid, so the event must not name a state.
    TEST_ASSERT(!BridgeHarness::has(*ev, "state"),
                std::string(c.label) + ": ...and asserts no pump state");
  }
}

// Every OTHER entity write settles too (issue #302 follow-up)
//
// The coupled switches were the half issue #302 named. The rest of the entity
// surface had the same hole: each setter guards on check_ready() and used to
// return in silence, so a setpoint moved on a dashboard while the pump was
// still synchronizing produced a log line and no event, where the identical
// write through a service settles `rejected`.
//
// Every entry below is driven through the C++ entry point the YAML lambda in
// packages/alpha_hwr_controls.yaml calls, on an unconnected component -- which
// is precisely the state a user's node is in for the first seconds after boot,
// and after every reconnect.
//
// This table is also the executable copy of the entity-to-command mapping in
// docs/programmatic-interface.md ("Which entity settles as which command"). A
// client is told to match on `command`, so that mapping is public API: if you
// change which command an entity settles as, both this table and that one have
// to move together, and this one is the half that fails loudly.
static void test_every_entity_write_settles_when_the_pump_is_not_ready() {
  std::cout << "\n=== every entity write settles rather than returning in silence ==="
            << std::endl;

  struct Case {
    const char *label;
    const char *command;   // the `command` its settle event must report
    void (*run)(AlphaHwrComponent &);
  };
  static const std::vector<Case> cases = {
      {"Start", "set_pump_enabled", [](AlphaHwrComponent &c) { c.pump_start(); }},
      {"Stop", "set_pump_enabled", [](AlphaHwrComponent &c) { c.pump_stop(); }},
      {"Mode select", "set_mode",
       [](AlphaHwrComponent &c) { c.set_control_mode(ControlMode::CONSTANT_SPEED); }},
      {"Remote Mode on", "set_remote_mode", [](AlphaHwrComponent &c) { c.enable_remote(); }},
      {"Remote Mode off", "set_remote_mode", [](AlphaHwrComponent &c) { c.disable_remote(); }},
      {"Constant pressure setpoint", "set_setpoint",
       [](AlphaHwrComponent &c) { c.set_constant_pressure(3.0f, nullptr); }},
      {"Constant speed setpoint", "set_setpoint",
       [](AlphaHwrComponent &c) { c.set_constant_speed(2500.0f, nullptr); }},
      {"Constant flow setpoint", "set_setpoint",
       [](AlphaHwrComponent &c) { c.set_constant_flow(1.2f, nullptr); }},
      {"Proportional pressure setpoint", "set_setpoint",
       [](AlphaHwrComponent &c) { c.set_proportional_pressure(2.5f, nullptr); }},
      {"Temperature range", "set_temperature_range",
       [](AlphaHwrComponent &c) { c.set_temperature_range(35.0f, 38.9f, true, nullptr); }},
      {"Cycle times", "set_cycle_times",
       [](AlphaHwrComponent &c) { c.set_cycle_time_control(5, 10, nullptr); }},
      {"Cycle flow", "set_cycle_times",
       [](AlphaHwrComponent &c) { c.set_cycle_flow(1.4f, nullptr); }},
      {"Flow limiter", "set_flow_limiter",
       [](AlphaHwrComponent &c) {
         c.set_flow_limiter_from_entity(esphome::alpha_hwr::services::SUB_LIMITER_CONFIG_MAX_FLOW,
                                        true, 1.6f);
       }},
      {"Schedule entry save", "set_schedule_entry",
       [](AlphaHwrComponent &c) {
         esphome::alpha_hwr::ScheduleEntry e;
         c.set_schedule_entry(0, 2, e, nullptr);
       }},
      {"Schedule entry clear", "clear_schedule_entry",
       [](AlphaHwrComponent &c) { c.clear_schedule_entry_async(0, 2, nullptr); }},
      {"Schedule entry clear by name", "clear_schedule_entry",
       [](AlphaHwrComponent &c) { c.clear_schedule_entry("Monday", 0, nullptr); }},
  };

  for (const auto &c : cases) {
    BridgeHarness h;
    c.run(h.component);

    const auto *ev = h.only_event();
    TEST_ASSERT(ev != nullptr, std::string(c.label) + ": fires exactly one settle event");
    if (ev == nullptr) continue;
    TEST_ASSERT(BridgeHarness::field(*ev, "command") == c.command,
                std::string(c.label) + ": ...reporting command " + c.command);
    TEST_ASSERT(BridgeHarness::field(*ev, "origin") == "entity",
                std::string(c.label) + ": ...as an entity write");
    TEST_ASSERT(BridgeHarness::field(*ev, "status") == "rejected",
                std::string(c.label) + ": ...settling rejected, as the service does");
    TEST_ASSERT(BridgeHarness::field(*ev, "detail") == "pump not connected/synchronized",
                std::string(c.label) + ": ...with the detail the service path uses");
    TEST_ASSERT(BridgeHarness::field(*ev, "op_id").empty(),
                std::string(c.label) + ": ...and the empty op_id entity writes carry");
  }
}

// The settled value fields stay unknown on all of them. This is the rule the
// tri-state encoding exists for: nothing was written and nothing was read back,
// so an automation must not be able to read a pump state out of a refusal.
static void test_a_refused_entity_write_names_no_pump_state() {
  std::cout << "\n=== a refused entity write claims nothing about the pump ===" << std::endl;

  BridgeHarness h;
  h.component.set_constant_speed(2500.0f, nullptr);
  const auto *ev = h.only_event();
  TEST_ASSERT(ev != nullptr, "the setpoint write settles");
  if (ev == nullptr) return;
  TEST_ASSERT(!BridgeHarness::has(*ev, "value"), "no settled value");
  TEST_ASSERT(!BridgeHarness::has(*ev, "enabled"), "no settled enable flag");
  TEST_ASSERT(!BridgeHarness::has(*ev, "mode"), "no settled mode");
  // ...but the request IS echoed, because `set_setpoint` alone would not say
  // which of the five setpoint entities the user had just moved.
  TEST_ASSERT(BridgeHarness::field(*ev, "requested_mode") == "constant_speed",
              "the requested mode is echoed, naming the control");
  TEST_ASSERT(BridgeHarness::field(*ev, "requested_value") == "2500",
              "...and the value the user asked for");
}

// A deterministic refusal outranks the readiness check (review on #304).
//
// The two statuses mean different things to a client: `rejected` says "retry
// once the link is up", `invalid` says "this request can never succeed". So the
// order of the guards decides which one an unknown day name gets, and getting
// it wrong is not cosmetic -- on a disconnected pump, which is every node for
// the first seconds after boot, a misspelled day used to settle `rejected` and
// send an obedient client round a retry loop with no exit.
//
// This is the case the ready-pump test in test_component_wiring.cpp cannot
// reach: with the pump ready there is no readiness failure for the day check to
// lose to, so the ordering is only observable from here.
static void test_a_bad_day_name_outranks_the_readiness_check() {
  std::cout << "\n=== an unknown day name settles invalid even when the pump is also unready ==="
            << std::endl;

  BridgeHarness h;
  h.component.clear_schedule_entry("Blursday", 0, nullptr);

  const auto *ev = h.only_event();
  TEST_ASSERT(ev != nullptr, "it settles exactly once");
  if (ev == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev, "command") == "clear_schedule_entry",
              "...as clear_schedule_entry");
  TEST_ASSERT(BridgeHarness::field(*ev, "status") == "invalid",
              "...invalid, NOT rejected: no reconnect can make 'Blursday' a day");
  TEST_ASSERT(BridgeHarness::field(*ev, "detail") == "unknown day name",
              "...saying which half of the request was wrong");
  TEST_ASSERT(BridgeHarness::field(*ev, "origin") == "entity", "...and as an entity write");

  // The valid-day arm still reports the retryable condition, so the reorder
  // did not simply swap which case is wrong.
  BridgeHarness h2;
  h2.component.clear_schedule_entry("Monday", 0, nullptr);
  const auto *ev2 = h2.only_event();
  TEST_ASSERT(ev2 != nullptr, "a well-formed clear against an unready pump also settles");
  if (ev2 == nullptr) return;
  TEST_ASSERT(BridgeHarness::field(*ev2, "status") == "rejected",
              "...as rejected, which IS worth retrying once the link is up");
  TEST_ASSERT(BridgeHarness::field(*ev2, "day") == "0",
              "...and names the day, now that it is parsed before the refusal is built");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  API Bridge Test Suite (issue #92 public surface)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_registered_service_surface();
  test_each_service_settles_as_the_command_it_is_named_for();
  test_set_pump_state_fires_one_event_when_it_never_reaches_the_pump();
  test_unparsable_arguments_settle_invalid_at_the_bridge();
  test_every_event_carries_the_common_fields();
  test_enabled_carries_two_different_meanings();
  test_upload_carries_its_three_specific_fields();
  test_clock_offset_is_omitted_when_nothing_was_measured();
  test_pump_state_reports_both_flags_and_the_derived_name();
  test_partially_numeric_arguments_are_rejected();
  test_well_formed_arguments_still_reach_the_operation_layer();
  test_an_oversized_argument_is_not_echoed_whole();
  test_infinities_are_omitted_like_nan();
  test_paired_echo_keys_are_guarded_independently();
  test_upload_keys_are_omitted_when_nothing_ran();
  test_a_rejected_pump_state_does_not_assert_a_state();
  test_a_switch_toggled_before_the_pump_is_ready_still_settles();
  test_every_entity_write_settles_when_the_pump_is_not_ready();
  test_a_refused_entity_write_names_no_pump_state();
  test_a_bad_day_name_outranks_the_readiness_check();

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
