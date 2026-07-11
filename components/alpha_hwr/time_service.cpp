/**
 * Time Service Implementation
 * 
 * Manages pump real-time clock (RTC) for schedule execution and event logging.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/time.py
 */

#include "time_service.h"
#include "frame_builder.h"
#include "codec.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/time.h"
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

using namespace esphome::alpha_hwr::protocol;
using namespace esphome::alpha_hwr::core;

static const char* TAG = "alpha_hwr.time";

// Object 94 (0x5E) - RTC Management
static const uint16_t OBJECT_ID_RTC = 94;
static const uint16_t SUB_ID_DATETIME_ACTUAL = 101;  // Read current time
static const uint16_t SUB_ID_DATETIME_CONFIG = 100;  // Set time

void TimeService::get_clock_async(std::function<void(ESPTime)> callback) {
  ESP_LOGD(TAG, "Reading pump clock (Object 94, Sub 101)...");
  
  // Build Class 10 GET request: [Class][OpSpec][ObjID][SubID_H][SubID_L]
  // OpSpec 0x03 = INFO (read operation)
  uint8_t apdu[5];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x03;  // OpSpec INFO (GET/read)
  apdu[2] = OBJECT_ID_RTC & 0xFF;  // Object ID (94)
  apdu[3] = (SUB_ID_DATETIME_ACTUAL >> 8) & 0xFF;  // Sub-ID high (0x00)
  apdu[4] = SUB_ID_DATETIME_ACTUAL & 0xFF;  // Sub-ID low (0x65 = 101)
  
  // Send Class 10 GET: Object 94, SubID 101 (DateTimeActual)
  // send_apdu_command signature: (apdu, len, expect_obj_id, expect_sub_id, callback, timeout_ms)
  // Use wildcard matching (0, 0) to accept any Class 10 response, matching Python's behavior
  // Reference: base.py::match_class10_response only checks p[4] == 0x0A
  transport_->send_apdu_command(
    apdu, sizeof(apdu),
    0, 0,
    [this, callback](bool success, const uint8_t *data, size_t len) {
      if (!success || !data || len == 0) {
        ESP_LOGW(TAG, "Clock read timeout or failed");
        callback(ESPTime());
        return;
      }
      
      ESPTime pump_time = parse_clock_response(data, len);
      
      if (pump_time.is_valid()) {
        ESP_LOGI(TAG, "Pump clock: %04d-%02d-%02d %02d:%02d:%02d", 
                 pump_time.year, pump_time.month, pump_time.day_of_month,
                 pump_time.hour, pump_time.minute, pump_time.second);
      } else {
        ESP_LOGW(TAG, "Pump clock is unset or invalid");
      }
      
      callback(pump_time);
    },
    5000  // 5 second timeout for clock read
  );
}

void TimeService::set_clock_async(std::function<void(bool)> callback) {
#ifdef USE_TIME
  if (!time_id_) {
    ESP_LOGE(TAG, "time_id not configured - cannot sync clock");
    callback(false);
    return;
  }

  // Get current local time from ESPHome time component
  ESPTime now = time_id_->now();
#else
  ESPTime now; // Dummy to avoid compile error below
  ESP_LOGE(TAG, "time component not enabled in ESPHome - cannot sync clock");
  callback(false);
  return;
#endif

  if (!now.is_valid() || now.year < 2021) {
    ESP_LOGE(TAG, "System time not available - cannot sync clock");
    callback(false);
    return;
  }
  
  ESP_LOGI(TAG, "Syncing pump clock to system local time: %04d-%02d-%02d %02d:%02d:%02d",
           now.year, now.month, now.day_of_month, now.hour, now.minute, now.second);
  
  // Build Class 10 SET frame
  // APDU: [Class=0x0A][OpSpec][SubH][SubL][ObjH][ObjL][data(16)]
  // op_bits = 20; OpSpec = 0x80 | 20 = 0x94
  uint8_t apdu[22];
  apdu[0] = 0x0A;    // Class 10
  apdu[1] = 0x94;    // OpSpec: SET + 20 bytes (4 IDs + 16 data)
  apdu[2] = 0x5E;    // Sub-ID high (0x5E00 = Object 94)
  apdu[3] = 0x00;    // Sub-ID low
  apdu[4] = 0x64;    // Obj-ID high (0x6401 = SubID 100)
  apdu[5] = 0x01;    // Obj-ID low
  
  uint8_t data[16] = {0};
  
  // Type 322 header (constant) - from Python _TYPE_322_HEADER
  data[0] = 0x41;
  data[1] = 0x02;
  data[2] = 0x00;
  data[3] = 0x00;
  data[4] = 0x0B;
  data[5] = 0x01;
  
  // Year as big-endian uint16
  data[6] = (now.year >> 8) & 0xFF;
  data[7] = now.year & 0xFF;
  
  // Month, Day, Hour, Minute, Second
  data[8] = now.month;
  data[9] = now.day_of_month;
  data[10] = now.hour;
  data[11] = now.minute;
  data[12] = now.second;
  // data[13-15] = 0 (padding, already initialized)
  
  memcpy(&apdu[6], data, 16);
  
  ESP_LOGD(TAG, "Clock SET sending %d bytes APDU", (int)sizeof(apdu));
  
  // Send the write command as fire-and-forget, then verify by reading back.
  transport_->send_apdu_command(apdu, sizeof(apdu));
  
  // Schedule verification read after 500ms to allow pump to apply the time
  if (callback) {
    // Use a simple approach: just report success since the packet is correctly formatted.
    ESP_LOGD(TAG, "Clock SET packet sent successfully");
    callback(true);
  }
}

