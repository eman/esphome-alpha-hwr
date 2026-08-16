#include "auth.h"
#include "transport.h"

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *TAG = "alpha_hwr.auth";

Authentication::Authentication(Transport &transport) : transport_(transport) {}

void Authentication::set_scheduler_callback(SchedulerCallback callback) {
  scheduler_callback_ = std::move(callback);
}

void Authentication::set_completion_callback(CompletionCallback callback) {
  completion_callback_ = std::move(callback);
}

void Authentication::start() {
  if (running_) {
    ESP_LOGW(TAG, "Authentication already in progress");
    return;
  }

  running_ = true;
  auth_sequence_++;
  // Reset the reply accounting before anything can be counted into it. A
  // handshake that reported the previous run's replies would report a live
  // pump for a dead one on every reconnect after the first.
  current_stage_ = 0;
  stage_replies_[0] = stage_replies_[1] = stage_replies_[2] = 0;
  gate_waits_ = 0;
  replies_total_ = 0;
  packets_total_ = 0;
  frames_total_ = 0;
  outcome_ = HandshakeOutcome::NOT_RUN;
  ESP_LOGI(TAG, "Starting 3-stage authentication handshake");

  // Start Stage 1 immediately
  stage1_legacy_burst(0);
}

void Authentication::cancel() {
  if (running_) {
    ESP_LOGW(TAG, "Authentication cancelled");
    running_ = false;
    auth_sequence_++;  // Invalidate any pending scheduler lambdas
    // Clear the stage marker for symmetry with complete(), which does the
    // same. It is an EQUIVALENT MUTANT and known to be one: removing it, or
    // start()'s copy, or both, leaves the suite green, and that is correct
    // rather than a coverage gap. Two things make a stale marker
    // unobservable -- on_frame() returns early while !running_, and start()
    // calls stage1_legacy_burst(0) synchronously, which assigns
    // current_stage_ = 1 before control returns and therefore before any
    // frame can be offered. A third adversarial pass reported the start()
    // reset as "not equivalent, and reachable"; it is neither, and the
    // experiment that showed so is recorded in tools/mutation_check.sh.
    // Kept anyway, on the same defence-in-depth grounds as the redundant
    // auth_sequence_++ in this function.
    current_stage_ = 0;
  }
}

bool Authentication::send_packet(const uint8_t* data, size_t len) {
  if (!running_) {
    ESP_LOGD(TAG, "Authentication cancelled, skipping packet send");
    return false;
  }

  std::vector<uint8_t> packet(data, data + len);
  // No command callback, deliberately: the pump's answers to these packets are
  // observed via on_frame() instead of matched as command responses. Passing a
  // callback here would make the transport await a response it can only match
  // for stage 2, and matching that one would consume the control-mode
  // notification before the telemetry parser sees it (auth_gate.h).
  this->transport_.send_command(packet);
  packets_total_++;

  return true;
}

void Authentication::schedule_(uint32_t delay_ms, std::function<void()> fn) {
  if (!scheduler_callback_)
    return;
  uint32_t seq = auth_sequence_;
  scheduler_callback_(delay_ms, [this, seq, fn = std::move(fn)]() {
    if (seq != this->auth_sequence_) return;  // Stale callback
    fn();
  });
}

uint8_t Authentication::packets_in_stage(uint8_t stage) {
  switch (stage) {
    case 1: return 3;
    case 2: return 5;
    case 3: return 2;
    default: return 0;
  }
}

void Authentication::on_frame(const uint8_t *data, size_t len) {
  if (!running_)
    return;

  // Counted BEFORE the class filter, and that ordering is the whole point of
  // this counter existing separately: a frame of a class no stage maps to is
  // still the pump talking. See complete().
  if (data != nullptr && len > 0 && frames_total_ < UINT16_MAX)
    frames_total_++;

  if (!auth_frame_answers_stage(data, len, current_stage_))
    return;

  const uint8_t stage = auth_stage_for_reply_class(data[4]);
  // Saturate rather than wrap. The count is only ever compared against a
  // stage's packet count with >=, so a byte that wrapped to 0 under a flood of
  // same-class frames would reopen the gate it had already satisfied.
  if (stage_replies_[stage - 1] < 255)
    stage_replies_[stage - 1]++;
  if (replies_total_ < UINT16_MAX)
    replies_total_++;

  ESP_LOGV(TAG, "Stage %u reply (Class 0x%02X, %u bytes): %u/%u answered",
           (unsigned) stage, data[4], (unsigned) len,
           (unsigned) stage_replies_[stage - 1],
           (unsigned) packets_in_stage(stage));
}

