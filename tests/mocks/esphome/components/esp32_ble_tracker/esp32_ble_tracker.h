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
constexpr int UUID_STR_LEN = 40;
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

  /// Parses the canonical hyphenated 128-bit form. Not a formality: the GENI
  /// characteristic UUID is built this way, and a from_raw() that returned a
  /// default-constructed UUID (as this mock first did) would compare equal to
  /// every other default, so any characteristic would satisfy a lookup for it.
  static ESPBTUUID from_raw(const std::string &text) {
    ESPBTUUID u;
    u.uuid_.len = ESP_UUID_LEN_128;
    int nibble = 0;
    for (char c : text) {
      int v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else continue;  // hyphens
      if (nibble >= 32) break;
      uint8_t &b = u.uuid_.uuid.uuid128[nibble / 2];
      if (nibble % 2 == 0) b = static_cast<uint8_t>(v << 4);
      else b = static_cast<uint8_t>(b | v);
      nibble++;
    }
    return u;
  }

  esp_bt_uuid_t get_uuid() const { return uuid_; }

  /// Writes into a caller-supplied buffer, like the real one.
  const char *to_str(char *buf) const {
    snprintf(buf, 40, "0x%04X", uuid_.uuid.uuid16);
    return buf;
  }

  bool operator==(const ESPBTUUID &o) const {
    if (uuid_.len != o.uuid_.len) return false;
    if (uuid_.len == ESP_UUID_LEN_128) {
      for (int i = 0; i < 16; i++)
        if (uuid_.uuid.uuid128[i] != o.uuid_.uuid.uuid128[i]) return false;
      return true;
    }
    return uuid_.uuid.uuid16 == o.uuid_.uuid.uuid16;
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
