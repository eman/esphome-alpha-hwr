// Host tests for the inbound-data watchdog (audit finding 8: "the deaf node
// reports Connected forever").
//
// The regression these guard: authentication is a pure scheduler chain that
// never inspects a reply, and of the six paths through
// subscribe_to_notifications(), four return without announcing anything while
// the remaining two — CCCD write failed and CCCD write succeeded — both fall
// through to the subscribed callback, so a failed CCCD write is indistinguishable
// from a good one. A link whose GATT writes succeed but whose notifications
// never arrive therefore reaches READY and stays there — and because the Pump
// Link Status ladder's first rung is is_ready(), it refreshes its own "last
// open" timestamp and can never fall through to Unreachable.
//
// The predicate under test is what decides to recycle such a link. The two
// directions matter equally: it must fire on a link that has gone quiet, and it
// must NOT fire on a healthy one, because the remedy is a forced disconnect and
// a false positive is a permanent reconnect loop.

#include <cstdint>
#include <iostream>

#include "../components/alpha_hwr/link_watchdog.h"

using esphome::alpha_hwr::link_data_timeout_expired;
using esphome::alpha_hwr::link_data_timeout_next;
using esphome::alpha_hwr::LinkGapSampler;

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

static constexpr uint32_t TIMEOUT_MS = 60000;  // the shipped default

// Mirrors the component's wiring: link_last_inbound_ms_ is seeded at
// connection-open and re-stamped on every received notification, and the
// predicate is evaluated from the ~1s loop() tick.
//
// The disconnect is modelled as ASYNCHRONOUS, which is the whole point.
// check_link_liveness_() calls force_disconnect() -> client_->disconnect() ->
// esp_ble_gattc_close(), which posts to the Bluetooth task; the session stays
// is_connected() until ESP_GATTC_DISCONNECT_EVT arrives on a later loop pass.
// Modelling it as instantaneous would hide the only thing keeping the watchdog
// from re-firing every tick in that gap: the re-arm of link_last_inbound_ms_
// before the disconnect call.
struct LinkSim {
  uint32_t now{0};
  uint32_t last_inbound{0};
  bool connected{false};
  int fired{0};
  /// Ticks between requesting the disconnect and the session going IDLE.
  int disconnect_latency_ticks{1};
  /// Set false to model the component WITHOUT the re-arm, to show it is
  /// load-bearing rather than decorative.
  bool rearm{true};

  /// The gap statistic, stamped at the same three points the component stamps
  /// last_inbound at. Set sample_recycles false to model it WITHOUT
  /// on_recycle(), which is what censors the sample at the budget.
  LinkGapSampler gap;
  bool sample_recycles{true};

  int pending_disconnect_{0};

  void open(uint32_t at_ms) {
    now = at_ms;
    connected = true;
    last_inbound = at_ms;  // seeded from the open, not from READY
    gap.on_open(at_ms);
    pending_disconnect_ = 0;
  }
  void notification() {
    last_inbound = now;
    gap.on_inbound(now);
  }
  /// A clean drop: the pump or the stack closes the link with no watchdog
  /// involvement. The watchdog does not run while disconnected, so nothing is
  /// sampled until the next open.
  void drop() {
    connected = false;
    pending_disconnect_ = 0;
  }

  /// Advance by one 1s loop() tick, evaluating the watchdog as the component
  /// does. Returns true if it fired on this tick.
  bool tick() {
    now += 1000;
    if (pending_disconnect_ > 0 && --pending_disconnect_ == 0)
      connected = false;  // the DISCONNECT event finally lands
    if (!link_data_timeout_expired(connected, now, last_inbound, TIMEOUT_MS))
      return false;
    fired++;
    if (sample_recycles)
      gap.on_recycle(now);  // record the interval before the re-arm erases it
    if (rearm)
      last_inbound = now;  // check_link_liveness_() re-arms before disconnecting
    // A repeat disconnect() on an already-disconnecting link does not postpone
    // the event, so only arm the countdown when none is in flight. (Modelling
    // it the other way round makes a re-firing watchdog look like it prevents
    // its own recovery, which overstates the bug.)
    if (pending_disconnect_ == 0)
      pending_disconnect_ = disconnect_latency_ticks;
    return true;
  }
};

