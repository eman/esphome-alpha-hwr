// Host tests for pairing_stall.h — when a pump that will not pair stops being
// mistaken for a pump that has not been asked yet.
//
// Issue #230: clearing only the client's bond leaves the pump holding a bond
// for an unencrypted peer. It sends no SEC_REQ and terminates the link, and
// the node loops on that roughly every 5 s, logging "waiting for pump to
// initiate pairing" each time. The loop is not recoverable without physical
// access to the pump, and nothing said so.
//
// The whole risk in the fix is the false positive: `enable_pairing` defaults
// to false and passive telemetry needs no bond, so an ordinary installation
// can run unbonded forever, and telling those users their pump refuses to pair
// would be worse than the silence it replaces. Most of what is below is about
// that -- the cases that must NOT report.
//
// ble_connection_manager.cpp is compiled by no host test, so this is where the
// rule can be answered wrongly and noticed.

#include <cstdint>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/pairing_stall.h"

// The disconnect reasons this test drives, by their ESP-IDF values. Spelled
// out here rather than pulled from the mock header, because the production
// header deliberately takes raw numbers so it can be compiled without ESP-IDF
// at all -- and a test that imported the enum would stop testing that.
static constexpr uint16_t REASON_PEER_TERMINATED = 0x0013;   // the pump decided
static constexpr uint16_t REASON_SUPERVISION_TIMEOUT = 0x0008;  // the radio did
static constexpr uint16_t REASON_LOCAL_HOST = 0x0016;
static constexpr uint16_t REASON_FAIL_ESTABLISH = 0x003E;
static constexpr uint16_t REASON_UNKNOWN = 0x0000;

using esphome::alpha_hwr::core::PAIRING_STALL_CYCLES;
using esphome::alpha_hwr::core::PAIRING_STALL_REMINDER_CYCLES;
using esphome::alpha_hwr::core::PairingStallDetector;
using esphome::alpha_hwr::core::disconnect_reason_is_link_loss;

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

// One connect-and-drop cycle in the state the issue describes: opened with no
// stored bond, no SEC_REQ, no data, dropped by the pump.
static bool stalled_cycle(PairingStallDetector &d) {
  d.on_connection_opened(false);
  return d.on_disconnected(REASON_PEER_TERMINATED);
}

// Drive the detector until it reports an established stall.
//
// Bounded, and that is the entire point of it being a function. The obvious
// spelling is `while (!stalled_cycle(d)) {}`, which is what this replaced, and
// under the `pairing-stall-never-reported` mutation -- the report suppressed
// outright -- that loop never terminates. A test that HANGS under a mutation is
// far worse than one that fails: tools/mutation_check.sh puts no time limit on
// a suite run, so the whole sweep parks on a spinning binary with no output and
// no indication of why. That is not hypothetical; it cost a 34-minute silent
// stall on the first sweep of this change. Ask for the property, give it a
// generous bound, and let the caller assert that it arrived.
static bool drive_to_stall(PairingStallDetector &d) {
  for (int i = 0; i < 64; i++) {
    if (stalled_cycle(d)) return true;
  }
  return false;
}

static void test_one_dropped_link_is_not_a_diagnosis() {
  // Spelled with a literal 1 rather than through PAIRING_STALL_CYCLES, so
  // lowering the threshold to one is a failure here instead of a test that
  // quietly retunes itself alongside the constant. A single dropped link has a
  // dozen ordinary causes and is not evidence of anything.
  PairingStallDetector d;
  TEST_ASSERT(!stalled_cycle(d),
              "One unbonded connect-and-drop reports nothing");
  TEST_ASSERT(!d.stalled(), "and establishes no fault");
}

