// Host tests for DhwDemandComponent itself — the ESPHome shell, not the pure
// predicates in dhw_demand_logic.h.
//
// Why this file exists. Until it did, `esphome compile` was the only thing in
// the entire toolchain that compiled dhw_demand.cpp: 538 lines carrying the
// sensor-callback wiring, the branch that selects between the pump-ON and
// pump-OFF paths, the flow latch, the derivative helper and every publish. The
// unit suite, cppcheck and the mutation check all read straight past it. That
// gap is not hypothetical — while fixing issue #149's test coverage the .cpp
// was left with a block referencing an out-of-scope variable and the 905-test
// suite plus cppcheck both reported green; only the firmware build caught it.
//
// So this drives the real component against tests/mocks: setters, setup(), then
// update() on an injected clock, asserting on what the output entities actually
// received. Everything asserted here is behaviour that lives in the .cpp and
// nowhere else — the pure predicates have their own file
// (test_dhw_demand_logic.cpp) and are not re-tested through this slower door.
//
// The clock is mock_millis, which the component reads through millis() both in
// update() and inside the sensor state callbacks, so the order matters: a test
// sets the time, publishes its inputs (stamping their freshness registers at
// that time), then ticks.

#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/hal.h"

#include "../components/dhw_demand/dhw_demand.h"

uint32_t mock_millis = 0;

using esphome::dhw_demand::DhwDemandComponent;

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

// A nonzero epoch for every scenario. 0 is the "never happened" sentinel for
// pump_on_since_ms_, pump_off_since_ms_ and every freshness register, so a test
// starting at t=0 would silently exercise the abstain paths instead of the ones
// it meant to.
static const uint32_t T0 = 100000;
static const uint32_t TICK_MS = 10000;

// The component plus the twelve entities it talks to, wired the way the codegen
// wires them. Inputs are left unpublished until a test publishes them, which is
// the real boot condition: has_state() false, so read_sensor_() returns NaN.
struct Rig {
  esphome::sensor::Sensor motor_speed, motor_current, pump_flow, flow;
  esphome::sensor::Sensor tank_lower_temp, dhw_charge, dhw_in_use;

  esphome::binary_sensor::BinarySensor demand;
  esphome::sensor::Sensor confidence, demand_level, session_duration;
  esphome::text_sensor::TextSensor method;

  DhwDemandComponent det;

  Rig() {
    det.set_motor_speed_sensor(&motor_speed);
    det.set_motor_current_sensor(&motor_current);
    det.set_pump_flow_sensor(&pump_flow);
    det.set_flow_sensor(&flow);
    det.set_tank_lower_temp_sensor(&tank_lower_temp);
    det.set_dhw_charge_sensor(&dhw_charge);
    det.set_dhw_in_use_sensor(&dhw_in_use);

    det.set_demand_sensor(&demand);
    det.set_confidence_sensor(&confidence);
    det.set_demand_level_sensor(&demand_level);
    det.set_session_duration_sensor(&session_duration);
    det.set_detection_method_sensor(&method);

    det.set_update_interval(TICK_MS);
  }

  /// Advance the clock without ticking, so a test can publish inputs at a
  /// chosen instant and have them stamped there.
  void at(uint32_t now) { mock_millis = now; }
  void tick(uint32_t now) {
    mock_millis = now;
    det.update();
  }

  const std::string &method_state() const { return method.state; }
};

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// ── 1. Idle steady state ─────────────────────────────────────────────────────
// The #129 regression, end to end: the four step-valued outputs are re-derived
// every tick from unchanged inputs, and must publish once and then go quiet.
// The gates are unit-tested in test_dhw_publish_gate.cpp; what is only testable
// here is that publish_result_() and update_session_() actually route through
// them.
void test_idle_publishes_once() {
  std::cout << "\n=== Idle steady state publishes once, then goes quiet ==="
            << std::endl;
  Rig r;
  r.det.setup();

  for (int i = 0; i < 10; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);  // pump confirmed off
    r.flow.publish_state(0.0f);
    r.tick(now);
  }

  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "Idle pump-off ticks report deterministic_idle");
  TEST_ASSERT(r.demand.state == false, "No demand while idle");
  TEST_ASSERT(near(r.confidence.state, 100.0f),
              "Idle confidence is 100% (confident there is no draw)");

  TEST_ASSERT(r.confidence.publish_count == 1,
              "Confidence published once across 10 identical ticks");
  TEST_ASSERT(r.demand_level.publish_count == 1,
              "Demand level published once across 10 identical ticks");
  TEST_ASSERT(r.method.publish_count == 1,
              "Detection method published once across 10 identical ticks");
  TEST_ASSERT(r.session_duration.publish_count == 1,
              "Session duration published once across 10 idle ticks");
  TEST_ASSERT(r.demand.publish_count == 1,
              "Demand binary published once (BinarySensor dedups its own)");
}

