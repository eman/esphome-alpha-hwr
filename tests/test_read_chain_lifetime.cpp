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
 * must one abandoned mid-flight -- Transport::reset() clears the command queue
 * without invoking callbacks, so an abandoned chain never reaches its terminal
 * branch. Nulling the pointer there would pass the first test and fail the
 * second, which is why that fix was rejected.
 */

#include <cstdint>
#include <iostream>
#include <memory>
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

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
