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
// Anonymous namespace: several test files in this suite define their own
// Rig, and cppcheck's whole-program pass reports same-named structs across
// translation units as an ODR violation even though each test is its own
// binary. Same treatment as test_read_chain_lifetime.cpp.
namespace {

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

}  // namespace


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
// the bench in minutes.
//
// The frozen value has to be **0 RPM** for this to test anything. An earlier
// version of this test froze at 2400 RPM, which is inert: fresh 2400 says the
// pump is on and frozen 2400 is assumed on, so the answer is identical under
// every possible staleness policy and the test passed with the staleness mask
// deleted outright. 0 RPM is where the two policies disagree — fresh 0 is a
// *confirmed-off* pump, which opens the pump-OFF branch and scores raw meter
// flow as household demand at confidence 1.0.
//
// The scenario is the real one: the pump was off and quiet, the link dropped,
// and the pump then started without anyone seeing it. alpha_hwr keeps
// republishing the last value rather than NaN, so the detector still reads
// 0 RPM while the meter climbs to recirculation level.
void test_frozen_motor_never_enters_pump_off_branch() {
  std::cout << "\n=== A frozen 0 RPM never opens the pump-off branch ==="
            << std::endl;
  Rig r;
  r.det.setup();

  // Pump off, house quiet. The last healthy reports before the link drops.
  for (int i = 0; i <= 3; i++) {  // ticks 0..3: reading still inside the 30 s bound
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    if (i == 0)
      r.motor_speed.publish_state(0.0f);  // the last motor report there will be
    r.flow.publish_state(0.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "A fresh 0 RPM with no flow is idle");

  // Past the staleness bound. The pump has started, unobserved; the meter is
  // now reading the recirculation loop, and the motor channel still says 0.
  bool saw_flow_method = false;
  for (int i = 4; i <= 9; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.flow.publish_state(2.2f);
    r.tick(now);
    if (r.method_state() == "deterministic_flow")
      saw_flow_method = true;
  }

  TEST_ASSERT(!saw_flow_method,
              "Recirculation is never scored as household flow while the "
              "motor reading is frozen (issue #149)");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A frozen motor holds the detector in pump-on uncertainty");
  TEST_ASSERT(r.demand.state == false, "No demand claimed from a frozen link");
  TEST_ASSERT(near(r.confidence.state, 50.0f),
              "Pump-on uncertainty publishes 50% confidence");

  // Now the same 0 RPM arrives fresh: the pump really is off and the meter
  // really is seeing a draw. Two ticks for the onset debounce. Nothing differs
  // from the loop above except the age of the motor reading, which is what
  // makes this a test of staleness rather than of anything else.
  for (int i = 10; i <= 11; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(2.2f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_flow",
              "The same 0 RPM, arriving fresh, does open the pump-off branch");
  TEST_ASSERT(r.demand.state == true, "Demand raised once the reading is live");
}

// ── 3a. A start hidden inside a frozen-motor gap is still a start ─────────────
// The frozen regime assumes the pump ON without knowing it. Keyed on the
// previous *tick*, the edge detector saw "assumed on" followed by "known on"
// and stamped nothing, so the settle window -- the 10 s after a start in which
// the pump's own loop-flow estimate is still ramping and reads low against a
// loop the meter already sees moving -- never applied to a start the link had
// missed. The edge is a transition between known states.
void test_a_start_hidden_in_a_motor_gap_is_still_an_edge() {
  std::cout << "\n=== A start the link missed still stamps its edge ==="
            << std::endl;
  Rig r;
  r.det.setup();

  // Pump off and known so, house quiet.
  for (int i = 0; i <= 1; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.pump_flow.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_idle", "Known off, no flow: idle");

  // The motor and loop channels go silent for 40 s: past the 30 s staleness
  // bound, so the detector is in the frozen regime, assuming ON. Meter at 0.
  for (int i = 2; i <= 5; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.flow.publish_state(0.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A frozen motor holds the detector in pump-on uncertainty");

  // Telemetry returns. The pump started during the gap and is ramping: the
  // loop estimate is still 1.0 GPM under the meter. Inside the settle window
  // that difference is the ramp, not a draw.
  uint32_t back = T0 + 6 * TICK_MS;
  r.at(back);
  r.motor_speed.publish_state(3172.0f);
  r.pump_flow.publish_state(0.713f);
  r.flow.publish_state(1.712f);
  r.tick(back);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "The ramp-up reading right after a start the link missed is not a draw");
  TEST_ASSERT(r.demand.state == false, "...so no demand is raised from it");

  // The same readings a tick later, outside the window, are a subtraction
  // verdict again: the stamp defers the judgement, it does not suppress it.
  uint32_t settled = T0 + 8 * TICK_MS;
  r.at(settled);
  r.motor_speed.publish_state(3172.0f);
  r.pump_flow.publish_state(0.713f);
  r.flow.publish_state(1.712f);
  r.tick(settled);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "Once the estimate has settled the same difference is a draw");
  TEST_ASSERT(r.demand.state == true, "...and demand is raised");
}

// ── 3b. Recirculation must not arm the onset debounce ────────────────────────
// Issue #147, at the pump-ON → pump-OFF transition. Flow was above threshold on
// the previous tick, but that tick was pump-ON, when the meter is reading the
// recirculation loop and says nothing about household demand. An unqualified
// "flow was present last tick" satisfies the debounce immediately and makes it
// a no-op at the one transition it exists for.
//
// The composition being tested is the call in dhw_demand.cpp, not the predicate
// -- the predicate has its own test, and the historical defect was in what the
// caller passed it.
void test_recirculation_does_not_arm_the_onset_debounce() {
  std::cout << "\n=== Recirculation does not arm the flow-onset debounce ==="
            << std::endl;
  Rig r;
  r.det.setup();

  // Pump running with a quiet loop; the meter reads recirculation throughout.
  for (int i = 0; i <= 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(2400.0f);
    r.pump_flow.publish_state(2.1f);
    r.flow.publish_state(2.2f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "Pump-on with a quiet loop claims nothing");

  // Pump stops. Meter flow is unchanged from the previous tick -- but that tick
  // was pump-ON, so it contributes no evidence.
  uint32_t t3 = T0 + 3 * TICK_MS;
  r.at(t3);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(2.2f);
  r.tick(t3);
  TEST_ASSERT(r.method_state() == "flow_onset_pending",
              "The first pump-off tick is pending, not a draw (issue #147)");
  TEST_ASSERT(r.demand.state == false,
              "No demand from flow carried over from the pump-ON regime");

  uint32_t t4 = T0 + 4 * TICK_MS;
  r.at(t4);
  r.motor_speed.publish_state(0.0f);
  r.flow.publish_state(2.2f);
  r.tick(t4);
  TEST_ASSERT(r.method_state() == "deterministic_flow",
              "A second pump-off tick of flow confirms the draw");
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
// This one runs with the release hold disabled, so it isolates the tracker's
// own arithmetic. The coupling between the hold and the session is the *next*
// test — with release_seconds at 0 the held and raw values are identical, so
// nothing here can tell them apart.
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

// ── 7b. The session sees the held demand, not the raw one ────────────────────
// update_session_() is fed the value the release hold produced. If it were fed
// raw_demand instead, the session would close the moment flow stopped while the
// demand binary sensor was still ON — the two would be describing different
// draws. Isolating that needs a non-zero release window and a gap tolerance
// short enough that the raw-fed version would already have closed.
void test_session_follows_the_held_demand() {
  std::cout << "\n=== The session tracks held demand, not raw ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(30);
  r.det.set_session_gap_tolerance_seconds(0);  // close on the first quiet tick
  r.det.setup();

  // Ticks 0-1: onset debounce, then one tick of real draw.
  for (int i = 0; i <= 1; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.5f);
    r.tick(now);
  }

  // Flow stops. Raw demand is false from here; the hold keeps it true.
  for (int i = 2; i <= 3; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(0.0f);
    r.tick(now);
  }

  TEST_ASSERT(r.demand.state == true, "Hold still latching at +30 s");
  TEST_ASSERT(near(r.session_duration.state, 20.0f),
              "Session is still open and counting through the hold — fed raw "
              "demand it would have closed at the first dry tick and read 0");
}

// ── 8. The continuation across a whole pump run ──────────────────────────────
// Audit finding 10, end to end. The tier's release used to be raw meter flow
// above 0.3 GPM, which cannot go false while the pump runs, so a draw that
// stopped mid-run went on being published as demand until the pump did.
//
// What is only testable here rather than in the predicate's own file: the
// capture is *retired* when the subtraction falsifies it, so a later loss of
// the subtraction cannot resurrect a claim already disproved. That clearing
// lives in the .cpp, and without it the last two assertions fail while every
// unit test still passes.
void test_a_stopped_draw_ends_the_continuation() {
  std::cout << "\n=== A draw that stops mid-run ends the continuation ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);  // observe raw demand, not the hold
  r.det.setup();

  // Two pump-off ticks of a real 1.80 GPM draw: past the onset debounce, so the
  // pump-on edge has confirmed demand evidence to capture.
  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.80f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_flow",
              "A draw is established while the pump is off");

  // Pump starts. The meter now reads the draw plus the loop.
  uint32_t on = T0 + 2 * TICK_MS;
  r.at(on);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(3.11f);
  r.tick(on);
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "The draw in progress carries across the pump start");

  // Past the settle window, with the draw still running: the subtraction agrees
  // and the tier holds.
  uint32_t drawing = on + TICK_MS;
  r.at(drawing);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(3.11f);
  r.tick(drawing);
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "A measured draw keeps the continuation alive");
  TEST_ASSERT(r.demand.state == true, "...and demand stays true");

  // The tap closes. The meter still reads 1.31 GPM -- the no-draw median, and
  // 4.4x the 0.3 threshold -- because that is the recirculation loop. Only the
  // subtraction can tell: 1.31 - 1.31 = 0.00.
  uint32_t stopped = drawing + TICK_MS;
  r.at(stopped);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(1.31f);
  r.tick(stopped);
  TEST_ASSERT(r.demand.state == true,
              "One stopped tick does not yet end it -- the retirement cannot "
              "be undone, and isolated bad differences are measured");
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "...and the tier still decides that tick");

  uint32_t stopped2 = stopped + TICK_MS;
  r.at(stopped2);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(1.31f);
  r.tick(stopped2);
  TEST_ASSERT(r.demand.state == false,
              "The draw stopping ends the demand, meter flow notwithstanding");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "...and the tick falls through to the fallback");

  // Now the pump slows below its speed floor, so no subtraction exists at all.
  // The disproved capture must not come back on the strength of the meter
  // reading that never fell.
  for (int i = 1; i <= 3; i++) {
    uint32_t slow = stopped2 + i * TICK_MS;
    r.at(slow);
    r.motor_speed.publish_state(1650.0f);
    r.pump_flow.publish_state(0.25f);
    r.flow.publish_state(0.71f);
    r.tick(slow);
  }
  TEST_ASSERT(r.demand.state == false,
              "Losing the subtraction does not resurrect a disproved draw");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "...and the continuation does not re-fire");
}