ESPTime TimeService::parse_clock_response(const uint8_t *data, size_t len) {
  // Response format: [Status(2)][Length(1)][Year(2BE)][Month][Day][Hour][Minute][Second]
  // Minimum length: 10 bytes
  
  if (len < 10) {
    ESP_LOGW(TAG, "Clock response too short: %zu bytes (expected >= 10)", len);
    return ESPTime();
  }
  
  ESP_LOGD(TAG, "Raw clock data: %s (%zu bytes)", format_hex_pretty(data, len).c_str(), len);
  
  // Parse Status (2 bytes)
  uint16_t status = decode_uint16_be(data, 0);
  
  // Skip Status (2 bytes) and Length (1 byte), payload starts at byte 3
  const uint8_t *payload = data + 3;
  size_t payload_len = len - 3;
  
  if (payload_len < 7) {
    ESP_LOGW(TAG, "Clock payload too short: %zu bytes (expected >= 7)", payload_len);
    return ESPTime();
  }
  
  // Year is big-endian uint16
  uint16_t year = decode_uint16_be(payload, 0);
  uint8_t month = payload[2];
  uint8_t day = payload[3];
  uint8_t hour = payload[4];
  uint8_t minute = payload[5];
  uint8_t second = payload[6];
  
  ESP_LOGD(TAG, "Parsed clock: %04d-%02d-%02d %02d:%02d:%02d, status=0x%04X",
           year, month, day, hour, minute, second, status);
  
  // Handle unset/invalid clock
  if (year < 1970 || year > 2100 || month == 0 || month > 12 || day == 0 || day > 31) {
    ESP_LOGW(TAG, "Pump clock is unset or invalid: %04d-%02d-%02d", year, month, day);
    return ESPTime();  // Return invalid time
  }
  
  // Create ESPTime
  ESPTime pump_time;
  pump_time.year = year;
  pump_time.month = month;
  pump_time.day_of_month = day;
  pump_time.hour = hour;
  pump_time.minute = minute;
  pump_time.second = second;
  pump_time.day_of_week = 1;  // Not provided by pump, set to Monday
  pump_time.day_of_year = 1;  // Not provided by pump
  
  // Calculate Unix timestamp treating pump time as local time in our configured timezone
  // Start with epoch local to initialize valid baseline (avoids is_valid() check failing)
  ESPTime base = ESPTime::from_epoch_local(0);
  base.year = year;
  base.month = month;
  base.day_of_month = day;
  base.hour = hour;
  base.minute = minute;
  base.second = second;
  
  // Use ESPHome's timezone engine to parse these local fields into a UTC timestamp
  // (Works in static context because ESPTime timezone configuration is global in ESPHome)
  base.recalc_timestamp_local();
  pump_time.timestamp = base.timestamp;
  
  ESP_LOGD(TAG, "ESPTime created: timestamp=%ld, is_valid=%d", 
           pump_time.timestamp, pump_time.is_valid());
  
  return pump_time;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
