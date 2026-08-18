/**
 * Session State Management Implementation
 * 
 * Reference: https://github.com/eman/alpha-hwr (src/alpha_hwr/core/session.py)
 */

#include "session.h"
#include "esphome/core/log.h"

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *const TAG = "alpha_hwr.session";

Session::Session() 
    : state_(SessionState::IDLE) {
  ESP_LOGD(TAG, "Session initialized: state=IDLE");
}

const char* Session::get_state_name() const {
  switch (state_) {
    case SessionState::IDLE:
      return "IDLE";
    case SessionState::SERVICE_DISCOVERY:
      return "SERVICE_DISCOVERY";
    case SessionState::SUBSCRIBING:
      return "SUBSCRIBING";
    case SessionState::STABILIZING:
      return "STABILIZING";
    case SessionState::READY:
      return "READY";
    default:
      return "UNKNOWN";
  }
}

bool Session::is_connected() const { return state_ != SessionState::IDLE; }

void Session::transition_to(SessionState new_state, const char* reason) {
  if (state_ == new_state) {
    ESP_LOGV(TAG, "Already in state %s", get_state_name());
    return;
  }
  
  const char* old_state_name = get_state_name();
  state_ = new_state;
  
  ESP_LOGI(TAG, "Session: %s -> %s (%s)", 
           old_state_name,
           get_state_name(),
           reason);
}

void Session::on_connected() {
  transition_to(SessionState::SERVICE_DISCOVERY, "BLE connected, starting discovery");
}

void Session::on_service_found() {
  if (state_ != SessionState::SERVICE_DISCOVERY) {
    ESP_LOGW(TAG, "on_service_found() called from unexpected state: %s", get_state_name());
  }
  transition_to(SessionState::SUBSCRIBING, "GENI service found");
}

void Session::on_subscribed() {
  if (state_ != SessionState::SUBSCRIBING) {
    ESP_LOGW(TAG, "on_subscribed() called from unexpected state: %s", get_state_name());
  }
  transition_to(SessionState::STABILIZING, "Notifications enabled");
}

void Session::on_ready() {
  if (state_ != SessionState::STABILIZING) {
    ESP_LOGW(TAG, "on_ready() called from unexpected state: %s", get_state_name());
  }
  transition_to(SessionState::READY, "Stabilize window elapsed");
}

void Session::on_disconnected() {
  transition_to(SessionState::IDLE, "BLE disconnected");
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
