// Host tests for the GENI authentication handshake (auth.cpp).
//
// Why this file exists: `esphome compile` was the only thing that compiled
// auth.cpp. It is 146 lines that decide, entirely on timers, whether the
// component believes it is talking to an unlocked pump — and the four packets
// it sends are hand-copied constants that the header itself warns not to touch.
// None of it had a host test.
//
// It is also the file that finding 8 (the deaf node) rests on. That work
// claimed authentication "declares success 1.2 s later without inspecting a
// single reply", and the number came from adding up the delays by eye. It is
// asserted here instead: the handshake is driven through a fake scheduler that
// accumulates the delays it is asked for, and nothing in the sequence consults
// a response.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

#include "../components/alpha_hwr/auth.h"
#include "../components/alpha_hwr/frame_parser.h"
#include "../components/alpha_hwr/transport.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::core::Authentication;
using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::core::AUTH_CLASS10;
using esphome::alpha_hwr::core::AUTH_EXT_1;
using esphome::alpha_hwr::core::AUTH_EXT_2;
using esphome::alpha_hwr::core::AUTH_LEGACY;

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

using Packet = std::vector<uint8_t>;

static Packet as_packet(const uint8_t *data, size_t len) {
  return Packet(data, data + len);
}

// Drives Authentication with a scheduler under the test's control. The real one
// is ESPHome's set_timeout; what matters for the sequence is only that a
// callback runs after a delay, so this records both and lets the test decide
// when (and whether) to run them.
struct AuthRig {
  Transport transport;
  Authentication auth{transport};

  std::vector<Packet> sent;
  std::vector<std::pair<uint32_t, std::function<void()>>> scheduled;
  size_t next_to_run{0};
  uint32_t virtual_elapsed_ms{0};
  int completions{0};

  AuthRig() {
    transport.set_write_callback([this](const uint8_t *data, size_t len) {
      this->sent.push_back(as_packet(data, len));
      return true;
    });
    auth.set_scheduler_callback([this](uint32_t delay, std::function<void()> fn) {
      this->scheduled.emplace_back(delay, std::move(fn));
    });
    auth.set_completion_callback([this] { this->completions++; });
  }

  // The transport paces its BLE writes, so give it room to drain. Every auth
  // packet is under the 20-byte chunk size, so one packet is one write.
  void drain() {
    for (int i = 0; i < 5; i++) {
      mock_millis += 60;
      transport.loop();
    }
  }

  /// Run every timer that has been scheduled and not yet run, including ones
  /// scheduled while draining, accumulating the delays asked for.
  void run_scheduled() {
    while (next_to_run < scheduled.size()) {
      size_t i = next_to_run++;
      virtual_elapsed_ms += scheduled[i].first;
      scheduled[i].second();
      drain();
    }
  }

  void run_full_handshake() {
    auth.start();
    drain();
    run_scheduled();
  }
};

// ── 1. The packet sequence ───────────────────────────────────────────────────
void test_handshake_sends_the_documented_sequence() {
  std::cout << "\n=== The handshake sends the documented packet sequence ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_full_handshake();

  TEST_ASSERT(r.sent.size() == 10,
              "Ten packets: 3 legacy + 5 class-10 + 2 extensions");
  if (r.sent.size() != 10)
    return;

  bool legacy_ok = true;
  for (int i = 0; i < 3; i++)
    legacy_ok &= (r.sent[i] == as_packet(AUTH_LEGACY, sizeof(AUTH_LEGACY)));
  TEST_ASSERT(legacy_ok, "Stage 1 is three byte-exact legacy magic packets");

  bool class10_ok = true;
  for (int i = 3; i < 8; i++)
    class10_ok &= (r.sent[i] == as_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10)));
  TEST_ASSERT(class10_ok, "Stage 2 is five byte-exact Class 10 unlock packets");

  TEST_ASSERT(r.sent[8] == as_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1)),
              "Stage 3 sends EXT_1 (Class 0x05) first");
  TEST_ASSERT(r.sent[9] == as_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2)),
              "Stage 3 sends EXT_2 (Class 0x0B) second — the order is specified");
  TEST_ASSERT(r.completions == 1, "Completion callback fires exactly once");
  TEST_ASSERT(!r.auth.is_running(), "Not running once complete");
}

