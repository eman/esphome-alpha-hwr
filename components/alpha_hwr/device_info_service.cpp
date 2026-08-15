/**
 * Device Information Service Implementation
 * 
 * Reads device identification strings using Class 7 commands.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/device_info.py
 */

#include "device_info_service.h"
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
      // Response frame (issue #179):
      //   [STX][LEN][DST][SRC][0x07][Count][...STRING...][CRC_H][CRC_L]
      //     0    1    2    3     4     5      6 ..            len-2
      //
      // The header is six bytes, not seven: byte 5 is a byte count for the
      // string that follows, not an [Cmd][ID] pair. Every captured frame has
      // `byte5 == len - 8` and the first character of the string at byte 6 --
      // e.g. `24 0E F8 E7 07 0A 41 4C 50 48 41 ...` is a 10-byte "ALPHA HWR\0"
      // in an 18-byte frame. Reading from byte 7 dropped the first character
      // of all five strings, which is what the two transcribed "Python fix"
      // string rewrites were papering over.
      static const size_t HEADER_LEN = 6;
      static const size_t CRC_LEN = 2;

      if (!success || !data || len < HEADER_LEN + CRC_LEN) {
        ESP_LOGW(TAG, "No response for String ID %d", string_id);
        on_complete(false, nullptr);
        return;
      }

      if (data[4] != 0x07) {
        ESP_LOGW(TAG, "Invalid Class 7 response for String ID %d (class=0x%02X)",
                 string_id, data[4]);
        on_complete(false, nullptr);
        return;
      }

      size_t string_len = len - HEADER_LEN - CRC_LEN;

      // The count byte is not needed to bound the read -- the frame length
      // already does that, and trusting a radio-supplied count would be an
      // overread waiting to happen. It is checked only so that a pump whose
      // layout differs from the captures says so, instead of silently handing
      // back a shifted string the way this parser did for its whole life.
      if (data[5] != string_len) {
        ESP_LOGW(TAG, "String ID %d: count byte %u disagrees with the frame's "
                      "%u string bytes; parsing by frame length",
                 string_id, (unsigned) data[5], (unsigned) string_len);
      }

      if (string_len == 0) {
        ESP_LOGW(TAG, "Empty string for ID %d", string_id);
        on_complete(true, "");
        return;
      }

      const uint8_t* string_data = data + HEADER_LEN;

      // Create null-terminated C string (strip trailing nulls)
      char string_buffer[128];
      size_t actual_len = 0;
      for (size_t i = 0; i < string_len && i < 127; i++) {
        if (string_data[i] == 0) break;  // Stop at first null
        string_buffer[actual_len++] = string_data[i];
      }
      string_buffer[actual_len] = '\0';
      
      // Trim trailing whitespace
      while (actual_len > 0 && (string_buffer[actual_len-1] == ' ' || 
                                 string_buffer[actual_len-1] == '\t' ||
                                 string_buffer[actual_len-1] == '\r' ||
                                 string_buffer[actual_len-1] == '\n')) {
        string_buffer[--actual_len] = '\0';
      }
      
      ESP_LOGV(TAG, "String ID %d: '%s' (%d bytes)", string_id, string_buffer, actual_len);
      on_complete(true, string_buffer);
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