static void test_a_stall_is_reported_only_once_it_is_a_pattern() {
  PairingStallDetector d;
  for (uint8_t i = 1; i < PAIRING_STALL_CYCLES; i++) {
    TEST_ASSERT(!stalled_cycle(d),
                "Cycle short of the threshold reports nothing — one dropped "
                "link has a dozen ordinary causes");
    TEST_ASSERT(!d.stalled(), "and no stall is established yet");
  }
  TEST_ASSERT(stalled_cycle(d),
              "The threshold cycle reports: the pump has been connected to "
              "three times and has never offered to pair");
  TEST_ASSERT(d.stalled(), "and the stall is established");
  TEST_ASSERT(d.consecutive_cycles() == PAIRING_STALL_CYCLES,
              "with the cycle count the report quotes");
}

static void test_the_report_is_throttled_but_the_fault_is_not() {
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "A stall is established at all");
  int reports = 0;
  for (int i = 0; i < PAIRING_STALL_REMINDER_CYCLES; i++) {
    if (stalled_cycle(d)) reports++;
    TEST_ASSERT(d.stalled(),
                "The fault stays established between reports — it is a latched "
                "string, not a log line, and must not blink out with the "
                "throttle");
  }
  TEST_ASSERT(reports == 1,
              "Exactly one repeat per reminder window — a WARN on every cycle "
              "would be twelve a minute, forever");
}

static void test_the_repeats_do_not_stop_when_the_counter_saturates() {
  // A stall lasts until someone walks to the pump, which can be days. The
  // cycle counter is a uint8_t and saturates in about 21 minutes at the
  // observed cadence; a report cadence derived from that counter modulo the
  // window then either fires every cycle or never again, decided by where the
  // ceiling happens to land.
  PairingStallDetector d;
  for (int i = 0; i < 600; i++) stalled_cycle(d);
  TEST_ASSERT(d.consecutive_cycles() == UINT8_MAX,
              "The cycle counter saturates rather than wrapping — a wrap would "
              "read as a stall that had just started");
  int reports = 0;
  for (int i = 0; i < PAIRING_STALL_REMINDER_CYCLES * 4; i++) {
    if (stalled_cycle(d)) reports++;
  }
  TEST_ASSERT(reports == 4,
              "and the reminder keeps its cadence past saturation");
}

static void test_the_reminder_window_is_twelve_cycles_and_not_one() {
  // Spelled with literals, on purpose. The other cadence tests derive their
  // loop bounds from PAIRING_STALL_REMINDER_CYCLES, so they retune themselves
  // with it: a skeptic pass set the constant to 1, 2, 200 and 255 in turn and
  // the suite stayed green every time, while the assertion count wandered from
  // 147 to 390 and nothing noticed. The branch form of the same regression --
  // `if (true)` in place of the window test -- IS pinned, which made the gap
  // easy to miss: the mechanism was covered and the number was not.
  //
  // One WARN a cycle against a ~5 s reconnect loop is twelve a minute, forever,
  // for a fault whose remedy needs someone to walk to the pump. The number is
  // the behaviour here, so the number is what is asserted.
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "Stalled, and the first report is out");
  for (int i = 1; i <= 11; i++) {
    TEST_ASSERT(!stalled_cycle(d),
                "Cycle " + std::to_string(i) + " of the window is silent");
  }
  TEST_ASSERT(stalled_cycle(d), "and the twelfth repeats the report");
}

static void test_one_open_does_not_close_many_cycles() {
  // `saw_open_ = false` on the way out of a closed cycle. Removing it left the
  // suite completely green, because the failed-open test above starts from a
  // virgin detector where the flag is already false from its initializer -- so
  // the reset itself was never exercised. What that buys, unmutated: a real
  // connection followed by a pump going out of range produces one cycle, not
  // one per failed open. Both events reach the caller as DISCONNECTs.
  PairingStallDetector d;
  d.on_connection_opened(false);
  TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED), "One real cycle");
  for (int i = 0; i < 20; i++) {
    TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
                "and the disconnects that follow with no open between them are "
                "not cycles of their own");
  }
  TEST_ASSERT(d.consecutive_cycles() == 1,
              "so an unreachable pump cannot manufacture a pattern out of one "
              "ordinary drop");
  TEST_ASSERT(!d.stalled(), "and nothing is reported");
}