void test_disabled_and_disconnected() {
  // 0 disables the watchdog outright — the documented escape hatch for a pump
  // that legitimately goes quiet. A full day of silence must not fire.
  TEST_ASSERT(!link_data_timeout_expired(true, 86400000u, 0, 0),
              "timeout_ms == 0 disables the watchdog entirely");

  // IDLE/ERROR: nothing to tear down, and last_inbound is stale by definition.
  TEST_ASSERT(!link_data_timeout_expired(false, 86400000u, 0, TIMEOUT_MS),
              "A disconnected session never trips the watchdog");
}

void test_boundary() {
  const uint32_t open_at = 5000;
  TEST_ASSERT(!link_data_timeout_expired(true, open_at, open_at, TIMEOUT_MS),
              "Zero elapsed does not fire");
  TEST_ASSERT(!link_data_timeout_expired(true, open_at + TIMEOUT_MS, open_at, TIMEOUT_MS),
              "Exactly the budget does not fire (strictly greater)");
  TEST_ASSERT(link_data_timeout_expired(true, open_at + TIMEOUT_MS + 1, open_at, TIMEOUT_MS),
              "One millisecond past the budget fires");
}

void test_healthy_polling_never_fires() {
  // AlphaHwrComponent::update() polls five telemetry registers plus the
  // schedule every 10s while READY, so the healthy inter-notification gap is
  // bounded by the poll interval rather than by pump behaviour. Run an hour of
  // it. This is the false-positive direction, and it is the one that matters:
  // firing here would mean a permanent reconnect loop on a working pump.
  LinkSim sim;
  sim.open(1000);
  for (int t = 0; t < 3600; t++) {
    sim.tick();
    if (t % 10 == 9)
      sim.notification();  // the poll answer, once per 10s cycle
  }
  TEST_ASSERT(sim.fired == 0, "An hour of healthy 10s polling never trips the watchdog");
  TEST_ASSERT(sim.connected, "...and the link is left up");
}

void test_deaf_from_connection_open() {
  // The finding's exact scenario: CCCD write fails, subscribe_to_notifications()
  // falls through to the subscribed callback anyway, auth completes ~3.2 s later
  // on its timers (2000 ms stabilize + a 1200 ms chain), READY is announced, and
  // not one byte ever arrives.
  LinkSim sim;
  sim.open(1000);
  int fired_at = -1;
  for (int t = 0; t < 300; t++) {
    if (sim.tick() && fired_at < 0)
      fired_at = t;
  }
  TEST_ASSERT(fired_at >= 0, "A link that is deaf from the open is eventually recycled");
  // Budget elapses at open+60000; the first tick strictly past it is t=60
  // (now = 1000 + 61*1000 = 62000, elapsed 61000).
  TEST_ASSERT(fired_at == 60, "...on the first 1s tick past the budget, not sooner or later");
}

void test_rearm_prevents_refiring_during_the_async_disconnect() {
  // The re-arm in check_link_liveness_() is the only thing stopping the
  // watchdog from firing on every tick between requesting the disconnect and
  // the DISCONNECT event landing. Give that gap a realistic 8 ticks and show
  // the difference, because an untested guard is an assumed one.
  LinkSim with_rearm;
  with_rearm.disconnect_latency_ticks = 8;
  with_rearm.open(1000);
  for (int t = 0; t < 200; t++)
    with_rearm.tick();
  TEST_ASSERT(with_rearm.fired == 1,
              "The re-arm holds the watchdog to one fire while the disconnect is in flight");
  TEST_ASSERT(!with_rearm.connected, "...and the link is down once the event lands");

  LinkSim without_rearm;
  without_rearm.disconnect_latency_ticks = 8;
  without_rearm.rearm = false;
  without_rearm.open(1000);
  for (int t = 0; t < 200; t++)
    without_rearm.tick();
  TEST_ASSERT(without_rearm.fired == 8,
              "Without it the same episode fires once per tick until the event lands");
}

void test_deaf_mid_session() {
  // The pump stops answering 10 minutes into a healthy session. Same
  // observable, same remedy — one timer covers both causes.
  LinkSim sim;
  sim.open(1000);
  for (int t = 0; t < 600; t++) {
    sim.tick();
    if (t % 10 == 9)
      sim.notification();
  }
  TEST_ASSERT(sim.fired == 0, "Ten healthy minutes pass untouched");

  for (int t = 0; t < 300; t++)
    sim.tick();  // silence from here on
  TEST_ASSERT(sim.fired == 1, "A session that goes deaf mid-run is recycled too");
}

