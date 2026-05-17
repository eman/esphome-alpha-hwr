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

void Authentication::stage1_legacy_burst(int repeat_count) {
  if (!running_) return;
  
  if (repeat_count < 3) {
    // Send legacy magic packet
    ESP_LOGD(TAG, "Stage 1: Sending legacy magic packet %d/3", repeat_count + 1);
    send_packet(AUTH_LEGACY, sizeof(AUTH_LEGACY));
    
    // Schedule next repeat after 50ms (Python uses 0.05s delay)
    if (scheduler_callback_) {
      uint32_t seq = auth_sequence_;
      scheduler_callback_(50, [this, repeat_count, seq]() {
        if (seq != this->auth_sequence_) return;  // Stale callback
        this->stage1_legacy_burst(repeat_count + 1);
      });
    }
  } else {
    // Stage 1 complete, wait 100ms then start Stage 2 (Python uses 0.1s)
    ESP_LOGD(TAG, "Stage 1 complete, waiting 100ms before Stage 2");
    if (scheduler_callback_) {
      uint32_t seq = auth_sequence_;
      scheduler_callback_(100, [this, seq]() {
        if (seq != this->auth_sequence_) return;
        this->stage2_class10_burst(0);
      });
    }
  }
}

void Authentication::stage2_class10_burst(int repeat_count) {
  if (!running_) return;
  
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
  
  // Send EXT_1 (Class 0x05) then EXT_2 (Class 0x0B)
  // Order per protocol/connection.md Step C
  send_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));
  send_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));
  
  // Wait 500ms for final stabilization (Python uses 0.5s)
  if (scheduler_callback_) {
    uint32_t seq = auth_sequence_;
    scheduler_callback_(500, [this, seq]() {
      if (seq != this->auth_sequence_) return;
      this->complete();
    });
  }
}

void Authentication::complete() {
  if (!running_) return;
  
  ESP_LOGI(TAG, "Authentication handshake complete");
  running_ = false;
  
  // Notify completion
  if (completion_callback_) {
    completion_callback_();
  }
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
