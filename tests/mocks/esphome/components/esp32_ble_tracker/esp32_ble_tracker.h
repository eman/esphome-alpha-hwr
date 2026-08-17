// Host-test stand-in for ESPHome's esp32_ble_tracker.
//
// Only what components/alpha_hwr reads: the UUID wrapper, the advertisement
// accessors the scan filter uses, and the listener base class. Advertisement
// contents are settable so a test can hand the filter a real ALPHA HWR service
// data blob, a wrong-family one, or nothing at all.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "esp_gattc_api.h"

namespace esphome {

/// The real component keeps UUID_STR_LEN here.
namespace esp32_ble {
/// Real value (ble_uuid.h). Was 40 here, which is exactly the kind of
/// divergence a host test cannot catch: ble_connection_manager.cpp sizes a
/// stack buffer from this constant, and the real to_str() takes a
/// std::span<char, 37> that enforces it at compile time.
constexpr size_t UUID_STR_LEN = 37;
}  // namespace esp32_ble

namespace esp32_ble_tracker {

class ESPBTUUID {
 public:
  ESPBTUUID() { uuid_.len = ESP_UUID_LEN_16; uuid_.uuid.uuid16 = 0; }

  static ESPBTUUID from_uint16(uint16_t v) {
    ESPBTUUID u;
    u.uuid_.len = ESP_UUID_LEN_16;
    u.uuid_.uuid.uuid16 = v;
    return u;
  }

  esp_bt_uuid_t get_uuid() const { return uuid_; }

  /// Writes into a caller-supplied buffer, like the real one.
  const char *to_str(char *buf) const {
    if (uuid_.len == ESP_UUID_LEN_128) {
      snprintf(buf, esp32_ble::UUID_STR_LEN, "%02X%02X%02X%02X-...-%02X%02X",
               uuid_.uuid.uuid128[15], uuid_.uuid.uuid128[14],
               uuid_.uuid.uuid128[13], uuid_.uuid.uuid128[12],
               uuid_.uuid.uuid128[1], uuid_.uuid.uuid128[0]);
    } else {
      snprintf(buf, esp32_ble::UUID_STR_LEN, "0x%04X", uuid_.uuid.uuid16);
    }
    return buf;
  }

  /// Parses the canonical hyphenated 128-bit form, **writing backwards** --
  /// `uuid128[15 - n]` -- exactly as ESPHome's ble_uuid.cpp does. The order is
  /// not cosmetic: this mock wrote forwards, so GENI_CHAR_UUID held the
  /// byte-reversed value of what the firmware holds. Every comparison in the
  /// test was still self-consistent, and therefore proved nothing about the
  /// constant matching what the pump's GATT database exposes.
  static ESPBTUUID from_raw(const std::string &text) {
    ESPBTUUID u;
    u.uuid_.len = ESP_UUID_LEN_128;
    int n = 0;
    for (size_t i = 0; i < text.size() && n < 16; i += 2) {
      if (text[i] == '-') i++;
      if (i + 1 >= text.size()) break;
      uint8_t msb = static_cast<uint8_t>(text[i]);
      uint8_t lsb = static_cast<uint8_t>(text[i + 1]);
      if (msb > '9') msb -= 7;
      if (lsb > '9') lsb -= 7;
      u.uuid_.uuid.uuid128[15 - n++] =
          static_cast<uint8_t>(((msb & 0x0F) << 4) | (lsb & 0x0F));
    }
    return u;
  }

  /// Promote a 16/32-bit UUID to its 128-bit form against the Bluetooth base
  /// UUID, byte-for-byte as ble_uuid.cpp does it.
  ESPBTUUID as_128bit() const {
    if (uuid_.len == ESP_UUID_LEN_128) return *this;
    uint8_t data[16] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
                        0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint32_t v =
        (uuid_.len == ESP_UUID_LEN_32) ? uuid_.uuid.uuid32 : uuid_.uuid.uuid16;
    for (uint16_t i = 0; i < uuid_.len; i++)
      data[12 + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    ESPBTUUID u;
    u.uuid_.len = ESP_UUID_LEN_128;
    for (int i = 0; i < 16; i++) u.uuid_.uuid.uuid128[i] = data[i];
    return u;
  }

  /// Mismatched lengths promote to 128-bit and compare, as the real one does.
  /// Returning false on a length mismatch -- which this mock used to do -- is
  /// wrong in a way the pump exercises: a service exposed as the full base UUID
  /// 0000FE5D-0000-1000-8000-00805F9B34FB matches from_uint16(0xFE5D) on the
  /// device, and would not have matched here.
  bool operator==(const ESPBTUUID &o) const {
    if (uuid_.len == o.uuid_.len) {
      if (uuid_.len == ESP_UUID_LEN_128) {
        for (int i = 0; i < 16; i++)
          if (uuid_.uuid.uuid128[i] != o.uuid_.uuid.uuid128[i]) return false;
        return true;
      }
      if (uuid_.len == ESP_UUID_LEN_32) return uuid_.uuid.uuid32 == o.uuid_.uuid.uuid32;
      return uuid_.uuid.uuid16 == o.uuid_.uuid.uuid16;
    }
    const ESPBTUUID a = as_128bit();
    const ESPBTUUID b = o.as_128bit();
    for (int i = 0; i < 16; i++)
      if (a.uuid_.uuid.uuid128[i] != b.uuid_.uuid.uuid128[i]) return false;
    return true;
  }

 private:
  esp_bt_uuid_t uuid_{};
};

struct ServiceData {
  ESPBTUUID uuid;
  std::vector<uint8_t> data;
};

class ESPBTDevice {
 public:
  const std::vector<ServiceData> &get_service_datas() const { return service_datas_; }
  const std::vector<ESPBTUUID> &get_service_uuids() const { return service_uuids_; }
  uint64_t address_uint64() const { return address_; }

  // Test-only setters. Not present on the real class; a test builds the
  // advertisement it wants to hand the filter.
  void add_service_data(const ESPBTUUID &uuid, std::vector<uint8_t> data) {
    service_datas_.push_back(ServiceData{uuid, std::move(data)});
  }
  void add_service_uuid(const ESPBTUUID &uuid) { service_uuids_.push_back(uuid); }
  void set_address(uint64_t a) { address_ = a; }

 private:
  uint64_t address_{0};
  std::vector<ServiceData> service_datas_;
  std::vector<ESPBTUUID> service_uuids_;
};

class ESPBTDeviceListener {
 public:
  virtual ~ESPBTDeviceListener() = default;
  virtual bool parse_device(const ESPBTDevice &device) { (void) device; return false; }
};

}  // namespace esp32_ble_tracker
}  // namespace esphome
