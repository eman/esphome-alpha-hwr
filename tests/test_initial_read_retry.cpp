// Host tests for the initial-read re-arm predicate.
//
// trigger_initial_data_reads() latches initial_data_read_done_ and the ONLY
// thing that clears it is a BLE disconnect. The chain has two callers -- the
// auth-complete callback, and update() for the case where the link persists
// through an ESP32 restart and no re-auth happens. On that second path it can
// fire before the pump is answering; its reads miss, the latch stays set, and
// nothing retries, so device info and the operating statistics stay unread for
// as long as the link stays up.
//
// The subtle part, and what the first version of this fix got wrong: the
// control cache and the schedule overview are NOT products of this chain. The
// schedule poll runs every 10 s from update() and the control sync
// re-schedules itself every 5 s until it succeeds, so both heal on their own
// once the pump answers -- and Pump Ready, which is gated on them, comes on
// regardless. A node can therefore look completely healthy while the one-shot
// reads have silently never landed. Anything treating "caches valid" as this
// chain's success condition stops retrying at exactly the wrong moment.
//
// Both directions matter. The predicate must fire on a stalled attempt, and it
// must NOT fire once the chain has genuinely landed: the remedy re-runs the
// whole read chain, so a false positive is a permanent read loop against the
// pump.

#include <cstdint>
#include <iostream>

#include "../components/alpha_hwr/initial_read_retry.h"

using esphome::alpha_hwr::core::next_initial_read_backoff_ms;
using esphome::alpha_hwr::core::should_rearm_initial_read;

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

namespace {

static constexpr uint32_t TIMEOUT_MS = 60000;  // INITIAL_READ_SYNC_TIMEOUT_MS
static constexpr uint32_t UPDATE_INTERVAL_MS = 10000;  // PollingComponent(10000)
static constexpr uint32_t MAX_MS = 600000;  // INITIAL_READ_RETRY_MAX_MS

// Mirrors update() as it actually runs, and in particular keeps the two things
// the old version of this simulation collapsed into one boolean:
//
//   * the control cache and schedule overview heal WITHOUT the chain -- the
//     schedule poll runs every 10 s from update() and the control sync
//     re-schedules itself every 5 s until it succeeds -- so they go valid on
//     their own once the pump answers, and
//   * device info and the operating statistics are issued ONCE per attempt and
//     have no retry of their own.
//
// Collapsing those made the earlier model unable to distinguish a chain that
// landed from one that did not, which is exactly the bug the predicate had.
//
// It also does NOT short-circuit on success before consulting the predicate.
// Production update() has no such early return, so the predicate's own success
// guard is the only thing standing between a healthy node and a permanent read
// loop; skipping it would leave that guard untested.
struct ReadChainSim {
  uint32_t now{0};
  bool session_ready{true};
  bool chain_done{false};
  uint32_t started_ms{0};
  uint32_t retry_interval{TIMEOUT_MS};

  /// millis() from which the pump answers anything at all.
  uint32_t pump_answers_from_ms{0};

  /// Set false to model the component WITHOUT the re-arm, to show the retry is
  /// load-bearing rather than decorative.
  bool rearm_enabled{true};

  // Chain products: issued once per attempt, no retry of their own.
  bool device_info_ok{false};
  bool statistics_ok{false};
  bool reads_in_flight{false};
  uint32_t reads_land_at{0};

  // Self-healing caches, independent of the chain.
  bool control_cache_valid{false};
  bool schedule_cache_valid{false};

  int triggers{0};
  int rearms{0};

  bool caches_synced() const {
    return session_ready && chain_done && control_cache_valid &&
           schedule_cache_valid;
  }
  bool products_complete() const { return device_info_ok && statistics_ok; }
  /// What the node actually presents to the user.
  bool pump_ready() const { return caches_synced(); }

  void tick() {
    now += UPDATE_INTERVAL_MS;
    const bool pump_up = now >= pump_answers_from_ms;

    // The chain-independent polls, which run whenever the session is up.
    if (session_ready && chain_done && pump_up) {
      control_cache_valid = true;
      schedule_cache_valid = true;
    }

    // The one-shot reads resolve, and only land if the pump was answering.
    if (reads_in_flight && now >= reads_land_at) {
      reads_in_flight = false;
      if (pump_up) {
        device_info_ok = true;
        statistics_ok = true;
      }
    }

    if (!session_ready)
      return;

    if (!chain_done) {
      chain_done = true;
      started_ms = now;
      triggers++;
      reads_in_flight = true;
      reads_land_at = now + UPDATE_INTERVAL_MS;
      return;
    }

    if (rearm_enabled &&
        should_rearm_initial_read(session_ready, chain_done, caches_synced(),
                                  products_complete(), now, started_ms,
                                  retry_interval)) {
      rearms++;
      chain_done = false;
      reads_in_flight = false;  // retired with the generation bump
      device_info_ok = false;
      statistics_ok = false;
      retry_interval = next_initial_read_backoff_ms(retry_interval, MAX_MS);
    } else if (products_complete() && caches_synced()) {
      retry_interval = TIMEOUT_MS;
    }
  }

