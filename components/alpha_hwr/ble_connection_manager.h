#pragma once

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <functional>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * BLE Connection Manager
 * 
 * Responsibilities:
 * - Service discovery and retry logic
 * - Notification subscription (CCCD write)
 * - Security/pairing configuration
 * - GATT and GAP event routing
 * - Connection lifecycle management
 * 
 * This module isolates all BLE-specific operations from the main component,
 * matching the separation in the Python reference implementation where
 * BLE operations are handled by the `client.py` layer.
 */
class BLEConnectionManager {
 public:
  BLEConnectionManager() = default;

  // Configuration
  void set_ble_client(ble_client::BLEClient *client) { client_ = client; }
  void set_pairing_enabled(bool enabled) { pairing_enabled_ = enabled; }
  void set_pairing_status_sensor(binary_sensor::BinarySensor *sensor) { pairing_status_sensor_ = sensor; }
  
  // Service and characteristic UUIDs
  void set_service_uuid(const esp32_ble_tracker::ESPBTUUID &uuid) { service_uuid_ = uuid; }
  void set_characteristic_uuid(const esp32_ble_tracker::ESPBTUUID &uuid) { characteristic_uuid_ = uuid; }
  
  // Callbacks for component integration
  void set_scheduler_callback(std::function<void(uint32_t, std::function<void()>)> callback) {
    scheduler_callback_ = callback;
  }
  void set_connection_callback(std::function<void()> callback) { connection_callback_ = callback; }
  void set_disconnection_callback(std::function<void()> callback) { disconnection_callback_ = callback; }
  void set_service_found_callback(std::function<void()> callback) { service_found_callback_ = callback; }
  void set_subscribed_callback(std::function<void()> callback) { subscribed_callback_ = callback; }
  void set_notification_callback(std::function<void(const uint8_t*, size_t)> callback) {
    notification_callback_ = callback;
  }
  
  // Connection management
  void init_security();
  void handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  void handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  
  // Device validation
  static bool is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device,
                                   uint16_t company_id,
                                   uint8_t product_family,
                                   uint8_t product_type,
                                   const esp32_ble_tracker::ESPBTUUID &service_uuid);
  
  // Debug helpers
  void dump_services();
  
 private:
  void subscribe_to_notifications();
  void handle_connection_opened(const esp_ble_gattc_cb_param_t *param);
  static void handle_service_discovered(const esp_ble_gattc_cb_param_t *param);
  void handle_service_discovery_complete(esp_gatt_if_t gattc_if);
  void handle_notification(const esp_ble_gattc_cb_param_t *param);
  void handle_auth_complete(const esp_ble_gap_cb_param_t *param);
  /// Returns true if the device at @p bda already has a stored bond.
  static bool check_is_bonded(const esp_bd_addr_t bda);
  
  // BLE client reference
  ble_client::BLEClient *client_{nullptr};
  
  // Service/characteristic UUIDs
  esp32_ble_tracker::ESPBTUUID service_uuid_;
  esp32_ble_tracker::ESPBTUUID characteristic_uuid_;
  
  // Configuration
  bool pairing_enabled_{false};
  
  // Service discovery retry mechanism
  uint8_t discovery_retry_count_{0};
  uint32_t scheduler_sequence_{0};  // Sequence counter to invalidate stale lambdas
  static const uint8_t MAX_DISCOVERY_RETRIES = 3;
  static const uint32_t DISCOVERY_RETRY_DELAY_MS = 1000;
  static const uint32_t POST_CONNECT_DELAY_MS = 500;
  
  // Callbacks
  std::function<void(uint32_t, std::function<void()>)> scheduler_callback_;
  std::function<void()> connection_callback_;
  std::function<void()> disconnection_callback_;
  std::function<void()> service_found_callback_;
  std::function<void()> subscribed_callback_;
  std::function<void(const uint8_t*, size_t)> notification_callback_;
  
  // Pairing status sensor
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