// The other half: a run where the subtraction is never available, so nothing
// can ever contradict the tier. A pump clamped below its speed floor is the
// real case. The expiry is the only thing that ends it there, and without one
// this run publishes demand for its entire length.
// The expiry means "unsupported for N seconds", not "N seconds since the pump
// started" -- and the difference lives entirely in the component, which
// re-stamps the support time on a CONFIRMED verdict. Without that re-stamp a
// draw measured on every single tick still expires mid-run, which is the P1
// three reviewers found in the first version of this fix.
void test_a_continuously_measured_draw_never_expires() {
  std::cout << "\n=== Testing Expiry Refresh On Confirmation ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_pump_on_continuation_max_seconds(300);
  r.det.setup();

  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.80f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_flow", "Draw established");

  // Pump runs fast enough to subtract, and the draw keeps measuring at
  // 1.80 GPM on every tick, for well past the 300 s ceiling.
  //
  // Note what this half does NOT test: a CONFIRMED verdict returns before the
  // age check, so the tier cannot expire while the subtraction is confirming
  // whether or not the support time is re-stamped. The mutation check caught
  // an earlier version of this test asserting exactly that and proving
  // nothing. The re-stamp only shows up in the *second* half below.
  const uint32_t on = T0 + 2 * TICK_MS;
  int continuation_ticks = 0;
  const int kConfirmTicks = 40;  // 400 s, past the 300 s ceiling
  for (int i = 0; i <= kConfirmTicks; i++) {
    uint32_t now = on + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(2400.0f);
    r.pump_flow.publish_state(1.31f);
    r.flow.publish_state(3.11f);  // 1.80 GPM of measured demand
    r.tick(now);
    if (r.method_state() == "deterministic_continuation")
      continuation_ticks++;
  }
  TEST_ASSERT(continuation_ticks == kConfirmTicks + 1,
              "A draw confirmed on every tick never expires, however long the "
              "pump runs");

  // Now the subtraction goes away -- the pump drops below its speed floor with
  // the draw still running. This is where the re-stamp is load-bearing: the
  // age must run from the last confirmation, not from the pump-on edge 400 s
  // ago. Without it the very first blind tick is already past the ceiling and
  // the tier expires on the spot.
  const uint32_t blind = on + (kConfirmTicks + 1) * TICK_MS;
  r.at(blind);
  r.motor_speed.publish_state(1650.0f);
  r.pump_flow.publish_state(0.25f);
  r.flow.publish_state(0.71f);
  r.tick(blind);
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "Losing the subtraction after 400 s of confirmation does not "
              "expire the tier immediately -- the clock runs from the last "
              "confirmation, not from the pump start");
  TEST_ASSERT(r.demand.state == true, "...and demand is unbroken");

  // It still expires eventually: 300 s after that last confirmation.
  int blind_continuation = 0;
  for (int i = 1; i <= 40; i++) {
    uint32_t now = blind + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(1650.0f);
    r.pump_flow.publish_state(0.25f);
    r.flow.publish_state(0.71f);
    r.tick(now);
    if (r.method_state() == "deterministic_continuation")
      blind_continuation++;
  }
  // 28, not 30: the last confirmation was one tick before `blind`, so the
  // first blind tick already carries 10 s of age. 300 s of ceiling minus that
  // tick and the `blind` tick itself leaves 28 inside the window.
  TEST_ASSERT(blind_continuation == 28,
              "...and it expires 300 s after the last confirmation, not never");
  TEST_ASSERT(r.demand.state == false, "The unsupported claim ends");
}