  void run_for_ms(uint32_t duration) {
    const uint32_t end = now + duration;
    while (now < end)
      tick();
  }
};

void test_healthy_link_is_never_rearmed() {
  ReadChainSim sim;
  sim.run_for_ms(10 * 60 * 1000);  // ten minutes

  TEST_ASSERT(sim.triggers == 1,
              "healthy link triggers the read chain exactly once");
  TEST_ASSERT(sim.rearms == 0, "healthy link is never re-armed");
  TEST_ASSERT(sim.products_complete() && sim.caches_synced(),
              "healthy link lands both the chain reads and the caches");
}

// The case the first version of this fix got wrong. The caches heal on their
// own at ~30 s, well inside the 60 s bound, so a predicate that treats
// "caches valid" as success stops retrying while device info and the
// statistics are still unread -- and Pump Ready comes on regardless, so
// nothing downstream reveals it.
void test_self_healing_caches_do_not_count_as_success() {
  ReadChainSim sim;
  sim.pump_answers_from_ms = 25000;
  sim.run_for_ms(5 * 60 * 1000);

  TEST_ASSERT(sim.pump_ready(),
              "the caches heal on their own, so the node reports ready");
  TEST_ASSERT(sim.rearms >= 1,
              "the chain is still re-armed even though the caches are valid "
              "and the node looks ready");
  TEST_ASSERT(sim.products_complete(),
              "device info and the statistics are actually read");
}

void test_stalled_attempt_is_rearmed_and_recovers() {
  ReadChainSim sim;
  // The pump ignores everything for the first 90 s -- long enough that the
  // first attempt, fired at t=10 s, is a total loss.
  sim.pump_answers_from_ms = 90000;
  sim.run_for_ms(10 * 60 * 1000);

  TEST_ASSERT(sim.rearms >= 1, "a stalled attempt is re-armed");
  TEST_ASSERT(sim.triggers >= 2, "re-arming actually re-runs the read chain");
  TEST_ASSERT(sim.products_complete(),
              "the chain reads land once the pump answers");
}

// Recovery must not depend on the pump happening to wake up in a lucky phase
// of the retry cycle: sweep the moment it starts answering across ten minutes
// and require every case to converge.
void test_recovery_does_not_depend_on_when_the_pump_wakes() {
  int failures = 0;
  for (uint32_t wake = 0; wake <= 600000; wake += 5000) {
    ReadChainSim sim;
    sim.pump_answers_from_ms = wake;
    sim.run_for_ms(60 * 60 * 1000);  // an hour is ample at the 10 min ceiling
    if (!sim.products_complete() || !sim.caches_synced())
      failures++;
  }
  TEST_ASSERT(failures == 0,
              "the node converges for every pump wake-up time across ten "
              "minutes, not just lucky ones");
}

// The regression proof: the same scenario with the re-arm removed must stay
// broken forever. If this ever passes with rearm_enabled=false, the retry is
// not what is doing the work.
void test_without_the_rearm_the_node_stays_stuck_forever() {
  ReadChainSim sim;
  sim.pump_answers_from_ms = 90000;
  sim.rearm_enabled = false;
  sim.run_for_ms(60 * 60 * 1000);  // a full hour

  TEST_ASSERT(sim.triggers == 1, "without the re-arm the chain runs only once");
  TEST_ASSERT(!sim.products_complete(),
              "without the re-arm device info and the statistics are never "
              "read, even after an hour with a fully answering pump");
  TEST_ASSERT(sim.pump_ready(),
              "...while the node still reports ready, which is what made this "
              "invisible on the bench");
}

void test_backoff_grows_and_is_capped() {
  TEST_ASSERT(next_initial_read_backoff_ms(TIMEOUT_MS, MAX_MS) == 120000u,
              "the interval doubles on the first retry");
  TEST_ASSERT(next_initial_read_backoff_ms(480000u, MAX_MS) == MAX_MS,
              "a double that would overshoot is clamped to the ceiling");
  TEST_ASSERT(next_initial_read_backoff_ms(MAX_MS, MAX_MS) == MAX_MS,
              "the ceiling is a fixed point");
  TEST_ASSERT(next_initial_read_backoff_ms(0x80000000u, MAX_MS) == MAX_MS,
              "a double that would overflow clamps rather than wrapping to a "
              "tiny interval");
}

// A pump that never returns the chain's values must not be re-read forever at
// the base rate; the backoff has to settle it to the ceiling.
void test_a_permanently_silent_pump_settles_to_the_ceiling() {
  ReadChainSim sim;
  sim.pump_answers_from_ms = 0xFFFFFFFFu;  // never
  sim.run_for_ms(60 * 60 * 1000);

  TEST_ASSERT(sim.retry_interval == MAX_MS,
              "the retry interval reaches the ceiling");
  TEST_ASSERT(sim.rearms <= 12,
              "an hour against a silent pump costs a dozen retries, not sixty");
  TEST_ASSERT(sim.rearms >= 4, "...but it does keep trying");
}

void test_no_rearm_before_the_timeout_elapses() {
  // One millisecond short of the bound: still the attempt's own time.
  TEST_ASSERT(!should_rearm_initial_read(true, true, false, false, 59999, 0,
                                         TIMEOUT_MS),
              "no re-arm one millisecond before the timeout");
  TEST_ASSERT(should_rearm_initial_read(true, true, false, false, 60000, 0,
                                        TIMEOUT_MS),
              "re-arms exactly on the timeout boundary");
}

void test_synchronised_is_never_rearmed_however_long_it_has_been() {
  TEST_ASSERT(!should_rearm_initial_read(true, true, /*caches_synchronized=*/true,
                                         /*chain_products_complete=*/true,
                                         3600000, 0, TIMEOUT_MS),
              "a synchronised chain is never re-armed, however old the attempt");
}

void test_an_untriggered_chain_is_not_rearmed() {
  // chain_done=false means update() is about to trigger it on the normal path;
  // re-arming here would double-trigger.
  TEST_ASSERT(!should_rearm_initial_read(true, /*chain_done=*/false, false, false,
                                         3600000, 0, TIMEOUT_MS),
              "a chain that never ran is not re-armed");
}

void test_a_down_session_is_not_rearmed() {
  // The disconnect path clears the latch itself; re-arming here would race it.
  TEST_ASSERT(!should_rearm_initial_read(/*session_ready=*/false, true, false, false,
                                         3600000, 0, TIMEOUT_MS),
              "a session that is not ready is not re-armed");
}

void test_millis_rollover_does_not_trigger_a_spurious_rearm() {
  // Attempt started 30 s before the ~49-day wrap, now 30 s after it: 60 s of
  // real elapsed time, so this SHOULD fire — and, crucially, the arithmetic
  // must not read as a 49-day gap on the tick before it.
  const uint32_t before_wrap = 0xFFFFFFFFu - 30000u;
  TEST_ASSERT(should_rearm_initial_read(true, true, false, false, 29999u,
                                        before_wrap, TIMEOUT_MS),
              "60 s spanning the millis() wrap re-arms");
  TEST_ASSERT(!should_rearm_initial_read(true, true, false, false, 9999u, before_wrap,
                                         TIMEOUT_MS),
              "40 s spanning the millis() wrap does not re-arm");

  // The case that actually separates rollover-correct subtraction from the
  // natural-looking `now >= started + timeout`: the sum wraps too, so that
  // form collapses to a comparison against a tiny number and fires against
  // almost any `now`. Here the attempt started 16 ms before the wrap and only
  // 10 ms have passed, so re-arming would abandon an attempt that has barely
  // begun — and would then do it again on every tick until the wrap.
  const uint32_t just_before_wrap = 0xFFFFFFF0u;
  TEST_ASSERT(!should_rearm_initial_read(true, true, false, false,
                                         just_before_wrap + 10u,
                                         just_before_wrap, TIMEOUT_MS),
              "an attempt started 10 ms ago just before the millis() wrap is "
              "not re-armed");
}

}  // namespace

int main() {
  std::cout << "=== Initial-read re-arm tests ===" << std::endl;

  test_healthy_link_is_never_rearmed();
  test_self_healing_caches_do_not_count_as_success();
  test_stalled_attempt_is_rearmed_and_recovers();
  test_recovery_does_not_depend_on_when_the_pump_wakes();
  test_without_the_rearm_the_node_stays_stuck_forever();
  test_no_rearm_before_the_timeout_elapses();
  test_synchronised_is_never_rearmed_however_long_it_has_been();
  test_an_untriggered_chain_is_not_rearmed();
  test_a_down_session_is_not_rearmed();
  test_backoff_grows_and_is_capped();
  test_a_permanently_silent_pump_settles_to_the_ceiling();
  test_millis_rollover_does_not_trigger_a_spurious_rearm();

  std::cout << "\nPassed: " << tests_passed << ", Failed: " << tests_failed
            << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