void Authentication::gate_stage_(uint8_t stage, std::function<void()> advance) {
  if (!running_) return;

  const uint8_t expected = packets_in_stage(stage);
  const uint8_t seen = stage_replies_[stage - 1];

  switch (auth_stage_gate(seen, expected, gate_waits_, AUTH_GATE_MAX_WAITS)) {
    case AuthGate::WAIT:
      gate_waits_++;
      schedule_(AUTH_GATE_POLL_MS, [this, stage, advance]() {
        this->gate_stage_(stage, advance);
      });
      return;

    case AuthGate::PROCEED_UNANSWERED:
      // The ceiling, not the pump, released this gate. Logged at WARN rather
      // than acted on: the handshake fails open (auth_gate.h), and it is
      // complete() that reports a handshake the pump ignored entirely.
      ESP_LOGW(TAG,
               "Stage %u: only %u of %u packets answered after %u ms of extra "
               "wait; continuing",
               (unsigned) stage, (unsigned) seen, (unsigned) expected,
               (unsigned) (AUTH_GATE_MAX_WAITS * AUTH_GATE_POLL_MS));
      break;

    case AuthGate::PROCEED_ANSWERED:
      ESP_LOGD(TAG, "Stage %u answered (%u/%u) after %u ms of extra wait",
               (unsigned) stage, (unsigned) seen, (unsigned) expected,
               (unsigned) (gate_waits_ * AUTH_GATE_POLL_MS));
      break;
  }

  gate_waits_ = 0;
  advance();
}

void Authentication::stage1_legacy_burst(int repeat_count) {
  if (!running_) return;

  current_stage_ = 1;

  if (repeat_count < 3) {
    // Send legacy magic packet
    ESP_LOGD(TAG, "Stage 1: Sending legacy magic packet %d/3", repeat_count + 1);
    send_packet(AUTH_LEGACY, sizeof(AUTH_LEGACY));

    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    schedule_(50, [this, repeat_count]() {
      this->stage1_legacy_burst(repeat_count + 1);
    });
  } else {
    // Stage 1 packets are out. Wait the documented 100 ms (Python uses 0.1s),
    // then leave the stage only once the pump has answered them.
    ESP_LOGD(TAG, "Stage 1 complete, waiting 100ms before Stage 2");
    schedule_(100, [this]() {
      this->gate_stage_(1, [this]() { this->stage2_class10_burst(0); });
    });
  }
}

void Authentication::stage2_class10_burst(int repeat_count) {
  if (!running_) return;

  current_stage_ = 2;

  if (repeat_count < 5) {
    // Send Class 10 unlock packet
    ESP_LOGD(TAG, "Stage 2: Sending Class 10 unlock packet %d/5", repeat_count + 1);
    send_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10));

    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    schedule_(50, [this, repeat_count]() {
      this->stage2_class10_burst(repeat_count + 1);
    });
  } else {
    // Wait 200ms (Python uses 0.2s), then gate. This is the tightest boundary
    // in the capture behind issue #174: the fifth packet's reply landed 123 ms
    // inside it. Inside, not outside -- the capture does not show a stage
    // being walked past, and auth_gate.h records why the issue reads it as
    // though it does.
    ESP_LOGD(TAG, "Stage 2 complete, waiting 200ms before Stage 3");
    schedule_(200, [this]() {
      this->gate_stage_(2, [this]() { this->stage3_extensions(); });
    });
  }
}