// One stopped reading followed by a confirming one must not accumulate toward
// the two-tick release: the streak counts *consecutive* ticks. Without the
// reset, two isolated bad differences minutes apart would retire a capture
// that a measurement confirmed in between.
void test_an_isolated_stopped_tick_does_not_accumulate() {
  std::cout << "\n=== Testing Stopped-Streak Reset ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.setup();

  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.80f);
    r.tick(now);
  }

  const uint32_t on = T0 + 2 * TICK_MS;
  // Arm, then get past the settle window with the draw measuring.
  for (int i = 0; i <= 1; i++) {
    uint32_t now = on + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(2400.0f);
    r.pump_flow.publish_state(1.31f);
    r.flow.publish_state(3.11f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_continuation", "Armed");

  // A single transient: the meter follows the loop down while the pump's own
  // reading is still high, so the difference goes negative for one tick.
  uint32_t bad = on + 2 * TICK_MS;
  r.at(bad);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(1.20f);  // -0.11 GPM
  r.tick(bad);
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "One transient does not release");

  // The draw measures again -- this must clear the streak.
  uint32_t good = bad + TICK_MS;
  r.at(good);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(3.11f);
  r.tick(good);

  // A second isolated transient. With the streak reset this is the *first*
  // consecutive stopped tick and must not retire the capture; without the
  // reset it is the second and does.
  uint32_t bad2 = good + TICK_MS;
  r.at(bad2);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.flow.publish_state(1.20f);
  r.tick(bad2);
  TEST_ASSERT(r.method_state() == "deterministic_continuation",
              "Two isolated transients with a confirmation between them do "
              "not add up to a release");
  TEST_ASSERT(r.demand.state == true, "...and demand is unbroken throughout");
}

