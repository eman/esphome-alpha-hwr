/**
 * Read-chain lifetime tests.
 *
 * HistoryService, EventLogService and ScheduleService each drive a sequential
 * multi-command read through a `shared_ptr<std::function>` that re-invokes
 * itself. Capturing that shared_ptr *inside* the closure it owns makes the
 * closure own itself: the refcount never reaches zero and the whole chain --
 * the closure, its captured result vector, and the caller's on_complete -- is
 * stranded on every invocation. All three did exactly that, and the chains
 * re-run on every authenticated reconnect (and, for single events, on every
 * refresh_single_events service call), so the loss was unbounded.
 *
 * These are ordinary host tests rather than a sanitizer job because
 * LeakSanitizer does not run on macOS ARM, where most of this is developed.
 * The instrument is a `weak_ptr` to a sentinel owned by the on_complete we hand
 * in: on_complete is captured by value into the suspect closure, so the
 * sentinel outliving the chain *is* the retention. Nothing here refers to the
 * implementation's own pointer, so the tests stay valid if it is renamed.
 *
 * Both exit paths matter. A chain that runs to completion must release, and so
 * must one abandoned mid-flight. When these were written Transport::reset()
 * cleared the command queue without invoking callbacks, so an abandoned chain
 * never reached its terminal branch at all: nulling the pointer there would have
 * passed the first test and failed the second, which is why that fix was
 * rejected. reset() now fails what it abandons (issue #259) and the chain does
 * reach its terminal branch, so that particular argument no longer holds -- but
 * the weak_ptr is still what makes the ownership right, and these tests still
 * pin both exits.
 *
 * The second half of the file is about what the caller HEARS on the abandoned
 * exit, which is issue #259's actual subject: a read that ends must say so, and
 * a read that ended early must not be cached as though it were the whole answer.
 */

#include <cstdint>
#include <iostream>
#include <memory>
#include <cstring>
#include <string>
#include <vector>

#include "fixture_crc.h"
#include "../components/alpha_hwr/event_log_service.h"
#include "../components/alpha_hwr/history_service.h"
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/session.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg)                    \
  do {                                            \
    if (cond) {                                   \
      tests_passed++;                             \
      std::cout << "[PASS] " << msg << std::endl; \
    } else {                                      \
      tests_failed++;                             \
      std::cout << "[FAIL] " << msg << std::endl; \
    }                                             \
  } while (0)

using esphome::alpha_hwr::core::Session;
using esphome::alpha_hwr::core::Transport;
namespace services = esphome::alpha_hwr::services;

namespace {

struct Sentinel {
  int unused{0};
};

/// Drive the transport FSM far enough for every queued command to time out, so
/// the chain walks to its terminal branch without a simulated pump.
void run_to_completion(Transport &t) {
  for (int i = 0; i < 4000; i++) {
    mock_millis += 51;
    t.loop();
  }
}

void step(Transport &t, int iters) {
  for (int i = 0; i < iters; i++) {
    mock_millis += 51;
    t.loop();
  }
}

struct Rig {
  Transport transport;
  Session session;
  /// Outgoing commands, counted by their leading 0x27 chunk. Lets a test
  /// assert that a chain actually ADVANCED, rather than inferring it.
  int commands_sent{0};
  Rig() {
    transport.set_write_callback([this](const uint8_t *d, size_t n) {
      if (n > 0 && d[0] == 0x27) commands_sent++;
      return true;
    });
    session.on_ready();
  }
};

}  // namespace

static void test_history_completion() {
  Rig rig;
  services::HistoryService svc(rig.transport, rig.session);

  auto sentinel = std::make_shared<Sentinel>();
  std::weak_ptr<Sentinel> watch = sentinel;
  svc.read_trends_async(
      [sentinel](bool, const std::vector<services::TrendSeries> &) {});
  sentinel.reset();

  run_to_completion(rig.transport);
  TEST_ASSERT(watch.expired(),
              "history: chain released after running to completion");
}

static void test_history_abandoned() {
  Rig rig;
  services::HistoryService svc(rig.transport, rig.session);

  auto sentinel = std::make_shared<Sentinel>();
  std::weak_ptr<Sentinel> watch = sentinel;
  svc.read_trends_async(
      [sentinel](bool, const std::vector<services::TrendSeries> &) {});
  sentinel.reset();

  step(rig.transport, 2);      // one command in flight
  rig.transport.reset();       // disconnect: clears the queue, no callbacks
  step(rig.transport, 10);
  TEST_ASSERT(watch.expired(),
              "history: chain released when a disconnect abandons it");
}

