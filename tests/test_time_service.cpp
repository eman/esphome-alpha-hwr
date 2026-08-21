// Host tests for the node's wall clock: TimeService's three accessors, the one
// sanity floor behind them, and what they do when there is no clock.
//
// Why this file exists (issue #270). The component used to resolve "what time
// is it" in five places, with three different floors -- year 2020, year 2021,
// and the literal 1609459200, two of which agreed by coincidence rather than by
// construction -- and each place read the clock its own way. Three of the five
// went straight to ::time(nullptr), so a build without the time component had
// one subsystem declining to act and three acting on a clock nothing had
// validated, in whatever zone libc happened to have. Nothing was known to be
// broken by that. It is pinned here because #262 was caused by one caller
// substituting the wrong timestamp for "now", and several independent notions
// of now is the condition that makes that class of bug easy to reintroduce.
//
// **What this file is, stated because an adversarial review found the claim
// that used to stand here overstated.** These are characterization tests for
// the accessor contract, plus the mutation coverage for the floor. They are NOT
// a regression test for #270: on `main` the three accessors already shared
// clock_is_synced() and already refused without USE_TIME, and #270's change was
// moving the five *callers* off ::time(nullptr). This file links no caller, so
// reverting #270's production change leaves every assertion below passing --
// verified, not assumed. The caller-side behaviour is pinned in
// tests/test_component_wiring.cpp instead.
//
// Two binaries are built from this file:
//
//   test_time_service          -DUSE_TIME  -- the real accessors
//   test_time_service_no_time              -- the #else stubs
//
// The second compiles the #else branch and pins that it refuses. Worth having,
// because nothing else builds that branch and replacing its body with a libc
// read does fail these tests. It does NOT verify the caller-side half of "every
// caller refuses under #ifndef USE_TIME": no caller is linked into it. That
// half holds by construction -- every caller goes through TimeService and no
// ::time(nullptr) remains in components/alpha_hwr/ -- which is a weaker
// guarantee than a test, and is said so rather than implied.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/time_service.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

using esphome::ESPTime;
using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::services::CLOCK_SYNCED_YEAR_FLOOR;
using esphome::alpha_hwr::services::TimeService;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  do {                                                                         \
    if (condition) {                                                           \
      tests_passed++;                                                          \
      std::cout << "[PASS] " << message << std::endl;                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      std::cout << "[FAIL] " << message << std::endl;                          \
    }                                                                          \
  } while (0)

#ifdef USE_TIME
// 2026-08-07 12:00:00 UTC, the same instant tests/test_write_operations.cpp
// anchors its fixtures to. Comfortably above the floor.
static constexpr time_t A_SYNCED_EPOCH = 1786104000;

// 2020-06-01 00:00:00 UTC. Above ESPTime::is_valid()'s own year>=2019 rule and
// below CLOCK_SYNCED_YEAR_FLOOR, so it is exactly the clock the floor exists to
// turn away: fields all in range, nothing having actually set it.
static constexpr time_t AN_UNSYNCED_EPOCH = 1590969600;
#endif

namespace {

struct Fixture {
  Transport transport;
  TimeService time_service{&transport};
#ifdef USE_TIME
  esphome::time::RealTimeClock node_clock;

  void attach_clock_at(time_t epoch) {
    node_clock.set_epoch_for_test(epoch);
    time_service.set_time_id(&node_clock);
  }
#endif

