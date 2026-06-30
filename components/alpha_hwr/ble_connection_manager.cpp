#include "ble_connection_manager.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *TAG = "alpha_hwr.ble";

// Static method to validate if a BLE device is an ALPHA HWR pump
// Primary method: Check company ID in manufacturer data
// Secondary fallback: Check for service UUID
bool BLEConnectionManager::is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device,
                                                uint16_t company_id,
                                                uint8_t product_family,
                                                uint8_t product_type,
                                                const esp32_ble_tracker::ESPBTUUID &service_uuid) {
  // Primary Discovery Method: Match by Company ID in manufacturer data
  const auto &mfg_datas = device.get_manufacturer_datas();
  
  for (const auto &mfg_data : mfg_datas) {
    esp_bt_uuid_t uuid = mfg_data.uuid.get_uuid();
    if (uuid.len == ESP_UUID_LEN_16 && uuid.uuid.uuid16 == company_id) {
      const auto &service_data = mfg_data.data;
      
      // Validate service data structure:
      // Byte 0-1: Frame header
      // Byte 2: Product Family (0x34 = ALPHA)
      // Byte 3: Product Type (0x07 = HWR)
      // Byte 4+: Additional data
      if (service_data.size() >= 5 &&
          service_data[2] == product_family && 
          service_data[3] == product_type) {
        ESP_LOGI(TAG, "Found ALPHA HWR pump via Company ID (primary method)");
        return true;
      }
    }
  }
  
  // Secondary Discovery Method: Check for service UUID
  for (const auto &svc_uuid : device.get_service_uuids()) {
    if (svc_uuid == service_uuid) {
      ESP_LOGI(TAG, "Found ALPHA HWR pump via service UUID");
      return true;
    }
  }
  
  return false;
}

void BLEConnectionManager::init_security() {
  if (!pairing_enabled_) {
    ESP_LOGI(TAG, "BLE pairing disabled - using passive telemetry only");
    return;
  }
  
  ESP_LOGI(TAG, "Configuring BLE Security for Pairing/Bonding...");
  
  // Set IO capabilities to "No Input No Output" (Just Works pairing)
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
  
  // Set authentication requirements: Bonding + Secure Connections
  uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
  
  // Set maximum/minimum encryption key size (16 bytes)
  uint8_t key_size = 16;
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MIN_KEY_SIZE, &key_size, sizeof(uint8_t));
  
  // Enable key distribution for encryption and identity
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));
  
  ESP_LOGI(TAG, "BLE security configuration complete");
}

void BLEConnectionManager::dump_services() {
  if (!client_) {
    return;
  }
  
  // Check configured service UUID
  auto *service = client_->get_service(service_uuid_);
  if (service) {
    ESP_LOGI(TAG, "Grundfos Service found (0xFE5D), handles 0x%04x-0x%04x",
             service->start_handle, service->end_handle);
    
    // Ensure characteristics are parsed
    if (service->characteristics.empty()) {
      service->parse_characteristics();
    }
    
    if (service->characteristics.empty()) {
      ESP_LOGW(TAG, "No characteristics found in service!");
    }
  } else {
    char uuid_buf[esphome::esp32_ble::UUID_STR_LEN];
    ESP_LOGW(TAG, "Service NOT found! Expected UUID: %s", service_uuid_.to_str(uuid_buf));
  }
}