/// Build a Class 10 response the transport will match, with `sub`/`obj` in the
/// standard positions and `body` as the payload after the 10-byte header.
/// The CRC is not filled in: try_dispatch_response does not validate it (a
/// separate finding), and these tests are about lifetime, not framing.
static std::vector<uint8_t> class10_response(uint16_t sub, uint16_t obj,
                                             const std::vector<uint8_t> &body) {
  std::vector<uint8_t> f{0x24, 0x00, 0x00, 0x07, 0x0A, 0x03};
  f.push_back((sub >> 8) & 0xFF);
  f.push_back(sub & 0xFF);
  f.push_back((obj >> 8) & 0xFF);
  f.push_back(obj & 0xFF);
  f.insert(f.end(), body.begin(), body.end());
  f.push_back(0xAA);
  f.push_back(0xBB);  // CRC placeholder
  f[1] = static_cast<uint8_t>(f.size() - 4);
  return f;
}

/// The event-log chain only allocates its closure *after* a successful metadata
/// read reports a non-zero entry count. A harness that never answers leaves
/// read_metadata_async to time out and return before the suspect closure
/// exists, so the sentinel expires regardless -- the test would pass against
/// the old self-cycle and prove nothing. Answer the metadata read, get an entry
/// read in flight, then abandon it.
static void test_event_log_abandoned() {
  Rig rig;
  services::EventLogService svc(rig.transport, rig.session);

  auto sentinel = std::make_shared<Sentinel>();
  std::weak_ptr<Sentinel> watch = sentinel;
  svc.read_entries_async(
      [sentinel](bool, const std::vector<services::EventLogEntry> &) {});
  sentinel.reset();

  // Let the metadata request reach the wire, then answer it:
  // 3-byte sub-header, then cycle=1, available=4, max=20.
  step(rig.transport, 2);
  auto meta = class10_response(0x0000, 0xF301,
                               {0x00, 0x00, 0x00,   // sub-header
                                0x00, 0x01,         // cycle counter
                                0x00, 0x04,         // available entries
                                0x00, 0x14,         // max entries
                                0x00});             // pad: the handler
                                                    // requires payload_len >= 10
  meta = with_crc(std::move(meta));
  const int before_answer = rig.commands_sent;
  rig.transport.on_notification(meta.data(), meta.size());

  // An entry read is now queued and the chain's closure exists.
  step(rig.transport, 2);

  // Assert that precondition rather than assuming it. The docstring above
  // explains why the sentinel assertion below is worthless without it -- and
  // it was worthless: deleting the with_crc() call one line up (so the
  // metadata frame is dropped for a bad CRC and the read times out instead)
  // left this test reporting 5 passed, 0 failed. The chain has to have
  // ADVANCED for "abandoned mid-entry-read" to mean anything.
  TEST_ASSERT(rig.commands_sent > before_answer,
              "event log: metadata read was answered and an entry read went out");
  rig.transport.reset();  // disconnect mid-chain
  step(rig.transport, 10);

  TEST_ASSERT(watch.expired(),
              "event log: chain released when abandoned mid-entry-read");
}

static void test_single_events_abandoned() {
  Rig rig;
  services::ScheduleService svc(rig.transport, rig.session);

  auto sentinel = std::make_shared<Sentinel>();
  std::weak_ptr<Sentinel> watch = sentinel;
  svc.read_single_events_async(
      [sentinel](bool, const std::vector<services::SingleEvent> &) {});
  sentinel.reset();

  step(rig.transport, 2);
  rig.transport.reset();
  step(rig.transport, 10);
  TEST_ASSERT(watch.expired(),
              "single events: chain released (re-runs on every HA refresh)");
}

// ---------------------------------------------------------------------------
// What the caller hears when a chain is abandoned (issue #259)
// ---------------------------------------------------------------------------

/// One trend channel's reply: 3-byte Class 10 sub-header then the 29-byte
/// TrendData body, whose first four bytes are the current value as a big-endian
/// float. 32 bytes is the minimum the handler accepts.
static std::vector<uint8_t> trend_reply(float current) {
  std::vector<uint8_t> body{0x00, 0x00, 0x1D};
  uint32_t bits;
  static_assert(sizeof(bits) == sizeof(current), "float is not 32-bit here");
  memcpy(&bits, &current, sizeof(bits));
  body.push_back((bits >> 24) & 0xFF);
  body.push_back((bits >> 16) & 0xFF);
  body.push_back((bits >> 8) & 0xFF);
  body.push_back(bits & 0xFF);
  while (body.size() < 32) body.push_back(0x01);
  return with_crc(class10_response(0x0000, 0x0000, body));
}

/// Drive the four trend reads to a complete answer.
static void answer_all_four_trends(Rig &rig) {
  for (int i = 0; i < 4; i++) {
    step(rig.transport, 2);
    auto reply = trend_reply(1.0f + i);
    rig.transport.on_notification(reply.data(), reply.size());
  }
  step(rig.transport, 2);
}

