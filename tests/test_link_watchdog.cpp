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
using esphome::alpha_hwr::link_gap_thresholds_censored;
using esphome::alpha_hwr::LinkGapSampler;
using esphome::alpha_hwr::LINK_DATA_TIMEOUT_BACKOFF_CAP_MS;

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

  /// The gap statistic, stamped at the same points the component stamps
  /// last_inbound at, plus the disconnect. Set sample_recycles false to model it
  /// WITHOUT on_recycle(), which is what censors the sample at the budget.
  LinkGapSampler gap;
  bool sample_recycles{true};
  /// Set false with sample_recycles to model the sampler as it shipped in #189:
  /// closing an interval on a notification and on nothing else.
  bool sample_disconnects{true};

  /// The window currently in force. The component backs this off on every
  /// recycle that produced no data (link_data_timeout_next) and resets it only
  /// on a notification received while READY, so the simulator does too: without
  /// the backoff it would answer questions about a watchdog that stopped
  /// shipping when the backoff landed, and both the fire cadence and what the
  /// gap statistic records depend on it.
  uint32_t window{TIMEOUT_MS};
  /// A deaf pump still answers the handshake, so the backoff reset is gated on
  /// READY rather than on any notification. Set false to model frames arriving
  /// on a session that never gets there.
  bool ready{true};

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
    if (ready)
      window = TIMEOUT_MS;
  }
  /// A clean drop: the pump or the stack closes the link with no watchdog
  /// involvement. Closes the interval the drop ended -- the watchdog was timing
  /// it until the link went away -- and nothing is sampled again until the next
  /// open.
  void drop() {
    connected = false;
    pending_disconnect_ = 0;
    if (sample_disconnects)
      gap.on_disconnect(now);
  }

  /// Advance by one 1s loop() tick, evaluating the watchdog as the component
  /// does. Returns true if it fired on this tick.
  bool tick() {
    now += 1000;
    if (pending_disconnect_ > 0 && --pending_disconnect_ == 0) {
      connected = false;  // the DISCONNECT event finally lands
      if (sample_disconnects)
        gap.on_disconnect(now);  // ...and the component's callback closes the interval
    }
    if (!link_data_timeout_expired(connected, now, last_inbound, window))
      return false;
    fired++;
    if (sample_recycles)
      gap.on_recycle(now);  // record the interval before the re-arm erases it
    window = link_data_timeout_next(window, LINK_DATA_TIMEOUT_BACKOFF_CAP_MS);
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
  // The re-arm in check_link_liveness_() stops the watchdog firing on every
  // tick between requesting the disconnect and the DISCONNECT event landing.
  // Give that gap a realistic 8 ticks. An untested guard is an assumed one.
  LinkSim with_rearm;
  with_rearm.disconnect_latency_ticks = 8;
  with_rearm.open(1000);
  for (int t = 0; t < 200; t++)
    with_rearm.tick();
  TEST_ASSERT(with_rearm.fired == 1,
              "The re-arm holds the watchdog to one fire while the disconnect is in flight");
  TEST_ASSERT(!with_rearm.connected, "...and the link is down once the event lands");

  // At the configured budget the backoff covers the same window on its own: the
  // fire doubles 60s to 120s, so the next tick is nowhere near expired even
  // with a stale stamp. This test asserted 8 fires here until the simulator
  // learned about the backoff, which is worth stating plainly -- the number was
  // measuring a watchdog that stopped shipping when the backoff landed.
  LinkSim without_rearm;
  without_rearm.disconnect_latency_ticks = 8;
  without_rearm.rearm = false;
  without_rearm.open(1000);
  for (int t = 0; t < 200; t++)
    without_rearm.tick();
  TEST_ASSERT(without_rearm.fired == 1,
              "At the default budget the backoff alone also holds it to one fire");

  // Where the re-arm is still the only guard: a permanently deaf link ends at
  // the one-hour cap, and link_data_timeout_next() returns the cap unchanged.
  // With nothing left to double, a stale stamp is expired on every tick of the
  // disconnect window, and each fire re-latches the failure reason on a link
  // that is already being torn down.
  LinkSim at_cap;
  at_cap.disconnect_latency_ticks = 8;
  at_cap.rearm = false;
  at_cap.window = LINK_DATA_TIMEOUT_BACKOFF_CAP_MS;
  at_cap.open(1000);
  for (int t = 0; t < 4000; t++)
    at_cap.tick();
  TEST_ASSERT(at_cap.fired == 8,
              "At the backoff cap, without the re-arm, it fires once per tick until the event lands");

  LinkSim at_cap_rearmed;
  at_cap_rearmed.disconnect_latency_ticks = 8;
  at_cap_rearmed.window = LINK_DATA_TIMEOUT_BACKOFF_CAP_MS;
  at_cap_rearmed.open(1000);
  for (int t = 0; t < 4000; t++)
    at_cap_rearmed.tick();
  TEST_ASSERT(at_cap_rearmed.fired == 1, "...and once with it, which is what the re-arm is for");
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
      for (int guard = 0; guard < 1000 && s.fired == before; guard++)
        s.tick();
      for (int guard = 0; guard < 1000 && s.connected; guard++)
        s.tick();  // the async DISCONNECT event lands
      s.open(s.now + 6000);  // reconnect, still deaf
    }
  };

  LinkSim censored;
  censored.sample_recycles = false;
  censored.sample_disconnects = false;
  run_deaf_cycles(censored, 3);
  TEST_ASSERT(censored.fired == 3, "Three budgets expire on a link that never speaks");
  TEST_ASSERT(censored.gap.max_ms() == 0,
              "As #189 shipped it, three expired budgets leave the maximum at zero");

  LinkSim sim;
  run_deaf_cycles(sim, 3);
  TEST_ASSERT(sim.gap.max_ms() > TIMEOUT_MS,
              "With it, the maximum exceeds the configured budget");
  // No notification ever arrives, so nothing resets the backoff: the three
  // windows are 60s, 120s and 240s, each recorded a tick after it expired. The
  // maximum is therefore the LAST window, not the configured budget -- which is
  // what lets a reading say which ceiling was hit, and is why "budget plus a
  // tick" would be the wrong bound to assert here.
  TEST_ASSERT(sim.gap.max_ms() > 4 * TIMEOUT_MS,
              "...and tracks the widened window rather than the configured one");
  TEST_ASSERT(sim.gap.max_ms() <= 4 * TIMEOUT_MS + 1000,
              "...landing one evaluation tick past the third window");
}