// ── 2. Flow onset debounce ───────────────────────────────────────────────────
// A first tick of flow is ambiguous; the component must spend one tick on
// flow_onset_pending before it will call a draw. Only reachable through
// detect_pump_off_(), which lives in the .cpp.
void test_flow_onset_takes_two_ticks() {
  std::cout << "\n=== Pump-off flow onset needs two ticks ===" << std::endl;
  Rig r;
  r.det.setup();

  r.at(T0);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(1.5f);
  r.tick(T0);

  TEST_ASSERT(r.method_state() == "flow_onset_pending",
              "First tick of flow is pending, not a draw");
  TEST_ASSERT(r.demand.state == false, "No demand on the ambiguous first tick");
  TEST_ASSERT(near(r.confidence.state, 50.0f),
              "Ambiguous onset publishes 50% confidence");

  r.at(T0 + TICK_MS);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(1.5f);
  r.tick(T0 + TICK_MS);

  TEST_ASSERT(r.method_state() == "deterministic_flow",
              "Second consecutive pump-off flow tick confirms the draw");
  TEST_ASSERT(r.demand.state == true, "Demand raised on the confirming tick");
  TEST_ASSERT(near(r.confidence.state, 100.0f),
              "Confirmed flow is a 100% confidence signal");
  TEST_ASSERT(near(r.demand_level.state, 0.6f),
              "Demand level scales with flow (1.5 / 2.5)");
}

// ── 3. Frozen motor channel ──────────────────────────────────────────────────
// The issue #149 failure, reproduced on the host in microseconds instead of on
// the bench in minutes. The motor sensor keeps its last value forever when the
// BLE link drops -- alpha_hwr does not publish NaN -- so a frozen 0 RPM would
// read as a confirmed-off pump and hand the loop's own recirculation to
// deterministic_flow at confidence 1.0.
//
// The second half is the part that makes this an assertion about staleness
// rather than about anything else: republishing the *same* 0 RPM, fresh, gets
// deterministic_flow within two ticks. Only the age of the reading differs.
void test_frozen_motor_never_enters_pump_off_branch() {
  std::cout << "\n=== A frozen motor reading never opens the pump-off branch ==="
            << std::endl;
  Rig r;
  r.det.setup();

  // Pump running, both flow channels reporting.
  r.at(T0);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(2.1f);
  r.flow.publish_state(2.2f);
  r.tick(T0);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "Pump-on with a quiet loop claims nothing");

  // The link drops: the motor channel stops refreshing but keeps its value.
  // The meter keeps reporting, so raw flow stays at recirculation level.
  bool saw_flow_method = false;
  for (int i = 1; i <= 6; i++) {  // out to 60 s, twice the 30 s staleness bound
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.flow.publish_state(2.2f);
    r.tick(now);
    if (r.method_state() == "deterministic_flow")
      saw_flow_method = true;
  }

  TEST_ASSERT(!saw_flow_method,
              "Recirculation is never scored as household flow while the "
              "motor reading is frozen");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A frozen motor holds the detector in pump-on uncertainty");
  TEST_ASSERT(r.demand.state == false, "No demand claimed from a frozen link");
  TEST_ASSERT(near(r.confidence.state, 50.0f),
              "Pump-on uncertainty publishes 50% confidence");
  TEST_ASSERT(std::isnan(r.motor_speed.state) == false &&
                  near(r.motor_speed.state, 2400.0f),
              "The frozen reading is still a perfectly valid 2400 RPM — age, "
              "not NaN, is what rejected it");

  // Same value, now fresh: the pump really is off and the meter really is
  // seeing a draw. Two ticks for the onset debounce.
  uint32_t t7 = T0 + 7 * TICK_MS;
  r.at(t7);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(2.2f);
  r.tick(t7);
  // The first fresh-off tick must still be pending, and this is the whole of
  // issue #147: flow was above threshold on the previous tick too, but that
  // tick was pump-ON, when the meter is reading the recirculation loop. An
  // unqualified "flow was present last tick" satisfies the debounce here and
  // makes it a no-op at the one transition it exists for.
  TEST_ASSERT(r.method_state() == "flow_onset_pending",
              "Flow carried over from the pump-ON ticks does not arm the "
              "debounce (issue #147)");

  uint32_t t8 = T0 + 8 * TICK_MS;
  r.at(t8);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(2.2f);
  r.tick(t8);
  TEST_ASSERT(r.method_state() == "deterministic_flow",
              "A fresh 0 RPM does open the pump-off branch");
  TEST_ASSERT(r.demand.state == true, "Demand raised once the reading is live");
}