/// The whole point of the issue. A disconnect used to leave this callback
/// unfired forever: no success, no failure, and no timeout either, because the
/// timeout went with the discarded queue entry.
static void test_abandoned_history_read_reports_failure() {
  Rig rig;
  services::HistoryService svc(rig.transport, rig.session);

  int calls = 0;
  bool reported = true;
  svc.read_trends_async(
      [&](bool ok, const std::vector<services::TrendSeries> &) {
        calls++;
        reported = ok;
      });

  step(rig.transport, 2);              // one channel read in flight
  rig.session.on_disconnected();       // the component's order: session first,
  rig.transport.reset();               // then the transport

  TEST_ASSERT(calls == 1,
              "history: the caller is told the read is over, rather than "
              "waiting on a reply the link can no longer carry");
  TEST_ASSERT(!reported,
              "history: ...and told it FAILED, not that a two-channel read "
              "succeeded");

  step(rig.transport, 20);
  TEST_ASSERT(calls == 1, "history: ...exactly once");
}

/// Reporting failure is not enough on its own: the chain still arrives at its
/// terminal branch with whatever landed before the drop, and that branch is
/// where the display cache is written. Missing channels are ordinarily
/// legitimate -- not every pump populates all four -- so the abandoned case has
/// to be told apart or every dropped link truncates the display.
static void test_abandoned_history_read_keeps_the_previous_data() {
  Rig rig;
  services::HistoryService svc(rig.transport, rig.session);

  svc.read_trends_async([](bool, const std::vector<services::TrendSeries> &) {});
  answer_all_four_trends(rig);
  const std::string full = svc.format_display();
  TEST_ASSERT(full.find("Power-on Time") != std::string::npos,
              "history: the first read landed all four channels");

  // A second read, abandoned after one channel.
  svc.read_trends_async([](bool, const std::vector<services::TrendSeries> &) {});
  step(rig.transport, 2);
  auto reply = trend_reply(9.0f);
  rig.transport.on_notification(reply.data(), reply.size());
  step(rig.transport, 2);
  rig.session.on_disconnected();
  rig.transport.reset();

  TEST_ASSERT(svc.format_display() == full,
              "history: a read cut short by a disconnect does not replace the "
              "display with the one channel it managed");
}

/// The same rule in the event log, where a single failed entry is tolerated by
/// design -- so a chain that ends after three entries of twenty looks exactly
/// like a log that is three entries long.
static void test_abandoned_event_log_read_reports_failure() {
  Rig rig;
  services::EventLogService svc(rig.transport, rig.session);

  int calls = 0;
  bool reported = true;
  svc.read_entries_async(
      [&](bool ok, const std::vector<services::EventLogEntry> &) {
        calls++;
        reported = ok;
      });

  step(rig.transport, 2);
  auto meta = with_crc(class10_response(0x0000, 0xF301,
                                        {0x00, 0x00, 0x00,
                                         0x00, 0x01,    // cycle counter
                                         0x00, 0x04,    // available entries
                                         0x00, 0x14,    // max entries
                                         0x00}));
  const int before = rig.commands_sent;
  rig.transport.on_notification(meta.data(), meta.size());
  step(rig.transport, 2);
  TEST_ASSERT(rig.commands_sent > before,
              "event log: an entry read went out, so the chain is live");

  rig.session.on_disconnected();
  rig.transport.reset();

  TEST_ASSERT(calls == 1 && !reported,
              "event log: the abandoned read reports failure, not a four-entry "
              "log with no entries in it");
}

/// One event-log entry's reply. The entry read uses the register-read shape, so
/// there is no 3-byte sub-header: `EventLogEntry::from_bytes` reads the payload
/// directly and takes the timestamp from bytes 10-13. A non-zero timestamp is
/// what makes the entry count -- zero ones are dropped.
static std::vector<uint8_t> event_entry_reply(uint32_t stamp) {
  std::vector<uint8_t> body(16, 0x00);
  body[4] = 0x01;   // cycle counter
  body[9] = 0x01;   // event type: Start
  body[10] = (stamp >> 24) & 0xFF;
  body[11] = (stamp >> 16) & 0xFF;
  body[12] = (stamp >> 8) & 0xFF;
  body[13] = stamp & 0xFF;
  return with_crc(class10_response(0x0000, 0xF402, body));
}

static std::vector<uint8_t> event_meta_reply(uint16_t available) {
  return with_crc(class10_response(0x0000, 0xF301,
                                   {0x00, 0x00, 0x00,
                                    0x00, 0x01,                            // cycle
                                    (uint8_t) (available >> 8), (uint8_t) available,
                                    0x00, 0x14,                            // max entries
                                    0x00}));
}

