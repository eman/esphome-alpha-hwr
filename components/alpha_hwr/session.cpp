/**
 * Session State Management Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/session.py
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
    case SessionState::AUTHENTICATING:
      return "AUTHENTICATING";
    case SessionState::READY:
      return "READY";
    case SessionState::ERROR:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

bool Session::is_connected() const {
  return (state_ != SessionState::IDLE && state_ != SessionState::ERROR);
}

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
  last_error_.clear();
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
  transition_to(SessionState::AUTHENTICATING, "Notifications enabled");
}

void Session::on_authenticating() {
  // Can be called from SUBSCRIBING (first auth) or READY (re-auth)
  if (state_ != SessionState::SUBSCRIBING && state_ != SessionState::READY && 
      state_ != SessionState::AUTHENTICATING) {
    ESP_LOGW(TAG, "on_authenticating() called from unexpected state: %s", get_state_name());
  }
  
  if (state_ != SessionState::AUTHENTICATING) {
    transition_to(SessionState::AUTHENTICATING, "Starting authentication");
  }
}

void Session::on_authenticated() {
  if (state_ != SessionState::AUTHENTICATING) {
    ESP_LOGW(TAG, "on_authenticated() called from unexpected state: %s", get_state_name());
  }
  transition_to(SessionState::READY, "Authentication complete");
}

void Session::on_disconnected() {
  transition_to(SessionState::IDLE, "BLE disconnected");
  last_error_.clear();
}

void Session::on_error(const char* error_message) {
  state_ = SessionState::ERROR;
  last_error_ = (error_message != nullptr) ? error_message : "Unknown error";
  ESP_LOGE(TAG, "Session ERROR: %s", last_error_.c_str());
}

void Session::reset() {
  ESP_LOGD(TAG, "Resetting session");
  state_ = SessionState::IDLE;
  last_error_.clear();
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