static void test_a_second_stall_gets_a_full_reminder_window() {
  // The crossing branch reseats since_report_, and nothing distinguished that
  // line from its absence. What it buys: an episode that ended part-way through
  // a reminder window does not shorten the next episode's first repeat. Without
  // it a relapse could re-warn one cycle after establishing.
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "First stall, first report");
  for (int i = 0; i < 5; i++) stalled_cycle(d);  // five cycles into the window

  d.on_connection_opened(false);
  d.note_data();
  d.on_disconnected(REASON_PEER_TERMINATED);
  TEST_ASSERT(!d.stalled(), "The episode ended mid-window");

  TEST_ASSERT(drive_to_stall(d), "A second stall establishes and reports");
  for (int i = 1; i <= 11; i++) {
    TEST_ASSERT(!stalled_cycle(d),
                "and its window starts from zero, not from where the first one "
                "left off (cycle " + std::to_string(i) + ")");
  }
  TEST_ASSERT(stalled_cycle(d), "with the repeat a full window later");
}

static void test_a_bonded_connection_is_never_a_stall_cycle() {
  PairingStallDetector d;
  for (int i = 0; i < 10; i++) {
    d.on_connection_opened(true);
    TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
                "A connection that opened with a stored bond is not waiting to "
                "pair, so dropping it says nothing about pairing");
  }
  TEST_ASSERT(!d.stalled(), "however many times it happens");
}

static void test_a_security_request_clears_it_however_it_is_answered() {
  PairingStallDetector d;
  for (int i = 0; i < 2; i++) stalled_cycle(d);
  d.on_connection_opened(false);
  d.note_security_request();
  TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
              "A pump that asked to secure the link is willing to pair, which "
              "is the whole thing being waited for");
  TEST_ASSERT(!stalled_cycle(d) && !stalled_cycle(d),
              "and the count restarted, so the threshold is three fresh "
              "cycles rather than one more");
  TEST_ASSERT(stalled_cycle(d), "reached on the third");
}

static void test_a_healthy_unbonded_link_never_reports() {
  // enable_pairing defaults to false and passive telemetry needs no bond, so
  // this configuration is supported and common. Its links open unbonded and
  // are eventually dropped, exactly like a stalled one -- the difference is
  // that data flowed in between. Without that term this test's node would
  // report a pairing fault after its third ordinary reconnect.
  PairingStallDetector d;
  for (int i = 0; i < 50; i++) {
    d.on_connection_opened(false);
    d.note_data();
    TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
                "An unbonded link that carried data is working; dropping it is "
                "an ordinary reconnect, not a refusal to pair");
  }
  TEST_ASSERT(!d.stalled(), "so nothing accumulates across a node's lifetime");
}

static void test_a_link_we_tore_down_ourselves_is_not_counted() {
  // The inbound-data watchdog recycles a link that subscribed and then heard
  // nothing, every 60 s, forever. Unbonded, no security, no data -- identical
  // to a stalled cycle in every term but who ended it. Counting those would
  // report a pairing refusal for a merely deaf pump three minutes in, and
  // because this fault outranks the watchdog's it would take "No data from
  // pump" off the fault surface and put the wrong cause there.
  PairingStallDetector d;
  for (int i = 0; i < 30; i++) {
    d.on_connection_opened(false);
    d.note_local_teardown();
    TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
                "A teardown this node initiated says nothing about whether the "
                "pump would have offered to pair");
  }
  TEST_ASSERT(!d.stalled(), "so a deaf link keeps its own diagnosis");
}

