// Host tests for the publish-on-change gates used by the polled template
// control entities (issue #127).
//
// The regression these guard: ESPHome's number/select publish_state() has no
// dedup, so a lambda that re-derives a cached pump value every update_interval
// emits an API state frame to every subscriber whether or not the value moved.

#include <cmath>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/publish_gate.h"

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

// Stand-ins for esphome::number::Number / esphome::select::Select, exposing
// only what the gates touch (the gates are templated precisely so the header
// stays free of the entity SDK).
struct FakeNumber {
  float state{NAN};
  bool has_state() const { return this->has_state_; }
  void publish(float value) {
    this->state = value;
    this->has_state_ = true;
    this->publishes++;
  }
  bool has_state_{false};
  int publishes{0};
};

struct FakeSelect {
  bool has_state() const { return this->has_state_; }
  const std::string &current_option() const { return this->option_; }
  void publish(const std::string &option) {
    this->option_ = option;
    this->has_state_ = true;
    this->publishes++;
  }
  std::string option_;
  bool has_state_{false};
  int publishes{0};
};

// One `update_interval` tick of a polled template entity: the lambda hands its
// value to the gate, and ESPHome publishes only when the gate returns one.
void poll(FakeNumber &entity, float value) {
  auto out = esphome::alpha_hwr::publish_number_if_changed(&entity, value);
  if (out.has_value())
    entity.publish(*out);
}

void poll(FakeSelect &entity, const std::string &option) {
  auto out = esphome::alpha_hwr::publish_option_if_changed(&entity, option);
  if (out.has_value())
    entity.publish(*out);
}

void test_number_gate() {
  std::cout << "\n=== Testing number publish gate ===" << std::endl;

  FakeNumber n;
  poll(n, 35.0f);
  TEST_ASSERT(n.publishes == 1, "First value publishes (entity was unknown)");

  poll(n, 35.0f);
  poll(n, 35.0f);
  TEST_ASSERT(n.publishes == 1, "Repeats of the same value are suppressed");

  poll(n, 36.0f);
  TEST_ASSERT(n.publishes == 2 && n.state == 36.0f, "A changed value publishes");

  // NAN is a real state here ("setpoint not applicable in this control mode"),
  // and NAN != NAN, so it has to be compared by identity rather than by value.
  poll(n, NAN);
  TEST_ASSERT(n.publishes == 3 && std::isnan(n.state), "Transition into NAN publishes once");

  poll(n, NAN);
  poll(n, NAN);
  TEST_ASSERT(n.publishes == 3, "Repeated NAN is suppressed (NAN != NAN must not leak through)");

  poll(n, 36.0f);
  TEST_ASSERT(n.publishes == 4 && n.state == 36.0f, "Transition out of NAN publishes");

  // 0.0 vs NAN must not be conflated, and a real 0 still publishes once.
  FakeNumber z;
  poll(z, NAN);
  poll(z, 0.0f);
  poll(z, 0.0f);
  TEST_ASSERT(z.publishes == 2 && z.state == 0.0f, "0.0 is distinct from NAN and publishes once");
}

void test_select_gate() {
  std::cout << "\n=== Testing select publish gate ===" << std::endl;

  FakeSelect s;
  poll(s, "Temperature Control");
  TEST_ASSERT(s.publishes == 1, "First option publishes (entity was unknown)");

  poll(s, "Temperature Control");
  poll(s, "Temperature Control");
  TEST_ASSERT(s.publishes == 1, "Repeats of the same option are suppressed");

  poll(s, "Constant Speed");
  TEST_ASSERT(s.publishes == 2 && s.current_option() == "Constant Speed", "A changed option publishes");
}

void test_steady_state_frame_floor() {
  std::cout << "\n=== Testing steady-state publish floor ===" << std::endl;

  // The measured shape of #127: 'Pump Control Mode' emitted one frame per poll
  // (822 of them over ~15 min), always "Temperature Control"; the setpoints did
  // the same at 5s. Over a minute of unchanged state the whole control package
  // must now cost zero frames beyond the first publish of each entity.
  FakeSelect mode;
  FakeNumber setpoints[9];

  for (int tick = 0; tick < 12; tick++) {  // 12 x 5s = one minute
    poll(mode, "Temperature Control");
    poll(setpoints[0], 35.0f);   // Temperature Range Min
    poll(setpoints[1], 38.9f);   // Temperature Range Max
    poll(setpoints[2], 5.0f);    // Cycle Time ON
    poll(setpoints[3], 15.0f);   // Cycle Time OFF
    poll(setpoints[4], 0.23f);   // Cycle Flow
    for (int i = 5; i < 9; i++)  // the four setpoints inactive in this mode
      poll(setpoints[i], NAN);
  }

  int total = mode.publishes;
  for (const auto &n : setpoints)
    total += n.publishes;
  TEST_ASSERT(total == 10, "One minute of unchanged state costs 10 frames, not 401");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Publish Gate Tests (issue #127)" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_number_gate();
  test_select_gate();
  test_steady_state_frame_floor();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