// ── 4. Flow latch ────────────────────────────────────────────────────────────
// flow_latch_active_() is .cpp-only: it derives its sample count from
// get_update_interval() and scans the circular buffer backwards. It is what
// keeps a corroborated draw alive across a momentary meter dropout, and what
// must stop doing so once the flow history has aged out.
void test_flow_latch_holds_then_expires() {
  std::cout << "\n=== Flow latch spans a meter dropout, then expires ==="
            << std::endl;
  Rig r;
  r.det.set_flow_latch_seconds(30);
  r.det.set_demand_release_seconds(0);  // observe raw demand, not the hold
  r.det.setup();

  // Two ticks of flow to get past the onset debounce.
  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.5f);
    r.tank_lower_temp.publish_state(120.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_flow", "Draw established");

  // Meter drops to zero, tank collapses hard (2 °F per 10 s = 0.2 °F/s against
  // a 0.05 threshold). Flow is gone but the latch still remembers it.
  uint32_t t2 = T0 + 2 * TICK_MS;
  r.at(t2);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(0.0f);
  r.tank_lower_temp.publish_state(118.0f);
  r.tick(t2);

  TEST_ASSERT(r.method_state() == "deterministic_thermal",
              "Thermal collapse carries the draw while the latch holds");
  TEST_ASSERT(r.demand.state == true, "Demand held across the meter dropout");
  TEST_ASSERT(near(r.demand_level.state, 0.5f),
              "Latched demand publishes the moderate default intensity");

  // Keep the collapse going past the 30 s latch. Once the buffer has no
  // above-threshold sample left, the no-flow guard takes over.
  for (int i = 3; i <= 8; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tank_lower_temp.publish_state(118.0f - 2.0f * (i - 2));
    r.tick(now);
  }

  TEST_ASSERT(r.demand.state == false,
              "Demand drops once the latch has aged out");
  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "A collapsing tank with no flow anywhere in history is idle");
}

// ── 5. Derivative across a NaN gap ───────────────────────────────────────────
// compute_deriv_() deliberately leaves prev and prev_ms untouched on a NaN
// reading, so the next real value differentiates over the whole gap rather than
// over one tick. Discriminating on purpose: 1 °F over the 60 s gap is
// −0.017 °F/s and stays silent, while the same drop over one 10 s tick would be
// −0.1 °F/s and fire deterministic_thermal.
void test_derivative_spans_a_nan_gap() {
  std::cout << "\n=== Derivative dt spans a NaN gap ===" << std::endl;
  Rig r;
  r.det.set_flow_latch_seconds(120);  // keep the latch armed across the gap
  r.det.set_demand_release_seconds(0);
  r.det.setup();

  // Flow long enough to arm the latch, and a first temperature sample.
  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.5f);
    r.tank_lower_temp.publish_state(120.0f);
    r.tick(now);
  }

  // Tank sensor drops out for 40 s; meter goes quiet at the same time.
  for (int i = 2; i <= 5; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tank_lower_temp.publish_state(NAN);
    r.tick(now);
  }

  // It comes back 1 °F lower, 60 s after the last real sample.
  uint32_t t6 = T0 + 6 * TICK_MS;
  r.at(t6);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(0.0f);
  r.tank_lower_temp.publish_state(119.0f);
  r.tick(t6);

  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "1 °F over a 60 s gap is not a thermal collapse (a per-tick dt "
              "would have read it as -0.1 °F/s and fired)");
  TEST_ASSERT(r.demand.state == false, "No demand from the gap-spanning drop");
}

