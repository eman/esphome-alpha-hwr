#include <iostream>
#include <vector>
#include <cstdint>
#include "../components/alpha_hwr/transport.h"

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

void test_transport_chunking() {
  std::cout << "\n=== Testing Transport BLE Chunking ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  
  std::vector<std::vector<uint8_t>> sent_chunks;
  transport.set_write_callback([&sent_chunks](const uint8_t* data, size_t len) -> bool {
    sent_chunks.push_back(std::vector<uint8_t>(data, data + len));
    return true;
  });

  // Create a 53-byte payload (e.g. Schedule write packet)
  std::vector<uint8_t> large_packet(53, 0xAA);
  transport.send_command(large_packet);

  // Tick 1: Advance time by 50ms so pacing allows the first chunk, then loop
  mock_millis += 50;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 1, "First chunk sent immediately after initial pacing");
  TEST_ASSERT(sent_chunks[0].size() == 20, "First chunk is exactly 20 bytes");

  // Tick 2 (no time passed): Should NOT send second chunk yet due to 50ms pacing
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 1, "Pacing prevented immediate second chunk");

  // Tick 3 (+51ms): Should send second chunk
  mock_millis += 51;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 2, "Second chunk sent after pacing delay");
  // Guard the index: without it the earlier size()==1 assertion leaves cppcheck
  // (correctly) unable to rule out an out-of-bounds read here.
  if (sent_chunks.size() > 1) {
    TEST_ASSERT(sent_chunks[1].size() == 20, "Second chunk is exactly 20 bytes");
  } else {
    TEST_ASSERT(false, "Second chunk is exactly 20 bytes (no second chunk sent)");
  }

  // Tick 4 (+51ms): Should send final 13 bytes
  mock_millis += 51;
  transport.loop();
  TEST_ASSERT(sent_chunks.size() == 3, "Final chunk sent");
  if (sent_chunks.size() > 2) {
    TEST_ASSERT(sent_chunks[2].size() == 13, "Final chunk is exactly 13 bytes");
  } else {
    TEST_ASSERT(false, "Final chunk is exactly 13 bytes (no final chunk sent)");
  }
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Transport FSM Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;
  
  test_transport_chunking();
  
  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}

