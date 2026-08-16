// Host tests for the handshake stage gate (auth_gate.h).
//
// The gate is the decision issue #174 turns on: may the handshake leave a
// stage. Extracted into its own header for the same reason link_watchdog.h and
// response_match.h were — the judgement is in the predicate, and a predicate
// tested here is the shipped one rather than a copy.

#include <cstddef>
#include <cstdint>
#include <iostream>

#include "../components/alpha_hwr/auth_gate.h"

using esphome::alpha_hwr::core::auth_frame_answers_stage;
using esphome::alpha_hwr::core::auth_stage_for_reply_class;
using esphome::alpha_hwr::core::auth_stage_gate;
using esphome::alpha_hwr::core::AuthGate;
using esphome::alpha_hwr::core::AUTH_GATE_MAX_WAITS;

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

// ── The gate decision ────────────────────────────────────────────────────────
void test_a_fully_answered_stage_proceeds_immediately() {
  std::cout << "\n=== A fully answered stage proceeds at the floor ==="
            << std::endl;

  TEST_ASSERT(auth_stage_gate(3, 3, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "3 of 3 replies: proceed without waiting");
  TEST_ASSERT(auth_stage_gate(5, 5, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "5 of 5 replies: proceed without waiting");
  TEST_ASSERT(auth_stage_gate(2, 2, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "2 of 2 replies: proceed without waiting");
}

void test_a_short_stage_waits() {
  std::cout << "\n=== A stage still missing replies waits ===" << std::endl;

  TEST_ASSERT(auth_stage_gate(4, 5, 0, AUTH_GATE_MAX_WAITS) == AuthGate::WAIT,
              "4 of 5: wait — this is the case the capture caught, the fifth "
              "reply arriving 81 ms after the old code had moved on");
  TEST_ASSERT(auth_stage_gate(0, 3, 0, AUTH_GATE_MAX_WAITS) == AuthGate::WAIT,
              "0 of 3: wait");
  TEST_ASSERT(auth_stage_gate(4, 5, AUTH_GATE_MAX_WAITS - 1,
                              AUTH_GATE_MAX_WAITS) == AuthGate::WAIT,
              "Still waiting on the last tick before the ceiling");
}

void test_the_ceiling_fails_open() {
  std::cout << "\n=== The ceiling proceeds anyway, and says so ===" << std::endl;

  TEST_ASSERT(auth_stage_gate(4, 5, AUTH_GATE_MAX_WAITS,
                              AUTH_GATE_MAX_WAITS) == AuthGate::PROCEED_UNANSWERED,
              "At the ceiling a short stage proceeds — the handshake must not "
              "stall on a pump that answers differently");
  TEST_ASSERT(auth_stage_gate(0, 3, AUTH_GATE_MAX_WAITS,
                              AUTH_GATE_MAX_WAITS) == AuthGate::PROCEED_UNANSWERED,
              "A stage answered not at all proceeds too, distinguishably");
  TEST_ASSERT(auth_stage_gate(0, 3, AUTH_GATE_MAX_WAITS + 7,
                              AUTH_GATE_MAX_WAITS) == AuthGate::PROCEED_UNANSWERED,
              "And stays proceeding past the ceiling rather than wrapping back "
              "into WAIT");

  // The two PROCEED verdicts must stay distinguishable: one is the pump
  // answering, the other is the ceiling expiring, and only the second is worth
  // a warning.
  TEST_ASSERT(AuthGate::PROCEED_ANSWERED != AuthGate::PROCEED_UNANSWERED,
              "Answered and unanswered are different verdicts, not one "
              "'proceed'");
}

void test_extra_replies_do_not_hold_the_gate() {
  std::cout << "\n=== A surplus of replies satisfies the gate ===" << std::endl;

  TEST_ASSERT(auth_stage_gate(6, 5, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "6 of 5 proceeds — a duplicate or an unrelated same-class frame "
              "must not hold the gate open to its ceiling");
  TEST_ASSERT(auth_stage_gate(255, 5, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "And a saturated counter proceeds rather than wrapping to zero");
}

void test_a_zero_packet_stage_is_answered() {
  std::cout << "\n=== A stage that sent nothing is trivially answered ==="
            << std::endl;
  // Not reachable from the three real stages, but the predicate must not
  // invent a wait for a stage with nothing outstanding.
  TEST_ASSERT(auth_stage_gate(0, 0, 0, AUTH_GATE_MAX_WAITS) ==
                  AuthGate::PROCEED_ANSWERED,
              "0 of 0 proceeds immediately");
}

// ── Which reply belongs to which stage ───────────────────────────────────────
void test_reply_classes_map_to_stages() {
  std::cout << "\n=== Reply classes map to the stage that sent them ==="
            << std::endl;

  TEST_ASSERT(auth_stage_for_reply_class(0x02) == 1,
              "Class 2 answers the stage 1 legacy burst");
  TEST_ASSERT(auth_stage_for_reply_class(0x0A) == 2,
              "Class 10 answers the stage 2 unlock burst");
  TEST_ASSERT(auth_stage_for_reply_class(0x05) == 3,
              "Class 5 answers EXT_1");
  TEST_ASSERT(auth_stage_for_reply_class(0x0B) == 3,
              "Class 11 answers EXT_2 — both extensions count to stage 3");

  TEST_ASSERT(auth_stage_for_reply_class(0x03) == 0,
              "Class 3 command ACKs belong to no handshake stage");
  TEST_ASSERT(auth_stage_for_reply_class(0x07) == 0,
              "Nor do Class 7 device-info replies");
  TEST_ASSERT(auth_stage_for_reply_class(0x00) == 0, "Nor class 0");
}

// ── What counts as a reply ───────────────────────────────────────────────────
static bool answers(uint8_t start, uint8_t class_byte, uint8_t stage) {
  const uint8_t frame[] = {start, 0x07, 0xF8, 0xE7, class_byte, 0x03, 0x00};
  return auth_frame_answers_stage(frame, sizeof(frame), stage);
}

void test_only_response_frames_count() {
  std::cout << "\n=== Only response frames count as replies ===" << std::endl;

  TEST_ASSERT(answers(0x24, 0x02, 1),
              "A 0x24 Class 2 frame answers stage 1");
  TEST_ASSERT(!answers(0x27, 0x02, 1),
              "A 0x27 frame does not — that is the request direction, and it "
              "is echoed on this link, so counting it would let our own packet "
              "stand in for the answer to itself");
  TEST_ASSERT(!answers(0x00, 0x02, 1),
              "Nor does a frame with neither start byte");
}

void test_a_reply_cannot_be_credited_to_a_future_stage() {
  std::cout << "\n=== A reply is never credited ahead of the stage in flight ==="
            << std::endl;

  TEST_ASSERT(!answers(0x24, 0x0A, 1),
              "A Class 10 frame during stage 1 is not a stage 2 answer — "
              "stage 2 has not sent anything yet, and Class 10 is also the "
              "class of ordinary telemetry");
  TEST_ASSERT(!answers(0x24, 0x05, 2),
              "Nor is a Class 5 frame during stage 2 a stage 3 answer");
  TEST_ASSERT(answers(0x24, 0x02, 3),
              "But a late stage 1 reply arriving during stage 3 still counts — "
              "it belongs to a stage that did send");
  TEST_ASSERT(!answers(0x24, 0x02, 0),
              "And nothing counts before the first stage starts");
}

void test_runt_frames_are_refused_without_reading_past_the_end() {
  std::cout << "\n=== Frames too short to hold a class byte are refused ==="
            << std::endl;

  const uint8_t frame[] = {0x24, 0x07, 0xF8, 0xE7, 0x02};
  TEST_ASSERT(auth_frame_answers_stage(frame, 5, 1),
              "5 bytes is exactly enough — the class byte is the last one");
  for (size_t len = 0; len < 5; len++) {
    TEST_ASSERT(!auth_frame_answers_stage(frame, len, 1),
                "Shorter than the class byte is refused, not read past");
  }
  TEST_ASSERT(!auth_frame_answers_stage(nullptr, 11, 1),
              "A null frame is refused rather than dereferenced");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Authentication Stage Gate Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_a_fully_answered_stage_proceeds_immediately();
  test_a_short_stage_waits();
  test_the_ceiling_fails_open();
  test_extra_replies_do_not_hold_the_gate();
  test_a_zero_packet_stage_is_answered();
  test_reply_classes_map_to_stages();
  test_only_response_frames_count();
  test_a_reply_cannot_be_credited_to_a_future_stage();
  test_runt_frames_are_refused_without_reading_past_the_end();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
