/**
 * Unit tests for Transport::try_dispatch_response()'s Class 3/7 wildcard
 * response matching logic (fix #46, hardened per review feedback on PR #50).
 *
 * These tests verify the matching decision without ESP32 or BLE dependencies
 * by extracting the pure logic into testable assertions.
 */

#include "../components/alpha_hwr/response_match.h"

#include <iostream>
#include <cstdint>

// Test result tracking (same framework as test_protocol.cpp)
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

/**
 * The predicate under test now lives in production
 * (components/alpha_hwr/response_match.h) and is called by
 * Transport::try_dispatch_response(). This file used to carry a hand-written
 * copy of it, which meant the test could keep passing while the shipped
 * predicate changed underneath it.
 *
 * The rule: a Class 3/7 wildcard match (expect_obj_id == 0 && expect_sub_id
 * == 0) must only be satisfied by a response of the *same class* the queued
 * command was actually sent as. Without that, an unrelated Class 10 telemetry
 * notification arriving while a Class 3 command (e.g. enable_remote_mode()) is
 * in flight could be mistaken for that command's ACK (PR #50 review).
 */
using esphome::alpha_hwr::protocol::class3_or_7_wildcard_matches;
using esphome::alpha_hwr::protocol::ignore_unrelated_while_awaiting_class3_or_7;

// ============================================================================
// Test: a Class 3 ACK correctly matches a queued Class 3 command
// ============================================================================
void test_class3_ack_matches_queued_class3_command() {
  std::cout << "\n=== Testing Class 3 ACK Matches Queued Class 3 Command ===" << std::endl;

  TEST_ASSERT(class3_or_7_wildcard_matches(0x03, 0x03, /*wildcard_expect=*/true),
              "Class 3 response matches a queued Class 3 wildcard command");
}

// ============================================================================
// Test: an unrelated Class 10 packet does NOT satisfy a queued Class 3 command
// (the core regression this fix addresses -- reported in PR #50 review)
// ============================================================================
void test_class10_packet_does_not_satisfy_queued_class3_command() {
  std::cout << "\n=== Testing Class 10 Packet Does Not Satisfy Queued Class 3 Command (Review Feedback) ===" << std::endl;

  TEST_ASSERT(!class3_or_7_wildcard_matches(0x03, 0x0A, /*wildcard_expect=*/true),
              "An unrelated Class 10 telemetry notification is not mistaken for a Class 3 command's ACK");
}

// ============================================================================
// Test: an unrelated Class 3 packet does NOT satisfy a queued Class 7 command
// ============================================================================
void test_class3_packet_does_not_satisfy_queued_class7_command() {
  std::cout << "\n=== Testing Class 3 Packet Does Not Satisfy Queued Class 7 Command ===" << std::endl;

  TEST_ASSERT(!class3_or_7_wildcard_matches(0x07, 0x03, /*wildcard_expect=*/true),
              "An unrelated Class 3 packet is not mistaken for a Class 7 command's response");
}

// ============================================================================
// Test: matching requires the wildcard expectation (expect_obj_id/sub_id == 0)
// ============================================================================
void test_class3_match_requires_wildcard_expectation() {
  std::cout << "\n=== Testing Class 3 Match Requires Wildcard Expectation ===" << std::endl;

  TEST_ASSERT(!class3_or_7_wildcard_matches(0x03, 0x03, /*wildcard_expect=*/false),
              "Same-class Class 3 response does not match without the wildcard expectation");
}

// ============================================================================
// Main
// ============================================================================
int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Transport Class 3/7 Wildcard Matching Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_class3_ack_matches_queued_class3_command();
  test_class10_packet_does_not_satisfy_queued_class3_command();
  test_class3_packet_does_not_satisfy_queued_class7_command();
  test_class3_match_requires_wildcard_expectation();

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  if (tests_failed == 0) {
    std::cout << "\n✓ ALL TESTS PASSED!" << std::endl;
    return 0;
  } else {
    std::cout << "\n✗ SOME TESTS FAILED!" << std::endl;
    return 1;
  }
}
