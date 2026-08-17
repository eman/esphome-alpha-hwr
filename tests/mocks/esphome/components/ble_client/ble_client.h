// Host-test stand-in for ESPHome's ble_client.
//
// Expanded from an 11-line write-only stub so the component's BLE lifecycle can
// be host-compiled (issue #174 audit tail). Only what components/alpha_hwr
// reads, and the state a test needs to steer: which service and characteristic
// discovery finds, and the peer address GAP events are matched against.

#pragma once

#include <cstdint>
#include <vector>

#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/core/component.h"

namespace esphome {

/// The real base layer keeps this sentinel; alpha_hwr.cpp compares against it
/// to tell "no connection" from a live conn_id.
namespace esp32_ble_client {
constexpr uint16_t UNSET_CONN_ID = 0xFFFF;
}  // namespace esp32_ble_client

namespace ble_client {

using esp32_ble_tracker::ESPBTUUID;

struct BLECharacteristic {
  ESPBTUUID uuid;
  uint16_t handle{0};
};

struct BLEService {
  ESPBTUUID uuid;
  std::vector<BLECharacteristic *> characteristics;
  uint16_t start_handle{0};
  uint16_t end_handle{0};

  /// The real one walks the GATT DB and fills `characteristics`. Here the test
  /// has already supplied them, so this only records that it was called --
  /// ble_connection_manager.cpp calls it before looking, and a mock that
  /// silently no-opped would hide a change in that order.
  int parse_calls{0};
  void parse_characteristics() { parse_calls++; }
};

enum class ClientState { INIT, DISCONNECTED, CONNECTING, CONNECTED, ESTABLISHED };

class BLEClient {
 public:
  virtual ~BLEClient() = default;

  virtual void write_value(uint16_t handle, const uint8_t *data, uint16_t length,
                           bool response = true) {
    (void) handle; (void) data; (void) length; (void) response;
  }

  virtual BLEService *get_service(const ESPBTUUID &uuid) {
    if (service_ != nullptr && service_->uuid == uuid) return service_;
    return nullptr;
  }

  virtual BLECharacteristic *get_characteristic(const ESPBTUUID &service_uuid,
                                                const ESPBTUUID &chr_uuid) {
    (void) service_uuid;
    if (characteristic_ != nullptr && characteristic_->uuid == chr_uuid) return characteristic_;
    return nullptr;
  }

  virtual void disconnect() { disconnect_calls_++; }

  virtual uint64_t get_address() const { return address_; }
  virtual void set_auto_connect(bool v) { auto_connect_ = v; }
  virtual void register_ble_node(void *node) { registered_nodes_++; (void) node; }

  virtual uint16_t get_conn_id() const { return conn_id_; }
  virtual esp_gatt_if_t get_gattc_if() const { return gattc_if_; }
  virtual uint8_t *get_remote_bda() { return remote_bda_; }

  // ── Test-only state. Not present on the real class. ──────────────────────
  void mock_set_service(BLEService *s) { service_ = s; }
  void mock_set_characteristic(BLECharacteristic *c) { characteristic_ = c; }
  void mock_set_address(uint64_t a) { address_ = a; }
  bool mock_auto_connect() const { return auto_connect_; }
  int mock_registered_nodes() const { return registered_nodes_; }
  void mock_set_remote_bda(const uint8_t addr[6]) {
    for (int i = 0; i < 6; i++) remote_bda_[i] = addr[i];
  }
  int mock_disconnect_calls() const { return disconnect_calls_; }
  void mock_reset() {
    service_ = nullptr;
    characteristic_ = nullptr;
    disconnect_calls_ = 0;
    for (int i = 0; i < 6; i++) remote_bda_[i] = 0;
  }

 protected:
  BLEService *service_{nullptr};
  BLECharacteristic *characteristic_{nullptr};
  int disconnect_calls_{0};
  uint16_t conn_id_{1};
  esp_gatt_if_t gattc_if_{1};
  uint8_t remote_bda_[6]{};
  uint64_t address_{0};
  bool auto_connect_{false};
  int registered_nodes_{0};
};

/// Base for a component that attaches to a BLEClient. The real one carries
/// node_state and the gattc/gap event plumbing; the component under test
/// overrides those handlers, so only the hooks it uses are here.
class BLEClientNode {
 public:
  virtual ~BLEClientNode() = default;
  virtual void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
    (void) event; (void) gattc_if; (void) param;
  }
  virtual void gap_event_handler(esp_gap_ble_cb_event_t event,
                                 esp_ble_gap_cb_param_t *param) {
    (void) event; (void) param;
  }
  BLEClient *parent() { return parent_; }
  void set_parent(BLEClient *p) { parent_ = p; }

 protected:
  BLEClient *parent_{nullptr};
};

}  // namespace ble_client
}  // namespace esphome