void test_an_unmeasurable_continuation_expires() {
  std::cout << "\n=== An unmeasured continuation expires ===" << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_pump_on_continuation_max_seconds(300);  // the shipped default
  r.det.setup();

  for (int i = 0; i < 2; i++) {
    uint32_t now = T0 + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(0.0f);
    r.flow.publish_state(1.80f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "deterministic_flow", "Draw established");

  // Pump starts and stays at 1650 rpm -- below pump_on_demand_min_speed_rpm,
  // where the pump's own flow estimate is not trustworthy enough to subtract.
  // The meter reads 0.71 GPM of loop flow throughout, still more than 2x threshold.
  const uint32_t on = T0 + 2 * TICK_MS;
  int continuation_ticks = 0;
  for (int i = 0; i <= 180; i++) {  // 30 minutes
    uint32_t now = on + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(1650.0f);
    r.pump_flow.publish_state(0.25f);
    r.flow.publish_state(0.71f);
    r.tick(now);
    if (r.method_state() == "deterministic_continuation")
      continuation_ticks++;
  }

  TEST_ASSERT(continuation_ticks == 30,
              "A blind 30-minute run holds demand for 5 minutes, not 30");
  TEST_ASSERT(r.demand.state == false,
              "The run ends with no demand claimed");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "...and the fallback has taken over");

  // The expiry retires the capture as well as declining, and that is what keeps
  // it from coming back ~49 days later. The age is `now - arm_stamp` on
  // uint32_t: it grows for a full lap of millis() and then wraps through zero,
  // so an expired continuation whose stamp was left in place reads as brand new
  // again. The lap is simulated by setting the clock just past the arm stamp --
  // modulo 2^32 that state is identical to 49 days on, which is the only way to
  // reach it without 424 000 ticks.
  const uint32_t lapped = on + 1000u;
  r.at(lapped);
  r.motor_speed.publish_state(1650.0f);
  r.pump_flow.publish_state(0.25f);
  r.flow.publish_state(0.71f);
  r.tick(lapped);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "An expired continuation does not return at the millis() wrap");
  TEST_ASSERT(r.demand.state == false, "...and claims no demand there either");
}

// ── 9. Nothing wired ─────────────────────────────────────────────────────────
// Every input and output is optional in the schema. All the null guards are in
// the .cpp, so this is the only place they can be exercised. The assertion is
// nominally tautological, but the test is not: dropping the nullptr guard in
// read_sensor_() segfaults here, and `make test` fails on the exit code.
// `make test-asan` (a CI job) turns the near misses into failures too.
// ── The dhw_in_use tier's support stamps (issue #330) ────────────────────────
// The rule itself is pinned in test_dhw_demand_logic.cpp. What lives only in
// the .cpp is the memory it reads: which ticks stamp "something metered a
// draw", which stamp "the subtraction measured a no-draw", which stamp nothing
// at all, and the speed captured beside each loop-flow reading. Each of those
// is a line in update() or a callback, and a wrong one is invisible to the
// pure-logic test.

// A pump-on tick with every channel published, so the staleness registers are
// stamped at `now` before the component reads them.
static void pump_on_tick(Rig &r, uint32_t now, float rpm, float loop,
                         float meter, float flag) {
  r.at(now);
  r.motor_speed.publish_state(rpm);
  if (!std::isnan(loop))
    r.pump_flow.publish_state(loop);
  r.flow.publish_state(meter);
  r.dhw_in_use.publish_state(flag);
  r.tick(now);
}