static void test_a_link_the_radio_dropped_is_not_a_refusal() {
  // The finding that most nearly shipped. The fourth term was stated in the
  // header ("the pump ended it, not us") and implemented only for teardowns
  // this component initiated, so a link lost to range or interference was
  // credited to the pump. A skeptic pass drove three unbonded opens dropped
  // with a supervision timeout straight to "Pump not accepting pairing" --
  // replacing a correct radio diagnostic with a pairing misdiagnosis, which is
  // the exact inversion of the point of this module. And the window is wide:
  // the default node is unbonded forever, and no data flows for the first
  // 2.5-3.5 s of any connection, against a stall that drops at about 2.1 s.
  PairingStallDetector d;
  for (int i = 0; i < 30; i++) {
    d.on_connection_opened(false);
    TEST_ASSERT(!d.on_disconnected(REASON_SUPERVISION_TIMEOUT),
                "A link lost to the radio is not the pump refusing to pair");
  }
  TEST_ASSERT(!d.stalled(), "however many times it happens");
}

static void test_the_reasons_that_do_not_count() {
  // An exclusion list, not an allow-list, and the direction matters. Admitting
  // only "the peer terminated" would rest the whole feature on one specimen's
  // firmware emitting 0x13; a pump that ends the link some other way would then
  // never be reported, which is the failure this module exists to prevent. So
  // unknown reasons count, and only the ones that positively mean "lost" or
  // "ours" are excluded.
  TEST_ASSERT(disconnect_reason_is_link_loss(REASON_SUPERVISION_TIMEOUT),
              "A supervision timeout is the radio, not a decision");
  TEST_ASSERT(disconnect_reason_is_link_loss(REASON_FAIL_ESTABLISH),
              "So is a link that never got established");
  TEST_ASSERT(disconnect_reason_is_link_loss(REASON_LOCAL_HOST),
              "A local-host termination is ours by definition -- which also "
              "covers a stock ble_client.disconnect action we never see");
  TEST_ASSERT(!disconnect_reason_is_link_loss(REASON_PEER_TERMINATED),
              "The pump terminating the link is the thing being counted");
  TEST_ASSERT(!disconnect_reason_is_link_loss(REASON_UNKNOWN),
              "An unrecognised reason counts: failing to detect the stall is "
              "worse than the narrower rule that would prevent it");
}

static void test_a_radio_drop_resets_a_stall_in_progress() {
  // Not merely "does not add to it". The report rests on three cycles the PUMP
  // ended, not three assorted failures that happened to line up, so a link the
  // radio took away has to break the run rather than pause it.
  PairingStallDetector d;
  for (uint8_t i = 1; i < PAIRING_STALL_CYCLES; i++) stalled_cycle(d);
  d.on_connection_opened(false);
  d.on_disconnected(REASON_SUPERVISION_TIMEOUT);
  for (uint8_t i = 1; i < PAIRING_STALL_CYCLES; i++)
    TEST_ASSERT(!stalled_cycle(d), "The count restarted after the radio drop");
  TEST_ASSERT(stalled_cycle(d), "and three fresh pump-ended cycles report");
}

static void test_a_stall_ends_the_moment_it_is_refuted() {
  // The stall has to un-say itself mid-connection, not at the next disconnect.
  // The caller drops its latched fault string on stalled() going false, and
  // nothing else will drop it: with enable_pairing false -- the default -- no
  // AUTH_CMPL ever fires, and a stalled link never reaches READY. Before this,
  // a pump that had just been put into pairing mode and was visibly sending
  // SEC_REQ went on being reported as one that would not pair.
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "A stall is established");
  d.on_connection_opened(false);
  TEST_ASSERT(d.stalled(), "and survives the next connection opening");
  d.note_security_request();
  TEST_ASSERT(!d.stalled(),
              "but not the pump offering to pair -- that refutes it outright, "
              "and it must stop being claimed immediately");

  PairingStallDetector e;
  TEST_ASSERT(drive_to_stall(e), "A stall is established");
  e.on_connection_opened(false);
  e.note_data();
  TEST_ASSERT(!e.stalled(), "Inbound data refutes it just as immediately");

  PairingStallDetector f;
  TEST_ASSERT(drive_to_stall(f), "A stall is established");
  f.on_connection_opened(true);
  TEST_ASSERT(!f.stalled(),
              "and so does a connection that opened with a bond -- the state "
              "being described has plainly ended");
}

