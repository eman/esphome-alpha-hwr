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

  static ESPBTUUID from_raw(const std::string &) { return ESPBTUUID(); }

  esp_bt_uuid_t get_uuid() const { return uuid_; }

  /// Writes into a caller-supplied buffer, like the real one.
  const char *to_str(char *buf) const {
    snprintf(buf, 40, "0x%04X", uuid_.uuid.uuid16);
    return buf;
  }

  bool operator==(const ESPBTUUID &o) const {
    if (uuid_.len != o.uuid_.len) return false;
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