void BLEConnectionManager::subscribe_to_notifications() {
  if (!client_) {
    ESP_LOGW(TAG, "BLE client not available");
    return;
  }
  
  auto *service = client_->get_service(service_uuid_);
  if (!service) {
    ESP_LOGW(TAG, "Service not found for notification subscription");
    return;
  }
  
  auto *chr = client_->get_characteristic(service->uuid, characteristic_uuid_);
  if (!chr) {
    ESP_LOGW(TAG, "Characteristic not found");
    return;
  }
  
  // Register for notifications (tells ESP-IDF we want to receive them)
  auto status = esp_ble_gattc_register_for_notify(client_->get_gattc_if(), 
                                                   client_->get_remote_bda(), 
                                                   chr->handle);
  if (status) {
    ESP_LOGW(TAG, "Failed to register for notifications: %d", status);
    return;
  }
  
  ESP_LOGI(TAG, "Registered for notifications (local)");
  
  // Now write to the CCCD descriptor to enable notifications on the server side
  // CCCD handle is typically characteristic handle + 1
  uint16_t cccd_handle = chr->handle + 1;
  uint8_t notify_enable[] = {0x01, 0x00};  // 0x0001 = enable notifications
  
  ESP_LOGI(TAG, "Writing to CCCD descriptor (handle 0x%04x) to enable notifications...", cccd_handle);
  
  status = esp_ble_gattc_write_char_descr(
      client_->get_gattc_if(),
      client_->get_conn_id(),
      cccd_handle,
      sizeof(notify_enable),
      notify_enable,
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);
  
  if (status) {
    ESP_LOGW(TAG, "Failed to write CCCD descriptor: %d", status);
  } else {
    ESP_LOGI(TAG, "CCCD write successful - notifications should now be enabled");
  }
  
  // Notify component that subscription is complete
  if (subscribed_callback_) {
    subscribed_callback_();
  }
}

void BLEConnectionManager::handle_connection_opened(const esp_ble_gattc_cb_param_t *param) {
  ESP_LOGI(TAG, "BLE connection opened. Pairing enabled: %s", pairing_enabled_ ? "YES" : "NO");
  
  // Notify component of connection
  if (connection_callback_) {
    connection_callback_();
  }
  
  // Reset discovery retry counter
  discovery_retry_count_ = 0;
  
  // Request encryption only when we already have a stored bond.
  // - Bonded:   request encryption immediately so the pump can resume the
  //             encrypted session before GATT discovery proceeds.
  // - Unbonded: stay silent — this pump initiates bonding itself via
  //             ESP_GAP_BLE_SEC_REQ_EVT.  A central-initiated pairing
  //             request on an unbonded pump returns 0x52 ("Pairing Not
  //             Supported"), causing the pump's own SEC_REQ to be missed.
  if (pairing_enabled_) {
    if (check_is_bonded(client_->get_remote_bda())) {
      ESP_LOGI(TAG, "Device is bonded - requesting encryption to resume secure session");
      esp_err_t ret = esp_ble_set_encryption(client_->get_remote_bda(), ESP_BLE_SEC_ENCRYPT);
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "✗ Failed to request encryption: 0x%x", ret);
      }
    } else {
      ESP_LOGI(TAG, "Device is not bonded - waiting for pump to initiate pairing");
    }
  } else {
    ESP_LOGI(TAG, "Skipping encryption request - pairing disabled");
  }

  // Update connection parameters for better stability
  esp_ble_conn_update_params_t conn_params;
  memcpy(conn_params.bda, client_->get_remote_bda(), 6);
  conn_params.min_int = 24;      // 30ms
  conn_params.max_int = 40;      // 50ms 
  conn_params.latency = 0;
  conn_params.timeout = 400;     // 4s
  esp_ble_gap_update_conn_params(&conn_params);
  
  // Wait for encryption/pairing to stabilize before discovery
  if (scheduler_callback_) {
    scheduler_sequence_++;
    uint32_t seq = scheduler_sequence_;
    scheduler_callback_(POST_CONNECT_DELAY_MS, [this, seq]() {
      if (seq != this->scheduler_sequence_) return;  // Stale callback
      ESP_LOGI(TAG, "Starting service discovery...");
    });
  }
}

