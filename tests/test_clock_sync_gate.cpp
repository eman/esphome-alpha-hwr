// Host tests for clock_sync_gate.h — can a clock sync run, and is the reason
// it cannot worth reporting.
//
// The defect these pin: the pump keeps its own RTC and runs schedule windows
// off it, so a clock that never gets set shows up as a schedule firing at the
// wrong hour, days later, with every sensor still healthy. Two of the three
// ways to reach that state used to be reported only at ESP_LOGD — which
// ESPHome compiles out entirely below DEBUG, and this component ships at INFO,
// so on a normal node they produced no output at all. Not quiet: absent.
//
// The second half is the retry. check_and_sync_time() deliberately does not
// stamp an attempt when nothing was written, so a sync blocked by a pump that
// is not yet synchronized retries on the next poll instead of backing off
// fifteen minutes. Applied to a *permanent* condition that becomes a spin —
// every 10 s poll walking the full path to fail at the same place, forever.
//
// So the gate has to tell three lookalikes apart: no time source configured
// (permanent), a time source that never answered (permanent in practice, and
// the likelier one since both packages set time_id), and a time source that has
// simply not answered yet (normal at boot, must stay silent). Only elapsed time
// separates the last two.

#include <cstdint>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/clock_sync_gate.h"

using esphome::alpha_hwr::core::CLOCK_SOURCE_GRACE_MS;
using esphome::alpha_hwr::core::ClockSyncAction;
using esphome::alpha_hwr::core::clock_sync_action;
using esphome::alpha_hwr::core::clock_sync_blocked;
using esphome::alpha_hwr::core::clock_sync_warns;

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

static const uint32_t GRACE = CLOCK_SOURCE_GRACE_MS;

// The window itself, as a literal. Every other test here is written against
// CLOCK_SOURCE_GRACE_MS, which means they hold for *any* value of it -- a grace
// of 1 ms accuses every booting node on its first ready poll, and a grace of a
// day leaves a dead time source unreported until tomorrow, and neither changes
// a single relative assertion. The number is a design decision, so it gets
// pinned like one.
static void test_the_grace_window_is_fifteen_minutes() {
  TEST_ASSERT(CLOCK_SOURCE_GRACE_MS == 15u * 60u * 1000u,
              "the grace window is 15 minutes");
  // Long enough that SNTP and a homeassistant platform have both answered on
  // any healthy node...
  TEST_ASSERT(CLOCK_SOURCE_GRACE_MS >= 5u * 60u * 1000u,
              "...at least five minutes, or booting nodes get accused");
  // ...and short enough that a genuinely dead source is reported the same hour.
  TEST_ASSERT(CLOCK_SOURCE_GRACE_MS <= 60u * 60u * 1000u,
              "...at most an hour, or a dead clock goes unreported all day");
}

// Boundary behaviour stated in absolute milliseconds, independent of the
// configured window. Without this the comparison can be made to ignore its own
// parameter for any small value and every relative test still passes.
static void test_the_boundary_holds_for_any_window() {
  TEST_ASSERT(clock_sync_action(true, false, 4, 5) == ClockSyncAction::WAIT,
              "tiny window: 4 ms of a 5 ms grace still waits");
  TEST_ASSERT(clock_sync_action(true, false, 5, 5) == ClockSyncAction::WARN_NO_SOURCE,
              "tiny window: 5 ms of a 5 ms grace reports");
  TEST_ASSERT(clock_sync_action(true, false, 899999, 900000) == ClockSyncAction::WAIT,
              "real window: one millisecond short still waits");
  TEST_ASSERT(clock_sync_action(true, false, 900000, 900000) == ClockSyncAction::WARN_NO_SOURCE,
              "real window: exactly the window reports");
}

// The configuration that works: a clock exists and has been set.
static void test_a_usable_clock_syncs() {
  TEST_ASSERT(clock_sync_action(true, true, 0, GRACE) == ClockSyncAction::SYNC,
              "time_id set and clock valid: sync, even at boot");
  TEST_ASSERT(clock_sync_action(true, true, GRACE * 10, GRACE) == ClockSyncAction::SYNC,
              "...and still sync long afterwards");
  TEST_ASSERT(!clock_sync_blocked(clock_sync_action(true, true, 0, GRACE)),
              "a syncing tick is not blocked");
  TEST_ASSERT(!clock_sync_warns(clock_sync_action(true, true, 0, GRACE)),
              "a syncing tick says nothing");
}

