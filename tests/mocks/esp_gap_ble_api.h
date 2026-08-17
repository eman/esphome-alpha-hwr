// Host-test stand-in for ESP-IDF's <esp_gap_ble_api.h>.
//
// Same rule as the GATTC mock: only what components/alpha_hwr reads, and every
// value is the real ESP-IDF value rather than a plausible-looking one.
//
// The ESP_AUTH_SMP_* list matters twice over. It is reproduced in full because
// ble_connection_manager.cpp switches over it to build a user-facing failure
// string, and a switch missing an enumerator would compile here while the
// firmware reported "unknown" for a reason it can name. And the *values* are
// real (78-102, contiguous) because that same code formats the reason as
// "%s (0x%02x)" into Pump Link Status -- an earlier version of this file
// numbered them 0x01-0x1A with an invented gap, so a test asserting a hex code
// would have agreed with itself and disagreed with every pump.

#pragma once

#include <cstdint>

#include "esp_bt_defs.h"

enum esp_bt_status_t {
  ESP_BT_STATUS_SUCCESS = 0,
  ESP_BT_STATUS_FAIL = 1,
};

// ── Security-manager failure reasons ────────────────────────────────────────
enum esp_ble_auth_fail_reason_t {
  ESP_AUTH_SMP_PASSKEY_FAIL = 78,
  ESP_AUTH_SMP_OOB_FAIL = 79,
  ESP_AUTH_SMP_PAIR_AUTH_FAIL = 80,
  ESP_AUTH_SMP_CONFIRM_VALUE_FAIL = 81,
  ESP_AUTH_SMP_PAIR_NOT_SUPPORT = 82,
  ESP_AUTH_SMP_ENC_KEY_SIZE = 83,
  ESP_AUTH_SMP_INVALID_CMD = 84,
  ESP_AUTH_SMP_UNKNOWN_ERR = 85,
  ESP_AUTH_SMP_REPEATED_ATTEMPT = 86,
  ESP_AUTH_SMP_INVALID_PARAMETERS = 87,
  ESP_AUTH_SMP_DHKEY_CHK_FAIL = 88,
  ESP_AUTH_SMP_NUM_COMP_FAIL = 89,
  ESP_AUTH_SMP_BR_PARING_IN_PROGR = 90,
  ESP_AUTH_SMP_XTRANS_DERIVE_NOT_ALLOW = 91,
  ESP_AUTH_SMP_INTERNAL_ERR = 92,
  ESP_AUTH_SMP_UNKNOWN_IO = 93,
  ESP_AUTH_SMP_INIT_FAIL = 94,
  ESP_AUTH_SMP_CONFIRM_FAIL = 95,
  ESP_AUTH_SMP_BUSY = 96,
  ESP_AUTH_SMP_ENC_FAIL = 97,
  ESP_AUTH_SMP_STARTED = 98,
  ESP_AUTH_SMP_RSP_TIMEOUT = 99,
  ESP_AUTH_SMP_DIV_NOT_AVAIL = 100,
  ESP_AUTH_SMP_UNSPEC_ERR = 101,
  ESP_AUTH_SMP_CONN_TOUT = 102,
};

enum esp_ble_sm_param_t {
  ESP_BLE_SM_AUTHEN_REQ_MODE = 0,
  ESP_BLE_SM_IOCAP_MODE = 1,
  ESP_BLE_SM_SET_INIT_KEY = 2,
  ESP_BLE_SM_SET_RSP_KEY = 3,
  ESP_BLE_SM_MAX_KEY_SIZE = 4,
  ESP_BLE_SM_MIN_KEY_SIZE = 5,
};

enum { ESP_BLE_ENC_KEY_MASK = 0x01, ESP_BLE_ID_KEY_MASK = 0x02 };
enum { ESP_IO_CAP_NONE = 3 };
enum { ESP_LE_AUTH_REQ_SC_BOND = 0x0D };
enum { ESP_BLE_SEC_ENCRYPT = 1 };

using esp_ble_sec_act_t = int;
using esp_ble_io_cap_t = uint8_t;
using esp_ble_key_type_t = uint8_t;

struct esp_ble_bond_dev_t {
  esp_bd_addr_t bd_addr;
};

struct esp_ble_conn_update_params_t {
  esp_bd_addr_t bda;
  uint16_t min_int;
  uint16_t max_int;
  uint16_t latency;
  uint16_t timeout;
};

