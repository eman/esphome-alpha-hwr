// Host-test stand-in for ESP-IDF's <esp_bt_defs.h>.
//
// Only what components/alpha_hwr actually references. Deliberately not a
// faithful copy of the SDK header: a mock that invents behaviour is worse than
// no mock, so everything here is either a plain type, an enumerator whose
// numeric value does not matter to the code under test, or a stub whose return
// the code checks. Where a value *is* load-bearing it is noted at the site.

#pragma once

#include <cstdint>

using esp_err_t = int;

#define ESP_OK 0
#define ESP_FAIL -1

/// Bluetooth device address. The length is load-bearing: gap_security_policy.h
/// asserts BD_ADDR_LEN == 6 against it.
using esp_bd_addr_t = uint8_t[6];

enum esp_bt_octet16_tag { ESP_BT_OCTET16_LEN = 16 };
using esp_bt_octet16_t = uint8_t[16];