  // "All three refuse", asked as one question, because the three agreeing is
  // itself the property under test -- a caller picks whichever shape it needs
  // and must not get a different answer for having picked differently.
  bool all_three_refuse() {
    ESPTime out{};
    const bool fields_refused = !time_service.current_time(out);
    const bool epoch_refused = time_service.now_unix() == 0;
    const bool flag_refused = !time_service.wall_clock_is_set();
    return fields_refused && epoch_refused && flag_refused;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// The floor
// ---------------------------------------------------------------------------

// The number itself, pinned as the design decision it is. Every other assertion
// here is written relative to the constant and would hold for any value of it.
static void test_there_is_exactly_one_floor() {
  TEST_ASSERT(CLOCK_SYNCED_YEAR_FLOOR == 2021,
              "the wall-clock sanity floor is year 2021");
  // The two floors this replaced. Neither is a constant any more, and neither
  // should come back as one: 1609459200 is 2021-01-01T00:00:00Z, which agreed
  // with the year floor by coincidence, and year 2020 disagreed with it
  // outright. Spelled as an assertion so the relationship is recorded rather
  // than remembered.
  ESPTime at_the_old_epoch_floor = ESPTime::from_epoch_utc(1609459200);
  TEST_ASSERT(at_the_old_epoch_floor.year == CLOCK_SYNCED_YEAR_FLOOR,
              "the retired 1609459200 literal was this same floor, by luck");
}

#ifdef USE_TIME

// ---------------------------------------------------------------------------
// With the time component
// ---------------------------------------------------------------------------

static void test_no_time_id_refuses() {
  Fixture f;  // deliberately never calls set_time_id()
  TEST_ASSERT(f.all_three_refuse(),
              "with no time_id configured, all three accessors refuse");
}

static void test_an_unsynced_clock_refuses() {
  Fixture f;
  f.attach_clock_at(AN_UNSYNCED_EPOCH);
  ESPTime raw = f.node_clock.now();
  TEST_ASSERT(raw.is_valid(),
              "the unsynced fixture clock passes ESPTime::is_valid() on its own");
  TEST_ASSERT(raw.year < CLOCK_SYNCED_YEAR_FLOOR,
              "...and sits below the floor, which is the case being tested");
  TEST_ASSERT(f.all_three_refuse(),
              "a configured but unsynced clock is refused by all three");
}

static void test_a_synced_clock_is_answered() {
  Fixture f;
  f.attach_clock_at(A_SYNCED_EPOCH);
  ESPTime out{};
  TEST_ASSERT(f.time_service.current_time(out), "current_time() answers");
  TEST_ASSERT(f.time_service.wall_clock_is_set(), "wall_clock_is_set() answers");
  TEST_ASSERT(f.time_service.now_unix() == static_cast<uint32_t>(A_SYNCED_EPOCH),
              "now_unix() answers the epoch the clock holds");
  TEST_ASSERT(out.timestamp == A_SYNCED_EPOCH,
              "current_time() answers the same instant");
}

// The property the consolidation exists for: the three shapes are three views
// of one read, so no caller can get a different answer for having asked
// differently. Swept rather than spot-checked, across the floor.
static void test_the_three_shapes_never_disagree() {
  bool agreed_everywhere = true;
  bool saw_a_refusal = false;
  bool saw_an_answer = false;

  // Two years either side of the floor, in month-sized steps.
  for (time_t epoch = 1546300800 /* 2019-01-01 */; epoch < 1704067200 /* 2023-01-01 */;
       epoch += 30 * 86400) {
    Fixture f;
    f.attach_clock_at(epoch);
    ESPTime out{};
    const bool fields = f.time_service.current_time(out);
    const uint32_t epoch_answer = f.time_service.now_unix();
    const bool flag = f.time_service.wall_clock_is_set();

    if (fields != flag) agreed_everywhere = false;
    if (fields != (epoch_answer != 0)) agreed_everywhere = false;
    if (fields && out.timestamp != static_cast<time_t>(epoch_answer))
      agreed_everywhere = false;
    // ...and each of them agrees with the floor, which is what makes this a
    // test of one floor rather than of three that happen to match today.
    if (fields != (f.node_clock.now().year >= CLOCK_SYNCED_YEAR_FLOOR))
      agreed_everywhere = false;

    if (fields) saw_an_answer = true; else saw_a_refusal = true;
  }

  TEST_ASSERT(saw_a_refusal && saw_an_answer,
              "the sweep crosses the floor, so both outcomes are exercised");
  TEST_ASSERT(agreed_everywhere,
              "across four years of clocks the three accessors never disagree");
}

// 0 from now_unix() means "this node cannot tell you what time it is". A caller
// that reads it as an instant has silently claimed 1970, which is the whole of
// issue #262 on the other side, so the sentinel must not be reachable by any
// clock the floor admits.
static void test_zero_is_never_a_time() {
  bool zero_only_when_refused = true;
  for (time_t epoch = 1704067200 /* 2024-01-01 */; epoch < 1893456000 /* 2030-01-01 */;
       epoch += 37 * 86400) {
    Fixture f;
    f.attach_clock_at(epoch);
    if (f.time_service.now_unix() == 0) zero_only_when_refused = false;
  }
  TEST_ASSERT(zero_only_when_refused,
              "no clock above the floor can produce the 0 sentinel");
}

#else  // !USE_TIME

// ---------------------------------------------------------------------------
// Without the time component
// ---------------------------------------------------------------------------

// The whole point of the second binary. Before #270 this build had
// TimeService refusing while three ::time(nullptr) callers went on answering
// from an unvalidated libc clock in an unset zone; now every caller comes
// through here and gets one answer.
static void test_every_accessor_refuses_without_the_time_component() {
  Fixture f;
  TEST_ASSERT(f.all_three_refuse(),
              "without USE_TIME, all three accessors refuse");
}

// And it refuses for the right reason -- because there is no time source, not
// because the process happens to be unsynced. libc here knows exactly what time
// it is; the accessors still decline, because a clock in an unknown zone is not
// the clock the pump's schedules run against.
static void test_refusing_is_not_an_accident_of_the_host_clock() {
  const time_t host_now = ::time(nullptr);
  TEST_ASSERT(host_now > 1704067200,
              "the host running this test does know what time it is");
  Fixture f;
  TEST_ASSERT(f.all_three_refuse(),
              "...and the accessors refuse anyway, rather than borrowing it");
}

#endif  // USE_TIME

int main() {
  // Pinned so the epoch<->fields mapping in the fixtures is the same on every
  // CI machine. The local/UTC conversion itself is not the subject here -- it
  // is covered at non-zero offsets in tests/test_schedule_service.cpp.
  setenv("TZ", "UTC", 1);
  tzset();

  std::cout << "===========================================================" << std::endl;
#ifdef USE_TIME
  std::cout << "  Wall-clock accessors, with the time component (issue #270)" << std::endl;
#else
  std::cout << "  Wall-clock accessors, WITHOUT the time component (issue #270)" << std::endl;
#endif
  std::cout << "===========================================================" << std::endl;

  std::cout << "\n=== One floor ===" << std::endl;
  test_there_is_exactly_one_floor();

#ifdef USE_TIME
  std::cout << "\n=== No clock is refused ===" << std::endl;
  test_no_time_id_refuses();
  test_an_unsynced_clock_refuses();

  std::cout << "\n=== A synced clock is answered ===" << std::endl;
  test_a_synced_clock_is_answered();

  std::cout << "\n=== The three shapes are one answer ===" << std::endl;
  test_the_three_shapes_never_disagree();
  test_zero_is_never_a_time();
#else
  std::cout << "\n=== Uniform refusal without USE_TIME ===" << std::endl;
  test_every_accessor_refuses_without_the_time_component();
  test_refusing_is_not_an_accident_of_the_host_clock();
#endif

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