// ── Events ──────────────────────────────────────────────────────────────────
enum esp_gap_ble_cb_event_t {
  ESP_GAP_BLE_AUTH_CMPL_EVT = 8,
  ESP_GAP_BLE_KEY_EVT = 9,
  ESP_GAP_BLE_SEC_REQ_EVT = 10,
  ESP_GAP_BLE_PASSKEY_NOTIF_EVT = 11,
  ESP_GAP_BLE_PASSKEY_REQ_EVT = 12,
  ESP_GAP_BLE_NC_REQ_EVT = 16,
  ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT = 23,
};

// ── Callback parameter union ────────────────────────────────────────────────
union esp_ble_gap_cb_param_t {
  /// esp_ble_sec_t is a UNION in the real SDK, and that aliasing is
  /// load-bearing rather than incidental: every member starts with bd_addr, so
  /// ble_connection_manager.cpp can read `ble_security.ble_req.bd_addr` for
  /// ESP_GAP_BLE_NC_REQ_EVT even though the stack fills `key_notif` for that
  /// event. This mock declared it a struct with four disjoint members, which
  /// made those different bytes -- so a test staging the member the SDK
  /// actually fills would have failed against correct firmware.
  union ble_security_t {
    struct ble_req_t {
      esp_bd_addr_t bd_addr;
    } ble_req;

    struct key_notif_t {
      esp_bd_addr_t bd_addr;
      uint32_t passkey;
    } key_notif;

    struct ble_key_t {
      esp_bd_addr_t bd_addr;
      esp_ble_key_type_t key_type;
    } ble_key;

    struct auth_cmpl_t {
      esp_bd_addr_t bd_addr;
      bool key_present;
      esp_ble_key_type_t key_type;
      bool success;
      esp_ble_auth_fail_reason_t fail_reason;
      uint8_t auth_mode;
    } auth_cmpl;
  } ble_security;

  struct remove_bond_dev_cmpl_t {
    esp_bt_status_t status;
    esp_bd_addr_t bd_addr;
  } remove_bond_dev_cmpl;
};

// ── Functions ───────────────────────────────────────────────────────────────
struct EspGapMockCalls {
  int security_rsp = 0;
  bool last_security_rsp_accept = false;
  int confirm_reply = 0;
  bool last_confirm_accept = false;
  int set_security_param = 0;
  int update_conn_params = 0;
  int bond_device_num = 0;
  int set_encryption = 0;
  uint8_t bonded_addr[6] = {0, 0, 0, 0, 0, 0};

  void reset() { *this = EspGapMockCalls(); }
};
inline EspGapMockCalls &esp_gap_mock() {
  static EspGapMockCalls calls;
  return calls;
}

inline esp_err_t esp_ble_gap_security_rsp(esp_bd_addr_t, bool accept) {
  esp_gap_mock().security_rsp++;
  esp_gap_mock().last_security_rsp_accept = accept;
  return ESP_OK;
}

inline esp_err_t esp_ble_confirm_reply(esp_bd_addr_t, bool accept) {
  esp_gap_mock().confirm_reply++;
  esp_gap_mock().last_confirm_accept = accept;
  return ESP_OK;
}

inline esp_err_t esp_ble_gap_set_security_param(esp_ble_sm_param_t, void *, uint8_t) {
  esp_gap_mock().set_security_param++;
  return ESP_OK;
}

inline esp_err_t esp_ble_gap_update_conn_params(esp_ble_conn_update_params_t *) {
  esp_gap_mock().update_conn_params++;
  return ESP_OK;
}

inline int esp_ble_get_bond_device_num(void) {
  return esp_gap_mock().bond_device_num;
}

inline esp_err_t esp_ble_get_bond_device_list(int *dev_num, esp_ble_bond_dev_t *dev_list) {
  // Hands back whatever the test staged. Deliberately does not invent a bond:
  // "is this peer bonded" drives a real branch in ble_connection_manager.cpp.
  const int staged = esp_gap_mock().bond_device_num;
  if (dev_num != nullptr) *dev_num = staged;
  for (int i = 0; i < staged && dev_list != nullptr; i++)
    for (int b = 0; b < 6; b++) dev_list[i].bd_addr[b] = esp_gap_mock().bonded_addr[b];
  return ESP_OK;
}

inline esp_err_t esp_ble_set_encryption(esp_bd_addr_t, esp_ble_sec_act_t) {
  esp_gap_mock().set_encryption++;
  return ESP_OK;
}
