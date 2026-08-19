#include "ble_connection_manager.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cinttypes>
#include <cstdio>
// memcpy/memcmp. This was missing: the file compiled anyway because the
// ESP-IDF toolchain's headers pull <cstring> in transitively, and so does
// libc++. libstdc++ does not, so host-compiling this file for the first time
// (issue #174 audit tail) turned it into a hard error rather than luck.
#include <cstring>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace core {

// pairing_stall.h names the disconnect reasons that mean "the link was lost"
// as raw numbers, so that header can stay free of ESP-IDF and be compiled on
// its own. This is where the real enumerators are in scope, so this is where
// the numbers are pinned: a value change in ESP-IDF fails the build here rather
// than silently turning the exclusion list into a list of codes that no longer
// occur -- and an exclusion that stops matching is a false pairing report, not
// a missing one.
static_assert(ESP_GATT_CONN_L2C_FAILURE == 0x0001, "reason value drifted");
static_assert(ESP_GATT_CONN_TIMEOUT == 0x0008, "reason value drifted");
static_assert(ESP_GATT_CONN_TERMINATE_PEER_USER == 0x0013, "reason value drifted");
static_assert(ESP_GATT_CONN_TERMINATE_LOCAL_HOST == 0x0016, "reason value drifted");
static_assert(ESP_GATT_CONN_LMP_TIMEOUT == 0x0022, "reason value drifted");
static_assert(ESP_GATT_CONN_FAIL_ESTABLISH == 0x003E, "reason value drifted");
static_assert(ESP_GATT_CONN_CONN_CANCEL == 0x0100, "reason value drifted");

static const char *TAG = "alpha_hwr.ble";

// Expected product identification bytes in the FE5D service data.
// Reference: Python client._scan_advertisement_data() service_data[3]/[4].
static const uint8_t EXPECTED_PRODUCT_FAMILY = 0x34;  // ALPHA family
static const uint8_t EXPECTED_PRODUCT_TYPE = 0x07;    // HWR type

// ─── Advertisement-info caching (from scan-time service data) ────────────────
//
// The pump advertises via Service Data (AD type 0x16) with UUID 0xFE5D.
// ESPHome strips the 2-byte UUID before invoking the
// on_ble_service_data_advertise trigger, so the payload layout is:
//   service_data[0..2]: 3-byte frame header  (Python service_data[0..2])
//   service_data[3]:    product_family       (Python service_data[3])  0x34 = ALPHA
//   service_data[4]:    product_type         (Python service_data[4])  0x07 = HWR
//   service_data[5]:    product_version      (Python service_data[5])  firmware discriminator

void BLEConnectionManager::cache_adv_info_from_service_data(const std::vector<uint8_t> &service_data) {
  if (adv_info_.valid) return;  // Only capture once per session

  if (service_data.size() < 6) {
    ESP_LOGD(TAG, "Service data too short (%zu bytes), skipping adv cache", service_data.size());
    return;
  }

  // Validate the product bytes before committing to the one-shot cache, so a
  // malformed payload or a different Grundfos product cannot poison it.
  if (service_data[3] != EXPECTED_PRODUCT_FAMILY || service_data[4] != EXPECTED_PRODUCT_TYPE) {
    ESP_LOGD(TAG, "FE5D service data is not an ALPHA HWR advertisement "
                  "(family=0x%02X type=0x%02X), skipping adv cache",
             service_data[3], service_data[4]);
    return;
  }

  adv_info_.product_family  = service_data[3];
  adv_info_.product_type    = service_data[4];
  adv_info_.product_version = service_data[5];
  adv_info_.valid = true;

  char hex_char[3];
  for (auto b : service_data) {
    snprintf(hex_char, sizeof(hex_char), "%02X", b);
    adv_info_.adv_hex += hex_char;
  }

  ESP_LOGI(TAG, "Pump advertisement: family=0x%02X type=0x%02X version=0x%02X",
           adv_info_.product_family, adv_info_.product_type, adv_info_.product_version);
  ESP_LOGD(TAG, "  Service data: %s", adv_info_.adv_hex.c_str());

  if (advertisement_callback_) advertisement_callback_(adv_info_);
}

// ─── Device validation (discovery / on_ble_advertise mode) ──────────────────