static void test_a_completed_bond_clears_it() {
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "A stall is established at all");
  d.on_connection_opened(false);
  d.note_bond_established();
  TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED) && !d.stalled(),
              "Pairing succeeded — the pump was put into pairing mode and the "
              "fault is over, before any bonded reconnect proves it");
}

static void test_a_failed_open_is_not_a_cycle() {
  // ESP_GATTC_OPEN_EVT fires for failed opens too, and a pump that is powered
  // down or out of range produces a stream of them. The caller does not run
  // its connection-opened handler on those, so nothing opens the cycle -- but
  // a DISCONNECT can still arrive, and counting it would report a pairing
  // problem for a pump that is not there to have one.
  PairingStallDetector d;
  for (int i = 0; i < 20; i++) {
    TEST_ASSERT(!d.on_disconnected(REASON_PEER_TERMINATED),
                "A disconnect with no open before it is not a connection that "
                "refused to pair");
  }
  TEST_ASSERT(!d.stalled() && d.consecutive_cycles() == 0,
              "and an unreachable pump accumulates nothing");
}

static void test_progress_does_not_leak_into_the_next_connection() {
  // The per-connection flag is cleared on the disconnect that consumed it. If
  // it were not, one good connection would immunise every connection after it
  // and the stall could never be detected again -- which is precisely the
  // sequence a bond clear produces: a working bonded link, then the loop.
  PairingStallDetector d;
  d.on_connection_opened(true);
  d.on_disconnected(REASON_PEER_TERMINATED);
  for (uint8_t i = 1; i < PAIRING_STALL_CYCLES; i++) {
    TEST_ASSERT(!stalled_cycle(d), "Cycles after a good connection still count");
  }
  TEST_ASSERT(stalled_cycle(d),
              "and the threshold is still reached — a bond clear after a "
              "healthy session is the reported case");
}

static void test_recovery_and_relapse() {
  PairingStallDetector d;
  TEST_ASSERT(drive_to_stall(d), "A stall is established at all");
  d.on_connection_opened(false);
  d.note_security_request();
  d.note_bond_established();
  d.note_data();
  d.on_disconnected(REASON_PEER_TERMINATED);
  TEST_ASSERT(!d.stalled() && d.consecutive_cycles() == 0,
              "Recovery is complete, not partial — the next stall starts from "
              "zero");
  int reports = 0;
  for (uint8_t i = 0; i < PAIRING_STALL_CYCLES; i++)
    if (stalled_cycle(d)) reports++;
  TEST_ASSERT(reports == 1,
              "and a second stall is reported again rather than being treated "
              "as already-announced");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Pairing Stall Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_one_dropped_link_is_not_a_diagnosis();
  test_a_stall_is_reported_only_once_it_is_a_pattern();
  test_the_report_is_throttled_but_the_fault_is_not();
  test_the_repeats_do_not_stop_when_the_counter_saturates();
  test_the_reminder_window_is_twelve_cycles_and_not_one();
  test_one_open_does_not_close_many_cycles();
  test_a_second_stall_gets_a_full_reminder_window();
  test_a_bonded_connection_is_never_a_stall_cycle();
  test_a_security_request_clears_it_however_it_is_answered();
  test_a_healthy_unbonded_link_never_reports();
  test_a_link_the_radio_dropped_is_not_a_refusal();
  test_the_reasons_that_do_not_count();
  test_a_radio_drop_resets_a_stall_in_progress();
  test_a_stall_ends_the_moment_it_is_refuted();
  test_a_link_we_tore_down_ourselves_is_not_counted();
  test_a_completed_bond_clears_it();
  test_a_failed_open_is_not_a_cycle();
  test_progress_does_not_leak_into_the_next_connection();
  test_recovery_and_relapse();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
