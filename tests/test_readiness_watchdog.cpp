// Host tests for readiness_watchdog.h — the progress watchdog.
//
// Issue #211, reported from a live installation: connected, pairing on,
// telemetry updating in Home Assistant, `Pump Ready` off indefinitely, no fault
// raised, no recycle, and an automation gated on `Pump Ready` waiting silently
// forever. The reporter's summary of why that shape is the worst one — "it's
// the one failure shape where the diagnostics actively point away from the
// problem."
//
// The inbound-data watchdog cannot cover it. Its observable is silence and it
// is re-armed by every notification, and this pump volunteers Class 10
// telemetry unprompted, so a session stuck anywhere keeps re-arming it. That is
// the first property below, and it is the reason this module exists rather than
// a tuning of the other one.
//
// The second property is the trap the reporter predicted before any code
// existed: a readiness timer re-armed by anything readiness-adjacent chases the
// state it is waiting for and can never fire. That is not hypothetical in this
// codebase — link_watchdog.h documents the Pump Link Status ladder refreshing
// `link_last_open_ms_` on every evaluation, which kept the rung below it
// permanently unreachable. The tests here pin the arming rule by asserting what
// does NOT move the deadline.

#include <cstdint>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/readiness_watchdog.h"

using esphome::alpha_hwr::LINK_READY_TIMEOUT_BACKOFF_CAP_MS;
using esphome::alpha_hwr::link_readiness_timeout_expired;
using esphome::alpha_hwr::link_readiness_timeout_next;

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

static const uint32_t WINDOW = 300000;  // the shipped default, 5 minutes

static void test_the_reported_failure_is_caught() {
  // The headline case, in the terms the report gives: the link is up, the pump
  // is streaming telemetry, and it never becomes usable. The data watchdog sees
  // a frame every few seconds and is satisfied; nothing else looks at all.
  const uint32_t opened = 1000;
  TEST_ASSERT(!link_readiness_timeout_expired(true, false, opened + WINDOW, opened, WINDOW),
              "At exactly the budget it has not expired — strictly greater, "
              "like the data watchdog, so the two agree on what a budget means");
  TEST_ASSERT(link_readiness_timeout_expired(true, false, opened + WINDOW + 1, opened, WINDOW),
              "One millisecond past it, a link that never became usable is "
              "finally reported instead of waited on forever");
}

static void test_a_usable_pump_is_never_reported() {
  const uint32_t opened = 1000;
  for (uint32_t elapsed : {0u, WINDOW, WINDOW + 1, 24u * 3600u * 1000u}) {
    TEST_ASSERT(!link_readiness_timeout_expired(true, true, opened + elapsed, opened, WINDOW),
                "A ready pump never expires, however long it has been up — the "
                "watchdog waits for a state, not for a duration");
  }
}

static void test_a_disconnected_link_is_not_its_business() {
  // Recovery here is a forced disconnect, so firing on an already-down link
  // would mean disconnecting something that is not connected, repeatedly, for
  // as long as the pump stayed away.
  TEST_ASSERT(!link_readiness_timeout_expired(false, false, 10u * WINDOW, 0, WINDOW),
              "A link that is down cannot be failing to become ready");
}

static void test_zero_disables_it() {
  TEST_ASSERT(!link_readiness_timeout_expired(true, false, 4000000000u, 0, 0),
              "0 disables the watchdog outright, matching data_timeout — an "
              "opt-out that silently still fired would be worse than no option");
}

static void test_the_deadline_is_not_moved_by_progress_toward_readiness() {
  // THE trap, and the reason this is a separate module rather than a flag on
  // the existing one. The predicate takes the connection-open stamp and the
  // ready flag and nothing else: there is deliberately no parameter for "last
  // notification", "session state" or "caches filled", because a timer that
  // accepted one would be re-armed by traffic on the way to the state it is
  // waiting for, and would then never expire on exactly the link it is for.
  //
  // Asserted as a property of the signature: a caller cannot pass that
  // information in even by mistake.
  const uint32_t opened = 1000;
  const uint32_t past_due = opened + WINDOW + 1;
  TEST_ASSERT(link_readiness_timeout_expired(true, false, past_due, opened, WINDOW),
              "Expired with the connection-open stamp unchanged...");
  TEST_ASSERT(link_readiness_timeout_expired(true, false, past_due + 3600000u, opened, WINDOW),
              "...and an hour later it is still expired, because nothing in "
              "this predicate can push the deadline out but a new connection");
}

