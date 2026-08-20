/**
 * Time Service Implementation
 * 
 * Manages pump real-time clock (RTC) for schedule execution and event logging.
 *
 * Frame shapes are bench-captured; see the header for both, and
 * send_set_clock_command() for how the write's fields were identified. The
 * Python reference this file was ported from described both of them wrongly,
 * so it is deliberately not cited as the authority here.
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
  // OpSpec 0x03 = GET with a 3-byte payload (bits 7-6 = 0b00 GET, bits 5-0 =
  // length). Called INFO here previously; INFO is 0b11, a different operation.
  uint8_t apdu[5];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x03;  // OpSpec: GET, 3 payload bytes
  apdu[2] = OBJECT_ID_RTC & 0xFF;  // Object ID (94)
  apdu[3] = (SUB_ID_DATETIME_ACTUAL >> 8) & 0xFF;  // Sub-ID high (0x00)
  apdu[4] = SUB_ID_DATETIME_ACTUAL & 0xFF;  // Sub-ID low (0x65 = 101)
  
  // Send Class 10 GET: Object 94, SubID 101 (DateTimeActual)
  // send_apdu_command signature: (apdu, len, expect_type_low_ver, expect_type_high, callback, timeout_ms)
  // Use wildcard matching (0, 0) to accept any Class 10 response, matching Python's behavior
  // Reference: base.py::match_class10_response only checks p[4] == 0x0A
  transport_->send_apdu_command(
    apdu, sizeof(apdu),
    0, 0,
    [callback](bool success, const uint8_t *data, size_t len) {
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

bool TimeService::current_time(ESPTime &out) const {
#ifdef USE_TIME
  if (time_id_ == nullptr) {
    ESP_LOGD(TAG, "time_id not configured - no wall clock to sync from");
    return false;
  }
  ESPTime now = time_id_->now();
  if (!clock_is_synced(now)) {
    ESP_LOGD(TAG, "System time not synced yet - nothing to write");
    return false;
  }
  out = now;
  return true;
#else
  (void) out;
  ESP_LOGD(TAG, "time component not enabled in this build - no wall clock");
  return false;
#endif
}

uint32_t TimeService::now_unix() const {
  ESPTime now;
  if (!current_time(now)) return 0;
  // clock_is_synced() floors the year at 2021, so a clock that passed it cannot
  // produce a negative or zero timestamp -- but the cast is from time_t, and
  // this returning 0 has to mean "no clock" and nothing else. Checking costs a
  // comparison and keeps the sentinel unambiguous rather than argued.
  if (now.timestamp <= 0) return 0;
  return static_cast<uint32_t>(now.timestamp);
}

bool TimeService::wall_clock_is_set() const {
#ifdef USE_TIME
  if (time_id_ == nullptr) {
    return false;
  }
  return clock_is_synced(time_id_->now());
#else
  return false;
#endif
}

void TimeService::send_set_clock_command(const ESPTime &local_now) {
  ESP_LOGI(TAG, "Writing pump clock (Object 94, Sub 100): %04d-%02d-%02d %02d:%02d:%02d",
           local_now.year, local_now.month, local_now.day_of_month,
           local_now.hour, local_now.minute, local_now.second);

  // Class 10 SET, same object-first shape as the Obj 91 Sub 421 DHW write:
  //   [0A][OpSpec][Obj][Sub H][Sub L][Type H][Type L][Ver][Size 3B][data]
  // OpSpec 0x94 = SET + 20 body bytes (1 obj + 2 sub + 2 type + 1 version +
  // 3 size + 11 data), and the size field says 11, so the two agree.
  //
  // The six leading bytes used to be carried as an opaque "Type 322 header"
  // copied from the Python reference, with the object and sub-id labelled
  // backwards ("Sub-ID high (0x5E00 = Object 94)"). The bytes are unchanged --
  // this is a renaming, not a re-encoding, and any edit that moves one is a
  // wire change. The names come from the sibling Obj 91 Sub 421 write, which
  // has the identical layout and is capture-verified, plus two checks that
  // close over these bytes alone: OpSpec 0x94 declares 20 body bytes and the
  // fields below sum to exactly 20, and the size field declares 11 data bytes
  // and exactly 11 follow it.
  uint8_t apdu[22];
  apdu[0] = 0x0A;   // Class 10
  apdu[1] = 0x94;   // OpSpec: SET + 20 bytes
  apdu[2] = OBJECT_ID_RTC & 0xFF;  // Obj-ID (94, 1 byte in this packet shape)
  apdu[3] = (SUB_ID_DATETIME_CONFIG >> 8) & 0xFF;  // Sub-ID high (100 = 0x0064)
  apdu[4] = SUB_ID_DATETIME_CONFIG & 0xFF;        // Sub-ID low
  apdu[5] = 0x01;   // Type high (321 = 0x0141)
  apdu[6] = 0x41;   // Type low
  apdu[7] = 0x02;   // Object version
  apdu[8] = 0x00;   // Size high
  apdu[9] = 0x00;   // Size mid
  apdu[10] = 0x0B;  // Size low (11 bytes)

  apdu[11] = 0x01;  // leading struct byte, constant in the reference encoder
  apdu[12] = static_cast<uint8_t>((local_now.year >> 8) & 0xFF);  // year, big-endian
  apdu[13] = static_cast<uint8_t>(local_now.year & 0xFF);
  apdu[14] = local_now.month;
  apdu[15] = local_now.day_of_month;
  apdu[16] = local_now.hour;
  apdu[17] = local_now.minute;
  apdu[18] = local_now.second;
  apdu[19] = 0x00;  // tail padding, part of the 11-byte struct
  apdu[20] = 0x00;
  apdu[21] = 0x00;

  ESP_LOGD(TAG, "Clock SET APDU: %s", format_hex_pretty(apdu, sizeof(apdu)).c_str());

  // Awaited, not fired and forgotten (issue #253).
  //
  // The callback exists to make the transport wait at all -- a null one means
  // the command is popped as soon as its last chunk goes out -- and reports
  // nothing onward: run_set_clock_() decides by reading SubID 101 back, which
  // is the only honest verdict for this write and does not change here.
  //
  // What the wait buys is that this write's acknowledgement is consumed by this
  // write. Every SET reply the pump sends is the same nine bytes whatever was
  // written (see the short-ACK branch in transport.cpp), so a reply left
  // unclaimed is one the next Class 10 write can be handed instead. This write
  // is not a rare one -- the clock sync runs on a schedule -- and it is the
  // best-attested SET in the corpus: nine instances in
  // resources/traffic_capture, every one answered in 38-90 ms, all with the
  // identical `24 05 F8 E7 0A 01 00 AE A2`.
  transport_->send_apdu_command(
      apdu, sizeof(apdu), 0, 0,
      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {
        ESP_LOGV(TAG, "Clock write %s", success ? "acknowledged" : "unanswered");
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);
}

ESPTime TimeService::parse_clock_response(const uint8_t *data, size_t len) {
  // What the pump actually sends, captured on the bench (2026-08-15):
  //
  //   00 00 0C 07 EA 08 0F 14 26 37 48 00 06 00 01   -> 2026-08-15 20:38:55
  //   00 00 0C 07 EA 08 0F 14 27 08 13 00 06 00 01   -> 2026-08-15 20:39:08
  //
  // Fifteen bytes, from the object body onwards (the transport hands the
  // callback data+10, so these start at the size header):
  //   [Size(3BE)][Year(2BE)][Month][Day][Hour][Minute][Second][5 trailing]
  //
  // The first three bytes were called "[Status(2)][Length(1)]" here for a long
  // time and are neither: 00 00 0C is the ordinary three-byte size header, and
  // 0x0C = 12 is exactly the number of bytes that follow it. The old naming
  // read the same offsets, so the arithmetic below was always right and only
  // the labels were wrong.
  //
  // Of the five trailing bytes the first moved between the two samples (0x48,
  // 0x13) and the last four did not. Unidentified; none of them are read.
  //
  // The >= 10 floor is kept rather than tightened to the observed 15: it is the
  // minimum this parser needs (3 header + 7 datetime), and a firmware that
  // sends a shorter tail should still yield a usable clock.
  if (len < 10) {
    ESP_LOGW(TAG, "Clock response too short: %zu bytes (expected >= 10)", len);
    return ESPTime();
  }
  
  ESP_LOGD(TAG, "Raw clock data: %s (%zu bytes)", format_hex_pretty(data, len).c_str(), len);
  
  // Low 16 bits of the 3-byte size header; logged only, as a sanity read.
  uint16_t declared_size = decode_uint16_be(data, 1);
  
  // Skip the 3-byte size header; the datetime starts at byte 3.
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
  
  ESP_LOGD(TAG, "Parsed clock: %04d-%02d-%02d %02d:%02d:%02d, declared_size=%u",
           year, month, day, hour, minute, second, declared_size);
  
  // Handle unset/invalid clock
  if (year < 1970 || year > 2100 || month == 0 || month > 12 || day == 0 || day > 31) {
    ESP_LOGW(TAG, "Pump clock is unset or invalid: %04d-%02d-%02d", year, month, day);
    return ESPTime();  // Return invalid time
  }
  
  // Create ESPTime. Value-initialized: the fields set below are every field
  // ESPTime has except is_dst, and leaving that one indeterminate is a read of
  // an uninitialized bool waiting for the first caller who looks at it.
  ESPTime pump_time{};
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
  
  // time_t width varies by platform (long long on ESP-IDF, long on many hosts):
  // widen to long long so one format string is correct everywhere.
  ESP_LOGD(TAG, "ESPTime created: timestamp=%lld, is_valid=%d",
           static_cast<long long>(pump_time.timestamp), pump_time.is_valid());
  
  return pump_time;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