void test_dhw_in_use_carries_a_measured_draw_then_lets_go() {
  std::cout << "\n=== dhw_in_use carries a measured draw, then lets go ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);  // observe raw demand, not the hold
  r.det.set_dhw_in_use_min_seconds(0);  // the sustain guard has its own tests
  r.det.setup();

  // The flag is up from the start, before anything is metered. One confirmed
  // pump-off tick so the pump-on edge below is a known boundary.
  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);

  // Pump starts. Inside the settle window the subtraction declines, so tier 3
  // is reached with the flag sustained and nothing metered: the #330 shape.
  uint32_t on = T0 + TICK_MS;
  pump_on_tick(r, on, 2400.0f, 1.31f, 1.31f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A sustained flag over a fresh pump start with nothing metered "
              "publishes nothing");

  // A tap opens and the subtraction measures it on four ticks. Each stamps.
  uint32_t last_measured = 0;
  for (int i = 2; i <= 5; i++) {
    last_measured = T0 + i * TICK_MS;
    pump_on_tick(r, last_measured, 2400.0f, 1.31f, 3.11f, 1.0f);
  }
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "The subtraction measures the draw while the loop reports");

  // The loop-flow channel goes silent. For 30 s the last reading is still
  // fresh and the subtraction keeps measuring; past that it is NaN and the
  // flag is the only thing left -- supported by the draw it saw measured.
  for (int i = 1; i <= 3; i++)
    pump_on_tick(r, last_measured + i * TICK_MS, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "A 30 s old loop reading is still differenced");
  last_measured += 3 * TICK_MS;

  uint32_t blind = last_measured + TICK_MS;  // loop reading now 40 s old
  pump_on_tick(r, blind, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "With the subtraction gone, the flag carries the metered draw");
  TEST_ASSERT(r.demand.state == true, "...and demand stays true");

  // A fresh meter reading 0.00 vetoes the carry for that tick only.
  pump_on_tick(r, blind + TICK_MS, 2400.0f, NAN, 0.0f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A fresh meter at 0.00 vetoes: no draw can be behind it");
  pump_on_tick(r, blind + 2 * TICK_MS, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "...and the carry resumes when the meter reads again");

  // The carry window runs from the last tick that measured the draw, not from
  // the first blind tick. 120 s after it the flag is on its own again.
  uint32_t at_limit = last_measured + 120000;
  for (uint32_t now = blind + 3 * TICK_MS; now <= at_limit; now += TICK_MS)
    pump_on_tick(r, now, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "The carry holds through the 120 s window");
  pump_on_tick(r, at_limit + TICK_MS, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "One tick past the window the flag alone publishes nothing");
  TEST_ASSERT(r.demand.state == false, "...and demand is false");
}

void test_a_measured_no_draw_ends_the_carry() {
  std::cout << "\n=== A measured no-draw ends the dhw_in_use carry ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  pump_on_tick(r, T0 + TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);
  pump_on_tick(r, T0 + 2 * TICK_MS, 2400.0f, 1.31f, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "The draw is measured");

  // The tap closes while the loop still reports: the subtraction measures a
  // no-draw, which stamps.
  pump_on_tick(r, T0 + 3 * TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "The subtraction measures the tap closed");

  // Then the loop channel goes silent with the flag still up. Without the
  // no-draw stamp this is the carried-draw shape: metered 40 s ago, inside the
  // flag's run, and the flag stays up for a while after a tap closes.
  for (int i = 4; i <= 7; i++)
    pump_on_tick(r, T0 + i * TICK_MS, 2400.0f, NAN, 1.31f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "A no-draw measured after the draw ends the carry");
  TEST_ASSERT(r.demand.state == false, "...so the flag alone publishes nothing");
}

void test_continuation_and_the_tier_itself_do_not_stamp() {
  std::cout << "\n=== Neither a continuation tick nor the tier's own firing "
               "counts as a metered draw ==="
            << std::endl;

  // Case 7 of #330: the tier fires on the meter standing above the loop by
  // more than any bias (1800 rpm, under the speed floor, so no subtraction).
  // On the next tick the meter reads only the loop. If the tier's own firing
  // had stamped last_metered_demand, support (a) would now carry it.
  {
    Rig r;
    r.det.set_demand_release_seconds(0);
    r.det.set_dhw_in_use_min_seconds(0);
    r.det.setup();

    pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 0.0f);
    // Edge tick. The loop reading published here is inside the settle window.
    pump_on_tick(r, T0 + TICK_MS, 1800.0f, 0.31f, 0.31f, 0.0f);
    // 10 s in: a fresh loop reading at the current speed, a meter 1.05 GPM
    // above it, and the flag rising now.
    pump_on_tick(r, T0 + 2 * TICK_MS, 1800.0f, 0.31f, 1.36f, 1.0f);
    TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
                "Under the speed floor, a meter 1.05 GPM over the loop "
                "supports the flag");
    TEST_ASSERT(r.demand.state == true, "...and demand is published");

    pump_on_tick(r, T0 + 3 * TICK_MS, 1800.0f, 0.31f, 0.56f, 1.0f);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "When the meter falls back toward the loop the tier cannot "
                "vouch for itself");
    TEST_ASSERT(r.demand.state == false, "...so demand ends");
  }

  // The speed captured beside the loop reading is what makes (b) decline
  // after a step-up. Two rigs, identical except that in one the pump moves
  // from 1650 to 1800 rpm after its last loop reading. Both stay under the
  // speed floor so the subtraction never answers and tier 3 decides; the
  // meter reads 0.85 GPM over the stored loop reading in both. Only the
  // captured speed separates them.
  {
    Rig steady, stepped;
    for (Rig *r : {&steady, &stepped}) {
      r->det.set_demand_release_seconds(0);
      r->det.set_dhw_in_use_min_seconds(0);
      r->det.setup();
      pump_on_tick(*r, T0, 0.0f, 0.0f, 0.0f, 0.0f);
      pump_on_tick(*r, T0 + TICK_MS, 1650.0f, 0.25f, 0.71f, 0.0f);
      // 10 s in: the loop reading whose speed (1650) is captured.
      pump_on_tick(*r, T0 + 2 * TICK_MS, 1650.0f, 0.25f, 0.71f, 1.0f);
    }
    for (int i = 3; i <= 5; i++) {
      pump_on_tick(steady, T0 + i * TICK_MS, 1650.0f, NAN, 1.10f, 1.0f);
      pump_on_tick(stepped, T0 + i * TICK_MS, 1800.0f, NAN, 1.10f, 1.0f);
    }
    TEST_ASSERT(steady.method_state() == "deterministic_dhw_in_use",
                "At the speed the loop reading was taken, 0.85 GPM over it "
                "supports the flag");
    TEST_ASSERT(stepped.method_state() == "pump_on_uncertain",
                "After a 150 rpm step-up the same reading describes a loop "
                "the meter is no longer seeing, and does not");
  }

  // Case 6: a continuation carried on a stored capture measures nothing at
  // its own tick, so it must not stamp. The draw is metered while the pump is
  // off, the flag rises after the pump starts, and the continuation then runs
  // blind (1650 rpm, no subtraction) until it expires at 300 s. If the
  // continuation ticks had stamped, the flag would carry the draw past the
  // expiry; they did not, so the last metered tick predates the flag's run.
  {
    Rig r;
    r.det.set_demand_release_seconds(0);
    r.det.set_dhw_in_use_min_seconds(0);
    r.det.setup();

    pump_on_tick(r, T0, 0.0f, 0.0f, 1.80f, 0.0f);
    pump_on_tick(r, T0 + TICK_MS, 0.0f, 0.0f, 1.80f, 0.0f);
    TEST_ASSERT(r.method_state() == "deterministic_flow",
                "A pump-off draw is metered");

    uint32_t on = T0 + 2 * TICK_MS;
    pump_on_tick(r, on, 1650.0f, 0.25f, 0.71f, 0.0f);
    TEST_ASSERT(r.method_state() == "deterministic_continuation",
                "The draw carries across the pump start");

    // The flag rises. Continuation outranks the flag for as long as it holds.
    uint32_t now = on + TICK_MS;
    for (; now < on + 300000; now += TICK_MS)
      pump_on_tick(r, now, 1650.0f, 0.25f, 0.71f, 1.0f);
    TEST_ASSERT(r.method_state() == "deterministic_continuation",
                "The blind continuation holds up to its expiry");

    pump_on_tick(r, now, 1650.0f, 0.25f, 0.71f, 1.0f);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "Once it expires, the flag has no metered draw inside its "
                "run to carry: continuation ticks did not stamp");
    TEST_ASSERT(r.demand.state == false, "...and demand ends with it");
  }
}