// No time_id is permanent from the first tick. Waiting cannot change it, and
// waiting is exactly what hid it before.
static void test_no_time_id_is_reported_immediately() {
  TEST_ASSERT(clock_sync_action(false, false, 0, GRACE) == ClockSyncAction::WARN_NO_TIME_ID,
              "no time_id at boot: reported at once, not after the grace window");
  TEST_ASSERT(clock_sync_action(false, false, GRACE * 100, GRACE) ==
                  ClockSyncAction::WARN_NO_TIME_ID,
              "...and still reported much later");
  TEST_ASSERT(clock_sync_blocked(clock_sync_action(false, false, 0, GRACE)),
              "no time_id blocks the sync");
  TEST_ASSERT(clock_sync_warns(clock_sync_action(false, false, 0, GRACE)),
              "no time_id is worth reporting");
}

// The case a bare `time_id != nullptr` check misses entirely, and the likelier
// one in practice: both entry packages set time_id, so a homeassistant time
// platform on a node that cannot reach Home Assistant looks configured and
// never produces a clock.
static void test_a_silent_source_is_reported_once_the_grace_expires() {
  TEST_ASSERT(clock_sync_action(true, false, GRACE, GRACE) == ClockSyncAction::WARN_NO_SOURCE,
              "a source silent for exactly the grace window is reported");
  TEST_ASSERT(clock_sync_action(true, false, GRACE * 4, GRACE) == ClockSyncAction::WARN_NO_SOURCE,
              "...and remains reported");
  TEST_ASSERT(clock_sync_blocked(clock_sync_action(true, false, GRACE, GRACE)),
              "a silent source blocks the sync");
  TEST_ASSERT(clock_sync_warns(clock_sync_action(true, false, GRACE, GRACE)),
              "a silent source is worth reporting");
}

// Booting before SNTP or the API answers is normal and must not accuse anyone.
static void test_a_source_that_has_not_answered_yet_stays_silent() {
  TEST_ASSERT(clock_sync_action(true, false, 0, GRACE) == ClockSyncAction::WAIT,
              "at boot, a not-yet-set clock waits");
  TEST_ASSERT(clock_sync_action(true, false, GRACE - 1, GRACE) == ClockSyncAction::WAIT,
              "one millisecond before the window closes, still waiting");
  TEST_ASSERT(clock_sync_blocked(clock_sync_action(true, false, 0, GRACE)),
              "waiting still blocks the sync");
  TEST_ASSERT(!clock_sync_warns(clock_sync_action(true, false, 0, GRACE)),
              "waiting says nothing -- this is the false alarm to avoid");
}

// The boundary is where "settling" turns into "misconfigured", so it is worth
// pinning from both sides rather than trusting one comparison.
static void test_the_grace_boundary() {
  TEST_ASSERT(clock_sync_action(true, false, GRACE - 1, GRACE) == ClockSyncAction::WAIT,
              "grace-1 waits");
  TEST_ASSERT(clock_sync_action(true, false, GRACE, GRACE) == ClockSyncAction::WARN_NO_SOURCE,
              "grace exactly reports");
  TEST_ASSERT(clock_sync_action(true, false, GRACE + 1, GRACE) == ClockSyncAction::WARN_NO_SOURCE,
              "grace+1 reports");
  // A zero grace must report immediately rather than needing one more tick --
  // the difference between `<` and `<=` on the comparison.
  TEST_ASSERT(clock_sync_action(true, false, 0, 0) == ClockSyncAction::WARN_NO_SOURCE,
              "a zero grace window reports at once");
}

// A valid clock outranks the timer: a source that answers late must not be
// reported merely because it was slow.
static void test_a_late_answer_is_not_an_error() {
  TEST_ASSERT(clock_sync_action(true, true, GRACE * 3, GRACE) == ClockSyncAction::SYNC,
              "a clock that arrived after the grace window still syncs, silently");
}

