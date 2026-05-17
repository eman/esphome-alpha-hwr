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
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char* TAG = "alpha_hwr.device_info";

// String IDs (from Python reference device_info.py)
static const uint8_t STRING_ID_PRODUCT_NAME = 1;
static const uint8_t STRING_ID_SERIAL = 9;
static const uint8_t STRING_ID_SOFTWARE_VERSION = 50;
static const uint8_t STRING_ID_HARDWARE_VERSION = 52;
static const uint8_t STRING_ID_BLE_VERSION = 58;

DeviceInfoService::DeviceInfoService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {
  ESP_LOGD(TAG, "Device Info Service initialized");
}

bool DeviceInfoService::read_device_info_async(std::function<void(bool)> on_complete) {
  ESP_LOGD(TAG, "Starting device info read (5 strings)");
  
  // Reset state
  pending_reads_ = 5;
  failed_reads_ = 0;
  completion_callback_ = on_complete;
  
  // Queue all 5 string reads
  bool success = true;
  
  // Product name (ID 1)
  success &= read_class7_string_async(STRING_ID_PRODUCT_NAME, 
    [this](bool ok, const char* value) {
      if (ok && value) {
        product_name_ = value;
        // Fix: Python prepends "A" if result is "LPHA HWR"
        if (product_name_ == "LPHA HWR") {
          product_name_ = "ALPHA HWR";
        }
        ESP_LOGD(TAG, "Product name: %s", product_name_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read product name");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Serial number (ID 9)
  success &= read_class7_string_async(STRING_ID_SERIAL,
    [this](bool ok, const char* value) {
      if (ok && value) {
        serial_number_ = value;
        // Fix: Python prepends "1" if result starts with "0"
        if (!serial_number_.empty() && serial_number_[0] == '0') {
          serial_number_ = "1" + serial_number_;
        }
        ESP_LOGD(TAG, "Serial number: %s", serial_number_.c_str());
      } else {
        ESP_LOGW(TAG, "Failed to read serial number");
        failed_reads_++;
      }
      on_string_read_complete();
    });
  
  // Software version (ID 50)
  success &= read_class7_string_async(STRING_ID_SOFTWARE_VERSION,
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
  
  // Hardware version (ID 52)
  success &= read_class7_string_async(STRING_ID_HARDWARE_VERSION,
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
  
  // BLE version (ID 58)
  success &= read_class7_string_async(STRING_ID_BLE_VERSION,
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
  
  return success;
}

bool DeviceInfoService::read_class7_string_async(uint8_t string_id, 
                                                   std::function<void(bool, const char*)> on_complete) {
  // Build Class 7 ReadString APDU: [0x07][0x01][StringID]
  uint8_t apdu[3] = {0x07, 0x01, string_id};
  
  // Build GENI packet
  uint8_t packet[20];  // Max needed: 4 (header) + 3 (APDU) + 2 (CRC) = 9 bytes
  size_t packet_len = protocol::build_geni_packet(0xE7, 0xF8, apdu, 3, packet);
  
  ESP_LOGV(TAG, "Reading Class 7 String ID %d", string_id);
  
  // Convert to vector for transport
  std::vector<uint8_t> packet_vec(packet, packet + packet_len);
  
  // Send command and wait for Class 7 response
  // We don't use Object/Sub-ID matching for Class 7 (set to 0)
  // Instead, we'll match on Class byte (0x07) in the response handler
  transport_.send_command(
    packet_vec,
    0,  // expect_obj_id (not used for Class 7)
    0,  // expect_sub_id (not used for Class 7)
    [this, string_id, on_complete](bool success, const uint8_t* data, size_t len) {
      if (!success || !data || len < 10) {
        ESP_LOGW(TAG, "No response for String ID %d", string_id);
        on_complete(false, nullptr);
        return;
      }
      
      // Verify it's a Class 7 response
      // Frame: [STX][LEN][DST][SRC][Class][Cmd][ID][...STRING...][CRC_H][CRC_L]
      if (len < 9 || data[4] != 0x07) {
        ESP_LOGW(TAG, "Invalid Class 7 response for String ID %d (class=0x%02X)", 
                 string_id, len > 4 ? data[4] : 0);
        on_complete(false, nullptr);
        return;
      }
      
      // Extract string data: skip frame header (7 bytes) and CRC (2 bytes)
      // String data starts at byte 7, ends 2 bytes before end
      if (len <= 9) {
        ESP_LOGW(TAG, "Empty string for ID %d", string_id);
        on_complete(true, "");
        return;
      }
      
      const uint8_t* string_data = data + 7;
      size_t string_len = len - 9;  // Total - header (7) - CRC (2)
      
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
  
  return true;
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
  uint8_t packet[20];
  size_t packet_len = protocol::build_geni_packet(0xE7, 0xF8, apdu, sizeof(apdu), packet);

  std::vector<uint8_t> packet_vec(packet, packet + packet_len);

  transport_.send_command(
    packet_vec,
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

      ESP_LOGI(TAG, "Statistics: starts=%u, hours=%.1f", start_count, operating_hours);
      if (on_complete) on_complete(true, start_count, operating_hours);
    },
    5000  // 5 second timeout
  );
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