void test_a_pump_off_verdict_supports_the_flag_across_a_start() {
  std::cout << "\n=== A pump-off verdict is a metered draw the flag may carry ==="
            << std::endl;
  // The stamp a pump-off flow verdict leaves is the only one this exercises:
  // the subtraction never runs here (1650 rpm, under the floor), and the
  // continuation is not armed because the tap closed before the pump started.
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  // The flag is up before the draw. Two pump-off ticks of flow, then the tap
  // closes and the pump is still off.
  pump_on_tick(r, T0, 0.0f, 0.0f, 1.80f, 1.0f);
  pump_on_tick(r, T0 + TICK_MS, 0.0f, 0.0f, 1.80f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_flow", "A pump-off draw");
  pump_on_tick(r, T0 + 2 * TICK_MS, 0.0f, 0.0f, 0.0f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_idle",
              "...and it closes before the pump starts");

  // The pump starts under the speed floor; the meter reads the loop. Nothing
  // measures here, and the continuation has no capture. The flag carries the
  // draw metered 20 s ago.
  pump_on_tick(r, T0 + 3 * TICK_MS, 1650.0f, 0.25f, 0.71f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "A flow verdict from before the start supports the flag");
  TEST_ASSERT(r.demand.state == true, "...and demand is published");
}

// Found in review on PR #331. During a telemetry gap the motor speed and the
// loop reading go stale together, because they arrive on the same poll. The
// meter-above-loop support compares the speed the loop reading was taken at
// against the *current* speed -- and a stale current speed is the same number
// as the stale captured one, so the check passed trivially while the pump's
// real speed was unknown. decide_pump_state() already masks a stale speed to
// NaN; the pump-on decision has to be fed that, not the raw cached value.
// Found in Copilot's re-review of PR #331. The meter reports on change and
// lags the pump's own channels, so after a speed drop it can go on reading the
// faster loop while speed and loop flow already describe the slower one. The
// difference then looks like a draw with no tap open. The component records
// the speed beside each meter reading, as it does beside each loop reading.
// Found in Copilot's third review of PR #331. The speed recorded beside a
// loop or meter reading was the cached speed even when the cache was past its
// bound, so a reading that arrived during a motor-telemetry gap was labelled
// with a speed nobody knew was current. Once telemetry recovered at that same
// speed, the label matched and vouched for it. Each channel is checked on its
// own: the reading that arrives in the gap is recorded with no speed at all.
static void run_recovery(Rig &r, bool meter_in_gap) {
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();
  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  for (int i = 1; i <= 3; i++)
    pump_on_tick(r, T0 + i * TICK_MS, 1800.0f, 0.31f, 0.31f, 1.0f);

  // The motor channel goes silent. 40 s on its last speed is past the 30 s
  // bound, and one channel reports into that gap.
  uint32_t gap = T0 + 3 * TICK_MS + 40000;
  r.at(gap);
  if (meter_in_gap)
    r.flow.publish_state(1.36f);
  else
    r.pump_flow.publish_state(0.31f);
  r.dhw_in_use.publish_state(1.0f);
  r.tick(gap);

  // Telemetry recovers at the same speed; the other channel reports now.
  uint32_t back = gap + TICK_MS;
  r.at(back);
  r.motor_speed.publish_state(1800.0f);
  if (meter_in_gap)
    r.pump_flow.publish_state(0.31f);
  else
    r.flow.publish_state(1.36f);
  r.dhw_in_use.publish_state(1.0f);
  r.tick(back);
}

// Found on the bench after PR #331. The subtraction differences the meter
// against the loop, and the meter reports on change and lags the pump's own
// channels. When the pump steps down, speed and loop flow drop together in one
// poll while the meter still reads the faster loop, so for that tick the
// difference is a draw that does not exist. That one-tick misfire predates
// #330. What #330 added is the carry: the misfire stamped the support time,
// and once the pump was under the speed floor nothing measured a no-draw to
// end it, so the dhw_in_use flag published demand on it for the whole carry
// window. A subtraction only stamps when the meter reading it used was taken
// at the speed it is being differenced at.
void test_a_skewed_subtraction_does_not_stamp_a_carry() {
  std::cout << "\n=== A subtraction against a meter from another speed does "
               "not stamp the carry ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  for (int i = 1; i <= 3; i++)
    pump_on_tick(r, T0 + i * TICK_MS, 3000.0f, 2.00f, 2.00f, 1.0f);

  // The step-down poll: speed and loop report together, the meter has not
  // reported since it read the 3000 rpm loop.
  uint32_t step = T0 + 4 * TICK_MS;
  r.at(step);
  r.motor_speed.publish_state(2400.0f);
  r.pump_flow.publish_state(1.31f);
  r.dhw_in_use.publish_state(1.0f);
  r.tick(step);

  // Under the floor at 1800 rpm. The meter catches up and reads the loop it
  // sees now, a little above the pump's own low-speed estimate. Nothing here
  // is a draw, and no subtraction runs under the floor to say so.
  for (int i = 1; i <= 4; i++) {
    uint32_t now = step + i * TICK_MS;
    pump_on_tick(r, now, 1800.0f, 0.31f, 0.70f, 1.0f);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "The step-down tick's skewed difference does not carry the "
                "flag through the run under the floor");
  }
  TEST_ASSERT(r.demand.state == false, "...so no demand is published");
}

