/**
 * Event Log Service Implementation
 *
 * Reads pump start/stop event log via Object 88.
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/event_log.py
 */

#include "event_log_service.h"
#include "frame_builder.h"
#include "codec.h"
#include "transport.h"
#include "session.h"

#include <cstring>
#include <memory>

namespace esphome {
namespace alpha_hwr {
namespace services {

static constexpr uint8_t OBJECT_EVENT_LOG = 88;
static constexpr uint16_t SUBID_METADATA = 10199;
static constexpr uint16_t SUBID_ENTRY_BASE = 10200;

EventLogService::EventLogService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {}

void EventLogService::read_metadata_async(
    std::function<void(bool, const EventLogMetadata &)> on_complete) {
  if (!session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read event log: session not ready");
    if (on_complete) on_complete(false, EventLogMetadata{});
    return;
  }

  // Build Class 10 READ for Object 88, SubID 10199
  uint8_t apdu[5];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x03;  // OpSpec GET
  apdu[2] = OBJECT_EVENT_LOG;
  apdu[3] = (SUBID_METADATA >> 8) & 0xFF;
  apdu[4] = SUBID_METADATA & 0xFF;

  // Type 243 (EventLogInfo) → response bytes 6-7 = 0xF301
  static constexpr uint16_t TYPE_EVENT_LOG_INFO = 0xF301;
  transport_.send_apdu_command(apdu, 5, TYPE_EVENT_LOG_INFO, 0,
      [this, on_complete](bool success, const uint8_t *payload, size_t payload_len) {
    if (!success || payload_len < 10) {  // 3 sub-header + 7 data minimum
      ESP_LOGW(TAG, "Event log metadata read failed (success=%d, len=%zu)", success, payload_len);
      if (on_complete) on_complete(false, EventLogMetadata{});
      return;
    }

    // Data starts at payload + 3 (sub-header: 3 bytes, same as schedule/history)
    const uint8_t *data = payload + 3;

    metadata_.cycle_counter = ((uint16_t)data[0] << 8) | data[1];
    metadata_.available_entries = ((uint16_t)data[2] << 8) | data[3];
    metadata_.max_entries = ((uint16_t)data[4] << 8) | data[5];
    ESP_LOGI(TAG, "Event log: cycle=%d, entries=%d/%d",
             metadata_.cycle_counter, metadata_.available_entries, metadata_.max_entries);

    if (on_complete) on_complete(true, metadata_);
  }, 5000);
}

void EventLogService::read_entries_async(
    std::function<void(bool, const std::vector<EventLogEntry> &)> on_complete) {

  // First read metadata to know how many entries
  read_metadata_async([this, on_complete](bool success, const EventLogMetadata &meta) {
    if (!success) {
      if (on_complete) on_complete(false, std::vector<EventLogEntry>{});
      return;
    }

    uint16_t count = std::min(meta.available_entries, meta.max_entries);
    if (count == 0) {
      cached_entries_.clear();
      entries_cached_ = true;
      ESP_LOGI(TAG, "Event log is empty");
      if (on_complete) on_complete(true, cached_entries_);
      return;
    }

    ESP_LOGI(TAG, "Reading %d event log entries...", count);

    auto entries = std::make_shared<std::vector<EventLogEntry>>();
    auto read_next = std::make_shared<std::function<void(uint16_t)>>();

    *read_next = [this, entries, on_complete, count, read_next](uint16_t idx) {
      if (idx >= count) {
        cached_entries_ = *entries;
        entries_cached_ = true;
        ESP_LOGI(TAG, "Read %zu event log entries", entries->size());
        if (on_complete) on_complete(true, cached_entries_);
        return;
      }

      uint16_t sub_id = SUBID_ENTRY_BASE + idx;
      uint8_t apdu[5];
      apdu[0] = 0x0A;
      apdu[1] = 0x03;
      apdu[2] = OBJECT_EVENT_LOG;
      apdu[3] = (sub_id >> 8) & 0xFF;
      apdu[4] = sub_id & 0xFF;

      // Entry responses use OpSpec 0x14 with bytes 6-7=0x0000, 8-9=0xF402
      // Must allow register-read matching since OpSpec 0x14 is normally filtered
      static constexpr uint16_t ENTRY_MATCH_SUB = 0xF402;
      this->transport_.send_apdu_command(apdu, 5, TYPE_EVENT_LOG_INFO, ENTRY_MATCH_SUB,
          [this, idx, entries, on_complete, count, read_next](
              bool success, const uint8_t *payload, size_t payload_len) {
        if (success) {
          // Event log entries use OpSpec 0x14 (register-read format) — no 3-byte sub-header
          if (payload_len >= 16) {
            EventLogEntry entry = EventLogEntry::from_bytes(payload);
            ESP_LOGD(TAG, "Entry %d: %s cycle=%d ts=%u",
                     idx, entry.event_type_str(), entry.cycle_counter, entry.timestamp);
            if (entry.timestamp > 0) {
              entries->push_back(entry);
            }
          }
        } else {
          ESP_LOGW(TAG, "Entry %d read failed", idx);
        }
        (*read_next)(idx + 1);
      }, 5000, true);  // allow_register_read=true
    };

    (*read_next)(0);
  });
}

std::string EventLogService::format_display() const {
  if (!entries_cached_ || cached_entries_.empty()) {
    return "No events";
  }

  std::string result;
  for (const auto &e : cached_entries_) {
    time_t ts = (time_t)e.timestamp;
    struct tm tm_info;
    localtime_r(&ts, &tm_info);

    char buf[80];
    snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d %02d:%02d",
             e.event_type_str(),
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min);

    if (!result.empty()) result += "\n";
    result += buf;
  }
  return result;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