/// The event log's timestamps are the pump's OWN clock, and must be rendered
/// verbatim (issue #289).
///
/// The distinction this pins is the one I got wrong while fixing #289, and
/// nothing caught it because no test looked at the rendering at all.
///
/// EventLogEntry::from_bytes() reads the four wire bytes and NOTHING converts
/// them. They are the pump's clock, which runs on local time -- unlike cached
/// single events, which go through local_unix_to_utc_resolved() on read and
/// really are UTC epochs in our domain. So the correct rendering takes the
/// value's fields as they stand; shifting it "to local" moves a local wall
/// clock by the offset a second time.
///
/// Driven under MockZoneOverride, where ESPHome's zone disagrees with the
/// process TZ, so any shift at all is visible. Under the plain TZ=UTC pin both
/// a shift and no shift look identical, which is why this needs the override.
static void test_event_log_timestamps_are_rendered_verbatim() {
  setenv("TZ", "UTC", 1);
  tzset();
  esphome::MockZoneOverride pst(-8 * 3600);

  Rig rig;
  services::EventLogService svc(rig.transport, rig.session);

  svc.read_entries_async(
      [](bool, const std::vector<services::EventLogEntry> &) {});
  step(rig.transport, 2);
  auto meta = event_meta_reply(1);
  rig.transport.on_notification(meta.data(), meta.size());
  step(rig.transport, 2);
  // 2023-11-14 22:13:20 when its fields are read as they stand.
  auto entry = event_entry_reply(1700000000u);
  rig.transport.on_notification(entry.data(), entry.size());
  step(rig.transport, 2);

  const std::string shown = svc.format_display();
  TEST_ASSERT(shown.find("2023-11-14 22:13") != std::string::npos,
              "the entry renders the pump's own wall clock verbatim");
  TEST_ASSERT(shown.find("2023-11-14 14:13") == std::string::npos,
              "and is NOT shifted into the node's zone -- that would move a "
              "local wall clock by the offset a second time");
}

/// The other half of the event-log gate, and the half nothing asserted.
///
/// The gate does two things: report failure, and leave the cache alone. The
/// verdict assertion above covers only the first, so a one-sided gate that
/// reported false and cached the truncated read anyway would pass. This is the
/// event-log analogue of test_abandoned_history_read_keeps_the_previous_data.
static void test_abandoned_event_log_read_keeps_the_previous_data() {
  Rig rig;
  services::EventLogService svc(rig.transport, rig.session);

  svc.read_entries_async(
      [](bool, const std::vector<services::EventLogEntry> &) {});
  step(rig.transport, 2);
  auto meta = event_meta_reply(2);
  rig.transport.on_notification(meta.data(), meta.size());
  for (uint32_t i = 0; i < 2; i++) {
    step(rig.transport, 2);
    auto entry = event_entry_reply(1700000000u + i * 3600u);
    rig.transport.on_notification(entry.data(), entry.size());
  }
  step(rig.transport, 2);

  const std::string full = svc.format_display();
  TEST_ASSERT(full.find("Start") != std::string::npos,
              "event log: the first read landed real entries");

  // A second read, abandoned after the metadata but before any entry lands.
  svc.read_entries_async(
      [](bool, const std::vector<services::EventLogEntry> &) {});
  step(rig.transport, 2);
  auto meta2 = event_meta_reply(2);
  rig.transport.on_notification(meta2.data(), meta2.size());
  step(rig.transport, 2);
  rig.session.on_disconnected();
  rig.transport.reset();

  TEST_ASSERT(svc.format_display() == full,
              "event log: a read cut short by a disconnect leaves the previous "
              "entries in place instead of publishing an empty log");
}

/// The failure mode is unbounded growth, not a single stranded allocation, so
/// assert repetition explicitly: N invocations must retain nothing.
static void test_no_accumulation() {
  Rig rig;
  services::HistoryService svc(rig.transport, rig.session);

  std::vector<std::weak_ptr<Sentinel>> watches;
  const int N = 20;
  for (int i = 0; i < N; i++) {
    auto s = std::make_shared<Sentinel>();
    watches.push_back(s);
    svc.read_trends_async(
        [s](bool, const std::vector<services::TrendSeries> &) {});
    s.reset();
    run_to_completion(rig.transport);
  }

  int alive = 0;
  for (auto &w : watches) {
    if (!w.expired())
      alive++;
  }
  std::cout << "       " << alive << "/" << N
            << " chains retained after " << N << " completed reads" << std::endl;
  TEST_ASSERT(alive == 0, "history: repeated reads accumulate nothing");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Read Chain Lifetime Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_history_completion();
  test_history_abandoned();
  test_event_log_abandoned();
  test_single_events_abandoned();
  test_no_accumulation();

  test_abandoned_history_read_reports_failure();
  test_abandoned_history_read_keeps_the_previous_data();
  test_abandoned_event_log_read_reports_failure();
  test_abandoned_event_log_read_keeps_the_previous_data();
  test_event_log_timestamps_are_rendered_verbatim();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