void BLEConnectionManager::handle_service_discovered(const esp_ble_gattc_cb_param_t *param) {
  // Service discovery callback — individual services are logged at verbose level only
  auto *search_res = &param->search_res;
  if (search_res->srvc_id.uuid.len == ESP_UUID_LEN_16) {
    ESP_LOGV(TAG, "Service discovered: 0x%04X", search_res->srvc_id.uuid.uuid.uuid16);
  }
}

void BLEConnectionManager::handle_service_discovery_complete(esp_gatt_if_t gattc_if) {
  ESP_LOGI(TAG, "Service discovery complete (attempt %d/%d). Checking for service...", 
           discovery_retry_count_ + 1, MAX_DISCOVERY_RETRIES);
  
  // Dump all services for debugging
  dump_services();
  
  // Check for our expected service
  if (client_) {
    auto *service = client_->get_service(service_uuid_);
    
    if (service) {
      ESP_LOGI(TAG, "✓ Service found, enabling notifications...");
      
      // Notify component that service was found
      if (service_found_callback_) {
        service_found_callback_();
      }
      
      // Check for characteristic
      auto *chr = client_->get_characteristic(service->uuid, characteristic_uuid_);
      if (chr) {
        subscribe_to_notifications();
      } else {
        char uuid_buf[esphome::esp32_ble::UUID_STR_LEN];
        ESP_LOGW(TAG, "Characteristic NOT found: %s", characteristic_uuid_.to_str(uuid_buf));
      }
    } else {
      // Service NOT found - implement retry logic
      ESP_LOGW(TAG, "✗ Service NOT found!");
      
      if (discovery_retry_count_ < MAX_DISCOVERY_RETRIES) {
        discovery_retry_count_++;
        ESP_LOGW(TAG, "Retrying service discovery in %dms (attempt %d/%d)...", 
                 DISCOVERY_RETRY_DELAY_MS, discovery_retry_count_ + 1, MAX_DISCOVERY_RETRIES);
        
        // Schedule a retry
        if (scheduler_callback_) {
          scheduler_sequence_++;
          uint32_t seq = scheduler_sequence_;
          scheduler_callback_(DISCOVERY_RETRY_DELAY_MS, [this, gattc_if, seq]() {
            if (seq != this->scheduler_sequence_) return;  // Stale callback
            ESP_LOGI(TAG, "Triggering service discovery retry...");
            esp_ble_gattc_search_service(gattc_if, this->client_->get_conn_id(), nullptr);
          });
        }
      } else {
        ESP_LOGE(TAG, "Failed to find service after %d attempts", MAX_DISCOVERY_RETRIES);
      }
    }
  }
}

void BLEConnectionManager::handle_notification(const esp_ble_gattc_cb_param_t *param) {
  auto *notify_evt = &param->notify;
  if (notify_evt->value_len > 0) {
    ESP_LOGV(TAG, "Received notification, %d bytes", notify_evt->value_len);
    if (notification_callback_) {
      notification_callback_(notify_evt->value, notify_evt->value_len);
    }
  }
}