// The pump advertises using Service Data (AD type 0x16) with UUID 0xFE5D,
// accessible in ESPHome via get_service_datas().  The layout (after UUID):
//   bytes 0-2: 3-byte frame header
//   byte  3:   product_family  (0x34 = ALPHA) → matches Python service_data[3]
//   byte  4:   product_type    (0x07 = HWR)   → matches Python service_data[4]
//   byte  5:   product_version                → matches Python service_data[5]
//
// The Python reference implementation matches on service data only; a
// service-UUID match is kept as a secondary catch-all for firmware variants
// that omit the product bytes.
bool BLEConnectionManager::is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device,
                                                uint16_t company_id,
                                                uint8_t product_family,
                                                uint8_t product_type,
                                                const esp32_ble_tracker::ESPBTUUID &service_uuid) {
  // Primary: Service Data (0x16) — this is what the pump actually sends.
  for (const auto &svc_data : device.get_service_datas()) {
    esp_bt_uuid_t uuid = svc_data.uuid.get_uuid();
    if (uuid.len == ESP_UUID_LEN_16 && uuid.uuid.uuid16 == company_id) {
      const auto &d = svc_data.data;
      // Need at least 6 bytes (3 header + family + type + version)
      if (d.size() >= 6 && d[3] == product_family && d[4] == product_type) {
        ESP_LOGI(TAG, "Found ALPHA HWR via service data (0x16), version=0x%02X", d[5]);
        return true;
      }
      // Log if UUID matched but product bytes don't — helps diagnose variants.
      if (d.size() >= 5) {
        ESP_LOGD(TAG, "Grundfos service data matched UUID but wrong product: "
                      "family=0x%02X type=0x%02X (expected 0x%02X/0x%02X)",
                 d.size() > 3 ? d[3] : 0, d.size() > 4 ? d[4] : 0,
                 product_family, product_type);
      }
    }
  }

  // Secondary: Service UUID match — catches devices that don't include product bytes.
  for (const auto &svc_uuid : device.get_service_uuids()) {
    if (svc_uuid == service_uuid) {
      ESP_LOGI(TAG, "Found ALPHA HWR via service UUID (secondary match)");
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

SubscribeOutcome BLEConnectionManager::attempt_subscribe_() {
  if (!client_) {
    ESP_LOGW(TAG, "BLE client not available");
    return SubscribeOutcome::NO_CLIENT;
  }

  auto *service = client_->get_service(service_uuid_);
  if (!service) {
    ESP_LOGW(TAG, "Service not found for notification subscription");
    return SubscribeOutcome::NO_SERVICE;
  }

  auto *chr = client_->get_characteristic(service->uuid, characteristic_uuid_);
  if (!chr) {
    ESP_LOGW(TAG, "Characteristic not found");
    return SubscribeOutcome::NO_CHARACTERISTIC;
  }

  // Register for notifications (tells ESP-IDF we want to receive them)
  auto status = esp_ble_gattc_register_for_notify(client_->get_gattc_if(),
                                                   client_->get_remote_bda(),
                                                   chr->handle);
  if (status) {
    ESP_LOGW(TAG, "Failed to register for notifications: %d", status);
    return SubscribeOutcome::REGISTER_FAILED;
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
    return SubscribeOutcome::CCCD_WRITE_FAILED;
  }

  ESP_LOGI(TAG, "CCCD write successful - notifications should now be enabled");
  return SubscribeOutcome::OK;
}

void BLEConnectionManager::report_subscribe_outcome_(SubscribeOutcome outcome) {
  if (!subscribe_failed(outcome))
    return;

  // Name the cause at the moment it happens. Before issue #175 all five
  // failures were logged and dropped, so the operator learned only what the
  // watchdog says 60 s later -- "No data from pump", the symptom every one of
  // them shares.
  //
  // The hold is what makes a blocking failure's reason survive the reconnect
  // that follows. A CCCD write failure does not take it: one of its documented
  // synchronous causes is the link already being gone, and holding over that
  // would relabel a link loss as a subscribe fault for the whole episode --
  // so it arrives at rank NONE and writes only when nothing is held.
  //
  // Nor does either kind outrank an auth failure. A subscribe that fails on a
  // link whose pairing just failed is that failure's consequence, and the
  // pairing reason is the one an operator can act on.
  const FailureHold rank = subscribe_outcome_holds_fault(outcome)
                               ? FailureHold::SUBSCRIBE
                               : FailureHold::NONE;
  if (failure_hold_admits(failure_hold_, rank)) {
    last_failure_ = subscribe_outcome_to_string(outcome);
    failure_hold_ = rank;
  }

  ESP_LOGW(TAG, "Notification subscribe failed: %s",
           subscribe_outcome_to_string(outcome));
}

void BLEConnectionManager::subscribe_to_notifications() {
  const SubscribeOutcome outcome = attempt_subscribe_();
  report_subscribe_outcome_(outcome);

  // Reached on OK and on CCCD_WRITE_FAILED alike, and on nothing else: the four
  // blocking outcomes return before this point and leave the session short of
  // READY, which is what the data watchdog is for.
  //
  // The failed CCCD write is deliberately not treated as fatal. The pump may be
  // bonded, and a bonded peer retains its CCCD across reconnections, so a link
  // whose CCCD write could not be issued may already be subscribed from an
  // earlier session. The dominant synchronous cause is ATT congestion, which is
  // transient. Recycling on either prediction would tear down links that work.
  // See subscribe_outcome.h.
  if (subscribe_outcome_blocks_session(outcome))
    return;

  if (subscribed_callback_) {
    subscribed_callback_();
  }
}

void BLEConnectionManager::force_disconnect(const char *reason, FailureHold rank) {
  ESP_LOGW(TAG, "Forcing BLE disconnect: %s", reason);
  // Latch the real cause before tearing the link down. The DISCONNECT event we
  // are about to provoke would otherwise overwrite it with "Local Host
  // Terminated", which is true but says nothing; holding it keeps the actual
  // reason readable through the reconnect loop that follows. Cleared by
  // handle_notification() the moment data flows again.
  // ...unless a subscribe step or a pairing failure already named the actual
  // cause. The watchdog reaches here 60 s after such a failure with "No data
  // from pump", which is that failure's symptom, and overwriting would put the
  // operator back on the generic string issue #175 exists to replace. Both
  // outrank DATA, so one rank comparison covers them (failure_hold.h) — an
  // earlier `!= SUBSCRIBE` here let the same overwrite through on AUTH.
  if (failure_hold_admits(failure_hold_, rank)) {
    last_failure_ = reason;
    failure_hold_ = rank;
  }
  // This teardown is ours, so the cycle it ends is not evidence about the
  // pump's willingness to pair. Without this, the watchdog recycling an
  // unbonded link that subscribed and went quiet would read as a pairing
  // refusal after three recycles -- and would replace the reason latched two
  // lines up, which is the true one, because the pairing fault outranks it.
  pairing_stall_.note_local_teardown();
  if (client_ != nullptr) {
    client_->disconnect();
  }
}

void BLEConnectionManager::release_pairing_stall_hold_() {
  // The stall is the one held reason that is an inference from an absence, so
  // it is the one that can be refuted outright -- and when it is, it has to
  // come off the surface rather than sit there until something of equal or
  // higher rank happens to be written. Nothing may: with enable_pairing false,
  // which is the default, AUTH_CMPL never fires at all, and a link that never
  // reaches READY produces no release either. Before this, a pump put into
  // pairing mode and visibly sending SEC_REQ went on being reported as one that
  // would not pair.
  if (failure_hold_ == FailureHold::PAIRING_STALL && !pairing_stall_.stalled()) {
    ESP_LOGD(TAG, "Pairing stall cleared - withdrawing held reason: %s", last_failure_.c_str());
    // The string is cleared, not merely unheld, and that is the difference
    // between this release and the others. Dropping the rank alone lets the
    // next reason overwrite it -- but on a link that keeps failing there may not
    // be a next reason for a long time, and evaluate_link_status() publishes
    // whatever is in here whenever the session is not ready. The diagnosis has
    // been refuted, not superseded, so the honest reading is that no cause is
    // currently known. Safe to clear unconditionally because the guard above
    // establishes that the stall is what wrote it.
    last_failure_.clear();
    failure_hold_ = FailureHold::NONE;
  }
}

void BLEConnectionManager::on_pump_ready() {
  if (failure_hold_released_by_pump_ready(failure_hold_)) {
    ESP_LOGD(TAG, "Pump ready - releasing held reason: %s", last_failure_.c_str());
    // Cleared, not merely unheld, for the reason the pairing-stall release
    // clears it: the string is published whenever the pump is not ready, and a
    // reason that has been refuted should read "None" rather than wait for some
    // later fault to overwrite it.
    last_failure_.clear();
    failure_hold_ = FailureHold::NONE;
  }
}

void BLEConnectionManager::on_session_ready() {
  if (failure_hold_released_by_session_ready(failure_hold_)) {
    ESP_LOGD(TAG, "Session ready - withdrawing held failure reason: %s",
             last_failure_.c_str());
    // Cleared, not merely unheld. This used to leave the string in place on the
    // reasoning that nothing displayed it past session-ready -- which stopped
    // being true when the fault surface moved to gating on the PUMP being ready
    // (issue #211). An unheld string that is still on display is the worst of
    // both: visible, and overwritable by the lowest-ranked reason that comes
    // along, which is the #175 defect this file exists to prevent.
    last_failure_.clear();
    failure_hold_ = FailureHold::NONE;
  }
}

void BLEConnectionManager::handle_connection_opened(const esp_ble_gattc_cb_param_t *param) {
  (void) param;  // status was already checked by the caller
  ESP_LOGI(TAG, "BLE connection opened. Pairing enabled: %s", pairing_enabled_ ? "YES" : "NO");
  
  // Pump Link Status: capture the bond state for this connection once, before the
  // component callback (which reads it via was_bonded_at_open()).
  bonded_at_open_ = check_is_bonded(client_->get_remote_bda());

  // Notify component of connection
  if (connection_callback_) {
    connection_callback_();
  }
  
  // Reset discovery retry counter and per-connection encryption state
  discovery_retry_count_ = 0;
  encryption_pending_ = false;
  subscription_deferred_ = false;
  
  // Track this connection for the pairing-stall detector before anything is
  // decided about it: the cycle is opened here and closed on DISCONNECT, and a
  // bonded open is not a candidate at all. See pairing_stall.h.
  pairing_stall_.on_connection_opened(bonded_at_open_);
  release_pairing_stall_hold_();

  // Request encryption only when we already have a stored bond.
  // - Bonded:   request encryption immediately so the pump can resume the
  //             encrypted session before GATT discovery proceeds.
  // - Unbonded: stay silent — this pump initiates bonding itself via
  //             ESP_GAP_BLE_SEC_REQ_EVT.  A central-initiated pairing
  //             request on an unbonded pump returns 0x52 ("Pairing Not
  //             Supported"), causing the pump's own SEC_REQ to be missed.
  //
  // Both halves of that are right, but "unbonded" covers two states that
  // behave nothing alike, and only the first is described above (issue #230):
  //
  //   A. Neither side is bonded and the pump is in pairing mode. It sends
  //      SEC_REQ, the silent wait is answered, and the node bonds. This is the
  //      first-time setup flow.
  //   B. The pump is bonded to us and we are not bonded to it -- after
  //      `ble_client.remove_bond`, an NVS erase, or a re-flash that lost NVS.
  //      The pump sees an unencrypted peer it holds a bond for, sends NO
  //      SEC_REQ, and terminates the link. Staying silent is still the right
  //      move (initiating returns the 0x52 above; that was tried), but nothing
  //      here can end it: the pump has to be put into Bluetooth pairing mode by
  //      hand. Without a diagnosis the node loops on this every ~5 s forever,
  //      saying "waiting for pump to initiate pairing" each time -- which reads
  //      as though patience is the answer.
  //
  // So the wait below is bounded by a report rather than by an action:
  // pairing_stall_ counts the cycles and the DISCONNECT handler names the state
  // once it is a pattern.
  if (pairing_enabled_) {
    if (bonded_at_open_) {  // reuse the check_is_bonded() result captured above
      ESP_LOGI(TAG, "Device is bonded - requesting encryption to resume secure session");
      esp_err_t ret = esp_ble_set_encryption(client_->get_remote_bda(), ESP_BLE_SEC_ENCRYPT);
      if (ret != ESP_OK) {
        ESP_LOGW(TAG, "✗ Failed to request encryption: 0x%x", ret);
      } else {
        // SMP negotiation is now in flight; hold off the CCCD write until
        // ESP_GAP_BLE_AUTH_CMPL_EVT so the unencrypted write cannot race it
        // (issue #12).
        encryption_pending_ = true;
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
        if (encryption_pending_) {
          // Bonded reconnect with SMP negotiation still in flight: defer the
          // CCCD write until AUTH_CMPL success (issue #12).  On slower chips
          // auth completes long before discovery and this path never runs.
          ESP_LOGI(TAG, "Encryption still negotiating - deferring notification "
                        "subscription until auth completes");
          subscription_deferred_ = true;
        } else {
          subscribe_to_notifications();
        }
      } else {
        char uuid_buf[esphome::esp32_ble::UUID_STR_LEN];
        ESP_LOGW(TAG, "Characteristic NOT found: %s", characteristic_uuid_.to_str(uuid_buf));
        // This, not attempt_subscribe_(), is where a missing characteristic
        // actually surfaces -- the checks above run the same lookups, so the
        // outcome of the same name inside attempt_subscribe_() is unreachable
        // from here. Reporting only there would have put the whole benefit of
        // issue #175 on a dead branch.
        report_subscribe_outcome_(SubscribeOutcome::NO_CHARACTERISTIC);
      }
    } else {
      // Service NOT found - implement retry logic
      ESP_LOGW(TAG, "✗ Service NOT found!");
      
      if (discovery_retry_count_ < MAX_DISCOVERY_RETRIES) {
        discovery_retry_count_++;
        ESP_LOGW(TAG, "Retrying service discovery in %" PRIu32 "ms (attempt %d/%d)...",
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
        // Retries exhausted: this is where a missing service really ends up.
        report_subscribe_outcome_(SubscribeOutcome::NO_SERVICE);
      }
    }
  } else {
    report_subscribe_outcome_(SubscribeOutcome::NO_CLIENT);
  }
}

void BLEConnectionManager::handle_notification(const esp_ble_gattc_cb_param_t *param) {
  auto *notify_evt = &param->notify;
  if (notify_evt->value_len > 0) {
    ESP_LOGV(TAG, "Received notification, %d bytes", notify_evt->value_len);
    // Inbound data refutes a held "no data from pump" reason by construction,
    // so release that hold here rather than waiting for the AUTH_CMPL clear
    // below — with pairing disabled (the default) AUTH_CMPL never fires at all,
    // so a watchdog hold would otherwise never be released. Scoped to the
    // watchdog's own hold: see failure_hold.h for why an auth-failure hold must
    // survive notifications.
    if (failure_hold_released_by_data(failure_hold_))
      failure_hold_ = FailureHold::NONE;
    // A link carrying data is not a link the pump refused to pair with. This is
    // what keeps the stall detector quiet on a healthy unbonded node, which is
    // a supported configuration -- enable_pairing defaults to false and passive
    // telemetry needs no bond (pairing_stall.h).
    pairing_stall_.note_data();
    release_pairing_stall_hold_();
    if (notification_callback_) {
      notification_callback_(notify_evt->value, notify_evt->value_len);
    }
  }
}

void BLEConnectionManager::handle_auth_complete(const esp_ble_gap_cb_param_t *param) {
  auto &auth_cmpl = param->ble_security.auth_cmpl;
  // Not our pump: every branch below writes state that describes *this* link --
  // the pairing sensor, the failure hold, encryption_pending_, the deferred CCCD
  // write. Acting on a stranger's AUTH_CMPL corrupts all four. The failure branch
  // is the worst of them: it latches the stranger's reason at AUTH rank, which
  // outranks every other hold, and then disconnects the pump.
  if (!gap_addr_is_pump_(auth_cmpl.bd_addr)) {
    // Logged at DEBUG, with both addresses, rather than dropped silently. This
    // is the one gate whose false negative is fatal: if the pump ever stopped
    // matching (a firmware change, a pump variant, an address-type quirk) the
    // symptom is a link that opens, never subscribes and parks in SUBSCRIBING
    // until the watchdog recycles it -- with nothing in the log to say why. The
    // two addresses side by side name the cause immediately.
    // "no address" is rendered as <unset> rather than 00:00:00:00:00:00, which
    // reads as a real address and would send a reader hunting for the device
    // that owns it. The all-zero case is not hypothetical -- it is exactly what
    // gap_addr_matches() treats as "no peer configured".
    const uint8_t *ours = client_ != nullptr ? client_->get_remote_bda() : nullptr;
    char ours_str[18];
    if (!core::gap_addr_is_set(ours)) {
      snprintf(ours_str, sizeof(ours_str), "<unset>");
    } else {
      snprintf(ours_str, sizeof(ours_str), "%02X:%02X:%02X:%02X:%02X:%02X",
               ours[0], ours[1], ours[2], ours[3], ours[4], ours[5]);
    }
    ESP_LOGD(TAG, "Ignoring AUTH_CMPL from %02X:%02X:%02X:%02X:%02X:%02X (pump is %s)",
             auth_cmpl.bd_addr[0], auth_cmpl.bd_addr[1], auth_cmpl.bd_addr[2],
             auth_cmpl.bd_addr[3], auth_cmpl.bd_addr[4], auth_cmpl.bd_addr[5], ours_str);
    return;
  }
  char addr_str[18];
  snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
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
    encryption_pending_ = false;
    // Recovery: a successful (re-)auth clears any held significant-failure reason,
    // so the fault sensor stops showing the old cause once the link is healthy.
    // This covers the hold set on an auth failure, which erases the bond, so
    // recovery must pass back through a fresh AUTH_CMPL success here to reach
    // READY. The watchdog's hold is released in handle_notification() instead.
    //
    // Clears an AUTH hold only. A DATA hold must survive this: reaching a
    // successful AUTH_CMPL does not prove the pump is answering — that is the
    // very defect the watchdog exists for — so releasing it here would drop the
    // deaf-link reason on a link that is still deaf. It is released by inbound
    // data instead, which is the evidence that actually refutes it.
    if (failure_hold_released_by_auth(failure_hold_))
      failure_hold_ = FailureHold::NONE;
    // The bond exists again: whatever the stall detector was counting is over.
    pairing_stall_.note_bond_established();
    release_pairing_stall_hold_();
    if (subscription_deferred_) {
      // Service discovery finished while SMP was negotiating; the link is now
      // encrypted, so the held-back CCCD write is safe to send (issue #12).
      subscription_deferred_ = false;
      ESP_LOGI(TAG, "Auth complete - performing deferred notification subscription");
      subscribe_to_notifications();
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
    {
      // Latch the failure reason for the Pump Link Status companion, and mark it
      // significant so the routine disconnects of the ensuing (possibly unbonded)
      // reconnect loop don't overwrite the real cause before recovery.
      char afbuf[64];
      snprintf(afbuf, sizeof(afbuf), "%s (0x%02x)", fail_reason, auth_cmpl.fail_reason);
      // Ranks above every other hold, so this admits unconditionally today --
      // through the same call as every other write site, so that stays true of
      // the rank rather than of a comment. Leaving a DATA origin in place would
      // let the next notification erase this reason, and an unbonded pump keeps
      // delivering notifications after a failed SMP.
      if (failure_hold_admits(failure_hold_, FailureHold::AUTH)) {
        last_failure_ = afbuf;
        failure_hold_ = FailureHold::AUTH;
      }
    }
    // Bonded reconnect whose encryption failed: the link may stay up in an
    // unauthenticated state, and a later discovery-complete would then send
    // the CCCD write unencrypted — the exact race this deferral exists to
    // prevent.  Tear the connection down instead; the normal reconnect path
    // (including any reconnect settle window) takes over from there.
    bool disconnect_needed = encryption_pending_;
    encryption_pending_ = false;
    subscription_deferred_ = false;
    if (disconnect_needed && client_ != nullptr) {
      ESP_LOGW(TAG, "Disconnecting - no GATT writes allowed on an unauthenticated bonded link");
      client_->disconnect();
    }
    if (pairing_status_sensor_ != nullptr) {
      pairing_status_sensor_->publish_state(false);
    }
  }
}

void BLEConnectionManager::handle_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                               esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      // ESP_GATTC_OPEN_EVT fires for both successful and failed opens. Only a
      // successful open is a real connection. The base layer (esp32_ble_client)
      // treats BOTH ESP_GATT_OK and ESP_GATT_ALREADY_OPEN as success, so mirror
      // that here — treating ALREADY_OPEN as a failure would instead leave the
      // component wedged (no encryption request, no session transition) until
      // the next disconnect. Any other status is a failed open: do not run the
      // connection-opened handler on one — otherwise, with the pump powered down
      // or out of range, the reconnect loop's stream of failed opens drives
      // phantom IDLE->SERVICE_DISCOVERY transitions. The base layer owns
      // failure/retry handling.
      if (param->open.status == ESP_GATT_OK ||
          param->open.status == ESP_GATT_ALREADY_OPEN) {
        handle_connection_opened(param);
      } else {
        ESP_LOGD(TAG, "Ignoring failed BLE open (status 0x%02x)", param->open.status);
      }
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
    
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected (reason: 0x%02x)", param->disconnect.reason);
      // Close the pairing-stall cycle first, so that when the pump has been
      // refusing to pair the reason below cannot claim the fault surface. What
      // it would put there is "Failed To Establish (0x3e)", which points at
      // radio trouble -- and the radio is fine here. The connections succeed;
      // it is the security state that is stuck (issue #230).
      // The reason matters to the detector, not only to the string below it: a
      // link the radio dropped is not the pump refusing anything, and counting
      // one would swap a correct radio diagnostic for a pairing misdiagnosis.
      const bool report_stall =
          pairing_stall_.on_disconnected(static_cast<uint16_t>(param->disconnect.reason));
      if (pairing_stall_.stalled()) {
        // Its own rank, below both SUBSCRIBE and AUTH, because those record
        // something that was seen happening and this records three connections
        // on which nothing did. The full argument is in failure_hold.h; the
        // short version is that an observed event outranks an inference from an
        // absence, and the first version of this change got it wrong by putting
        // the stall at AUTH rank, where it replaced issue #14's bond-erasing
        // 0x61 about fifteen seconds after that failure had manufactured the
        // very conditions the stall detects.
        if (failure_hold_admits(failure_hold_, FailureHold::PAIRING_STALL)) {
          last_failure_ = "Pump not accepting pairing";
          failure_hold_ = FailureHold::PAIRING_STALL;
        }
      }
      if (report_stall) {
        ESP_LOGW(TAG, "Pump has not offered to pair across %u connections",
                 (unsigned) pairing_stall_.consecutive_cycles());
        ESP_LOGW(TAG, "  Either it is bonded to a client that is no longer bonded to it "
                      "(a cleared bond, an NVS erase, a re-flash), or it is not in pairing mode");
        ESP_LOGW(TAG, "  This node cannot recover either case: initiating from here returns "
                      "0x52 and loses the pump's own request");
        ESP_LOGW(TAG, "  Put the pump into Bluetooth pairing mode, at the pump");
        // Not one button press: the front panel auto-locks and is unlocked from
        // the Grundfos GO app, and the button commonly needs several presses
        // before it takes. A log line cannot carry a seven-step procedure, so it
        // carries the pointer.
        ESP_LOGW(TAG, "  It takes more than a button press: see docs/configuration.md, "
                      "enable_pairing");
        if (!pairing_enabled_) {
          // Not "this node would decline": it cannot. ESPHome's own
          // BLEClientBase::gap_event_handler() answers SEC_REQ with `true` for
          // its configured peer unconditionally -- which is exactly what the
          // DECLINE branch further down this file already says. What
          // enable_pairing actually governs is init_security(), which returns
          // early and configures no IO capability, no bonding requirement and no
          // key distribution, so the offer is consented to and no bond is set
          // up. Saying otherwise would send an operator who has just been told
          // to walk to the pump away believing it would not help.
          ESP_LOGW(TAG, "  enable_pairing is false, so this node configures no bonding; set it "
                        "true before re-pairing, or the pump's offer goes nowhere");
        }
      }
      // Latch a human-readable failure reason for the Pump Link Status companion
      // (set before the callback so the component can read it on the same event).
      // Skip while any hold is in place (an auth/encryption failure, or the
      // watchdog's deaf-link reason), so the routine disconnects of the ensuing
      // reconnect loop don't overwrite the real cause before recovery.
      if (failure_hold_admits(failure_hold_, FailureHold::NONE)) {
        const char *rname;
        switch (param->disconnect.reason) {
          case ESP_GATT_CONN_L2C_FAILURE:          rname = "L2CAP Failure"; break;
          case ESP_GATT_CONN_TIMEOUT:              rname = "Connection Timeout"; break;
          case ESP_GATT_CONN_TERMINATE_PEER_USER:  rname = "Remote Terminated"; break;
          case ESP_GATT_CONN_TERMINATE_LOCAL_HOST: rname = "Local Host Terminated"; break;
          case ESP_GATT_CONN_LMP_TIMEOUT:          rname = "LL Response Timeout"; break;
          case ESP_GATT_CONN_FAIL_ESTABLISH:       rname = "Failed To Establish"; break;
          case ESP_GATT_CONN_CONN_CANCEL:          rname = "Connection Cancelled"; break;
          // ESP_GATT_CONN_UNKNOWN (0) and ESP_GATT_CONN_NONE (0x0101) are
          // "no known reason" sentinels; let them fall to the default below.
          default:                                 rname = "Disconnected"; break;
        }
        char fbuf[48];
        snprintf(fbuf, sizeof(fbuf), "%s (0x%02x)", rname, param->disconnect.reason);
        last_failure_ = fbuf;
      }
      scheduler_sequence_++;  // Invalidate any pending scheduler callbacks
      if (disconnection_callback_) {
        disconnection_callback_();
      }
      discovery_retry_count_ = 0;
      encryption_pending_ = false;
      subscription_deferred_ = false;
      break;
    }
    
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
      snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      const bool sec_req_from_pump = gap_addr_is_pump_(param->ble_security.ble_req.bd_addr);
      if (sec_req_from_pump) {
        // The pump is willing to pair. That is the single thing a stalled pump
        // never says, so it ends the stall count whatever enable_pairing
        // decides below -- consent is this node's question, willingness is the
        // pump's (pairing_stall.h).
        pairing_stall_.note_security_request();
        release_pairing_stall_hold_();
      }
      switch (core::gap_security_action(sec_req_from_pump, pairing_enabled_)) {
        case core::GapSecurityAction::ACCEPT:
          ESP_LOGI(TAG, "BLE security request from pump %s - accepting", addr_str);
          esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
          break;
        case core::GapSecurityAction::DECLINE:
          // No reply, rather than a reply of `false`. ESPHome's own
          // BLEClientBase::gap_event_handler() has already answered this event
          // with `true` for its configured peer -- unconditionally, with no
          // enable_pairing notion of its own -- so a contradicting `false` from
          // here would arrive second against an SMP exchange already underway.
          // enable_pairing therefore governs what this component does (security
          // params, encryption requests, and the consent above); it cannot stop
          // the ble_client itself from consenting. Say so rather than imply a
          // guarantee that is not ours to make.
          ESP_LOGW(TAG, "BLE security request from pump %s while enable_pairing is false", addr_str);
          ESP_LOGW(TAG, "  Not consenting here; note ble_client accepts it independently");
          break;
        case core::GapSecurityAction::IGNORE:
          ESP_LOGD(TAG, "Ignoring BLE security request from %s (not our pump)", addr_str);
          break;
      }
      break;
    }

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
      // Log-only, but still address-filtered: this fires for every peer on the
      // node, and reporting a stranger's passkey as the pump's is misleading.
      if (!gap_addr_is_pump_(param->ble_security.key_notif.bd_addr)) {
        break;
      }
      char addr_str[18];
      snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.key_notif.bd_addr[0], param->ble_security.key_notif.bd_addr[1],
              param->ble_security.key_notif.bd_addr[2], param->ble_security.key_notif.bd_addr[3],
              param->ble_security.key_notif.bd_addr[4], param->ble_security.key_notif.bd_addr[5]);
      ESP_LOGI(TAG, "BLE passkey notification from %s: %06" PRIu32, addr_str, param->ble_security.key_notif.passkey);
      ESP_LOGI(TAG, "  Note: Using 'Just Works' pairing - passkey is for display only");
      break;
    }
      
    case ESP_GAP_BLE_KEY_EVT:
      // Log-only, address-filtered on the same rationale as the passkey events:
      // key exchange with somebody else's peer is not this component's business
      // to narrate.
      if (!gap_addr_is_pump_(param->ble_security.ble_key.bd_addr)) {
        break;
      }
      ESP_LOGD(TAG, "BLE key event (key exchange in progress)");
      ESP_LOGD(TAG, "  Key type: 0x%02x", param->ble_security.ble_key.key_type);
      break;

    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
      // This component never removes a bond, so every one of these events is
      // somebody else's -- a bluetooth_proxy unpair, most likely. Unfiltered, it
      // reported "BLE bond removed successfully" about the pump when the pump's
      // bond was untouched, which is the most misleading line in this switch.
      if (!gap_addr_is_pump_(param->remove_bond_dev_cmpl.bd_addr)) {
        break;
      }
      if (param->remove_bond_dev_cmpl.status == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "BLE bond removed successfully");
      } else {
        ESP_LOGW(TAG, "BLE bond removal failed: status=%d", param->remove_bond_dev_cmpl.status);
      }
      break;
      
    case ESP_GAP_BLE_NC_REQ_EVT: {
      char addr_str[18];
      snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
              param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
              param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
              param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
      // Unlike SEC_REQ, nothing in ESPHome answers this event *by default*, so
      // DECLINE is enforceable here rather than advisory. It is not quite "the
      // only responder": ESPHome ships an on_numeric_comparison_request trigger
      // and a ble_client.numeric_comparison_reply action, so a config that
      // wires those up on this same client would reply twice. Nothing in this
      // repo's packages does, and a user who adds one has asked for manual
      // control of exactly this event.
      switch (core::gap_security_action(gap_addr_is_pump_(param->ble_security.ble_req.bd_addr),
                                        pairing_enabled_)) {
        case core::GapSecurityAction::ACCEPT:
          ESP_LOGI(TAG, "BLE numeric comparison request from pump %s", addr_str);
          ESP_LOGI(TAG, "  Auto-accepting (Just Works mode)");
          esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, true);
          break;
        case core::GapSecurityAction::DECLINE:
          ESP_LOGW(TAG, "Rejecting numeric comparison from pump %s - enable_pairing is false", addr_str);
          esp_ble_confirm_reply(param->ble_security.ble_req.bd_addr, false);
          break;
        case core::GapSecurityAction::IGNORE:
          ESP_LOGD(TAG, "Ignoring numeric comparison request from %s (not our pump)", addr_str);
          break;
      }
      break;
    }
      
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
      // Log-only. Address-filtered for the same reason as PASSKEY_NOTIF: the
      // warning below claims Just Works was configured and something asked for
      // a passkey anyway, which is only true of a peer we configured.
      if (!gap_addr_is_pump_(param->ble_security.ble_req.bd_addr)) {
        break;
      }
      char addr_str[18];
      snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
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
