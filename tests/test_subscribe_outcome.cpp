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
using esphome::alpha_hwr::core::subscribe_outcome_holds_fault;
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

void test_the_cccd_failure_is_reported_but_does_not_hold() {
  std::cout << "\n=== A failed CCCD write is reported but does not hold ==="
            << std::endl;

  // The judgement call, asserted rather than left to the comment. One
  // documented synchronous cause of a CCCD write failure is the connection
  // already being gone -- so holding its reason would relabel a link loss as a
  // subscribe fault and suppress the real disconnect reason for the whole
  // reconnect episode. It is recorded; it just does not outrank a genuine one.
  TEST_ASSERT(subscribe_failed(SubscribeOutcome::CCCD_WRITE_FAILED),
              "A failed CCCD write is a failure, and names itself as one");
  TEST_ASSERT(!subscribe_outcome_holds_fault(SubscribeOutcome::CCCD_WRITE_FAILED),
              "...but does not hold, so a real disconnect reason still wins");
  TEST_ASSERT(!subscribe_outcome_blocks_session(SubscribeOutcome::CCCD_WRITE_FAILED),
              "...and the session still advances: a bonded peer may already be "
              "subscribed, and the dominant cause is transient congestion");

  TEST_ASSERT(!subscribe_outcome_holds_fault(SubscribeOutcome::OK),
              "Success holds nothing");
}

void test_holding_is_confined_to_paths_that_cannot_recover() {
  std::cout << "\n=== Only unrecoverable outcomes hold the fault ==="
            << std::endl;

  // Holding is not free: it suppresses the next disconnect reason. So the set
  // that holds must be exactly the set for which no other reason is coming.
  for (auto o : ALL) {
    const bool holds = subscribe_outcome_holds_fault(o);
    const bool blocked = subscribe_outcome_blocks_session(o);
    TEST_ASSERT(holds == blocked,
                std::string("\"") + subscribe_outcome_to_string(o) +
                    "\" holds its reason only if nothing else will produce one");
  }
}

void test_the_enum_cannot_grow_without_landing_in_the_tables() {
  std::cout << "\n=== A new enumerator cannot slip past these tables ==="
            << std::endl;

  // The first version of this file CLAIMED ALL[] made that true. It did not:
  // ALL[] is hand-written, so a seventh enumerator with a to_string() case
  // passed the whole suite while silently defaulting to failed / non-blocking /
  // non-holding.
  //
  // The scan below closes that half. The other half -- an enumerator added
  // WITHOUT a to_string() case -- it cannot close: both counts stay put and
  // the missing switch case is only a -Wswitch warning, which the suite does
  // not build with -Werror. So this target alone compiles with -Werror=switch
  // (see tests/Makefile). Neither mechanism is sufficient by itself and the
  // claim needs both.
  //
  // Same trick test_write_operations.cpp uses for WriteCommand: scan the whole
  // underlying range and count how many values to_string() recognises.
  int recognised = 0;
  for (int i = 0; i < 256; i++) {
    const auto o = static_cast<SubscribeOutcome>(i);
    if (std::string(subscribe_outcome_to_string(o)) != "Subscribe: unknown")
      recognised++;
  }
  const int table_size = static_cast<int>(sizeof(ALL) / sizeof(ALL[0]));
  TEST_ASSERT(recognised == table_size,
              "Every enumerator to_string() knows about is also in ALL[] — add "
              "one without the other and this fails");
}

void test_each_fault_string_matches_its_own_enumerator() {
  std::cout << "\n=== Fault strings correspond to their outcomes ==="
            << std::endl;

  // Distinctness is not correspondence. Swapping the NO_SERVICE and
  // NO_CHARACTERISTIC strings passed every assertion in the earlier version of
  // this file -- while pointing the operator at the wrong cause, which defeats
  // the entire purpose of the change.
  struct Pair { SubscribeOutcome o; const char *needle; };
  static const Pair EXPECT[] = {
      {SubscribeOutcome::OK, "ok"},
      {SubscribeOutcome::NO_CLIENT, "client"},
      {SubscribeOutcome::NO_SERVICE, "service"},
      {SubscribeOutcome::NO_CHARACTERISTIC, "characteristic"},
      {SubscribeOutcome::REGISTER_FAILED, "register"},
      {SubscribeOutcome::CCCD_WRITE_FAILED, "CCCD"},
  };
  for (const auto &e : EXPECT) {
    const std::string got = subscribe_outcome_to_string(e.o);
    TEST_ASSERT(got.find(e.needle) != std::string::npos,
                std::string("\"") + got + "\" names its own cause (\"" +
                    e.needle + "\")");
  }

  // The confusable pair, checked the other way round too.
  TEST_ASSERT(std::string(subscribe_outcome_to_string(SubscribeOutcome::NO_SERVICE))
                  .find("characteristic") == std::string::npos,
              "The service string does not also claim the characteristic");
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
  test_the_cccd_failure_is_reported_but_does_not_hold();
  test_holding_is_confined_to_paths_that_cannot_recover();
  test_every_outcome_names_itself_distinctly();
  test_each_fault_string_matches_its_own_enumerator();
  test_the_enum_cannot_grow_without_landing_in_the_tables();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
