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
  TEST_ASSERT(r.demand.state == false,
              "The draw stopping ends the demand, meter flow notwithstanding");
  TEST_ASSERT(r.method_state() == "pump_on_uncertain",
              "...and the tick falls through to the fallback");

  // Now the pump slows below its speed floor, so no subtraction exists at all.
  // The disproved capture must not come back on the strength of the meter
  // reading that never fell.
  for (int i = 1; i <= 3; i++) {
    uint32_t slow = stopped + i * TICK_MS;
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
  // The meter reads 0.71 GPM of loop flow throughout, still 2.4x threshold.
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
  test_recirculation_does_not_arm_the_onset_debounce();
  test_flow_latch_holds_then_expires();
  test_derivative_spans_a_nan_gap();
  test_release_hold_reports_coherently();
  test_session_duration_tracks_the_draw();
  test_session_follows_the_held_demand();
  test_a_stopped_draw_ends_the_continuation();
  test_an_unmeasurable_continuation_expires();
  test_unwired_component_is_inert();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
