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
  replies_matched_ = 0;
  reads_sent_ = 0;
  ESP_LOGI(TAG, "Starting 3-stage authentication handshake");

  // Start Stage 1 immediately
  stage1_legacy_burst(0);
}

void Authentication::cancel() {
  if (running_) {
    ESP_LOGW(TAG, "Authentication cancelled");
    running_ = false;
    auth_sequence_++;  // Invalidate any pending scheduler lambdas
  }
}

bool Authentication::send_packet(const uint8_t* data, size_t len) {
  if (!running_) {
    ESP_LOGD(TAG, "Authentication cancelled, skipping packet send");
    return false;
  }

  std::vector<uint8_t> packet(data, data + len);
  this->transport_.send_command(packet);

  return true;
}

bool Authentication::send_read(
    const uint8_t *data, size_t len,
    std::function<void(bool, const uint8_t *, size_t)> on_reply) {
  if (!running_) {
    ESP_LOGD(TAG, "Authentication cancelled, skipping read");
    return false;
  }

  std::vector<uint8_t> packet(data, data + len);
  reads_sent_++;

  // Wildcard expectation (0, 0): these classes carry no Object/Sub-ID to match
  // on, so the transport matches them by class byte against the class this
  // command was queued as. See response_match.h for why that is sufficient
  // here and what makes it insufficient for Class 10.
  uint32_t seq = auth_sequence_;
  this->transport_.send_command(
      packet, 0, 0,
      [this, seq, on_reply](bool success, const uint8_t *frame, size_t frame_len) {
        // A reply arriving for a sequence that has since been cancelled or
        // restarted belongs to a connection that no longer exists. Same guard
        // the scheduled callbacks use, for the same reason.
        if (seq != this->auth_sequence_) return;
        if (!this->running_) return;
        if (success) this->replies_matched_++;
        on_reply(success, frame, frame_len);
      },
      REPLY_TIMEOUT_MS);

  return true;
}

void Authentication::stage1_legacy_burst(int repeat_count) {
  if (!running_) return;
  
  if (repeat_count < 3) {
    // The Class 2 identity read. Paced by the pump's answer rather than by a
    // 50 ms timer: the next one goes out when this one is answered, so a slower
    // pump stretches the sequence instead of being talked over, and a faster
    // one is not waited on. The repeats are kept -- nothing establishes that
    // this pump needs three identical reads, and nothing establishes that no
    // pump does (issue #174).
    ESP_LOGD(TAG, "Stage 1: Sending legacy magic packet %d/3", repeat_count + 1);
    send_read(AUTH_LEGACY, sizeof(AUTH_LEGACY),
              [this, repeat_count](bool, const uint8_t *, size_t) {
                this->stage1_legacy_burst(repeat_count + 1);
              });
  } else {
    // Stage 1 complete. The 100 ms that used to separate the stages is gone
    // with them: it existed to let stage 1 land, and a matched reply is a
    // stronger statement that it landed than any delay is.
    ESP_LOGD(TAG, "Stage 1 complete, starting Stage 2");
    stage2_class10_burst(0);
  }
}

void Authentication::stage2_class10_burst(int repeat_count) {
  if (!running_) return;
  
  // The one stage still paced by timers, and deliberately.
  //
  // Its packet is a Class 10 GET, so unlike the other three the transport could
  // always have matched its reply -- the problem is what matching costs. That
  // reply is the operation-status frame the pump also volunteers as a passive
  // notification, and TelemetryService::handle_passive_notification() decodes
  // it and publishes the control mode, operation mode and setpoint from it. A
  // matched command *consumes* the frame: packet_callback_ never runs for it,
  // so the publish would silently stop happening during every connect.
  //
  // Handing it to both is not a one-line change either, because the two paths
  // are given different things. The wildcard match path passes the callback the
  // whole frame; the Class 10 path passes a payload slice (data + 10), which is
  // not enough to re-derive the frame the telemetry decoder wants. Doing this
  // properly means giving TelemetryService a payload-level entry point that
  // both callers share -- worth doing, and larger than this change.
  //
  // So this stage keeps its 50 ms repeats and its 200 ms tail, and they are
  // still transcribed sleeps rather than measurements. Issue #174.
  if (repeat_count < 5) {
    // Send Class 10 unlock packet
    ESP_LOGD(TAG, "Stage 2: Sending Class 10 unlock packet %d/5", repeat_count + 1);
    send_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    if (scheduler_callback_) {
      uint32_t seq = auth_sequence_;
      scheduler_callback_(50, [this, repeat_count, seq]() {
        if (seq != this->auth_sequence_) return;
        this->stage2_class10_burst(repeat_count + 1);
      });
    }
  } else {
    // Stage 2 complete, wait 200ms then start Stage 3 (Python uses 0.2s)
    ESP_LOGD(TAG, "Stage 2 complete, waiting 200ms before Stage 3");
    if (scheduler_callback_) {
      uint32_t seq = auth_sequence_;
      scheduler_callback_(200, [this, seq]() {
        if (seq != this->auth_sequence_) return;
        this->stage3_extensions();
      });
    }
  }
}

void Authentication::stage3_extensions() {
  if (!running_) return;

  ESP_LOGD(TAG, "Stage 3: Sending extension packets");

  // The two INFO queries, Class 5 then Class 11. Sent one after the other
  // rather than both at once: they are distinct classes, and the transport
  // matches a wildcard reply against whichever command is at the head of the
  // queue, so two in flight together would be matched in queue order rather
  // than by class. Serialising them removes the question.
  //
  // The 500 ms "final stabilization" that used to follow is gone. It was the
  // last of the transcribed sleeps, and what it was waiting for -- the pump
  // having dealt with these two packets -- is exactly what their replies say.
  send_read(AUTH_EXT_1, sizeof(AUTH_EXT_1), [this](bool, const uint8_t *, size_t) {
    if (!this->running_) return;
    this->send_read(AUTH_EXT_2, sizeof(AUTH_EXT_2),
                    [this](bool, const uint8_t *, size_t) { this->complete(); });
  });
}

void Authentication::complete() {
  if (!running_) return;
  
  // Say what the pump actually did, not just that the sequence ended. Before
  // this the replies reached nothing, so a pump that answered everything and a
  // pump that answered nothing produced the same single line -- which is why
  // ten unanswered packets could have gone unnoticed indefinitely.
  //
  // Counted over the five matched reads only; stage 2's five are still sent
  // blind, so nothing here knows whether they were answered.
  if (replies_matched_ == reads_sent_) {
    ESP_LOGI(TAG, "Authentication handshake complete (%u/%u reads answered)",
             replies_matched_, reads_sent_);
  } else {
    // Fail open. Two logs from one specimen justify waiting for a reply; they
    // do not justify requiring one, and a variant that stays quiet until first
    // polled still has to get past this point. Teardown stays with the link
    // watchdog, whose budget is measured.
    ESP_LOGW(TAG,
             "Authentication handshake complete, but only %u of %u reads were "
             "answered -- proceeding anyway",
             replies_matched_, reads_sent_);
  }
  running_ = false;
  
  // Notify completion
  if (completion_callback_) {
    completion_callback_();
  }
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