void BLEConnectionManager::handle_auth_complete(const esp_ble_gap_cb_param_t *param) {
  auto &auth_cmpl = param->ble_security.auth_cmpl;
  char addr_str[18];
  sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
          auth_cmpl.bd_addr[0], auth_cmpl.bd_addr[1], auth_cmpl.bd_addr[2],
          auth_cmpl.bd_addr[3], auth_cmpl.bd_addr[4], auth_cmpl.bd_addr[5]);
  
  if (auth_cmpl.success) {
    ESP_LOGI(TAG, "✓ BLE authentication complete (Pairing/Bonding successful)!");
    ESP_LOGI(TAG, "  Device: %s", addr_str);
    ESP_LOGI(TAG, "  Auth mode: 0x%02x", auth_cmpl.auth_mode);
    ESP_LOGD(TAG, "  Key present: 0x%02x", auth_cmpl.key_present);
    ESP_LOGD(TAG, "  Key type: 0x%02x", auth_cmpl.key_type);
    if (pairing_status_sensor_ != nullptr) {
      pairing_status_sensor_->publish_state(true);
    }
  } else {
    // Decode failure reason for better debugging
    const char *fail_reason = "Unknown";
    switch (auth_cmpl.fail_reason) {
      case ESP_AUTH_SMP_PASSKEY_FAIL: fail_reason = "Passkey Entry Failed"; break;
      case ESP_AUTH_SMP_OOB_FAIL: fail_reason = "OOB Data Not Available"; break;
      case ESP_AUTH_SMP_PAIR_AUTH_FAIL: fail_reason = "Authentication Requirements Not Met"; break;
      case ESP_AUTH_SMP_CONFIRM_VALUE_FAIL: fail_reason = "Confirm Value Mismatch"; break;
      case ESP_AUTH_SMP_PAIR_NOT_SUPPORT: fail_reason = "Pairing Not Supported"; break;
      case ESP_AUTH_SMP_ENC_KEY_SIZE: fail_reason = "Encryption Key Size Too Small"; break;
      case ESP_AUTH_SMP_INVALID_CMD: fail_reason = "Invalid SMP Command"; break;
      case ESP_AUTH_SMP_UNKNOWN_ERR: fail_reason = "Unspecified Error"; break;
      case ESP_AUTH_SMP_REPEATED_ATTEMPT: fail_reason = "Repeated Pairing Attempts"; break;
      case ESP_AUTH_SMP_INVALID_PARAMETERS: fail_reason = "Invalid Parameters"; break;
      case ESP_AUTH_SMP_DHKEY_CHK_FAIL: fail_reason = "DHKey Check Failed"; break;
      case ESP_AUTH_SMP_NUM_COMP_FAIL: fail_reason = "Numeric Comparison Failed"; break;
      case ESP_AUTH_SMP_BR_PARING_IN_PROGR: fail_reason = "BR/EDR Pairing In Progress"; break;
      case ESP_AUTH_SMP_XTRANS_DERIVE_NOT_ALLOW: fail_reason = "Cross-Transport Key Derivation Not Allowed"; break;
      case ESP_AUTH_SMP_INTERNAL_ERR: fail_reason = "Internal Error"; break;
      case ESP_AUTH_SMP_UNKNOWN_IO: fail_reason = "Unknown IO Capability"; break;
      case ESP_AUTH_SMP_INIT_FAIL: fail_reason = "Pairing Initiation Failed"; break;
      case ESP_AUTH_SMP_CONFIRM_FAIL: fail_reason = "Confirmation Failed"; break;
      case ESP_AUTH_SMP_BUSY: fail_reason = "Security Manager Busy"; break;
      case ESP_AUTH_SMP_ENC_FAIL: fail_reason = "Encryption Start Failed"; break;
      case ESP_AUTH_SMP_STARTED: fail_reason = "Pairing Already Started"; break;
      case ESP_AUTH_SMP_RSP_TIMEOUT: fail_reason = "Response Timeout"; break;
      case ESP_AUTH_SMP_DIV_NOT_AVAIL: fail_reason = "Diversifier Not Available"; break;
      case ESP_AUTH_SMP_UNSPEC_ERR: fail_reason = "Unspecified Failure"; break;
      case ESP_AUTH_SMP_CONN_TOUT: fail_reason = "Connection Timeout"; break;
      default: fail_reason = "Other"; break;
    }
    ESP_LOGW(TAG, "✗ BLE authentication failed!");
    ESP_LOGW(TAG, "  Device: %s", addr_str);
    ESP_LOGW(TAG, "  Failure reason: %s (0x%02x)", fail_reason, auth_cmpl.fail_reason);
    ESP_LOGW(TAG, "  Auth mode: 0x%02x", auth_cmpl.auth_mode);
    if (pairing_status_sensor_ != nullptr) {
      pairing_status_sensor_->publish_state(false);
    }
  }
}

