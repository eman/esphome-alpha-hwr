#include <iostream>
#include <cstdint>
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/transport.h"
#include "../components/alpha_hwr/session.h"

// Define the mocked millis symbol
uint32_t mock_millis = 0;

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

void test_schedule_initial_state() {
  esphome::alpha_hwr::core::Transport transport;
  esphome::alpha_hwr::core::Session session;
  esphome::alpha_hwr::services::ScheduleService service(transport, session);
  TEST_ASSERT(true, "ScheduleService constructs cleanly with mock ESPHome");
}

int main() {
  std::cout << "=== Schedule Service Tests ===" << std::endl;
  test_schedule_initial_state();
  
  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
    return 1;
  }
}
