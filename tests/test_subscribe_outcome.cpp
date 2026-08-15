// Host tests for subscribe_outcome.h (issue #175).
//
// BLEConnectionManager::subscribe_to_notifications() has six terminal paths and
// used to discard the answer on five of them. Four returned early after logging
// -- and since subscribed_callback_() is the only thing that advances the
// session out of SUBSCRIBING, those four parked it there until the 60 s data
// watchdog recycled the link with a generic "No data from pump". The fifth, a
// CCCD write that failed synchronously, fell through to the *same* callback as
// success, so a failed subscribe was indistinguishable from a good one.
//
// ble_connection_manager.cpp is compiled by no host test, so the decision was
// extracted to a header the way link_watchdog.h and initial_read_retry.h were.
// What is pinned here is the policy, which is the part with a judgement call in
// it: recycle immediately on the four that cannot recover, and deliberately
// *not* on the CCCD failure.

#include <cstdint>
#include <iostream>
#include <set>
#include <string>

#include "../components/alpha_hwr/subscribe_outcome.h"

using esphome::alpha_hwr::core::SubscribeOutcome;
using esphome::alpha_hwr::core::subscribe_failed;
using esphome::alpha_hwr::core::subscribe_outcome_blocks_session;
using esphome::alpha_hwr::core::subscribe_outcome_should_recycle;
using esphome::alpha_hwr::core::subscribe_outcome_to_string;

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

// Every outcome, so a new enumerator cannot be added without landing in the
// tables below. -Wswitch plus warnings-as-errors covers subscribe_outcome_to_
// string(); this covers the predicates, which have no switch to warn on.
static const SubscribeOutcome ALL[] = {
    SubscribeOutcome::OK,
    SubscribeOutcome::NO_CLIENT,
    SubscribeOutcome::NO_SERVICE,
    SubscribeOutcome::NO_CHARACTERISTIC,
    SubscribeOutcome::REGISTER_FAILED,
    SubscribeOutcome::CCCD_WRITE_FAILED,
};

void test_only_ok_is_a_success() {
  std::cout << "\n=== Exactly one outcome is a success ===" << std::endl;

  int failures = 0;
  for (auto o : ALL)
    if (subscribe_failed(o)) failures++;

  TEST_ASSERT(!subscribe_failed(SubscribeOutcome::OK),
              "OK is the success case");
  TEST_ASSERT(failures == 5,
              "...and the other five are all failures, none of them silently "
              "counted as success the way CCCD_WRITE_FAILED used to be");
}

void test_the_four_blocking_outcomes_are_exactly_the_early_returns() {
  std::cout << "\n=== The blocking set is the four that never call the callback ==="
            << std::endl;

  TEST_ASSERT(subscribe_outcome_blocks_session(SubscribeOutcome::NO_CLIENT),
              "No BLE client blocks the session");
  TEST_ASSERT(subscribe_outcome_blocks_session(SubscribeOutcome::NO_SERVICE),
              "A missing service blocks the session");
  TEST_ASSERT(
      subscribe_outcome_blocks_session(SubscribeOutcome::NO_CHARACTERISTIC),
      "A missing characteristic blocks the session");
  TEST_ASSERT(subscribe_outcome_blocks_session(SubscribeOutcome::REGISTER_FAILED),
              "A failed register-for-notify blocks the session");

  // The two that reach subscribed_callback_() do not block, and that is the
  // whole distinction the set encodes.
  TEST_ASSERT(!subscribe_outcome_blocks_session(SubscribeOutcome::OK),
              "Success does not block the session");
  TEST_ASSERT(
      !subscribe_outcome_blocks_session(SubscribeOutcome::CCCD_WRITE_FAILED),
      "A failed CCCD write does not block the session — it reaches the "
      "callback, so the session still advances");
}

void test_the_cccd_failure_is_deliberately_not_recycled() {
  std::cout << "\n=== A failed CCCD write is reported but not acted on ==="
            << std::endl;

  // This is the judgement call in the whole change, so it is asserted rather
  // than left to the comment. The pump is bonded; a bonded peer retains its
  // CCCD across reconnections, so a link whose CCCD write could not be issued
  // may already be subscribed from an earlier session. Recycling on that
  // prediction tears down links that work. The watchdog settles it on the only
  // evidence that can — whether data actually arrives.
  TEST_ASSERT(subscribe_failed(SubscribeOutcome::CCCD_WRITE_FAILED),
              "A failed CCCD write is a failure, and names itself as one");
  TEST_ASSERT(
      !subscribe_outcome_should_recycle(SubscribeOutcome::CCCD_WRITE_FAILED),
      "...but it does not recycle the link: predicting deafness is worse than "
      "observing it, and a bonded peer may already be subscribed");

  TEST_ASSERT(!subscribe_outcome_should_recycle(SubscribeOutcome::OK),
              "Success certainly does not recycle the link");
}

void test_recycling_is_confined_to_paths_that_cannot_recover() {
  std::cout << "\n=== Only unrecoverable outcomes recycle the link ==="
            << std::endl;

  // A forced disconnect is not free -- issue #176 records that each recycle
  // re-enters the encryption-on-open path on a bonded pump, where a failure can
  // erase the bond (issue #14). So the set that triggers one must be exactly
  // the set that has nothing to lose, i.e. the ones already stuck.
  for (auto o : ALL) {
    const bool recycles = subscribe_outcome_should_recycle(o);
    const bool blocked = subscribe_outcome_blocks_session(o);
    TEST_ASSERT(recycles == blocked,
                std::string("\"") + subscribe_outcome_to_string(o) +
                    "\" recycles the link only if it is stuck without one");
  }
}

void test_every_outcome_names_itself_distinctly() {
  std::cout << "\n=== Each outcome carries its own fault string ==="
            << std::endl;

  // The point of the change is that the operator sees a cause rather than the
  // watchdog's shared symptom, so two outcomes sharing a string would defeat
  // it as surely as no string at all.
  std::set<std::string> seen;
  for (auto o : ALL)
    seen.insert(subscribe_outcome_to_string(o));

  TEST_ASSERT(seen.size() == 6,
              "All six outcomes produce distinct fault strings");

  bool all_prefixed = true;
  for (auto o : ALL)
    all_prefixed = all_prefixed &&
                   std::string(subscribe_outcome_to_string(o)).rfind("Subscribe:", 0) == 0;
  TEST_ASSERT(all_prefixed,
              "...each naming the subscribe step, so the fault sensor says "
              "where in the handshake it happened");

  // The strings reach a fixed-width fault surface, so an overlong one would be
  // truncated into something unreadable rather than merely long.
  bool all_short = true;
  for (auto o : ALL)
    all_short = all_short && std::string(subscribe_outcome_to_string(o)).size() < 64;
  TEST_ASSERT(all_short, "...and each fits the fault string budget");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Subscribe Outcome Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_only_ok_is_a_success();
  test_the_four_blocking_outcomes_are_exactly_the_early_returns();
  test_the_cccd_failure_is_deliberately_not_recycled();
  test_recycling_is_confined_to_paths_that_cannot_recover();
  test_every_outcome_names_itself_distinctly();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