// ── 2. The 1.2 s figure ──────────────────────────────────────────────────────
// Finding 8 turns on this number, and on the fact that nothing in the sequence
// waits for the pump to answer. Both are asserted rather than read off.
void test_handshake_completes_on_timers_alone() {
  std::cout << "\n=== The handshake completes on timers alone, in 1200 ms ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_full_handshake();

  // 3x50 (stage 1 repeats) + 100 (1->2) + 5x50 (stage 2 repeats) + 200 (2->3)
  // + 500 (final settle). The last stage-1 and stage-2 repeat timers are what
  // advance to the next stage, so all eleven are on the path.
  TEST_ASSERT(r.virtual_elapsed_ms == 1200,
              "Total scheduled delay is exactly 1200 ms");
  TEST_ASSERT(r.scheduled.size() == 11,
              "Eleven timers: 3 + 1 + 5 + 1 + 1");
  TEST_ASSERT(r.completions == 1,
              "Completion is reached without a single inbound frame — nothing "
              "in the handshake inspects a reply (issue #14 / finding 8)");
}

// ── 3. The constants ─────────────────────────────────────────────────────────
// auth.h warns not to modify these packets. They were transcribed by hand from
// the Python reference, so check them against the production CRC routine rather
// than trusting the transcription.
void test_auth_packets_carry_valid_crcs() {
  std::cout << "\n=== Every hardcoded auth packet has a valid CRC ==="
            << std::endl;
  using esphome::alpha_hwr::protocol::frame_crc_valid;

  TEST_ASSERT(frame_crc_valid(AUTH_LEGACY, sizeof(AUTH_LEGACY)),
              "AUTH_LEGACY CRC checks out against codec.cpp");
  TEST_ASSERT(frame_crc_valid(AUTH_CLASS10, sizeof(AUTH_CLASS10)),
              "AUTH_CLASS10 CRC checks out against codec.cpp");
  TEST_ASSERT(frame_crc_valid(AUTH_EXT_1, sizeof(AUTH_EXT_1)),
              "AUTH_EXT_1 CRC checks out against codec.cpp");
  TEST_ASSERT(frame_crc_valid(AUTH_EXT_2, sizeof(AUTH_EXT_2)),
              "AUTH_EXT_2 CRC checks out against codec.cpp");

  // A frame the pump would reject, to show the check above can fail.
  uint8_t corrupted[sizeof(AUTH_LEGACY)];
  for (size_t i = 0; i < sizeof(AUTH_LEGACY); i++)
    corrupted[i] = AUTH_LEGACY[i];
  corrupted[6] ^= 0x01;  // flip a register-address bit, leave the CRC alone
  TEST_ASSERT(!frame_crc_valid(corrupted, sizeof(corrupted)),
              "A one-bit change to the payload fails the same check");
}

// ── 4. Re-entry ──────────────────────────────────────────────────────────────
void test_start_while_running_is_ignored() {
  std::cout << "\n=== A second start() while running is ignored ===" << std::endl;
  mock_millis = 0;
  AuthRig r;

  r.auth.start();
  r.drain();
  TEST_ASSERT(r.auth.is_running(), "Running after start()");
  const size_t after_first = r.sent.size();

  r.auth.start();  // must not restage the burst
  r.drain();
  TEST_ASSERT(r.sent.size() == after_first,
              "The second start() sends nothing");

  r.run_scheduled();
  TEST_ASSERT(r.sent.size() == 10,
              "The interrupted-looking sequence still delivers exactly ten "
              "packets, not two interleaved handshakes");
  TEST_ASSERT(r.completions == 1, "And completes once, not twice");
}