// --- The gap statistic (issue #176) -----------------------------------------
//
// These guard the property that makes the number worth acting on: it samples
// the intervals the watchdog is timed over, all of them. Both failures below
// bias it in the same direction — downward, toward "the budget was never
// close" — which is the direction that argues for keeping a default nobody has
// validated.

void test_gap_samples_the_interval_from_connection_open() {
  // The open-to-first-notification interval is inside the watchdog's window and
  // is its worst case (17.2 s budgeted against 60 s), so it has to be in the
  // sample. Excluding it as "handshake, not cadence" left the statistic unable
  // to report the case the default is tightest against.
  LinkSim sim;
  sim.open(1000);
  for (int t = 0; t < 6; t++)
    sim.tick();
  sim.notification();  // first inbound data 6 s after the open
  TEST_ASSERT(sim.gap.max_ms() == 6000,
              "The interval from connection-open to the first notification is sampled");

  // ...and on every later connection too, not just the first after boot.
  sim.drop();
  sim.open(sim.now + 4000);
  for (int t = 0; t < 9; t++)
    sim.tick();
  sim.notification();
  TEST_ASSERT(sim.gap.max_ms() == 9000,
              "...on reconnects as well, which is where the handshake repeats");
}

void test_gap_ignores_time_spent_disconnected() {
  // A link that is down is not a link that is quiet: the watchdog does not run
  // between the drop and the next open, so those minutes must not land in a
  // statistic that is read as "how long this pump goes without talking".
  LinkSim sim;
  sim.open(1000);
  sim.notification();
  sim.drop();
  sim.now += 600000;  // ten minutes off the air
  sim.open(sim.now);
  sim.notification();
  TEST_ASSERT(sim.gap.max_ms() == 0,
              "Time between a disconnect and the next open is not sampled");
}

void test_gap_tracks_the_longest_quiet_interval() {
  // Steady state is bounded by our own 10 s poll, and the running maximum keeps
  // the worst interval rather than the latest one.
  LinkSim sim;
  sim.open(1000);
  for (int t = 0; t < 600; t++) {
    sim.tick();
    if (t % 10 == 9)
      sim.notification();
  }
  TEST_ASSERT(sim.fired == 0, "Ten healthy minutes are not recycled");
  TEST_ASSERT(sim.gap.max_ms() == 10000,
              "A healthy link reports the poll interval as its longest gap");

  for (int t = 0; t < 25; t++)
    sim.tick();  // one 25 s lull, well inside the budget
  sim.notification();
  TEST_ASSERT(sim.gap.max_ms() == 25000, "A single lull moves the maximum...");
  for (int t = 0; t < 60; t++) {
    sim.tick();
    if (t % 10 == 9)
      sim.notification();
  }
  TEST_ASSERT(sim.gap.max_ms() == 25000, "...and healthy traffic afterwards does not lower it");
}

void test_gap_is_not_censored_at_the_budget() {
  // The regression this exists for. An interval that ends in a recycle is never
  // closed by a notification -- the watchdog re-arms and disconnects -- so
  // without on_recycle() every excursion past the budget is discarded and the
  // maximum asymptotes to just under it whatever the pump does. Read as
  // "60 s was comfortable", it means the opposite.
  // Three full deaf cycles: open, budget expires, recycle, reconnect, repeat --
  // the flap this whole watchdog is about.
  auto run_deaf_cycles = [](LinkSim &s, int cycles) {
    s.open(1000);
    for (int c = 0; c < cycles; c++) {
      const int before = s.fired;
      for (int guard = 0; guard < 200 && s.fired == before; guard++)
        s.tick();
      for (int guard = 0; guard < 200 && s.connected; guard++)
        s.tick();  // the async DISCONNECT event lands
      s.open(s.now + 6000);  // reconnect, still deaf
    }
  };

  LinkSim censored;
  censored.sample_recycles = false;
  run_deaf_cycles(censored, 3);
  TEST_ASSERT(censored.fired == 3, "Three budgets expire on a link that never speaks");
  TEST_ASSERT(censored.gap.max_ms() == 0,
              "Without the recycle sample, a link that never speaks reports no gap at all");

  LinkSim sim;
  run_deaf_cycles(sim, 3);
  TEST_ASSERT(sim.gap.max_ms() > TIMEOUT_MS,
              "With it, the maximum exceeds the configured budget: the ceiling was reached");
  TEST_ASSERT(sim.gap.max_ms() <= TIMEOUT_MS + 1000,
              "...by no more than the 1s evaluation tick, since it is the budget plus a tick");
}