static void test_the_clock_survives_a_rollover() {
  // millis() wraps at 49.7 days. Unsigned subtraction gives the true interval;
  // comparing the two stamps directly would make a link that connected just
  // before the wrap look like it had been waiting for weeks, and recycle it
  // instantly. Same reasoning, and the same fix, as the data watchdog.
  const uint32_t opened = 0xFFFFFF00u;
  const uint32_t now = 0x00000100u;  // 512 ms later in real time
  TEST_ASSERT(!link_readiness_timeout_expired(true, false, now, opened, WINDOW),
              "512 ms across the rollover is 512 ms, not seven weeks");
  TEST_ASSERT(link_readiness_timeout_expired(true, false, opened + WINDOW + 1, opened, WINDOW),
              "...and a genuine overrun spanning the rollover still fires");
  // The assertion with teeth, and the one this file originally lacked while
  // claiming "same reasoning, and the same fix, as the data watchdog". Both
  // stamps above sit AFTER the wrap, where `connected_since + timeout` wraps
  // too and the naive comparison happens to agree -- so the rollover-unsafe
  // mutation survived them. Here `now` is still BELOW the wrap: unsigned
  // subtraction says 1 s elapsed, while `now > connected_since + timeout`
  // compares against a wrapped sum and recycles a healthy link. This is the
  // case tests/test_link_watchdog.cpp already had for the sibling predicate.
  //
  // `now` must sit BELOW the wrap for this to bite, which is the detail that
  // makes the case easy to write uselessly. A first attempt used
  // `0xFFFFFF00 + 1000`, which itself wraps to 744 -- and there the naive
  // comparison agrees with the correct one, so the mutation survived it. With
  // `now` just short of the boundary the sum `connected_since + timeout` wraps
  // while `now` does not, and the naive form declares a 240 ms old link
  // expired.
  TEST_ASSERT(!link_readiness_timeout_expired(true, false, 0xFFFFFFF0u, 0xFFFFFF00u, WINDOW),
              "A healthy link 240 ms into its window, just below the wrap, is "
              "not recycled");
}

static void test_the_backoff_bounds_a_link_that_cannot_recover() {
  // A pump that is merely slow must not be recycled forever, which is the same
  // argument the data watchdog's backoff makes, so this is the same function.
  uint32_t w = WINDOW;
  w = link_readiness_timeout_next(w, LINK_READY_TIMEOUT_BACKOFF_CAP_MS);
  TEST_ASSERT(w == 2 * WINDOW, "The window doubles after a recycle");
  for (int i = 0; i < 20; i++) w = link_readiness_timeout_next(w, LINK_READY_TIMEOUT_BACKOFF_CAP_MS);
  TEST_ASSERT(w == LINK_READY_TIMEOUT_BACKOFF_CAP_MS,
              "and stops at the ceiling rather than growing without bound");
  TEST_ASSERT(link_readiness_timeout_next(0, LINK_READY_TIMEOUT_BACKOFF_CAP_MS) == 0,
              "A disabled watchdog stays disabled — backing off from 'never' "
              "is meaningless and would silently switch it on");
}

static void test_the_two_watchdogs_share_one_ceiling() {
  // Not a coincidence to be preserved by hand: both answer the same question --
  // how often may a link that cannot recover be re-opened -- and each re-open
  // takes another run at the encryption-on-open window that can erase the bond.
  TEST_ASSERT(LINK_READY_TIMEOUT_BACKOFF_CAP_MS ==
                  esphome::alpha_hwr::LINK_DATA_TIMEOUT_BACKOFF_CAP_MS,
              "The readiness ceiling IS the data ceiling; two different answers "
              "to one question would be a distinction with no reason");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Readiness Watchdog Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_the_reported_failure_is_caught();
  test_a_usable_pump_is_never_reported();
  test_a_disconnected_link_is_not_its_business();
  test_zero_disables_it();
  test_the_deadline_is_not_moved_by_progress_toward_readiness();
  test_the_clock_survives_a_rollover();
  test_the_backoff_bounds_a_link_that_cannot_recover();
  test_the_two_watchdogs_share_one_ceiling();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
