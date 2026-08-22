/**
 * Device Information Service Implementation
 * 
 * Reads device identification strings using Class 7 commands.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/device_info.py
 */

#include "device_info_service.h"

#include "class7_string.h"
#include "transport.h"
#include "session.h"
#include "frame_builder.h"
#include "esphome/core/log.h"
#include <cinttypes>
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

// String IDs (from Python reference device_info.py)
static const uint8_t STRING_ID_PRODUCT_NAME = 1;
static const uint8_t STRING_ID_SERIAL = 9;
static const uint8_t STRING_ID_SOFTWARE_VERSION = 50;
static const uint8_t STRING_ID_HARDWARE_VERSION = 52;
static const uint8_t STRING_ID_BLE_VERSION = 58;

DeviceInfoService::DeviceInfoService(core::Transport &transport)
    : transport_(transport) {
  ESP_LOGD(TAG, "Device Info Service initialized");
}

bool DeviceInfoService::read_device_info_async(std::function<void(bool)> on_complete) {
  ESP_LOGD(TAG, "Starting device info read (5 strings)");
  
  // Reset state
  pending_reads_ = 5;
  failed_reads_ = 0;
  completion_callback_ = on_complete;
  
  // Queue all 5 string reads
  
  // Read Product Name (ID 1)
  read_class7_string_async(STRING_ID_PRODUCT_NAME,
      [this](bool ok, const char* value) {
      if (ok && value) {
        product_name_ = value;
        ESP_LOGD(TAG, "Product name: %s", product_name_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read product name");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Read Serial Number (ID 9)
  read_class7_string_async(STRING_ID_SERIAL,
      [this](bool ok, const char* value) {
      if (ok && value) {
        serial_number_ = value;
        ESP_LOGD(TAG, "Serial number: %s", serial_number_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read serial number");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Read Software Version (ID 50)
  read_class7_string_async(STRING_ID_SOFTWARE_VERSION,
      [this](bool ok, const char* value) {
      if (ok && value) {
        software_version_ = value;
        ESP_LOGD(TAG, "Software version: %s", software_version_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read software version");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Read Hardware Version (ID 52)
  read_class7_string_async(STRING_ID_HARDWARE_VERSION,
      [this](bool ok, const char* value) {
      if (ok && value) {
        hardware_version_ = value;
        ESP_LOGD(TAG, "Hardware version: %s", hardware_version_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read hardware version");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Read BLE Version (ID 58)
  read_class7_string_async(STRING_ID_BLE_VERSION,
      [this](bool ok, const char* value) {
      if (ok && value) {
        ble_version_ = value;
        ESP_LOGD(TAG, "BLE version: %s", ble_version_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read BLE version");
        failed_reads_++;
      }
      on_string_read_complete();
    });

  return true;
}

void DeviceInfoService::read_class7_string_async(uint8_t string_id, 
                                                   std::function<void(bool, const char*)> on_complete) {
  // Build Class 7 ReadString APDU: [0x07][0x01][StringID]
  uint8_t apdu[3] = {0x07, 0x01, string_id};
  
  // Send command and wait for Class 7 response
  // We don't use Object/Sub-ID matching for Class 7 (set to 0)
  // Instead, we'll match on Class byte (0x07) in the response handler
  transport_.send_apdu_command(
    apdu, 3,
    0,  // expect_type_low_ver (not used for Class 7)
    0,  // expect_type_high (not used for Class 7)
    [string_id, on_complete](bool success, const uint8_t* data, size_t len) {
      // The decode itself lives in protocol::decode_class7_string(), pure and
      // directly testable (issue #282). It used to be inline here, in a lambda
      // inside a private method, so the only way to reach it was through a
      // Transport -- which made its memory-safety guard unprovable once #278
      // gave the transport a length floor of its own: each guard masked the
      // other's mutation, and CI found the equivalent mutant. The guard has not
      // moved or weakened; it is just now reachable by a test holding a 7-byte
      // frame, with no transport in front of it.
      if (!success) {
        ESP_LOGW(TAG, "No response for String ID %d", string_id);
        on_complete(false, nullptr);
        return;
      }

      const protocol::Class7StringResult decoded = protocol::decode_class7_string(data, len);
      if (decoded.status != protocol::Class7Status::OK) {
        ESP_LOGW(TAG, "Bad Class 7 response for String ID %d (%s, len=%zu)", string_id,
                 protocol::class7_status_name(decoded.status), len);
        on_complete(false, nullptr);
        return;
      }

      if (decoded.count_disagrees) {
        ESP_LOGW(TAG, "String ID %d: count byte %u disagrees with the frame's "
                      "%u string bytes; parsing by frame length",
                 string_id, (unsigned) decoded.declared_count,
                 (unsigned) decoded.string_bytes);
      }
      if (decoded.truncated) {
        ESP_LOGW(TAG, "String ID %d: %u string bytes truncated to %u",
                 string_id, (unsigned) decoded.string_bytes,
                 (unsigned) protocol::CLASS7_MAX_STRING_LEN);
      }
      if (decoded.value.empty()) {
        ESP_LOGW(TAG, "Empty string for ID %d", string_id);
      }

      ESP_LOGV(TAG, "String ID %d: '%s' (%u bytes)", string_id, decoded.value.c_str(),
               (unsigned) decoded.value.size());
      on_complete(true, decoded.value.c_str());
    },
    3000  // 3 second timeout
  );
}

void DeviceInfoService::on_string_read_complete() {
  pending_reads_--;
  ESP_LOGV(TAG, "String read complete, %d remaining", pending_reads_);
  
  if (pending_reads_ == 0 && completion_callback_) {
    bool all_ok = (failed_reads_ == 0);
    if (all_ok) {
      ESP_LOGI(TAG, "All device info strings read successfully");
    } else {
      ESP_LOGW(TAG, "Device info read completed with %d failure(s)", failed_reads_);
    }
    completion_callback_(all_ok);
    completion_callback_ = nullptr;
  }
}

void DeviceInfoService::read_statistics_async(std::function<void(bool, uint32_t, float)> on_complete) {
  ESP_LOGI(TAG, "Reading operating statistics (Object 93, Sub 1)...");

  // Build Class 10 read for Object 93, Sub-ID 1 (operation_history_pump_obj)
  // APDU: [Class=0x0A][OpSpec=0x03][ObjID=0x5D][SubH=0x00][SubL=0x01]
  uint8_t apdu[] = {0x0A, 0x03, 0x5D, 0x00, 0x01};  // Object 93 = 0x5D, Sub 1
  transport_.send_apdu_command(
    apdu, sizeof(apdu),
    0,  // Use wildcard matching (reference only checks Class 10)
    0,  // Use wildcard matching
    [on_complete](bool success, const uint8_t *data, size_t len) {
      if (!success || !data || len < 15) {
        ESP_LOGW(TAG, "Statistics read failed or no data (len=%zu)", len);
        if (on_complete) on_complete(false, 0, 0.0f);
        return;
      }

      // data is already the payload (transport strips the 10-byte frame header and 2-byte CRC)
      // Skip 3-byte Class 10 header [00 00 XX]
      const uint8_t *stats = data + 3;
      size_t stats_len = len - 3;

      // Need at least 12 bytes: starts(4) + starts_1h(2) + starts_24h(2) + operating_time(4)
      if (stats_len < 12) {
        ESP_LOGW(TAG, "Statistics data too short: %zu bytes", stats_len);
        if (on_complete) on_complete(false, 0, 0.0f);
        return;
      }

      // Bytes 0-3: start_count (uint32 BE)
      uint32_t start_count = (uint32_t(stats[0]) << 24) | (uint32_t(stats[1]) << 16) |
                             (uint32_t(stats[2]) << 8) | uint32_t(stats[3]);

      // Bytes 8-11: operating_time in seconds (uint32 BE)
      uint32_t op_time_sec = (uint32_t(stats[8]) << 24) | (uint32_t(stats[9]) << 16) |
                             (uint32_t(stats[10]) << 8) | uint32_t(stats[11]);
      float operating_hours = op_time_sec / 3600.0f;

      ESP_LOGI(TAG, "Statistics: starts=%" PRIu32 ", hours=%.1f", start_count, operating_hours);
      if (on_complete) on_complete(true, start_count, operating_hours);
    },
    5000  // 5 second timeout
  );
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