void test_gap_records_a_floor_not_a_measurement() {
  // The recorded value is what the link was given, not what it would have
  // taken: the tear-down ends the observation. A pump that would have spoken at
  // 90 s and one that never speaks again are indistinguishable here, and both
  // read as "reached the ceiling" -- which is the honest report, and the reason
  // the collection period wants a deliberately long data_timeout.
  LinkSim quiet_but_alive;
  quiet_but_alive.open(1000);
  for (int t = 0; t < 600; t++)
    quiet_but_alive.tick();

  LinkSim permanently_deaf;
  permanently_deaf.open(1000);
  for (int t = 0; t < 6000; t++)
    permanently_deaf.tick();

  TEST_ASSERT(quiet_but_alive.gap.max_ms() == permanently_deaf.gap.max_ms(),
              "A censored interval reports the budget it hit, not how long the quiet lasted");
}

void test_millis_rollover() {
  // millis() wraps every ~49 days. The elapsed test is unsigned subtraction,
  // which stays correct across the wrap; comparing the timestamps directly
  // would not. A node whose last notification landed just before the wrap must
  // not be declared deaf on the first tick after it.
  const uint32_t before_wrap = 0xFFFFF000u;  // 4096 ms before the wrap
  const uint32_t after_wrap = 1000u;         // 5096 ms of true elapsed time

  TEST_ASSERT(!link_data_timeout_expired(true, after_wrap, before_wrap, TIMEOUT_MS),
              "A healthy link is not falsely recycled across the millis() rollover");

  // The discriminating case, and the one with real teeth. Writing the test as
  // `now > last + timeout` instead agrees with the subtraction form on both
  // assertions around it, so neither would catch that spelling. Here the
  // addition overflows while the subtraction does not: a node approaching ~49
  // days of uptime, one second into a perfectly healthy window, would be torn
  // down on every tick until the counter wrapped past the budget.
  TEST_ASSERT(!link_data_timeout_expired(true, before_wrap + 1000, before_wrap, TIMEOUT_MS),
              "A healthy link 1s into its window just below the wrap is not recycled");

  // ...and a genuinely deaf link is still caught on the far side of the wrap.
  const uint32_t long_after = before_wrap + TIMEOUT_MS + 1;  // wraps around
  TEST_ASSERT(link_data_timeout_expired(true, long_after, before_wrap, TIMEOUT_MS),
              "A deaf link is still caught when the budget spans the rollover");
}

void test_predicate_has_no_notion_of_ready() {
  // Four of the six subscribe paths return without ever calling
  // subscribed_callback_(), leaving the session parked in SUBSCRIBING forever.
  // Session::is_connected() is true there, so timing the window from the open
  // rather than from READY covers those too.
  //
  // Honest caveat: this asserts nothing the boundary test does not already
  // cover. The predicate takes a bool and cannot tell SUBSCRIBING from READY —
  // that is precisely the property being recorded, and the coverage it implies
  // lives at the call site (which passes Session::is_connected(), not
  // is_ready()), not here.
  const uint32_t open_at = 1000;
  TEST_ASSERT(link_data_timeout_expired(/*connected=*/true, open_at + TIMEOUT_MS + 1, open_at,
                                        TIMEOUT_MS),
              "The budget does not depend on having reached READY");
}

