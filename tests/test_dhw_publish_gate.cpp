// Host tests for the DHW detector's publish-on-change gates (issue #129).
//
// The regression these guard: sensor/text_sensor publish_state() notify the
// frontend unconditionally, so the detector's four step-valued outputs emitted
// an API state frame per subscriber on every 10 s tick even with the node idle
// and nothing moving — 0.40 frames/s/subscriber of pure repetition. The binary
// `demand` output was quiet the whole time because BinarySensor dedups
// internally; that split within one component is the shape of the bug.

#include <cmath>
#include <iostream>
#include <string>

#include "../components/dhw_demand/publish_gate.h"

using esphome::dhw_demand::float_state_differs;
using esphome::dhw_demand::publish_sensor_if_changed;
using esphome::dhw_demand::publish_text_sensor_if_changed;

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

// Stand-ins for esphome::sensor::Sensor / esphome::text_sensor::TextSensor,
// exposing only what the gates touch (they are templated precisely so the
// header stays free of the entity SDK). publish_state() counts calls: one call
// is one state callback, which is one API frame per subscriber.
struct FakeSensor {
  bool has_state() const { return this->has_state_; }
  float get_raw_state() const { return this->raw_state_; }
  bool get_force_update() const { return this->force_update_; }
  void publish_state(float value) {
    this->raw_state_ = value;
    this->has_state_ = true;
    this->publishes++;
  }
  float raw_state_{NAN};
  bool has_state_{false};
  bool force_update_{false};
  int publishes{0};
};

struct FakeTextSensor {
  bool has_state() const { return this->has_state_; }
  const std::string &get_raw_state() const { return this->raw_state_; }
  void publish_state(const char *value) {
    this->raw_state_ = value;
    this->has_state_ = true;
    this->publishes++;
  }
  std::string raw_state_;
  bool has_state_{false};
  int publishes{0};
};

void test_sensor_gate() {
  std::cout << "\n=== Testing sensor publish gate ===" << std::endl;

  FakeSensor s;
  publish_sensor_if_changed(&s, 100.0f);
  TEST_ASSERT(s.publishes == 1, "First value publishes (entity was unknown)");

  publish_sensor_if_changed(&s, 100.0f);
  publish_sensor_if_changed(&s, 100.0f);
  TEST_ASSERT(s.publishes == 1, "Repeats of the same value are suppressed");

  publish_sensor_if_changed(&s, 50.0f);
  TEST_ASSERT(s.publishes == 2 && s.get_raw_state() == 50.0f,
              "A changed value publishes");

  // 0.0 is the resting value of demand_level and session_duration; it must
  // publish once on the way down and then stay quiet.
  FakeSensor level;
  publish_sensor_if_changed(&level, 0.45f);
  publish_sensor_if_changed(&level, 0.0f);
  publish_sensor_if_changed(&level, 0.0f);
  publish_sensor_if_changed(&level, 0.0f);
  TEST_ASSERT(level.publishes == 2 && level.get_raw_state() == 0.0f,
              "Falling to 0.0 publishes once, then stays quiet");

  // An input dropping out publishes NaN, and NaN != NaN, so it has to be
  // compared by identity rather than by value.
  FakeSensor n;
  publish_sensor_if_changed(&n, NAN);
  publish_sensor_if_changed(&n, NAN);
  TEST_ASSERT(n.publishes == 1 && std::isnan(n.get_raw_state()),
              "Repeated NaN is suppressed (NaN != NaN must not leak through)");

  publish_sensor_if_changed(&n, 0.0f);
  publish_sensor_if_changed(&n, 0.0f);
  TEST_ASSERT(n.publishes == 2 && n.get_raw_state() == 0.0f,
              "0.0 is distinct from NaN and publishes once");

  publish_sensor_if_changed(&n, NAN);
  TEST_ASSERT(n.publishes == 3, "Transition back into NaN publishes");
}

