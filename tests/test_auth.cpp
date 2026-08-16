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
// accumulates the delays it is asked for.
//
// Since issue #174 the sequence DOES consult responses, and most of what this
// file asserts is the shape of that: 1200 ms when the pump answers, a bounded
// stretch when it answers late, and completion anyway when it never answers at
// all. The reply path is exercised end to end — frames are injected at
// Transport::on_notification() and reach the handshake through the real frame
// observer, so a CRC check or an observer that stopped being wired would show
// up here rather than in a comment.

#include <cstdint>
#include <cstddef>
#include <functional>
#include <iostream>
#include <utility>
#include <vector>

#include "../components/alpha_hwr/auth.h"
#include "../components/alpha_hwr/auth_gate.h"
#include "../components/alpha_hwr/frame_parser.h"
#include "../components/alpha_hwr/transport.h"
#include "fixture_crc.h"

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
  /// Frames the transport handed to the packet callback, i.e. the ones no
  /// command consumed. The observer must not take frames out of this path.
  std::vector<Packet> to_packet_callback;
  /// When true, every packet the handshake sends is answered by the pump
  /// before the next scheduled timer runs.
  bool pump_answers{false};
  /// Index into `sent` of one packet the pump does NOT answer, or -1 for none.
  /// A stage short by exactly one reply is what distinguishes "waits for all of
  /// them" from "waits for most of them" — see
  /// test_a_stage_waits_for_its_last_packet().
  int unanswered_packet{-1};
  /// Packets written but not yet answered; drained one BLE tick later.
  std::vector<Packet> pending_replies;

  AuthRig() {
    transport.set_write_callback([this](const uint8_t *data, size_t len) {
      const int index = static_cast<int>(this->sent.size());
      this->sent.push_back(as_packet(data, len));
      // Queued rather than injected here: a reply cannot arrive in the middle
      // of the write that provoked it, and injecting re-entrantly from inside
      // transport.loop() would test a sequence the radio cannot produce.
      if (this->pump_answers && index != this->unanswered_packet)
        this->pending_replies.push_back(as_packet(data, len));
      return true;
    });
    // Exactly the wiring alpha_hwr.cpp installs.
    transport.set_frame_observer([this](const uint8_t *data, size_t len) {
      this->auth.on_frame(data, len);
    });
    transport.set_packet_callback([this](const uint8_t *data, size_t len) {
      this->to_packet_callback.push_back(as_packet(data, len));
    });
    auth.set_scheduler_callback([this](uint32_t delay, std::function<void()> fn) {
      this->scheduled.emplace_back(delay, std::move(fn));
    });
    auth.set_completion_callback([this] { this->completions++; });
  }

  /// Feed the transport a CRC-valid response frame of `class_byte`.
  ///
  /// Sized to the real thing where the capture in issue #174 gives a size: the
  /// Class 2 replies are 11 bytes, the Class 10 ones 22, and the Class 5 and
  /// Class 11 ones 9. Only the start byte, the length field and the class byte
  /// are load-bearing; the rest is filler, and the CRC is stamped with the
  /// production routine so a real transport accepts it.
  void inject_reply(uint8_t class_byte, size_t total_len) {
    std::vector<uint8_t> frame(total_len, 0x00);
    frame[0] = 0x24;                                     // response start
    frame[1] = static_cast<uint8_t>(total_len - 4);      // length field
    frame[2] = 0xF8;
    frame[3] = 0xE7;
    frame[4] = class_byte;
    frame[5] = static_cast<uint8_t>(total_len - 8);      // APDU body length
    frame = with_crc(std::move(frame));
    transport.on_notification(frame.data(), frame.size());
  }

  /// The pump's answer to `request`: same class, sizes from the capture.
  void reply_to(const Packet &request) {
    if (request.size() < 5)
      return;
    switch (request[4]) {
      case 0x02: inject_reply(0x02, 11); break;
      case 0x0A: inject_reply(0x0A, 22); break;
      case 0x05: inject_reply(0x05, 9); break;
      case 0x0B: inject_reply(0x0B, 9); break;
      default: break;
    }
  }

  // The transport paces its BLE writes, so give it room to drain. Every auth
  // packet is under the 20-byte chunk size, so one packet is one write.
  void drain() {
    for (int i = 0; i < 5; i++) {
      mock_millis += 60;
      transport.loop();
      // The pump answers what was written on the previous tick.
      std::vector<Packet> due;
      due.swap(pending_replies);
      for (const auto &request : due)
        reply_to(request);
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

  /// The handshake as it runs against the pump in the capture: every packet
  /// answered, promptly.
  void run_answered_handshake() {
    pump_answers = true;
    run_full_handshake();
  }
};

/// Extra wait a single gate can add before it gives up, in ms.
static constexpr uint32_t GATE_CEILING_MS =
    esphome::alpha_hwr::core::AUTH_GATE_MAX_WAITS *
    esphome::alpha_hwr::core::AUTH_GATE_POLL_MS;

// ── 1. The packet sequence ───────────────────────────────────────────────────
void test_handshake_sends_the_documented_sequence() {
  std::cout << "\n=== The handshake sends the documented packet sequence ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_answered_handshake();

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
// Finding 8 turned on this number. It still holds, but it is now the ANSWERED
// case rather than the only case: the delays are floors, and a pump that keeps
// up with them costs nothing extra.
void test_an_answered_handshake_still_takes_1200_ms() {
  std::cout << "\n=== An answered handshake completes in 1200 ms, unchanged ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_answered_handshake();

  // 3x50 (stage 1 repeats) + 100 (1->2) + 5x50 (stage 2 repeats) + 200 (2->3)
  // + 500 (final settle). The last stage-1 and stage-2 repeat timers are what
  // advance to the next stage, so all eleven are on the path.
  TEST_ASSERT(r.virtual_elapsed_ms == 1200,
              "Total scheduled delay is exactly 1200 ms — the gates cost "
              "nothing when the pump has already answered");
  TEST_ASSERT(r.scheduled.size() == 11,
              "Eleven timers: 3 + 1 + 5 + 1 + 1, with no gate re-checks");
  TEST_ASSERT(r.completions == 1, "And it completes once");
  TEST_ASSERT(r.auth.replies_seen() == 10,
              "All ten replies were counted");
  TEST_ASSERT(r.auth.packets_sent() == 10, "Against ten packets sent");
}

// ── 2b. The unanswered case ──────────────────────────────────────────────────
// The fail-open half. A pump that answers nothing must still authenticate —
// the evidence that this pump answers is two logs from one specimen, which
// justifies waiting for a reply and not requiring one (auth_gate.h).
void test_an_unanswered_handshake_stretches_but_still_completes() {
  std::cout << "\n=== A silent pump stretches the handshake but still completes ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_full_handshake();  // pump_answers stays false

  TEST_ASSERT(r.completions == 1,
              "Completion is still reached with no inbound frame at all — the "
              "gate fails open, so a pump variant that stays quiet during the "
              "handshake is not stranded");
  TEST_ASSERT(r.sent.size() == 10, "And all ten packets still went out");
  TEST_ASSERT(r.auth.replies_seen() == 0,
              "With the silence recorded — the signal the 60 s data watchdog "
              "would otherwise have been the first to produce");

  // Three gates, each waiting its full ceiling before giving up.
  TEST_ASSERT(r.virtual_elapsed_ms == 1200 + 3 * GATE_CEILING_MS,
              "Total delay is 1200 ms plus three full gate ceilings");
  TEST_ASSERT(r.virtual_elapsed_ms == 2700,
              "...which is 2700 ms — the number link_watchdog.h's sizing note "
              "is written against");
}

// The ceiling is what keeps the fail-open bounded. Without it a deaf pump
// would hold the handshake open indefinitely and outlast the very watchdog
// meant to catch it.
void test_a_late_reply_is_waited_for_rather_than_walked_past() {
  std::cout << "\n=== A stage that answers late is waited for ===" << std::endl;
  mock_millis = 0;
  AuthRig r;

  // Stage 1 and 2 answer promptly; stage 3's replies are withheld until the
  // gate has already waited a few ticks.
  r.pump_answers = true;
  r.auth.start();
  r.drain();

  bool withheld = false;
  std::vector<Packet> held;
  while (r.next_to_run < r.scheduled.size()) {
    size_t i = r.next_to_run++;
    r.virtual_elapsed_ms += r.scheduled[i].first;
    // Withhold the extension replies the first time stage 3 sends.
    if (!withheld && r.sent.size() == 8) {
      r.pump_answers = false;
      withheld = true;
    }
    r.scheduled[i].second();
    r.drain();
    // Release them three ticks into the final gate.
    if (withheld && r.virtual_elapsed_ms >= 1200 + 3 * 50 && !r.pump_answers) {
      r.pump_answers = true;
      r.inject_reply(0x05, 9);
      r.inject_reply(0x0B, 9);
    }
  }

  TEST_ASSERT(r.completions == 1, "It completes");
  TEST_ASSERT(r.auth.replies_seen() == 10,
              "Having counted the late extension replies rather than "
              "completing without them");
  TEST_ASSERT(r.virtual_elapsed_ms > 1200,
              "And it waited past the 500 ms floor to do so");
  // Bounded by ONE ceiling, not three. Only stage 3's gate waits in this
  // scenario, so `< 1200 + 3 * GATE_CEILING_MS` was 3x too loose to falsify
  // anything: a gate that ignored the late replies and burned its full ceiling
  // reached 1700 ms and still passed it. Measured, on the mutation that does
  // exactly that: real code 1400 ms, mutant 1700 ms, old bound 2700 ms.
  TEST_ASSERT(r.virtual_elapsed_ms < 1200 + GATE_CEILING_MS,
              "And advanced strictly inside stage 3's own ceiling — as soon as "
              "the replies landed, not on a fixed longer timer");
}

// ── 2d. All of them, not most of them ────────────────────────────────────────
// The property the whole change turns on — a stage is not left until *every*
// one of its packets has been answered — was asserted nowhere until a second
// adversarial pass went looking. Both of the cases above are blind to it: at
// 10-of-10 the `>=` in the gate holds whatever the expected count is, and at
// 0-of-N it fails whatever the expected count is. Only a stage short by
// exactly one can tell "waits for all five" from "waits for four".
//
// Nothing tied packets_in_stage()'s 3/5/2 to the `repeat_count <` bounds that
// actually send the packets, so drift on the gate side was silent: all three
// of `case 1: return 2`, `case 2: return 4` and `case 3: return 1` left the
// full suite green, and `case 2: return 4` is precisely the stage-2 boundary
// crossed one packet early that this change exists to prevent. Now in
// tools/mutation_check.sh as auth-gate-expects-one-packet-too-few.
static void assert_stage_waits_for_its_last_packet(int unanswered_index,
                                                   const char *what) {
  mock_millis = 0;
  AuthRig r;
  r.pump_answers = true;
  r.unanswered_packet = unanswered_index;
  r.run_full_handshake();

  TEST_ASSERT(r.auth.replies_seen() == 9, what);
  // Exactly one gate — the one whose stage is short — burns its full ceiling.
  // If the gate expected one packet fewer than the stage sends, it would be
  // satisfied and this would read 1200.
  TEST_ASSERT(r.virtual_elapsed_ms == 1200 + GATE_CEILING_MS,
              "...and that stage's gate waited out its whole ceiling rather "
              "than counting the short stage as answered");
  TEST_ASSERT(r.completions == 1, "...then completed anyway, failing open");
}

void test_a_stage_waits_for_its_last_packet() {
  std::cout << "\n=== A stage short one reply waits, per stage ===" << std::endl;
  // Packet indices: 0-2 stage 1, 3-7 stage 2, 8-9 stage 3.
  assert_stage_waits_for_its_last_packet(
      2, "Stage 1 with 2 of 3 answered is not treated as answered");
  assert_stage_waits_for_its_last_packet(
      7, "Stage 2 with 4 of 5 answered is not treated as answered");
  assert_stage_waits_for_its_last_packet(
      9, "Stage 3 with 1 of 2 answered is not treated as answered");
}

// ── 2e. What may not be counted ──────────────────────────────────────────────
// Two guards in on_frame()'s path that the first round of tests left standing
// on nothing.
void test_frames_of_no_handshake_class_are_not_counted() {
  std::cout << "\n=== Frames of an unmapped class are not replies ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auth.start();
  r.drain();

  // Class 3 command ACKs and Class 7 device-info frames both occur on this
  // link and answer no handshake stage. Dropping the `stage != 0` term of
  // auth_frame_answers_stage() would credit them -- and then index
  // stage_replies_[stage - 1] with stage == 0, i.e. one before the array.
  r.inject_reply(0x03, 11);
  r.inject_reply(0x07, 16);
  TEST_ASSERT(r.auth.replies_seen() == 0,
              "A Class 3 ACK and a Class 7 device-info frame answer no stage, "
              "so neither is counted (and neither indexes stage_replies_[-1])");
}

void test_a_telemetry_notification_during_stage_1_is_not_a_stage_2_reply() {
  std::cout << "\n=== An unsolicited Class 10 frame during stage 1 is not "
               "credited ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auth.start();
  r.drain();  // stage 1 is in flight

  // This pump pushes control-mode notifications unprompted during the
  // handshake. One landing here answers nothing: stage 2 has not sent yet.
  // Pinned through Authentication rather than only against the predicate,
  // because the stage marker is what makes the predicate's answer correct --
  // `current_stage_ = 1` mutated to 2 survives a unit-level test.
  r.inject_reply(0x0A, 22);
  TEST_ASSERT(r.auth.replies_seen() == 0,
              "Not counted while stage 1 is the stage in flight");

  r.run_scheduled();
  TEST_ASSERT(r.completions == 1, "And the handshake still completes");
}

void test_a_flood_of_replies_cannot_wrap_the_counter() {
  std::cout << "\n=== A saturated reply counter does not wrap ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auth.start();
  r.drain();

  // stage_replies_ is a byte. 256 same-class frames wrap it to zero without
  // the saturation guard, which would reopen a gate the pump had already
  // satisfied -- the exact failure the guard's comment describes.
  for (int i = 0; i < 256; i++)
    r.inject_reply(0x02, 11);

  r.run_scheduled();
  TEST_ASSERT(r.completions == 1, "It completes");
  TEST_ASSERT(r.virtual_elapsed_ms == 1200 + 2 * GATE_CEILING_MS,
              "Stage 1's gate is satisfied — only stages 2 and 3, which the "
              "pump never answered here, wait out their ceilings. A wrapped "
              "counter would make it three");
}

// ── 2c. The observer takes nothing ───────────────────────────────────────────
// The regression the whole design exists to avoid. Matching the stage-2 reply
// as a command response would consume it, and the control-mode notification
// the pump sends during that stage would stop being decoded and published.
void test_the_handshake_consumes_none_of_the_replies() {
  std::cout << "\n=== Watching the replies does not take them ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_answered_handshake();

  TEST_ASSERT(r.to_packet_callback.size() == 10,
              "All ten replies still reached the packet callback — the "
              "observer is non-consuming, so the telemetry parser sees exactly "
              "what it saw before (issue #174)");

  int class10 = 0;
  for (const auto &p : r.to_packet_callback)
    if (p.size() > 4 && p[4] == 0x0A)
      class10++;
  TEST_ASSERT(class10 == 5,
              "Including all five Class 10 frames, the ones a command match "
              "would have swallowed");
}

// A frame the transport rejects must not be counted either: the handshake sees
// frames after the CRC check, not before it.
void test_a_corrupt_reply_is_not_counted() {
  std::cout << "\n=== A frame failing CRC is not counted as a reply ==="
            << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.auth.start();
  r.drain();

  std::vector<uint8_t> frame(11, 0x00);
  frame[0] = 0x24;
  frame[1] = 0x07;
  frame[2] = 0xF8;
  frame[3] = 0xE7;
  frame[4] = 0x02;
  frame[9] = 0xAA;  // deliberately wrong CRC
  frame[10] = 0xBB;
  r.transport.on_notification(frame.data(), frame.size());

  TEST_ASSERT(r.auth.replies_seen() == 0,
              "A corrupt frame is dropped by the transport and never reaches "
              "the handshake");
}

// Frames arriving with no handshake running must not be banked for the next
// one, or a reconnect onto a dead pump would inherit a live pump's evidence.
void test_replies_do_not_carry_across_handshakes() {
  std::cout << "\n=== Reply accounting is per handshake ===" << std::endl;
  mock_millis = 0;
  AuthRig r;
  r.run_answered_handshake();
  TEST_ASSERT(r.auth.replies_seen() == 10, "The first handshake was answered");

  // Frames while nothing is running are ignored...
  r.inject_reply(0x02, 11);
  r.inject_reply(0x0A, 22);

  // ...and the next handshake starts from zero.
  r.pump_answers = false;
  r.auth.start();
  r.drain();
  TEST_ASSERT(r.auth.replies_seen() == 0,
              "The second handshake starts with no inherited replies");
  r.run_scheduled();
  TEST_ASSERT(r.auth.replies_seen() == 0,
              "And a silent second handshake reports silence, not the first "
              "handshake's ten");
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
  test_an_answered_handshake_still_takes_1200_ms();
  test_an_unanswered_handshake_stretches_but_still_completes();
  test_a_late_reply_is_waited_for_rather_than_walked_past();
  test_a_stage_waits_for_its_last_packet();
  test_frames_of_no_handshake_class_are_not_counted();
  test_a_telemetry_notification_during_stage_1_is_not_a_stage_2_reply();
  test_a_flood_of_replies_cannot_wrap_the_counter();
  test_the_handshake_consumes_none_of_the_replies();
  test_a_corrupt_reply_is_not_counted();
  test_replies_do_not_carry_across_handshakes();
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
