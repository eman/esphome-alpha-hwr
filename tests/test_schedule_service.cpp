#include <iostream>
#include <vector>
#include <cstdint>
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/transport.h"
#include "../components/alpha_hwr/session.h"

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

void test_schedule_write_payload() {
  std::cout << "\n=== Testing ScheduleService Write Payload ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  esphome::alpha_hwr::core::Session session;
  esphome::alpha_hwr::services::ScheduleService service(transport, session);

  // Intercept the BLE write from Transport
  std::vector<std::vector<uint8_t>> sent_chunks;
  transport.set_write_callback([&sent_chunks](const uint8_t* data, size_t len) -> bool {
    sent_chunks.push_back(std::vector<uint8_t>(data, data + len));
    return true;
  });

  // Set session to ready state to allow writes
  session.on_authenticated();

  // Write a simple schedule (layer 1)
  std::vector<esphome::alpha_hwr::ScheduleEntry> entries;
  entries.push_back(esphome::alpha_hwr::ScheduleEntry("Monday", 6, 0, 8, 0)); // Mon 6am-8am
  
  bool success = service.write_entries(entries, 1);
  TEST_ASSERT(success, "write_entries() accepted the schedule");

  // Run the FSM to dispatch all chunks (53 bytes total -> 3 chunks)
  mock_millis += 50;
  transport.loop();
  mock_millis += 51;
  transport.loop();
  mock_millis += 51;
  transport.loop();

  TEST_ASSERT(sent_chunks.size() == 3, "Schedule packet was split into 3 chunks");

  // Reassemble the packet chunks to inspect the APDU
  std::vector<uint8_t> full_packet;
  for (const auto& chunk : sent_chunks) {
    full_packet.insert(full_packet.end(), chunk.begin(), chunk.end());
  }
  std::cout << "Packet size: " << full_packet.size() << " bytes" << std::endl;
  std::cout << "Bytes: ";
  for (uint8_t b : full_packet) {
    printf("%02X ", b);
  }
  std::cout << std::endl;

  TEST_ASSERT(full_packet.size() == 59, "Full GENI packet length is 59 bytes (53 APDU + 6 header/footer)");

  // Check the payload details
  // APDU: Class=10 (0x0A), OpSpec=5 (0xB3), Obj=84 (0x54), Sub=1001 (0x03E9)
  // GENI format: [0x27][LEN][DST][SRC] + APDU
  TEST_ASSERT(full_packet[4] == 0x0A, "Class 10 (Settings)");
  TEST_ASSERT(full_packet[5] == 0xB3, "OpSpec 5 (Write)");
  TEST_ASSERT(full_packet[6] == 0x54, "Object 84 (ClockProgram)");
  TEST_ASSERT(full_packet[7] == 0x03 && full_packet[8] == 0xE9, "SubID 1001 (Layer 1)");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Schedule Service Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;
  
  test_schedule_write_payload();
  
  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}
