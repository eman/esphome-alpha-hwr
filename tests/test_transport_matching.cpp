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
 * The rule: a wildcard match (expect_type_low_ver == 0 && expect_type_high
 * == 0) must only be satisfied by a response of the *same class* the queued
 * command was actually sent as. Without that, an unrelated Class 10 telemetry
 * notification arriving while a Class 3 command (e.g. send_remote_mode_command()) is
 * in flight could be mistaken for that command's ACK (PR #50 review).
 *
 * The wildcard-matched set was {3, 7} and is now {2, 3, 5, 7, 11}, so that the
 * opening sequence's four reads can be matched on their replies instead of
 * sent blind on timers (issue #174). That same-class term is the whole safety
 * argument for the three new members, which makes it the thing this file has
 * to pin hardest -- for every member, not just for the two it was written for.
 */
using esphome::alpha_hwr::protocol::class_wildcard_matches;
using esphome::alpha_hwr::protocol::ignore_unrelated_while_awaiting_wildcard_class;
using esphome::alpha_hwr::protocol::is_wildcard_matched_class;

// ============================================================================
// Test: a Class 3 ACK correctly matches a queued Class 3 command
// ============================================================================
void test_class3_ack_matches_queued_class3_command() {
  std::cout << "\n=== Testing Class 3 ACK Matches Queued Class 3 Command ===" << std::endl;

  TEST_ASSERT(class_wildcard_matches(0x03, 0x03, /*wildcard_expect=*/true),
              "Class 3 response matches a queued Class 3 wildcard command");
}

// ============================================================================
// Test: an unrelated Class 10 packet does NOT satisfy a queued Class 3 command
// (the core regression this fix addresses -- reported in PR #50 review)
// ============================================================================
void test_class10_packet_does_not_satisfy_queued_class3_command() {
  std::cout << "\n=== Testing Class 10 Packet Does Not Satisfy Queued Class 3 Command (Review Feedback) ===" << std::endl;

  TEST_ASSERT(!class_wildcard_matches(0x03, 0x0A, /*wildcard_expect=*/true),
              "An unrelated Class 10 telemetry notification is not mistaken for a Class 3 command's ACK");
}

// ============================================================================
// Test: an unrelated Class 3 packet does NOT satisfy a queued Class 7 command
// ============================================================================
void test_class3_packet_does_not_satisfy_queued_class7_command() {
  std::cout << "\n=== Testing Class 3 Packet Does Not Satisfy Queued Class 7 Command ===" << std::endl;

  TEST_ASSERT(!class_wildcard_matches(0x07, 0x03, /*wildcard_expect=*/true),
              "An unrelated Class 3 packet is not mistaken for a Class 7 command's response");
}

// ============================================================================
// Test: matching requires the wildcard expectation (expect_type_low_ver/type_high == 0)
// ============================================================================
void test_class3_match_requires_wildcard_expectation() {
  std::cout << "\n=== Testing Class 3 Match Requires Wildcard Expectation ===" << std::endl;

  TEST_ASSERT(!class_wildcard_matches(0x03, 0x03, /*wildcard_expect=*/false),
              "Same-class Class 3 response does not match without the wildcard expectation");
}

// ============================================================================
// The wildcard-matched set itself.
//
// Pinned exhaustively over all 256 class bytes rather than by asserting the
// five members individually. Membership is the entire safety argument for this
// predicate, and a per-member test cannot fail for the mutation that actually
// matters -- a *sixth* class being admitted. Class 10 is the one that must
// never join: telemetry notifications arrive unsolicited and continuously, so
// admitting it would let one be taken for the answer to any queued Class 10
// command. The loop below fails for that and for every other stray addition.
// ============================================================================
void test_wildcard_matched_set_is_exactly_these_five() {
  std::cout << "\n=== Testing The Wildcard-Matched Class Set ===" << std::endl;

  bool set_is_exact = true;
  int members = 0;
  for (int c = 0; c <= 0xFF; c++) {
    const uint8_t class_byte = static_cast<uint8_t>(c);
    const bool expected = (c == 0x02) || (c == 0x03) || (c == 0x05) || (c == 0x07) || (c == 0x0B);
    if (is_wildcard_matched_class(class_byte) != expected) {
      std::cout << "  class 0x" << std::hex << c << std::dec
                << ": expected " << expected << ", got "
                << is_wildcard_matched_class(class_byte) << std::endl;
      set_is_exact = false;
    }
    if (expected) members++;
  }

  TEST_ASSERT(set_is_exact,
              "The wildcard-matched set is exactly {2, 3, 5, 7, 11} across all 256 class bytes");
  TEST_ASSERT(members == 5, "...and that is five classes, not four or six");
  TEST_ASSERT(!is_wildcard_matched_class(0x0A),
              "Class 10 is NOT wildcard-matched -- it is the class the pump volunteers unbidden");
}

// ============================================================================
// Test: each newly admitted class matches its own queued command, and nothing
// else. The opening sequence (issue #174) sends one Class 2 read and two INFO
// queries on Classes 5 and 11; before this change none of their replies could
// be matched at all.
// ============================================================================
void test_new_classes_match_their_own_queued_command() {
  std::cout << "\n=== Testing Classes 2, 5 and 11 Match Their Own Commands ===" << std::endl;

  TEST_ASSERT(class_wildcard_matches(0x02, 0x02, /*wildcard_expect=*/true),
              "A Class 2 reply matches a queued Class 2 read (the identity read)");
  TEST_ASSERT(class_wildcard_matches(0x05, 0x05, /*wildcard_expect=*/true),
              "A Class 5 reply matches a queued Class 5 INFO query");
  TEST_ASSERT(class_wildcard_matches(0x0B, 0x0B, /*wildcard_expect=*/true),
              "A Class 11 reply matches a queued Class 11 INFO query");
}

void test_class10_notification_cannot_answer_any_new_class() {
  std::cout << "\n=== Testing Class 10 Notifications Cannot Answer The New Classes ===" << std::endl;

  // The failure this whole predicate exists to prevent (PR #50 review), now
  // asserted for each class admitted since. A control-mode notification lands
  // during the opening sequence on this pump, so this is the live case.
  TEST_ASSERT(!class_wildcard_matches(0x02, 0x0A, /*wildcard_expect=*/true),
              "A Class 10 notification is not mistaken for the Class 2 read's reply");
  TEST_ASSERT(!class_wildcard_matches(0x05, 0x0A, /*wildcard_expect=*/true),
              "A Class 10 notification is not mistaken for the Class 5 INFO reply");
  TEST_ASSERT(!class_wildcard_matches(0x0B, 0x0A, /*wildcard_expect=*/true),
              "A Class 10 notification is not mistaken for the Class 11 INFO reply");
}

void test_new_classes_do_not_answer_each_other() {
  std::cout << "\n=== Testing The New Classes Do Not Answer Each Other ===" << std::endl;

  // Both INFO queries go out back to back with nothing between them, so a
  // predicate that matched any set member against any other would let the
  // Class 11 reply satisfy the queued Class 5 query and vice versa. Being in
  // the set is not enough; the classes have to be equal.
  TEST_ASSERT(!class_wildcard_matches(0x05, 0x0B, /*wildcard_expect=*/true),
              "A Class 11 reply does not answer the queued Class 5 INFO query");
  TEST_ASSERT(!class_wildcard_matches(0x0B, 0x05, /*wildcard_expect=*/true),
              "A Class 5 reply does not answer the queued Class 11 INFO query");
  TEST_ASSERT(!class_wildcard_matches(0x02, 0x03, /*wildcard_expect=*/true),
              "A Class 3 ACK does not answer the queued Class 2 read");
  TEST_ASSERT(!class_wildcard_matches(0x02, 0x07, /*wildcard_expect=*/true),
              "A Class 7 string reply does not answer the queued Class 2 read");
}

void test_new_classes_still_require_the_wildcard_expectation() {
  std::cout << "\n=== Testing The New Classes Honour The Wildcard Expectation ===" << std::endl;

  TEST_ASSERT(!class_wildcard_matches(0x02, 0x02, /*wildcard_expect=*/false),
              "A Class 2 reply does not match when the command expects specific type bytes");
  TEST_ASSERT(!class_wildcard_matches(0x05, 0x05, /*wildcard_expect=*/false),
              "A Class 5 reply does not match when the command expects specific type bytes");
  TEST_ASSERT(!class_wildcard_matches(0x0B, 0x0B, /*wildcard_expect=*/false),
              "A Class 11 reply does not match when the command expects specific type bytes");
}

void test_ignore_gate_engages_for_the_new_classes() {
  std::cout << "\n=== Testing The Ignore Gate Covers The New Classes ===" << std::endl;

  // Without this, a Class 10 telemetry notification arriving while one of the
  // opening reads is in flight falls through to the Class 10 DataObject path
  // below it, which is exactly where it could be matched by accident.
  TEST_ASSERT(ignore_unrelated_while_awaiting_wildcard_class(0x02, 0x0A),
              "A Class 10 notification is ignored while the Class 2 read is in flight");
  TEST_ASSERT(ignore_unrelated_while_awaiting_wildcard_class(0x05, 0x0A),
              "A Class 10 notification is ignored while the Class 5 INFO query is in flight");
  TEST_ASSERT(ignore_unrelated_while_awaiting_wildcard_class(0x0B, 0x0A),
              "A Class 10 notification is ignored while the Class 11 INFO query is in flight");
}

// ============================================================================
// Main
// ============================================================================

// ============================================================================
// The second half of the gate: while a Class 3/7 command is in flight, a packet
// that is neither must be ignored outright rather than falling through to the
// Class 10 wildcard path, where it could be matched by accident.
//
// This predicate was extracted alongside the one above but initially had no
// assertions, so a regression in it would have left this file green.
// ============================================================================
void test_class10_ignored_while_awaiting_class3() {
  std::cout << "\n=== Testing Unrelated Packet Ignored While Awaiting Class 3/7 ===" << std::endl;

  TEST_ASSERT(ignore_unrelated_while_awaiting_wildcard_class(0x03, 0x0A),
              "Class 10 packet is ignored while a Class 3 command is in flight");
  TEST_ASSERT(ignore_unrelated_while_awaiting_wildcard_class(0x07, 0x0A),
              "Class 10 packet is ignored while a Class 7 command is in flight");
}

void test_class3_and_7_not_ignored_for_each_other() {
  std::cout << "\n=== Testing Class 3/7 Responses Are Not Ignored ===" << std::endl;

  // Both are handled by the match predicate above, so neither may be dropped
  // here -- the mismatch case (Class 3 answer to a queued Class 7) has to reach
  // that predicate to be rejected for the right reason.
  TEST_ASSERT(!ignore_unrelated_while_awaiting_wildcard_class(0x03, 0x03),
              "A Class 3 response is not ignored while awaiting Class 3");
  TEST_ASSERT(!ignore_unrelated_while_awaiting_wildcard_class(0x03, 0x07),
              "A Class 7 response is not ignored while awaiting Class 3");
  TEST_ASSERT(!ignore_unrelated_while_awaiting_wildcard_class(0x07, 0x03),
              "A Class 3 response is not ignored while awaiting Class 7");
}

void test_no_ignoring_when_queued_command_is_class10() {
  std::cout << "\n=== Testing Gate Is Inert For Class 10 Commands ===" << std::endl;

  // The gate must only engage for queued Class 3/7 commands. If it fired for a
  // queued Class 10 command it would drop that command's own answer.
  TEST_ASSERT(!ignore_unrelated_while_awaiting_wildcard_class(0x0A, 0x0A),
              "A Class 10 response is not ignored while awaiting Class 10");
  TEST_ASSERT(!ignore_unrelated_while_awaiting_wildcard_class(0x0A, 0x03),
              "A Class 3 packet is not ignored while awaiting Class 10");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Transport Wildcard Class Matching Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_class3_ack_matches_queued_class3_command();
  test_class10_packet_does_not_satisfy_queued_class3_command();
  test_class3_packet_does_not_satisfy_queued_class7_command();
  test_class3_match_requires_wildcard_expectation();
  test_class10_ignored_while_awaiting_class3();
  test_class3_and_7_not_ignored_for_each_other();
  test_no_ignoring_when_queued_command_is_class10();
  test_wildcard_matched_set_is_exactly_these_five();
  test_new_classes_match_their_own_queued_command();
  test_class10_notification_cannot_answer_any_new_class();
  test_new_classes_do_not_answer_each_other();
  test_new_classes_still_require_the_wildcard_expectation();
  test_ignore_gate_engages_for_the_new_classes();

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