void BLEConnectionManager::handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      handle_connection_opened(param);
      break;
      
    case ESP_GATTC_SEARCH_RES_EVT:
      handle_service_discovered(param);
      break;
      
    case ESP_GATTC_SEARCH_CMPL_EVT:
      handle_service_discovery_complete(gattc_if);
      break;
    
    case ESP_GATTC_WRITE_DESCR_EVT: {
      auto *write_descr = &param->write;
      if (write_descr->status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "✓ CCCD descriptor write successful - notifications enabled on server");
      } else {
        ESP_LOGW(TAG, "✗ CCCD descriptor write failed: status=%d", write_descr->status);
      }
      break;
    }
    
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      auto *reg_notify = &param->reg_for_notify;
      if (reg_notify->status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "✓ Notification registration confirmed (handle 0x%04x)", reg_notify->handle);
      } else {
        ESP_LOGW(TAG, "✗ Notification registration failed: status=%d", reg_notify->status);
      }
      break;
    }
    
    case ESP_GATTC_NOTIFY_EVT:
      handle_notification(param);
      break;
    
    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "Disconnected (reason: 0x%02x)", param->disconnect.reason);
      scheduler_sequence_++;  // Invalidate any pending scheduler callbacks
      if (disconnection_callback_) {
        disconnection_callback_();
      }
      discovery_retry_count_ = 0;
      break;
    
    default:
      break;
  }
}

void BLEConnectionManager::handle_gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      handle_auth_complete(param);
      break;
      
    case ESP_GAP_BLE_SEC_REQ_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGI(TAG, "BLE security request from device %s - accepting", addr_str);
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    }
      
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.key_notif.bd_addr[0], param->ble_security.key_notif.bd_addr[1],
              param->ble_security.key_notif.bd_addr[2], param->ble_security.key_notif.bd_addr[3],
              param->ble_security.key_notif.bd_addr[4], param->ble_security.key_notif.bd_addr[5]);
      ESP_LOGI(TAG, "BLE passkey notification from %s: %06d", addr_str, param->ble_security.key_notif.passkey);
      ESP_LOGI(TAG, "  Note: Using 'Just Works' pairing - passkey is for display only");
      break;
    }
      
    case ESP_GAP_BLE_KEY_EVT:
      ESP_LOGD(TAG, "BLE key event (key exchange in progress)");
      ESP_LOGD(TAG, "  Key type: 0x%02x", param->ble_security.ble_key.key_type);
      break;
      
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      if (param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "BLE bond removed successfully");
      } else {
        ESP_LOGW(TAG, "BLE bond removal failed: status=%d", param->remove_bond_dev_cmpl.status);
      }
      break;
      
    case ESP_GAP_BLE_NC_REQ_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGI(TAG, "BLE numeric comparison request from %s", addr_str);
      ESP_LOGI(TAG, "  Auto-accepting (Just Works mode)");
      esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
      break;
    }
      
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
      char addr_str[18];
      sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      ESP_LOGW(TAG, "BLE passkey entry request from %s - unexpected in Just Works mode!", addr_str);
      break;
    }
      
    default:
      // Don't log all GAP events to reduce noise
      break;
  }
}

bool BLEConnectionManager::check_is_bonded(const esp_bd_addr_t bda) {
  int bond_count = esp_ble_get_bond_device_num();
  if (bond_count <= 0) {
    return false;
  }

  std::vector<esp_ble_bond_dev_t> bond_list(static_cast<size_t>(bond_count));
  esp_err_t err = esp_ble_get_bond_device_list(&bond_count, bond_list.data());
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read bond device list: 0x%x — assuming unbonded", err);
    return false;
  }

  // Clamp iteration to the count actually written by the API call.
  const int count = std::min(bond_count, static_cast<int>(bond_list.size()));
  for (int i = 0; i < count; i++) {
    if (memcmp(bond_list[i].bd_addr, bda, sizeof(esp_bd_addr_t)) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