// The other side of the same rule: a draw measured with the meter and the
// pump at the same speed still stamps, so it is carried exactly as before.
void test_a_subtraction_at_one_speed_still_stamps_a_carry() {
  std::cout << "\n=== A subtraction against a meter at the same speed still "
               "stamps the carry ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  pump_on_tick(r, T0 + TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);
  pump_on_tick(r, T0 + 2 * TICK_MS, 2400.0f, 1.31f, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "A tap is measured with every reading at 2400 rpm");
  // 0.70 GPM clears the veto but sits 0.39 over the loop, inside support (b)'s
  // 0.75 margin, so only the carried draw can make the flag fire here (found
  // in review on PR #333: at 2.11 GPM support (b) passed this on its own).
  pump_on_tick(r, T0 + 3 * TICK_MS, 1800.0f, 0.31f, 0.70f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "Under the floor the measured draw carries the flag");
}

void test_a_reading_taken_in_a_motor_gap_carries_no_speed() {
  std::cout << "\n=== A reading taken while the motor speed was stale carries "
               "no speed ==="
            << std::endl;
  {
    Rig r;
    run_recovery(r, true);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "A meter reading from the gap is not vouched for by the speed "
                "that came back after it");
  }
  {
    Rig r;
    run_recovery(r, false);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "...nor is a loop reading from the gap");
  }
}

void test_a_meter_lagging_a_speed_drop_cannot_support_the_flag() {
  std::cout << "\n=== A meter reading from before a speed drop cannot support "
               "the flag ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  // Recirculating at 2400 rpm with no tap: the meter reads the loop.
  for (int i = 1; i <= 3; i++)
    pump_on_tick(r, T0 + i * TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);

  // The pump steps down to 1800 rpm, under the subtraction's floor. Its own
  // channels report the slower loop at once; the meter has not reported yet,
  // so its last reading, 20 s old and inside its 60 s bound, is the 2400 one.
  uint32_t drop = T0 + 4 * TICK_MS;
  for (int i = 0; i < 2; i++) {
    uint32_t now = drop + i * TICK_MS;
    r.at(now);
    r.motor_speed.publish_state(1800.0f);
    r.pump_flow.publish_state(0.31f);
    r.dhw_in_use.publish_state(1.0f);
    r.tick(now);
    TEST_ASSERT(r.method_state() == "pump_on_uncertain",
                "1.31 GPM read at 2400 rpm minus 0.31 read at 1800 is not a "
                "draw, and the flag is not supported by it");
  }

  // A tap opens and the meter reports it at the new speed: 1.05 over the loop.
  pump_on_tick(r, drop + 2 * TICK_MS, 1800.0f, 0.31f, 1.36f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "A meter reading taken at the current speed still supports it");
}