// ── Backoff (issue #176) ─────────────────────────────────────────────────────
// A link that stays deaf was recycled every ~66 s indefinitely: ~1,300 passes a
// day, each one re-entering the encryption-on-open path where a failure can
// erase the bond (issue #14). Doubling with a ceiling bounds that without
// costing recovery -- a link that can recover still does on the first or second
// try.
void test_backoff_doubles_to_the_cap() {
  std::cout << "\n=== The window doubles, then stops at the cap ===" << std::endl;

  const uint32_t cap = esphome::alpha_hwr::LINK_DATA_TIMEOUT_BACKOFF_CAP_MS;
  TEST_ASSERT(cap == 3600000u, "The default ceiling is one hour");

  uint32_t w = 60000;
  w = link_data_timeout_next(w, cap);
  TEST_ASSERT(w == 120000u, "60 s doubles to 120 s");
  w = link_data_timeout_next(w, cap);
  TEST_ASSERT(w == 240000u, "...then 240 s");

  // Walk it to the ceiling and confirm it stops there rather than overshooting.
  int steps = 2;
  while (w < cap && steps < 100) {
    w = link_data_timeout_next(w, cap);
    steps++;
  }
  TEST_ASSERT(w == cap, "The window lands exactly on the cap, not past it");
  TEST_ASSERT(steps == 6,
              "...after 6 doublings from 60 s — about 28 recycles a day "
              "against ~1,300 before");
  TEST_ASSERT(link_data_timeout_next(w, cap) == cap,
              "Once at the cap it stays there however long the link stays deaf");
}

void test_backoff_leaves_a_disabled_watchdog_disabled() {
  std::cout << "\n=== Backoff cannot switch the watchdog on ===" << std::endl;

  // 0 means disabled everywhere else in this header, and doubling "never" is
  // meaningless. Returning anything else would silently arm a watchdog the
  // operator turned off.
  TEST_ASSERT(link_data_timeout_next(0, 3600000u) == 0,
              "A disabled watchdog stays disabled after a backoff step");
}

void test_backoff_never_shrinks_a_configured_window() {
  std::cout << "\n=== A budget larger than the cap is not overridden ==="
            << std::endl;

  // Someone who configures two hours means it. Clamping down to the ceiling
  // would be the backoff making the watchdog *more* aggressive, which is the
  // opposite of its purpose.
  const uint32_t two_hours = 7200000u;
  TEST_ASSERT(link_data_timeout_next(two_hours, 3600000u) == two_hours,
              "A window already past the cap is returned unchanged");
  TEST_ASSERT(link_data_timeout_next(3600000u, 3600000u) == 3600000u,
              "A window exactly at the cap is returned unchanged");
}

void test_backoff_cannot_wrap_into_a_tiny_window() {
  std::cout << "\n=== Doubling past 2^31 clamps rather than wrapping ==="
            << std::endl;

  // Unreachable from any sane configuration, but a wrap would turn a very long
  // window into a near-zero one — a watchdog that fires constantly, which is
  // the failure this whole change exists to prevent.
  const uint32_t huge = 0x90000000u;  // doubling overflows uint32
  const uint32_t out = link_data_timeout_next(huge, 3600000u);
  TEST_ASSERT(out == huge,
              "A window past the cap is returned unchanged before any doubling "
              "can overflow");

  // And with a cap above it, the doubling itself is what must not wrap.
  // Pinned exactly, not as an inequality. `out2 >= huge` let the overflow
  // clamp be mutated to `return current_ms;` with the suite still green -- the
  // two differ for real inputs (2147483648 with a 4294967295 cap gives
  // 4294967295 against 2147483648) even though the shipped 1-hour cap makes the
  // branch unreachable in production. A dead branch is still worth pinning if
  // it is the one standing between a doubling and a wrap.
  const uint32_t out2 = link_data_timeout_next(huge, 0xFFFFFFFFu);
  TEST_ASSERT(out2 == 0xFFFFFFFFu,
              "Doubling toward a huge cap clamps to the cap rather than "
              "wrapping to a tiny window");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Link Watchdog Tests (audit finding 8)" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_disabled_and_disconnected();
  test_boundary();
  test_healthy_polling_never_fires();
  test_deaf_from_connection_open();
  test_rearm_prevents_refiring_during_the_async_disconnect();
  test_deaf_mid_session();
  test_gap_samples_the_interval_from_connection_open();
  test_gap_ignores_time_spent_disconnected();
  test_gap_tracks_the_longest_quiet_interval();
  test_gap_is_not_censored_at_the_budget();
  test_gap_records_a_floor_not_a_measurement();
  test_millis_rollover();
  test_predicate_has_no_notion_of_ready();
  test_backoff_doubles_to_the_cap();
  test_backoff_leaves_a_disabled_watchdog_disabled();
  test_backoff_never_shrinks_a_configured_window();
  test_backoff_cannot_wrap_into_a_tiny_window();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