// The four states are distinct, and exactly one of them syncs. A gate that
// collapses any two has stopped doing its job.
static void test_the_decision_table() {
  struct Row {
    bool has_time_id;
    bool clock_set;
    uint32_t uptime;
    ClockSyncAction expected;
    const char *name;
  };
  const Row rows[] = {
      {true, true, 0, ClockSyncAction::SYNC, "clock ready"},
      {true, false, 0, ClockSyncAction::WAIT, "early, no clock yet"},
      {true, false, GRACE, ClockSyncAction::WARN_NO_SOURCE, "source never answered"},
      {false, false, 0, ClockSyncAction::WARN_NO_TIME_ID, "no time_id, at boot"},
      {false, false, GRACE * 5, ClockSyncAction::WARN_NO_TIME_ID, "no time_id, much later"},
  };
  for (const auto &r : rows) {
    TEST_ASSERT(clock_sync_action(r.has_time_id, r.clock_set, r.uptime, GRACE) == r.expected,
                std::string("decision table: ") + r.name);
  }
  // No counting assertions here. An earlier version totted up how many rows
  // synced and how many warned; both are recomputed from the same calls the
  // rows already assert, so neither could fail unless a row had already failed.
  // The combination (no time_id, clock somehow set) is also absent on purpose:
  // wall_clock_is_set() returns false whenever time_id_ is null, so it cannot
  // occur, and asserting a precedence rule over an unreachable input reads as
  // coverage while protecting nothing.
}

// blocked and warns are separate questions: every warn blocks, but not every
// block warns. Collapsing them reintroduces the false alarm at boot.
static void test_blocked_and_warns_are_not_the_same_question() {
  const ClockSyncAction wait = clock_sync_action(true, false, 0, GRACE);
  TEST_ASSERT(clock_sync_blocked(wait) && !clock_sync_warns(wait),
              "WAIT blocks without warning -- the one combination that matters");
  const ClockSyncAction sync = clock_sync_action(true, true, 0, GRACE);
  TEST_ASSERT(!clock_sync_blocked(sync) && !clock_sync_warns(sync),
              "SYNC neither blocks nor warns");
}

// Compiled with -Werror=switch. check_and_sync_time() switches over the same
// enum with no default, so a new state is a diagnostic there as well -- this
// target is simply where it is fatal, since ESPHome's build does not use
// -Werror. Worth stating precisely: while that caller used a ternary, this
// mirror pinned nothing about it.
static const char *action_name(ClockSyncAction a) {
  switch (a) {
    case ClockSyncAction::SYNC:
      return "SYNC";
    case ClockSyncAction::WAIT:
      return "WAIT";
    case ClockSyncAction::WARN_NO_TIME_ID:
      return "WARN_NO_TIME_ID";
    case ClockSyncAction::WARN_NO_SOURCE:
      return "WARN_NO_SOURCE";
  }
  return "UNREACHABLE";
}

static void test_every_action_is_accounted_for() {
  TEST_ASSERT(std::string(action_name(clock_sync_action(true, true, 0, GRACE))) == "SYNC",
              "a ready clock names SYNC");
  TEST_ASSERT(std::string(action_name(clock_sync_action(true, false, 0, GRACE))) == "WAIT",
              "an early tick names WAIT");
  TEST_ASSERT(std::string(action_name(clock_sync_action(false, false, 0, GRACE))) ==
                  "WARN_NO_TIME_ID",
              "a missing time_id names WARN_NO_TIME_ID");
  TEST_ASSERT(std::string(action_name(clock_sync_action(true, false, GRACE, GRACE))) ==
                  "WARN_NO_SOURCE",
              "a silent source names WARN_NO_SOURCE");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Clock Sync Gate Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_a_usable_clock_syncs();
  test_no_time_id_is_reported_immediately();
  test_a_silent_source_is_reported_once_the_grace_expires();
  test_a_source_that_has_not_answered_yet_stays_silent();
  test_the_grace_boundary();
  test_the_grace_window_is_fifteen_minutes();
  test_the_boundary_holds_for_any_window();
  test_a_late_answer_is_not_an_error();
  test_the_decision_table();
  test_blocked_and_warns_are_not_the_same_question();
  test_every_action_is_accounted_for();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