void test_a_stale_speed_cannot_validate_the_meter_above_the_loop() {
  std::cout << "\n=== A stale motor speed cannot vouch for a loop reading ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 0.0f);
  pump_on_tick(r, T0 + TICK_MS, 1800.0f, 0.31f, 0.31f, 0.0f);
  pump_on_tick(r, T0 + 2 * TICK_MS, 1800.0f, 0.31f, 0.31f, 1.0f);
  // 20 s in, all three channels report together at 1800 rpm: a meter 1.05
  // GPM over the loop, which supports the flag while the speed is current.
  uint32_t gap_start = T0 + 3 * TICK_MS;
  pump_on_tick(r, gap_start, 1800.0f, 0.31f, 1.36f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_dhw_in_use",
              "With every reading current, the meter above the loop supports "
              "the flag");

  // The motor and loop channels go silent together. The meter reports only
  // on change, so a steady reading does not re-report either; only the flag
  // does. 40 s on, the speed is past its 30 s bound while the loop and meter
  // readings are both inside the 60 s one -- and both were labelled with a
  // speed that was current when they were taken. Only the current speed's
  // own age can say the comparison is no longer possible.
  for (int i = 1; i <= 4; i++) {
    uint32_t now = gap_start + i * TICK_MS;
    r.at(now);
    r.dhw_in_use.publish_state(1.0f);
    r.tick(now);
  }
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "With the speed 40 s stale, a matching captured speed proves "
              "nothing and the meter above the loop does not support the flag");
  TEST_ASSERT(r.demand.state == false, "...so no demand is published");
}

// Also found in review on PR #331. The support stamps are millis() values and
// the rule compares them as ages, which wrap. A stamp left in place for ~49.7
// days becomes "recent" again, so an expired draw would come back to support a
// flag run. The component retires a stamp once it is older than the carry
// window, the same reason it retires an expired continuation capture.
void test_an_expired_support_stamp_cannot_return_after_a_wrap() {
  std::cout << "\n=== An expired support stamp is retired before millis() "
               "wraps back to it ==="
            << std::endl;
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  pump_on_tick(r, T0 + TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);
  uint32_t metered = T0 + 2 * TICK_MS;
  pump_on_tick(r, metered, 2400.0f, 1.31f, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "A draw is metered, stamping the support time");

  // The carry window passes with the flag up and the loop silent.
  for (uint32_t now = metered + 40000; now <= metered + 130000; now += TICK_MS)
    pump_on_tick(r, now, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "Past the 120 s carry window the flag alone publishes nothing");

  // One full millis() period later the clock reads 100 s after the stamp.
  // mock_millis is a uint32_t, so setting it back to metered + 100 s *is* that
  // instant: 2^32 ms on, the subtraction the stamp is compared with wraps to
  // the same 100 s. A retained stamp would be inside the 120 s window again.
  // 100 s rather than less so the last loop reading is past the meter's 60 s
  // bound: the meter-above-loop support must not be what answers here.
  pump_on_tick(r, metered + 100000, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "After a wrap the expired draw does not come back to carry "
              "the flag");
}

void test_dhw_in_use_support_thresholds_are_configurable() {
  std::cout << "\n=== The support rule's thresholds reach the decision ==="
            << std::endl;
  // The carried-draw shape from the first test, with the carry disabled and
  // the bias margin set beyond any meter: both supports must decline.
  Rig r;
  r.det.set_demand_release_seconds(0);
  r.det.set_dhw_in_use_min_seconds(0);
  r.det.set_dhw_in_use_carry_max_seconds(0);
  r.det.set_dhw_in_use_loop_bias_gpm(5.0f);
  r.det.setup();

  pump_on_tick(r, T0, 0.0f, 0.0f, 0.0f, 1.0f);
  pump_on_tick(r, T0 + TICK_MS, 2400.0f, 1.31f, 1.31f, 1.0f);
  pump_on_tick(r, T0 + 2 * TICK_MS, 2400.0f, 1.31f, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "deterministic_pump_on_subtraction",
              "The draw is measured");
  for (int i = 3; i <= 6; i++)
    pump_on_tick(r, T0 + i * TICK_MS, 2400.0f, NAN, 3.11f, 1.0f);
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "With the carry disabled and the margin out of reach, the flag "
              "publishes nothing");
}

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
  test_a_start_hidden_in_a_motor_gap_is_still_an_edge();
  test_recirculation_does_not_arm_the_onset_debounce();
  test_flow_latch_holds_then_expires();
  test_derivative_spans_a_nan_gap();
  test_release_hold_reports_coherently();
  test_session_duration_tracks_the_draw();
  test_session_follows_the_held_demand();
  test_a_stopped_draw_ends_the_continuation();
  test_a_continuously_measured_draw_never_expires();
  test_an_isolated_stopped_tick_does_not_accumulate();
  test_an_unmeasurable_continuation_expires();
  test_dhw_in_use_carries_a_measured_draw_then_lets_go();
  test_a_measured_no_draw_ends_the_carry();
  test_continuation_and_the_tier_itself_do_not_stamp();
  test_a_pump_off_verdict_supports_the_flag_across_a_start();
  test_a_skewed_subtraction_does_not_stamp_a_carry();
  test_a_subtraction_at_one_speed_still_stamps_a_carry();
  test_a_reading_taken_in_a_motor_gap_carries_no_speed();
  test_a_meter_lagging_a_speed_drop_cannot_support_the_flag();
  test_a_stale_speed_cannot_validate_the_meter_above_the_loop();
  test_an_expired_support_stamp_cannot_return_after_a_wrap();
  test_dhw_in_use_support_thresholds_are_configurable();
  test_unwired_component_is_inert();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