void test_force_update_opts_out() {
  std::cout << "\n=== Testing force_update escape hatch ===" << std::endl;

  // force_update is ESPHome's own "record every publish, even unchanged" flag,
  // so a consumer that genuinely wants a per-tick heartbeat asks for it there.
  FakeSensor s;
  s.force_update_ = true;
  for (int tick = 0; tick < 5; tick++) {
    publish_sensor_if_changed(&s, 100.0f);
  }
  TEST_ASSERT(s.publishes == 5,
              "force_update: true keeps every publish, gate or no gate");
}

void test_text_sensor_gate() {
  std::cout << "\n=== Testing text_sensor publish gate ===" << std::endl;

  FakeTextSensor t;
  publish_text_sensor_if_changed(&t, "deterministic_idle");
  TEST_ASSERT(t.publishes == 1, "First method publishes (entity was unknown)");

  publish_text_sensor_if_changed(&t, "deterministic_idle");
  publish_text_sensor_if_changed(&t, "deterministic_idle");
  TEST_ASSERT(t.publishes == 1, "Repeats of the same method are suppressed");

  publish_text_sensor_if_changed(&t, "deterministic_flow");
  TEST_ASSERT(t.publishes == 2 && t.get_raw_state() == "deterministic_flow",
              "A changed method publishes");

  // Methods that differ only in suffix must not be conflated by a prefix match.
  publish_text_sensor_if_changed(&t, "deterministic_flow_x");
  TEST_ASSERT(t.publishes == 3, "A longer method sharing a prefix publishes");
}

void test_idle_tick_costs_nothing() {
  std::cout << "\n=== Testing idle-tick frame floor ===" << std::endl;

  // The measured shape of #129: 495 s idle, one subscriber, 50 publishes on
  // each of the four entities and not one distinct value among them. The same
  // window must now cost four frames — one per entity, on the first tick.
  FakeSensor confidence, demand_level, session_duration;
  FakeTextSensor method;

  for (int tick = 0; tick < 50; tick++) {  // 50 x 10s ~ the measured window
    publish_sensor_if_changed(&confidence, 100.0f);
    publish_sensor_if_changed(&demand_level, 0.0f);
    publish_sensor_if_changed(&session_duration, 0.0f);
    publish_text_sensor_if_changed(&method, "deterministic_idle");
  }

  int total = confidence.publishes + demand_level.publishes +
              session_duration.publishes + method.publishes;
  TEST_ASSERT(total == 4, "An idle 495 s window costs 4 frames, not 200");
}

void test_running_session_still_ticks() {
  std::cout << "\n=== Testing a live session is not silenced ===" << std::endl;

  // Session duration climbs every tick while a draw is in progress, so the gate
  // must be transparent there — the win is only on the idle 0 s stream.
  FakeSensor session_duration;
  for (int tick = 0; tick < 6; tick++) {
    publish_sensor_if_changed(&session_duration, tick * 10.0f);
  }
  TEST_ASSERT(session_duration.publishes == 6,
              "Every tick of a running session publishes");

  // ...and once the session closes, the trailing zeros collapse to one.
  for (int tick = 0; tick < 6; tick++) {
    publish_sensor_if_changed(&session_duration, 0.0f);
  }
  TEST_ASSERT(session_duration.publishes == 7,
              "Session end publishes 0 s once, then stays quiet");
}

void test_predicate_directly() {
  std::cout << "\n=== Testing float_state_differs predicate ===" << std::endl;

  TEST_ASSERT(float_state_differs(false, NAN, NAN),
              "An entity with no state always differs");
  TEST_ASSERT(!float_state_differs(true, 1.0f, 1.0f), "Equal values agree");
  TEST_ASSERT(float_state_differs(true, 1.0f, 1.0001f),
              "Values differing below display precision still differ");
  TEST_ASSERT(!float_state_differs(true, NAN, NAN), "NaN equals NaN here");
  TEST_ASSERT(float_state_differs(true, NAN, 0.0f), "NaN differs from 0.0");
  TEST_ASSERT(float_state_differs(true, 0.0f, NAN), "0.0 differs from NaN");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "DHW Demand Publish Gate Tests (issue #129)" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_sensor_gate();
  test_force_update_opts_out();
  test_text_sensor_gate();
  test_idle_tick_costs_nothing();
  test_running_session_still_ticks();
  test_predicate_directly();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
