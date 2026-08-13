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


// ---------------------------------------------------------------------------
// Reassembly: a continuation fragment may legitimately begin with 0x24/0x27,
// which are ordinary payload bytes mid-frame. Treating such a fragment as a new
// packet discarded the frame in flight and dispatched the fragment as a runt --
// observed 8 times in the reference captures. These pin the rule and its
// staleness escape hatch.
// ---------------------------------------------------------------------------

// A 27-byte Class 10 frame whose second fragment starts with `lead`.
static std::vector<uint8_t> frame_with_lead_byte(uint8_t lead) {
  std::vector<uint8_t> f{0x24, 0x00, 0x00, 0x07, 0x0A, 0x03,
                         0x00, 0x00, 0xDE, 0x01};
  while (f.size() < 20)
    f.push_back(0x11);          // filler inside the first 20-byte fragment
  f.push_back(lead);            // first byte of the continuation fragment
  while (f.size() < 25)
    f.push_back(0x22);
  f.push_back(0xAA);
  f.push_back(0xBB);            // CRC placeholder
  f[1] = static_cast<uint8_t>(f.size() - 4);
  return f;
}

static void test_continuation_fragment_leading_frame_start(uint8_t lead,
                                                           const char *label) {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto frame = frame_with_lead_byte(lead);
  transport.on_notification(frame.data(), 20);
  transport.on_notification(frame.data() + 20, frame.size() - 20);

  TEST_ASSERT(packets.size() == 1, label);
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == frame.size(),
                "  ...and it is the whole frame, not a runt");
  }
}

static void test_reassembly_continuation_0x24() {
  test_continuation_fragment_leading_frame_start(
      0x24, "Continuation starting 0x24 does not restart reassembly");
}

static void test_reassembly_continuation_0x27() {
  test_continuation_fragment_leading_frame_start(
      0x27, "Continuation starting 0x27 does not restart reassembly");
}

// A frame whose tail never arrives must not swallow the next frame forever.
static void test_reassembly_stale_partial_recovers() {
  esphome::alpha_hwr::core::Transport transport;
  std::vector<std::vector<uint8_t>> packets;
  transport.set_packet_callback([&packets](const uint8_t *d, size_t n) {
    packets.push_back(std::vector<uint8_t>(d, d + n));
  });

  auto truncated = frame_with_lead_byte(0x11);
  transport.on_notification(truncated.data(), 20);   // tail never sent

  mock_millis += 2000;                                // past the staleness bound

  std::vector<uint8_t> whole{0x24, 0x00, 0x00, 0x07, 0x0A, 0x03,
                             0x00, 0x00, 0xDE, 0x01, 0x01, 0xAA, 0xBB};
  whole[1] = static_cast<uint8_t>(whole.size() - 4);
  transport.on_notification(whole.data(), whole.size());

  TEST_ASSERT(packets.size() == 1,
              "A stale partial frame is abandoned so the next frame parses");
  if (packets.size() == 1) {
    TEST_ASSERT(packets[0].size() == whole.size(),
                "  ...and the recovered packet is the new frame alone");
  }
}

int main() {
  test_reassembly_continuation_0x24();
  test_reassembly_continuation_0x27();
  test_reassembly_stale_partial_recovers();

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