// ── 6. Demand release hold ───────────────────────────────────────────────────
// The hold is DemandHold's, but the three fields the .cpp overrides while it is
// latching are not: reporting the underlying branch's "deterministic_idle" at
// 100% next to a demand sensor reading ON says the exact opposite of what the
// component means.
void test_release_hold_reports_coherently() {
  std::cout << "\n=== Release hold publishes a coherent triple ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(30);
  r.det.setup();

  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.5f);
    r.tick(now);
  }
  TEST_ASSERT(r.demand.state == true && near(r.demand_level.state, 0.6f),
              "Draw established at 0.6 intensity");

  // Flow stops. Raw demand is false from here on.
  uint32_t t2 = T0 + 2 * TICK_MS;
  r.at(t2);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(0.0f);
  r.tick(t2);

  TEST_ASSERT(r.demand.state == true, "Binary sensor stays ON during the hold");
  TEST_ASSERT(r.method_state() == "demand_release_hold",
              "Method names the hold rather than the idle branch beneath it");
  TEST_ASSERT(near(r.confidence.state, 50.0f),
              "Held demand publishes 50%, not the idle branch's 100%");
  TEST_ASSERT(near(r.demand_level.state, 0.6f),
              "Held demand carries the last live intensity, not 0.0");

  // 30 s after the last true tick the hold releases.
  for (int i = 3; i <= 5; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.demand.state == false, "Hold releases after demand_release_seconds");
  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "Once released the underlying branch reports again");
}

// ── 7. Session accounting ────────────────────────────────────────────────────
// update_session_() must drive the tracker from the *held* demand and on the
// caller's tick timestamp, not a fresh millis(), or the session and the hold
// disagree about when the draw ended.
void test_session_duration_tracks_the_draw() {
  std::cout << "\n=== Session duration tracks the draw ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_session_gap_tolerance_seconds(20);
  r.det.setup();

  for (int i = 0; i < 5; i++) {  // ticks 0..4: onset pending, then 4 of draw
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.5f);
    r.tick(now);
  }
  // Session started on tick 1 (the first demand tick) and ran to tick 4.
  TEST_ASSERT(near(r.session_duration.state, 30.0f),
              "Live session duration counts from the first demand tick");

  for (int i = 5; i <= 8; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tick(now);
  }
  TEST_ASSERT(near(r.session_duration.state, 0.0f),
              "Session closes and duration returns to 0 past the gap tolerance");
}

// ── 8. Nothing wired ─────────────────────────────────────────────────────────
// Every input and output is optional in the schema. All the null guards are in
// the .cpp, so this is the only place they can be exercised; under ASan a
// missing one is a crash rather than a judgement call.
void test_unwired_component_is_inert() {
  std::cout << "\n=== A component with nothing wired ticks safely ==="
            << std::endl;
  DhwDemandComponent det;
  det.set_update_interval(TICK_MS);
  det.setup();
  det.dump_config();
  for (int i = 0; i < 3; i++) {
    mock_millis = T0 + i * TICK_MS;
    det.update();
  }
  TEST_ASSERT(true, "setup(), dump_config() and update() survive null sensors");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "DHW Demand Component Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_idle_publishes_once();
  test_flow_onset_takes_two_ticks();
  test_frozen_motor_never_enters_pump_off_branch();
  test_flow_latch_holds_then_expires();
  test_derivative_spans_a_nan_gap();
  test_release_hold_reports_coherently();
  test_session_duration_tracks_the_draw();
  test_unwired_component_is_inert();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