void Authentication::stage3_extensions() {
  if (!running_) return;

  current_stage_ = 3;

  ESP_LOGD(TAG, "Stage 3: Sending extension packets");

  // Send EXT_1 (Class 0x05) then EXT_2 (Class 0x0B)
  // Order per protocol/connection.md Step C
  send_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));
  send_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));

  // Wait 500ms for final stabilization (Python uses 0.5s), then complete only
  // once both extension packets have been answered.
  schedule_(500, [this]() {
    this->gate_stage_(3, [this]() { this->complete(); });
  });
}

void Authentication::complete() {
  if (!running_) return;

  // The decision, before the reporting. Kept as a value so it can be asserted:
  // the mutation reverting SILENT's test to the reply count is invisible if
  // this only ever reaches a log line.
  outcome_ = frames_total_ == 0    ? HandshakeOutcome::SILENT
             : replies_total_ == 0 ? HandshakeOutcome::UNRECOGNISED
                                   : HandshakeOutcome::ANSWERED;

  switch (outcome_) {
    case HandshakeOutcome::SILENT: {
    // Nothing came back at all, on any class -- the silent link, named at the
    // end of the handshake (2700 ms of scheduled delay on a pump that answers
    // nothing, since every gate runs to its ceiling first). The earliest
    // indication is the stage-1 gate's PROCEED_UNANSWERED warning at 750 ms;
    // this line is the summary.
    //
    // Gated on frames_total_, not replies_total_. An earlier version used the
    // reply count and justified it as "the only test that cannot
    // false-POSITIVE -- if any frame came back, the link is not deaf". That
    // was false, and the same commit shipped the test proving it:
    // replies_total_ counts only the four classes auth_stage_for_reply_class()
    // maps, so a pump ACKing the handshake on Class 3 -- a class
    // test_frames_of_no_handshake_class_are_not_counted() asserts is not
    // counted, and which does occur on this link -- answered ten of ten and
    // would have been reported deaf.
    //
    // What this counter claims is therefore narrow and true: no CRC-valid
    // frame of any class arrived between start() and here. Two things still
    // fall outside it, and neither is a defect in the test so much as the
    // limit of what the handshake can observe: frames that fail CRC (dropped
    // in transport.cpp before the observer runs) and frames that arrive after
    // complete() (on_frame() returns early once !running_). A pump that only
    // ever answers late, or only ever corrupt, is the watchdog's to catch.
    //
    // The head start over the watchdog is tens of seconds, not the 57 s a
    // naive 60 - 2.7 would give: the watchdog's window opens at
    // connection-open and the handshake does not start there (500 ms
    // post-connect, discovery, 2000 ms stabilize come first). Taking
    // link_watchdog.h's measured 5.90/6.17/5.94 s open-to-READY rather than
    // its constants, which under-predict, this line lands ~7-8 s after open,
    // so ~52 s. First cycle only: link_data_timeout_next() doubles the window
    // after each recycle, so on a persistently silent pump the watchdog's
    // message arrives later each time while this one stays put.
      ESP_LOGE(TAG,
               "Handshake complete but no frame of any kind arrived during it "
               "(%u packets sent) - the link is open and silent; expect the "
               "data watchdog to recycle it",
               (unsigned) packets_total_);
      break;
    }
    case HandshakeOutcome::UNRECOGNISED: {
      // Something answered, but nothing on a class this handshake recognises.
      // Not silence, so not the branch above; not an answered handshake either.
      ESP_LOGW(TAG,
               "Handshake complete: %u frames arrived but none on a class any "
               "stage expects - the pump is talking, but not answering these "
               "packets",
               (unsigned) frames_total_);
      break;
    }
    case HandshakeOutcome::ANSWERED: {
      ESP_LOGI(TAG,
               "Authentication handshake complete (%u of %u packets answered)",
               (unsigned) replies_total_, (unsigned) packets_total_);
      break;
    }
    case HandshakeOutcome::NOT_RUN:
      break;  // unreachable: assigned above
  }

  running_ = false;
  current_stage_ = 0;

  // Notify completion
  if (completion_callback_) {
    completion_callback_();
  }
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
