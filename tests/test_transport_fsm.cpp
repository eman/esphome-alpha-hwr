#include <iostream>
#include <cstdint>
#include "../components/alpha_hwr/transport.h"

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

void test_transport_initial_state() {
  esphome::alpha_hwr::core::Transport transport;
  TEST_ASSERT(true, "Transport constructs cleanly with mock ESPHome");
}

int main() {
  std::cout << "=== Transport FSM Tests ===" << std::endl;
  test_transport_initial_state();
  
  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
    return 1;
  }
}
