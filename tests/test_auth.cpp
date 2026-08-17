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
// single reply", and the number came from adding up the delays by eye. This
// file asserted it instead -- and then issue #174 fixed it, so the assertions
// have been turned around: stages 1 and 3 now advance on the pump's replies,
// and what is pinned is that they *wait* for them.
//
// Replies are injected through the real Transport::on_notification(), so they
// are reassembled, CRC-checked and matched by production code. A change that
// broke the match would fail here rather than pass against a stub. Stage 2 is
// still on timers -- deliberately, see stage2_class10_burst() -- and the fake
// scheduler that pinned the old behaviour is what still drives it.

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

// The pump's actual answers to the three matched reads, captured on hardware
// and quoted in auth.h's packet comments (issue #174).
static const uint8_t REPLY_CLASS2[] = {0x24, 0x07, 0xF8, 0xE7, 0x02, 0x03,
                                       0x34, 0x07, 0x02, 0x89, 0x7A};
static const uint8_t REPLY_CLASS5[] = {0x24, 0x05, 0xF8, 0xE7, 0x05,
                                       0x01, 0xA1, 0x27, 0x58};
static const uint8_t REPLY_CLASS11[] = {0x24, 0x05, 0xF8, 0xE7, 0x0B,
                                        0x01, 0x80, 0x08, 0x1A};

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

  /// Replies to inject, keyed by the class of the packet that asked. Captured
  /// from a real pump and reproduced byte-for-byte (issue #174) so that a
  /// change breaking CRC validation or frame reassembly fails here rather than
  /// in a comment. Class 10 is absent deliberately: stage 2 is still sent
  /// blind, so nothing is waiting to match its reply.
  bool auto_answer{true};
  std::vector<uint8_t> pending_answers;

  AuthRig() {
    transport.set_write_callback([this](const uint8_t *data, size_t len) {
      this->sent.push_back(as_packet(data, len));
      // Queue the pump's answer rather than injecting it here. A real pump
      // cannot reply before the write it is answering has returned, and the
      // transport only enters AWAITING_RESPONSE after this callback -- so
      // answering inline would arrive before anything was waiting and fall
      // through to the passive path, testing the opposite of the intent.
      if (this->auto_answer && len > 4) this->pending_answers.push_back(data[4]);
      return true;
    });
    auth.set_scheduler_callback([this](uint32_t delay, std::function<void()> fn) {
      this->scheduled.emplace_back(delay, std::move(fn));
    });
    auth.set_completion_callback([this] { this->completions++; });
  }

  /// Feed the pump's reply for a request of class @p request_class.
  void answer_class(uint8_t request_class) {
    switch (request_class) {
      case 0x02:
        transport.on_notification(REPLY_CLASS2, sizeof(REPLY_CLASS2));
        break;
      case 0x05:
        transport.on_notification(REPLY_CLASS5, sizeof(REPLY_CLASS5));
        break;
      case 0x0B:
        transport.on_notification(REPLY_CLASS11, sizeof(REPLY_CLASS11));
        break;
      default:
        break;  // Class 10: sent blind, nothing awaits it.
    }
  }

  /// Push the clock far enough forward that any outstanding read gives up.
  void expire_pending_reads() {
    for (int i = 0; i < 4; i++) {
      mock_millis += Authentication::REPLY_TIMEOUT_MS;
      transport.loop();
    }
  }

  // The transport paces its BLE writes, so give it room to drain. Every auth
  // packet is under the 20-byte chunk size, so one packet is one write.
  /// Loop, then deliver whatever the pump owes us, then loop again -- a reply
  /// can start the next read, whose own reply then needs delivering. Bounded
  /// rather than while(!pending.empty()) so a sequence that somehow never
  /// settles fails the assertions instead of hanging the suite.
  ///
  /// @param rounds 60 ms each. Keep the total under REPLY_TIMEOUT_MS when the
  ///   test is asserting that an *unanswered* read blocks the sequence --
  ///   otherwise the read times out, the sequence advances on its own, and the
  ///   test silently measures the fail-open path instead.
  void drain(int rounds = 40) {
    for (int i = 0; i < rounds; i++) {
      mock_millis += 60;
      transport.loop();
      if (pending_answers.empty()) continue;
      std::vector<uint8_t> due;
      due.swap(pending_answers);
      for (uint8_t request_class : due) answer_class(request_class);
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

// ── 2. What is still on a timer, and what is not ─────────────────────────────
// This file used to assert the opposite: 1200 ms of scheduled delay across
// eleven timers, and completion "without a single inbound frame". That was the
// defect (issue #174), pinned so it could not drift. Stage 2 is all that is
// left of it.
void test_only_stage2_runs_on_timers() {
  std::cout << "\n=== Only stage 2 is still driven by timers ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_full_handshake();

  // 5 x 50 ms between stage 2's repeats, plus its 200 ms tail. Stages 1 and 3
  // schedule nothing at all now -- their pacing is the pump's replies.
  TEST_ASSERT(r.scheduled.size() == 6,
              "Six timers, all stage 2's: five repeats plus the tail");
  TEST_ASSERT(r.virtual_elapsed_ms == 450,
              "450 ms of scheduled delay, down from 1200 -- the 750 ms removed "
              "is stage 1's and stage 3's transcribed sleeps");
  TEST_ASSERT(r.completions == 1, "Completion still fires exactly once");
}

// ── 2b. The property the change exists for ───────────────────────────────────
// A read is not left until the pump has answered it. This is the half that a
// counting or timer-based gate gets wrong, so it is asserted by holding one
// reply back and checking the sequence stops there rather than running on.
void test_a_read_is_not_left_until_it_is_answered() {
  std::cout << "\n=== A matched read waits for its own reply ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auto_answer = false;  // Answer nothing at all.
  r.auth.start();
  r.drain(5);  // 300 ms: enough to write, well under REPLY_TIMEOUT_MS.

  // Stage 1's first read is out and unanswered. Nothing else may follow it:
  // not its own repeats, not stage 2.
  TEST_ASSERT(r.sent.size() == 1,
              "Exactly one packet is in flight while its reply is outstanding");
  TEST_ASSERT(r.completions == 0, "...and the sequence has not completed");

  // Now answer it, and only it. The second read may go out; the third may not.
  r.answer_class(0x02);
  r.drain(5);
  TEST_ASSERT(r.sent.size() == 2,
              "Answering the first read releases exactly the second, not the rest");
  TEST_ASSERT(r.completions == 0, "Still not complete");
}

// ── 2c. Fail open ────────────────────────────────────────────────────────────
// A pump that answers nothing must still reach READY. Two logs from one
// specimen justify waiting for a reply; they do not justify requiring one, and
// a variant that stays quiet until first polled has to get through here.
void test_an_unanswered_sequence_still_completes() {
  std::cout << "\n=== A pump that answers nothing still completes ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auto_answer = false;
  r.auth.start();

  // Let every outstanding read time out in turn. Each timeout releases the
  // next packet, so this has to be driven repeatedly, not once.
  for (int i = 0; i < 12; i++) {
    r.expire_pending_reads();
    r.run_scheduled();
  }

  // Pin the constant as a literal. Every other assertion here derives its
  // timings from REPLY_TIMEOUT_MS, so the suite would certify any value it was
  // given -- the same way the clock-sync grace period certified both 1 ms and
  // 24 hours before it was pinned this way.
  TEST_ASSERT(Authentication::REPLY_TIMEOUT_MS == 1000u,
              "The per-read timeout is 1000 ms, not whatever the tests are told");
  TEST_ASSERT(r.sent.size() == 10,
              "All ten packets are still sent when none of them is answered");
  TEST_ASSERT(r.completions == 1,
              "And the sequence completes anyway -- an unanswered read is not "
              "a failure, it is a read that went unanswered");
  TEST_ASSERT(!r.auth.is_running(), "Not left running");
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

  // Hold back the replies so A stops with exactly one read outstanding. That
  // outstanding read is the new form of the same hazard: its callback is as
  // capable of re-entering a restarted sequence as a leaked timer was, and it
  // is guarded by the same sequence number.
  r.auto_answer = false;
  r.auth.start();  // handshake A
  r.drain(5);
  const size_t sent_by_a = r.sent.size();
  TEST_ASSERT(sent_by_a == 1, "Handshake A got one read out before cancel");

  r.auth.cancel();
  r.auth.start();  // handshake B, before A's read has been answered

  // A's reply arrives now, after the restart. It matches the command A queued,
  // so auth's callback runs -- and must do nothing, because the sequence it
  // belongs to is gone.
  r.answer_class(0x02);
  r.auto_answer = true;
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
  std::cout << "\n=== Without a scheduler the handshake stalls at stage 2 ==="
            << std::endl;
  mock_millis = 0;
  Transport transport;
  std::vector<Packet> sent;
  std::vector<uint8_t> owed;
  transport.set_write_callback([&sent, &owed](const uint8_t *data, size_t len) {
    sent.push_back(as_packet(data, len));
    if (len > 4) owed.push_back(data[4]);
    return true;
  });
  Authentication auth(transport);
  int completions = 0;
  auth.set_completion_callback([&completions] { completions++; });

  // No scheduler is set. Stage 1 does not need one any more -- it is paced by
  // replies -- so answer everything and let it run as far as it can get.
  auth.start();
  for (int i = 0; i < 20; i++) {
    mock_millis += 60;
    transport.loop();
    std::vector<uint8_t> due;
    due.swap(owed);
    for (uint8_t c : due) {
      if (c == 0x02) transport.on_notification(REPLY_CLASS2, sizeof(REPLY_CLASS2));
      if (c == 0x05) transport.on_notification(REPLY_CLASS5, sizeof(REPLY_CLASS5));
      if (c == 0x0B) transport.on_notification(REPLY_CLASS11, sizeof(REPLY_CLASS11));
    }
  }

  // Stage 1's three reads complete on their replies and stage 2's first packet
  // goes out -- and there it stops, because stage 2's repeat is the one thing
  // still scheduled rather than answered.
  TEST_ASSERT(sent.size() == 4,
              "Stage 1's three reads finish on replies, then stage 2 stalls "
              "after its first packet with nothing to schedule the rest");
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
  test_only_stage2_runs_on_timers();
  test_a_read_is_not_left_until_it_is_answered();
  test_an_unanswered_sequence_still_completes();
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