// ── 5. Cancellation ──────────────────────────────────────────────────────────
// cancel() cannot unschedule anything — the scheduler callbacks are already
// queued — so it works by bumping a sequence number the lambdas compare against.
// This is the mechanism that stops a handshake from a previous connection
// firing into the current one.
void test_cancel_invalidates_pending_timers() {
  std::cout << "\n=== cancel() invalidates timers already queued ===" << std::endl;
  mock_millis = 0;
  AuthRig r;

  r.auth.start();
  r.drain();
  const size_t sent_before_cancel = r.sent.size();
  TEST_ASSERT(sent_before_cancel > 0, "Stage 1 began immediately");
  TEST_ASSERT(!r.scheduled.empty(), "A timer is queued");

  r.auth.cancel();
  TEST_ASSERT(!r.auth.is_running(), "Not running after cancel()");

  r.run_scheduled();  // every already-queued lambda now fires
  TEST_ASSERT(r.sent.size() == sent_before_cancel,
              "No further packets escape from the stale timers");
  TEST_ASSERT(r.completions == 0,
              "A cancelled handshake never reports completion");
}

void test_restart_after_cancel_runs_a_full_handshake() {
  std::cout << "\n=== A restart after cancel runs the full sequence ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;

  r.auth.start();
  r.drain();
  r.auth.cancel();
  r.run_scheduled();
  r.sent.clear();

  r.auth.start();
  r.drain();
  r.run_scheduled();

  TEST_ASSERT(r.sent.size() == 10,
              "The restarted handshake sends all ten packets");
  TEST_ASSERT(r.completions == 1,
              "And completes once — the cancelled run's timers stayed dead");
}

// The case the sequence number actually exists for, and the one the ordering
// above hides. `running_ = false` alone is enough to stop a cancelled
// handshake, because every stage and send_packet() checks it — so a test that
// drains the stale timers *before* restarting passes with the sequence bump
// deleted. The mutation check caught exactly that.
//
// The dangerous order is cancel, restart, and only then the old timers firing:
// `running_` is true again by that point, so nothing but the sequence number
// distinguishes a stale callback from a live one. A leaked stage-1 timer
// re-enters the burst mid-flight and the two handshakes interleave.
void test_stale_timers_cannot_re_enter_a_restarted_handshake() {
  std::cout << "\n=== Stale timers cannot re-enter a restarted handshake ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;

  r.auth.start();  // handshake A
  r.drain();
  const size_t sent_by_a = r.sent.size();
  TEST_ASSERT(sent_by_a == 1, "Handshake A got one packet out before cancel");
  TEST_ASSERT(!r.scheduled.empty(), "...and left a timer queued");

  r.auth.cancel();
  r.auth.start();  // handshake B, before A's timer has fired
  r.drain();

  // Now let everything queued run: A's orphan first, then B's.
  r.run_scheduled();

  TEST_ASSERT(r.sent.size() == sent_by_a + 10,
              "Exactly ten packets belong to B — A's orphaned timer did not "
              "re-enter the burst and add more");
  TEST_ASSERT(r.completions == 1,
              "And exactly one completion, not one per handshake");
}

// ── 6. Degenerate wiring ─────────────────────────────────────────────────────
// Nothing here should crash or half-complete if a caller forgets a callback.
void test_missing_scheduler_stalls_without_completing() {
  std::cout << "\n=== Without a scheduler the handshake stalls, not completes ==="
            << std::endl;
  mock_millis = 0;
  Transport transport;
  std::vector<Packet> sent;
  transport.set_write_callback([&sent](const uint8_t *data, size_t len) {
    sent.push_back(as_packet(data, len));
    return true;
  });
  Authentication auth(transport);
  int completions = 0;
  auth.set_completion_callback([&completions] { completions++; });

  auth.start();
  for (int i = 0; i < 5; i++) {
    mock_millis += 60;
    transport.loop();
  }

  TEST_ASSERT(sent.size() == 1,
              "Only the first stage-1 packet goes out with no way to schedule "
              "the rest");
  TEST_ASSERT(completions == 0,
              "And completion is never claimed — a stalled handshake must not "
              "look like a successful one");
  TEST_ASSERT(auth.is_running(),
              "It stays marked running, so the caller's timeout is what "
              "resolves it");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Authentication Handshake Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_handshake_sends_the_documented_sequence();
  test_handshake_completes_on_timers_alone();
  test_auth_packets_carry_valid_crcs();
  test_start_while_running_is_ignored();
  test_cancel_invalidates_pending_timers();
  test_restart_after_cancel_runs_a_full_handshake();
  test_stale_timers_cannot_re_enter_a_restarted_handshake();
  test_missing_scheduler_stalls_without_completing();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
