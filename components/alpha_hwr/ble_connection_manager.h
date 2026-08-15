#pragma once

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "subscribe_outcome.h"
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>
#include <functional>
#include <string>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * Pump identification data decoded from the BLE advertisement before any
 * connection is established.
 *
 * Populated by cache_adv_info_from_service_data(), which is wired to the
 * esp32_ble_tracker on_ble_service_data_advertise trigger in YAML (see
 * packages/alpha_hwr_base.yaml).  Available on the first scan hit, so it
 * can drive conditional logic (e.g. encryption timing) before the GATT
 * connection opens.
 *
 * Reference: Python client._scan_advertisement_data()
 *   service_data["0000fe5d-..."][3] = product_family  (0x34 = ALPHA)
 *   service_data["0000fe5d-..."][4] = product_type    (0x07 = HWR)
 *   service_data["0000fe5d-..."][5] = product_version
 */
struct PumpAdvertisementInfo {
  uint8_t product_family{0};   // 0x34 = ALPHA family
  uint8_t product_type{0};     // 0x07 = HWR type
  uint8_t product_version{0};  // BLE firmware version discriminator
  std::string adv_hex;         // FE5D service-data payload (hex, UUID stripped), for debugging
  bool valid{false};           // True once successfully parsed
};

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

  // Advertisement info — populated at scan time via cache_adv_info_from_service_data(),
  // before the connection opens.  Call this from an on_ble_service_data_advertise
  // trigger in YAML (see packages/alpha_hwr_base.yaml for the wiring).
  const PumpAdvertisementInfo &get_advertisement_info() const { return adv_info_; }
  void set_advertisement_callback(std::function<void(const PumpAdvertisementInfo &)> cb) {
    advertisement_callback_ = std::move(cb);
  }
  void cache_adv_info_from_service_data(const std::vector<uint8_t> &service_data);

  // Device validation (used in discovery mode / YAML on_ble_advertise filters)
  static bool is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device,
                                   uint16_t company_id,
                                   uint8_t product_family,
                                   uint8_t product_type,
                                   const esp32_ble_tracker::ESPBTUUID &service_uuid);

  /// Tear down the current GATT link, latching @p reason for the Pump Link
  /// Status companion sensor. Used by the component's inbound-data watchdog
  /// (link_watchdog.h): recovery in this component is driven by the BLE
  /// disconnection callback, so dropping the link is how a dead-but-open
  /// connection gets recycled. No-op when no client is attached.
  void force_disconnect(const char *reason);

  // Debug helpers
  void dump_services();

  // Pump Link Status support: latched human-readable reason of the most recent
  // failed attempt, and the bond state observed at the last connection-open.
  const std::string &get_last_failure() const { return last_failure_; }
  bool was_bonded_at_open() const { return bonded_at_open_; }
  
 private:
  void subscribe_to_notifications();
  /// The subscribe attempt itself, reduced to its outcome. Split out from
  /// subscribe_to_notifications() so that what the caller does about a failure
  /// is a decision over a value (subscribe_outcome.h) rather than five
  /// scattered early returns that each dropped what they knew (issue #175).
  SubscribeOutcome attempt_subscribe_();
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

  // Bonded-reconnect encryption tracking (see issue #12).
  // encryption_pending_ is true from the moment esp_ble_set_encryption() is
  // issued until ESP_GAP_BLE_AUTH_CMPL_EVT arrives.  While it is set, the
  // CCCD write (notification subscription) must not be sent: an unencrypted
  // GATT write racing SMP negotiation can be rejected with "insufficient
  // authentication", a failure path that can end in bond erasure.
  bool encryption_pending_{false};
  // Set when service discovery completed while encryption was still pending;
  // the subscription is then performed from the AUTH_CMPL success handler.
  bool subscription_deferred_{false};
  
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
  std::function<void(const PumpAdvertisementInfo &)> advertisement_callback_;

  // Pump Link Status support
  std::string last_failure_;     // latched last failure reason (human-readable)
  bool bonded_at_open_{false};   // bond state captured at the last connection-open
  // Why last_failure_ is being held over the routine disconnects of a reconnect
  // loop, if it is. One value rather than two booleans on purpose: the origin
  // decides how the hold is released, so a "held" flag and a separate "who set
  // it" flag can disagree, and every site that sets one must remember the
  // other. That went wrong immediately -- a successful auth cleared the hold
  // but left the origin set, so a LATER pairing failure could be silently
  // erased by the next notification.
  //
  // AUTH is released only by a successful AUTH_CMPL, because the failure it
  // records erases the bond and recovery must pass back through one. DATA is
  // released by any inbound notification, which refutes it by construction --
  // and must NOT be released by AUTH_CMPL, which never fires at all when
  // pairing is disabled (the default).
  //
  // Crucially the two are not interchangeable: an SMP failure on an UNBONDED
  // pump latches its reason WITHOUT tearing the link down, and that link then
  // subscribes and delivers notifications normally (passive telemetry needs no
  // bond). Releasing an AUTH hold on inbound data would wipe exactly the
  // pairing diagnostic the hold exists to preserve.
  enum class FailureHold : uint8_t {
    NONE,  // no hold; the next disconnect reason may overwrite last_failure_
    AUTH,  // auth/encryption failure; released by a successful AUTH_CMPL
    DATA,  // inbound-data watchdog; released by any received notification
  };
  FailureHold failure_hold_{FailureHold::NONE};

  // Advertisement identifiers decoded at scan time (pre-connection)
  PumpAdvertisementInfo adv_info_;

  // Pairing status sensor
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