void test_gap_records_an_interval_ended_by_a_plain_drop() {
  // Not every quiet interval ends in a notification or a recycle. A link that
  // goes quiet and is then dropped by the stack -- supervision timeout, pump
  // power loss, the encryption-failure teardown -- was being timed against the
  // budget right up to the drop, and discarding that interval censors the
  // sample at a threshold nobody configured.
  LinkSim sim;
  sim.open(1000);
  for (int t = 0; t < 10; t++)
    sim.tick();
  sim.notification();  // a healthy session, 10s in
  for (int t = 0; t < 45; t++)
    sim.tick();  // 45s of quiet, inside the 60s budget...
  sim.drop();    // ...ended by the link going away, not by the watchdog
  TEST_ASSERT(sim.fired == 0, "The watchdog never fired: the drop came first");
  TEST_ASSERT(sim.gap.max_ms() == 45000,
              "The interval the drop ended is recorded, not discarded");

  // ...but the downtime after it is not, however long the link stays away.
  sim.now += 600000;
  sim.open(sim.now);
  sim.notification();
  TEST_ASSERT(sim.gap.max_ms() == 45000, "...while the downtime that follows still is not");
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

void test_gap_ignores_a_disconnect_with_no_open_before_it() {
  // A connection attempt that fails without ever opening still reports a
  // disconnect. Sampling it would record the downtime since the previous
  // session as though the link had been up and silent throughout -- an inflated
  // reading, which argues for a longer timeout and is exactly as wrong as the
  // deflated one the recycle sample fixes.
  LinkGapSampler s;
  s.on_disconnect(500000);
  TEST_ASSERT(s.max_ms() == 0, "A disconnect with no open before it samples nothing");

  s.on_open(600000);
  s.on_inbound(604000);
  s.on_disconnect(605000);
  TEST_ASSERT(s.max_ms() == 4000, "...while a real session still reports its own intervals");

  s.on_disconnect(900000);
  TEST_ASSERT(s.max_ms() == 4000,
              "...and a second disconnect callback does not sample the downtime after the first");
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

// ---------------------------------------------------------------------------
// The tail histogram (issue #176 part 1). The counters exist so the
// data_timeout default can be chosen from what installations actually do; a
// running maximum gives one point of the distribution and these give its shape.
// Everything below guards a way the counters could look plausible and mean
// something other than what the analysis will assume they mean.
// ---------------------------------------------------------------------------

void test_gap_buckets_count_each_interval_once() {
  std::cout << "\n=== One interval counts once, in every rung below it ==="
            << std::endl;

  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(47000);  // one 47s quiet interval, ended by data

  TEST_ASSERT(gap.over_count(0) == 1 && gap.over_count(1) == 1 &&
                  gap.over_count(2) == 1 && gap.over_count(3) == 1,
              "A 47s interval counts once in 15s, 20s, 30s and 45s");
  TEST_ASSERT(gap.over_count(4) == 0 && gap.over_count(5) == 0,
              "...and not in 60s or 90s, which it never reached");
  TEST_ASSERT(gap.watched_ms() == 47000,
              "The watched total is the interval itself, counted once");

  // Three more intervals, none of them long enough to reach any rung. A
  // counter that incremented per sample rather than per exceedance would climb
  // here, which is the shape of a histogram that says the pump is worse than
  // it is.
  gap.on_inbound(57000);
  gap.on_inbound(67000);
  gap.on_inbound(77000);
  TEST_ASSERT(gap.over_count(0) == 1,
              "Ordinary 10s poll intervals do not touch the 15s counter");
  TEST_ASSERT(gap.watched_ms() == 77000,
              "...but they do count toward the watched total");
}

void test_gap_bucket_boundary_is_strictly_greater() {
  std::cout << "\n=== A rung counts what a budget of T would have fired on ==="
            << std::endl;

  // The equivalence the whole statistic rests on: over_count(i) must be the
  // number of times a data_timeout of THRESHOLDS[i] would have expired. So the
  // comparison has to be the same strict `>` link_data_timeout_expired() uses,
  // and this asserts the two agree at the boundary from both sides rather than
  // asserting each separately -- a later edit to either is then caught against
  // the other.
  for (size_t i = 0; i < LinkGapSampler::bucket_count(); i++) {
    const uint32_t t = LinkGapSampler::threshold_ms(i);

    LinkGapSampler exact;
    exact.on_open(1000);
    exact.on_inbound(1000 + t);
    const bool watchdog_fires_at_t =
        link_data_timeout_expired(true, 1000 + t, 1000, t);
    TEST_ASSERT(exact.over_count(i) == 0 && !watchdog_fires_at_t,
                "A gap of exactly the threshold neither counts nor fires");

    LinkGapSampler over;
    over.on_open(1000);
    over.on_inbound(1000 + t + 1);
    const bool watchdog_fires_past_t =
        link_data_timeout_expired(true, 1000 + t + 1, 1000, t);
    TEST_ASSERT(over.over_count(i) == 1 && watchdog_fires_past_t,
                "One millisecond past it, both count and fire");
  }
}

void test_gap_buckets_are_nested() {
  std::cout << "\n=== The rungs stay nested, and bounded by the samples ==="
            << std::endl;

  // Cumulative counts have structure a miswired counter cannot fake:
  // over_count(i) >= over_count(i+1) for every i. Driven with a deterministic
  // spread of gaps rather than one crafted value so the invariant is tested
  // against a mixture, which is what a real run looks like.
  LinkGapSampler gap;
  uint32_t now = 0;
  uint32_t step = 7000;
  gap.on_open(now);
  for (int n = 0; n < 40; n++) {
    step = (step * 31u + 3001u) % 100000u;  // 0..99999 ms, no <random> needed
    now += step;
    gap.on_inbound(now);
  }

  bool nested = true;
  for (size_t i = 0; i + 1 < LinkGapSampler::bucket_count(); i++) {
    if (gap.over_count(i) < gap.over_count(i + 1))
      nested = false;
  }
  TEST_ASSERT(nested, "Every rung counts at least as much as the one above it");
  TEST_ASSERT(gap.over_count(0) <= 40,
              "No rung counts more intervals than were sampled");
  TEST_ASSERT(gap.watched_ms() == now,
              "The watched total is the whole span, since nothing interrupted "
              "it");
}

void test_gap_watched_time_excludes_the_downtime() {
  std::cout << "\n=== Watched time is armed time, not wall time ===" << std::endl;

  // The denominator has to be the time the counts were drawn from. Counting
  // wall time instead would divide real excursions by hours the link was not
  // even up, and every rate in the report would read low.
  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(10000);
  gap.on_disconnect(20000);   // 20s of armed link so far
  gap.on_open(600000);        // ...then ten minutes down
  gap.on_inbound(610000);

  TEST_ASSERT(gap.watched_ms() == 30000,
              "Two sessions of armed link total 30s, with the downtime between "
              "them excluded");
}

void test_gap_truncated_counts_only_intervals_not_closed_by_data() {
  std::cout << "\n=== Truncated counts the intervals that did not end on their "
               "own ==="
            << std::endl;

  // This is the trust check on every other counter. An interval cut short by a
  // recycle or a drop is a lower bound, so a run full of them has a tail that
  // was cut off rather than observed -- and the reading looks identical to a
  // clean one without this number. That is not hypothetical: the pre-fix
  // maximum read 2.6s against a budget it had breached five times.
  // Each ending is exercised on its own session. An earlier version of this
  // chained the drop straight after a recycle, which is the one sequence where
  // the two overlap -- so it asserted truncated() == 2 there and quietly
  // encoded the double-count as the expected answer. A test that agrees with
  // the bug is worse than no test.
  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(10000);
  TEST_ASSERT(gap.truncated() == 0, "An interval ended by data is not truncated");

  gap.on_disconnect(60000);
  TEST_ASSERT(gap.truncated() == 1, "One ended by a plain drop is");

  gap.on_disconnect(90000);
  TEST_ASSERT(gap.truncated() == 1,
              "A disconnect with no open before it samples nothing, so it "
              "cannot inflate the count either");

  gap.on_open(100000);
  gap.on_recycle(180000);
  TEST_ASSERT(gap.truncated() == 2, "One ended by a watchdog recycle is");
}

void test_gap_buckets_are_censored_at_the_window_in_force() {
  std::cout << "\n=== The rungs are censored at the window, and the backoff "
               "moves it ==="
            << std::endl;

  // Documented rather than asserted away. With budget B in force the watchdog
  // closes the interval on the first tick past B, so no sample can exceed it
  // and every rung at or above B reads a structural zero -- which is
  // indistinguishable from "the pump never went quiet that long". Worse, the
  // backoff widens B after each recycle, so the cutoff is not even constant
  // across a run. This is why a measurement run has to raise data_timeout past
  // the top rung, and why the component warns when it has not been.
  LinkSim sim;
  sim.open(0);
  while (sim.fired < 1)
    sim.tick();

  TEST_ASSERT(sim.gap.over_count(4) == 1,
              "The first recycle records an interval past the 60s budget");
  TEST_ASSERT(sim.gap.over_count(5) == 0,
              "...and the 90s rung reads zero, because a 60s budget cannot let "
              "an interval get there");

  // Reconnect and stay deaf. The window is now 120s, so the next interval the
  // watchdog gives up on is long enough to reach 90s -- the same pump, a
  // different answer, purely because the cutoff moved.
  sim.open(sim.now + 6000);
  while (sim.fired < 2)
    sim.tick();
  TEST_ASSERT(sim.gap.over_count(5) == 1,
              "Once the backoff widens the window to 120s the 90s rung starts "
              "counting");
  TEST_ASSERT(sim.gap.truncated() == 2,
              "Both were cut short by the watchdog, and both say so");
}

void test_gap_a_recycle_is_one_truncated_interval_not_two() {
  std::cout << "\n=== A recycle and the disconnect it causes are one interval "
               "==="
            << std::endl;

  // The component's real sequence, which LinkSim does not reach: the watchdog
  // samples and calls force_disconnect(), and because that is asynchronous the
  // DISCONNECT event arrives a tick or two later and the disconnection callback
  // calls on_disconnect(). Left armed, that second call samples the gap between
  // the re-arm and the event -- so every recycle counted as two truncated
  // intervals and link_gaps_truncated read about twice the recycle count.
  //
  // LinkSim missed it because its scenarios re-open before the pending
  // disconnect lands, which is why this drives the sampler directly.
  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(10000);
  gap.on_recycle(71000);
  gap.on_disconnect(72000);

  TEST_ASSERT(gap.truncated() == 1,
              "One recycle is one truncated interval, not one per callback");
  TEST_ASSERT(gap.max_ms() == 61000,
              "...and the interval it gave up on is still the one recorded");

  // The next session starts clean rather than measuring across the downtime.
  gap.on_open(600000);
  gap.on_inbound(610000);
  TEST_ASSERT(gap.max_ms() == 61000 && gap.truncated() == 1,
              "The reconnect neither samples the downtime nor adds a truncation");
}

void test_gap_counters_survive_a_reconnect() {
  std::cout << "\n=== A reconnect does not clear the totals ===" << std::endl;

  // They are since-boot totals. Clearing them per session would make a
  // flapping link -- the exact condition worth measuring -- report near-zero
  // counts over a denominator of nothing.
  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(50000);
  const uint32_t over15 = gap.over_count(0);
  const uint64_t watched = gap.watched_ms();

  gap.on_disconnect(60000);
  gap.on_open(70000);
  gap.on_inbound(80000);

  TEST_ASSERT(gap.over_count(0) >= over15,
              "The 15s count carries across the reconnect");
  TEST_ASSERT(gap.watched_ms() > watched,
              "...and the watched total keeps accumulating rather than "
              "restarting");
}

void test_gap_inbound_with_no_open_before_it_does_not_sample() {
  std::cout << "\n=== A notification with no link behind it arms, it does not "
               "measure ==="
            << std::endl;

  // The mirror of the disconnect guard. Measuring from a disconnect stamp to a
  // stray later notification records the downtime as a quiet interval on a live
  // link, which permanently increments the top rungs and reads afterwards as a
  // genuine multi-minute excursion -- an inflated reading, which argues for a
  // longer timeout, which is the self-serving direction.
  LinkGapSampler gap;
  gap.on_open(0);
  gap.on_inbound(10000);
  gap.on_disconnect(20000);

  gap.on_inbound(320000);  // five minutes later, with nothing open
  TEST_ASSERT(gap.max_ms() == 10000 && gap.over_count(0) == 0,
              "The five minutes of downtime are not sampled");

  // ...and it armed from that frame rather than discarding everything after
  // it, so the next interval is measured from the right place.
  gap.on_inbound(330000);
  TEST_ASSERT(gap.max_ms() == 10000 && gap.watched_ms() == 30000,
              "The interval after it is measured from the stray frame, not "
              "from before the disconnect");
}

void test_gap_thresholds_are_the_documented_set() {
  std::cout << "\n=== The rungs are the documented ladder ===" << std::endl;

  // components/alpha_hwr/__init__.py names its entities after these values and
  // alpha_hwr.h static_asserts each setter against its index. This pins the
  // third side of that triangle: the values themselves, in this order.
  TEST_ASSERT(LinkGapSampler::bucket_count() == 6, "Six rungs");
  TEST_ASSERT(LinkGapSampler::threshold_ms(0) == 15000 &&
                  LinkGapSampler::threshold_ms(1) == 20000 &&
                  LinkGapSampler::threshold_ms(2) == 30000 &&
                  LinkGapSampler::threshold_ms(3) == 45000 &&
                  LinkGapSampler::threshold_ms(4) == 60000 &&
                  LinkGapSampler::threshold_ms(5) == 90000,
              "15/20/30/45/60/90s, in the order the entity names assume");

  bool ascending = true;
  for (size_t i = 0; i + 1 < LinkGapSampler::bucket_count(); i++) {
    if (LinkGapSampler::threshold_ms(i) >= LinkGapSampler::threshold_ms(i + 1))
      ascending = false;
  }
  TEST_ASSERT(ascending, "Strictly ascending, which the nesting invariant needs");

  LinkGapSampler gap;
  TEST_ASSERT(LinkGapSampler::threshold_ms(6) == 0 && gap.over_count(6) == 0,
              "Out of range reads zero rather than off the end of the array");
}

void test_gap_thresholds_censored_predicate() {
  std::cout << "\n=== The censoring warning fires on the budgets that earn it "
               "==="
            << std::endl;

  const uint32_t top = LinkGapSampler::threshold_ms(LinkGapSampler::bucket_count() - 1);

  TEST_ASSERT(link_gap_thresholds_censored(60000, top),
              "The shipped 60s default cannot observe the top rungs");
  TEST_ASSERT(link_gap_thresholds_censored(90000, top),
              "A budget equal to the top rung censors it too -- the only "
              "samples that can reach it are ones the watchdog cut off, so the "
              "counter quietly becomes a recycle count");
  TEST_ASSERT(!link_gap_thresholds_censored(91000, top),
              "One second past the top rung is enough to observe all of them");
  TEST_ASSERT(!link_gap_thresholds_censored(600000, top),
              "The documented measurement-run budget is clear");
  TEST_ASSERT(!link_gap_thresholds_censored(0, top),
              "A disabled watchdog censors nothing, because nothing recycles");

  // Every rung is independently optional, so the ladder's top is not the top of
  // a given config. Warning a 15s-only setup about the 90s rung it never asked
  // for is noise, and a setup with no rung at all has nothing to censor.
  TEST_ASSERT(!link_gap_thresholds_censored(60000, 15000),
              "A config declaring only the 15s rung is well served by a 60s "
              "budget and must not be warned");
  TEST_ASSERT(link_gap_thresholds_censored(20000, 30000),
              "...while the same budget against a 30s rung is censored");
  TEST_ASSERT(!link_gap_thresholds_censored(60000, 0),
              "No rung configured is nothing to censor, whatever the budget");
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
  test_gap_records_an_interval_ended_by_a_plain_drop();
  test_gap_ignores_a_disconnect_with_no_open_before_it();
  test_gap_records_a_floor_not_a_measurement();
  test_millis_rollover();
  test_predicate_has_no_notion_of_ready();
  test_backoff_doubles_to_the_cap();
  test_backoff_leaves_a_disabled_watchdog_disabled();
  test_backoff_never_shrinks_a_configured_window();
  test_backoff_cannot_wrap_into_a_tiny_window();
  test_gap_buckets_count_each_interval_once();
  test_gap_bucket_boundary_is_strictly_greater();
  test_gap_buckets_are_nested();
  test_gap_watched_time_excludes_the_downtime();
  test_gap_truncated_counts_only_intervals_not_closed_by_data();
  test_gap_a_recycle_is_one_truncated_interval_not_two();
  test_gap_buckets_are_censored_at_the_window_in_force();
  test_gap_counters_survive_a_reconnect();
  test_gap_inbound_with_no_open_before_it_does_not_sample();
  test_gap_thresholds_are_the_documented_set();
  test_gap_thresholds_censored_predicate();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
