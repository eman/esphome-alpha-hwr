// Host-test stand-in for ESP-IDF's <esp_gattc_api.h>.
//
// Only the events, status codes and union members components/alpha_hwr reads.
// Anything the component never touches is absent on purpose: an unused field in
// a mock is a place for a future test to accidentally assert something the real
// stack does differently.
//
// The numeric values of the *status* enumerators are load-bearing only in that
// the code compares against these same names, so they are internally
// consistent. The disconnect reason codes carry their real BLE values because
// ble_connection_manager.cpp maps them to user-facing fault strings and logs
// them as hex, so a test reading `0x13` should see the same thing a pump does.

#pragma once

#include <cstdint>

#include "esp_bt_defs.h"

// ── Status ──────────────────────────────────────────────────────────────────
enum esp_gatt_status_t {
  ESP_GATT_OK = 0x00,
  ESP_GATT_ALREADY_OPEN = 0x09,
};

// ── Disconnect / connection-close reasons ───────────────────────────────────
// Real HCI values: these are logged as hex and mapped to fault strings.
enum esp_gatt_conn_reason_t {
  ESP_GATT_CONN_UNKNOWN = 0x00,
  ESP_GATT_CONN_L2C_FAILURE = 0x01,
  ESP_GATT_CONN_TIMEOUT = 0x08,
  ESP_GATT_CONN_TERMINATE_PEER_USER = 0x13,
  ESP_GATT_CONN_TERMINATE_LOCAL_HOST = 0x16,
  ESP_GATT_CONN_FAIL_ESTABLISH = 0x3E,
  ESP_GATT_CONN_LMP_TIMEOUT = 0x22,
  ESP_GATT_CONN_CONN_CANCEL = 0x0100,
  ESP_GATT_CONN_NONE = 0x0101,
};

enum esp_gatt_auth_req_t { ESP_GATT_AUTH_REQ_NONE = 0 };

enum esp_gatt_write_type_t {
  ESP_GATT_WRITE_TYPE_NO_RSP = 1,
  ESP_GATT_WRITE_TYPE_RSP = 2,
};

// ── UUIDs ───────────────────────────────────────────────────────────────────
enum { ESP_UUID_LEN_16 = 2, ESP_UUID_LEN_32 = 4, ESP_UUID_LEN_128 = 16 };

struct esp_bt_uuid_t {
  uint16_t len;
  union {
    uint16_t uuid16;
    uint32_t uuid32;
    uint8_t uuid128[16];
  } uuid;
};

struct esp_gatt_id_t {
  esp_bt_uuid_t uuid;
  uint8_t inst_id;
};

struct esp_gatt_srvc_id_t {
  esp_gatt_id_t id;
  bool is_primary;
  /// ble_connection_manager.cpp reads `srvc_id.uuid.len` directly rather than
  /// going through `id`, so the field it uses is the one provided here.
  esp_bt_uuid_t uuid;
};

// ── Events ──────────────────────────────────────────────────────────────────
enum esp_gattc_cb_event_t {
  ESP_GATTC_OPEN_EVT = 2,
  ESP_GATTC_DISCONNECT_EVT = 5,
  ESP_GATTC_SEARCH_RES_EVT = 6,
  ESP_GATTC_SEARCH_CMPL_EVT = 7,
  ESP_GATTC_NOTIFY_EVT = 10,
  ESP_GATTC_REG_FOR_NOTIFY_EVT = 38,
  ESP_GATTC_WRITE_DESCR_EVT = 40,
};

using esp_gatt_if_t = uint8_t;

// ── Callback parameter union ────────────────────────────────────────────────
// Only the members the component reads. `search_res.srvc_id` is a bare
// esp_bt_uuid_t holder here rather than the SDK's nested id/is_primary, because
// that is the shape the component uses (`search_res->srvc_id.uuid.len`).
union esp_ble_gattc_cb_param_t {
  struct gattc_open_evt_param {
    esp_gatt_status_t status;
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
    uint16_t mtu;
  } open;

  struct gattc_disconnect_evt_param {
    esp_gatt_conn_reason_t reason;
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
  } disconnect;

  struct gattc_search_res_evt_param {
    uint16_t conn_id;
    esp_gatt_srvc_id_t srvc_id;
    uint16_t start_handle;
    uint16_t end_handle;
  } search_res;

  struct gattc_search_cmpl_evt_param {
    esp_gatt_status_t status;
    uint16_t conn_id;
  } search_cmpl;

  struct gattc_notify_evt_param {
    uint16_t conn_id;
    esp_bd_addr_t remote_bda;
    uint16_t handle;
    uint16_t value_len;
    uint8_t *value;
    bool is_notify;
  } notify;

  struct gattc_reg_for_notify_evt_param {
    esp_gatt_status_t status;
    uint16_t handle;
  } reg_for_notify;

  struct gattc_write_evt_param {
    esp_gatt_status_t status;
    uint16_t conn_id;
    uint16_t handle;
  } write;
};

// ── Functions ───────────────────────────────────────────────────────────────
// Stubs. Tests that need to observe a call record it through the counters
// below rather than through side effects, because the real stack's side
// effects are asynchronous and a mock that faked them would be inventing
// behaviour.
struct EspGattcMockCalls {
  int close = 0;
  int write_char = 0;
  int search_service = 0;
  int register_for_notify = 0;
  int write_char_descr = 0;
  esp_err_t next_result = ESP_OK;

  void reset() { *this = EspGattcMockCalls(); }
};
inline EspGattcMockCalls &esp_gattc_mock() {
  static EspGattcMockCalls calls;
  return calls;
}

inline esp_err_t esp_ble_gattc_close(esp_gatt_if_t, uint16_t) {
  esp_gattc_mock().close++;
  return esp_gattc_mock().next_result;
}

inline esp_err_t esp_ble_gattc_write_char(esp_gatt_if_t, uint16_t, uint16_t, uint16_t,
                                          uint8_t *, esp_gatt_write_type_t,
                                          esp_gatt_auth_req_t) {
  esp_gattc_mock().write_char++;
  return esp_gattc_mock().next_result;
}

inline esp_err_t esp_ble_gattc_search_service(esp_gatt_if_t, uint16_t, esp_bt_uuid_t *) {
  esp_gattc_mock().search_service++;
  return esp_gattc_mock().next_result;
}

inline esp_err_t esp_ble_gattc_register_for_notify(esp_gatt_if_t, esp_bd_addr_t, uint16_t) {
  esp_gattc_mock().register_for_notify++;
  return esp_gattc_mock().next_result;
}

inline esp_err_t esp_ble_gattc_write_char_descr(esp_gatt_if_t, uint16_t, uint16_t, uint16_t,
                                                uint8_t *, esp_gatt_write_type_t,
                                                esp_gatt_auth_req_t) {
  esp_gattc_mock().write_char_descr++;
  return esp_gattc_mock().next_result;
}
