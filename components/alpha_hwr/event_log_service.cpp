/**
 * Event Log Service Implementation
 *
 * Reads pump start/stop event log via Object 88.
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/event_log.py
 */

#include "event_log_service.h"

#include "esphome/core/time.h"
#include "frame_builder.h"
#include "codec.h"
#include "transport.h"
#include "session.h"

#include <cinttypes>
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

  // Type 243 v1 (EventLogInfo). The reply's type header is 00 00 F3 01 at
  // bytes 6-9, so this value matches bytes 8-9 -- (TypeL << 8) | Version --
  // not bytes 6-7 as this comment used to say.
  static constexpr uint16_t TYPE_EVENT_LOG_INFO = 0xF301;
  transport_.send_apdu_command(apdu, sizeof(apdu), TYPE_EVENT_LOG_INFO, 0,
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
  // One chain at a time (issue #284). The clamp below bounds how long a single
  // chain can be; this is what stops a second one being queued behind it. The
  // re-arm backoff bumps the read generation, which retires the timers but does
  // NOT stop commands already sitting in the transport queue -- so without this
  // a slow chain gets a duplicate queued behind it and a slow read becomes an
  // unbounded one.
  if (entries_reading_) {
    ESP_LOGD(TAG, "Event log read already in flight; not queuing a second chain");
    if (on_complete) on_complete(false, cached_entries_);
    return;
  }
  entries_reading_ = true;

  // First read metadata to know how many entries
  read_metadata_async([this, on_complete](bool success, const EventLogMetadata &meta) {
    if (!success) {
      entries_reading_ = false;
      if (on_complete) on_complete(false, std::vector<EventLogEntry>{});
      return;
    }

    // Both operands are uint16 fields straight off the wire, and until issue
    // #284 nothing bounded either one -- so `count` could be 65,535 and the
    // chain would issue that many Class 10 reads. See EVENT_LOG_MAX_CHAIN_ENTRIES
    // for what that costs and why the ceiling is the transport's unwind cap
    // rather than the address map's larger 1001.
    static_assert(EVENT_LOG_MAX_CHAIN_ENTRIES + EVENT_LOG_ABANDON_HEADROOM <=
                      core::Transport::MAX_ABANDON_STEPS,
                  "the abandon cap bounds the WHOLE drain, not this chain's "
                  "share of it: a chain sized to the cap exactly leaves no room "
                  "for the telemetry reads that drain alongside it, and its "
                  "terminal callback is the one dropped (issues #259, #284)");
    static_assert(EVENT_LOG_MAX_CHAIN_ENTRIES <= EVENT_LOG_ADDRESSABLE_ENTRIES,
                  "the chain must not walk past the sub-ids the object holds");

    uint16_t count = std::min(meta.available_entries, meta.max_entries);
    if (count > EVENT_LOG_MAX_CHAIN_ENTRIES) {
      // Reported, never silent. A short read of a log is a different thing from
      // a complete one, and a user comparing entry counts against the pump's
      // own display needs to know which they are looking at.
      ESP_LOGW(TAG,
               "Pump reports %u event log entries (available=%u, max=%u); "
               "reading the first %u",
               (unsigned) count, (unsigned) meta.available_entries,
               (unsigned) meta.max_entries, (unsigned) EVENT_LOG_MAX_CHAIN_ENTRIES);
      count = EVENT_LOG_MAX_CHAIN_ENTRIES;
    }
    // Note what is NOT collapsed here: a metadata read that FAILED returned
    // early above with success=false, so reaching this line with count == 0
    // means the pump answered and said the log is empty. "We do not know" and
    // "there are none" stay different answers.
    if (count == 0) {
      cached_entries_.clear();
      entries_cached_ = true;
      entries_reading_ = false;
      ESP_LOGI(TAG, "Event log is empty");
      if (on_complete) on_complete(true, cached_entries_);
      return;
    }

    ESP_LOGI(TAG, "Reading %d event log entries...", count);

    auto entries = std::make_shared<std::vector<EventLogEntry>>();
    auto read_next = std::make_shared<std::function<void(uint16_t)>>();

    // See the note in HistoryService::read_trends_async(): capturing `read_next`
    // inside the closure it owns is a self-reference cycle that leaks the chain
    // once per invocation. The transport command queue holds the strong ref.
    std::weak_ptr<std::function<void(uint16_t)>> read_next_weak = read_next;

    *read_next = [this, entries, on_complete, count, read_next_weak](uint16_t idx) {
      auto self = read_next_weak.lock();
      if (!self) {
        // The chain was abandoned without reaching either terminal branch
        // below -- a disconnect dropped the transport's strong reference. The
        // flag has to be released here too, or the event log is never read
        // again for the life of the boot.
        entries_reading_ = false;
        return;
      }
      if (idx >= count) {
        // The same gate read_metadata_async() opens with -- which is the
        // first thing read_entries_async() calls -- applied at the other end.
        // See the note in HistoryService::read_trends_async(). A single
        // entry that fails to read is tolerated by design (the branch below
        // logs it and moves on), so a partially-read log is indistinguishable
        // from a complete one here unless the abandoned case is named. Since
        // issue #259 a disconnect mid-chain unwinds to this branch, and caching
        // three entries of twenty would leave the display claiming the log is
        // three entries long.
        if (!session_.is_ready()) {
          entries_reading_ = false;
          ESP_LOGD(TAG, "Event log read abandoned with %zu of %u entries; keeping the previous data",
                   entries->size(), (unsigned) count);
          if (on_complete) on_complete(false, cached_entries_);
          return;
        }
        cached_entries_ = *entries;
        entries_cached_ = true;
        entries_reading_ = false;
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

      // Entry responses carry the type header 00 00 F4 02 (Type 244 v2) and a
      // 20-byte body. That body length used to collide with the transport's
      // telemetry filter and the read needed allow_register_read=true to get its
      // own answer delivered; the filter now only guards wildcard commands, so
      // this exact-match read no longer needs the override.
      static constexpr uint16_t ENTRY_MATCH_OBJ = 0xF402;
      this->transport_.send_apdu_command(apdu, 5, ENTRY_MATCH_OBJ, 0,
          [idx, entries, on_complete, self](
              bool success, const uint8_t *payload, size_t payload_len) {
        if (success) {
          // Event log entries use OpSpec 0x14 (register-read format) — no 3-byte sub-header
          if (payload_len >= 16) {
            EventLogEntry entry = EventLogEntry::from_bytes(payload);
            ESP_LOGD(TAG, "Entry %d: %s cycle=%d ts=%" PRIu32,
                     idx, entry.event_type_str(), entry.cycle_counter, entry.timestamp);
            if (entry.timestamp > 0) {
              entries->push_back(entry);
            }
          }
        } else {
          ESP_LOGW(TAG, "Entry %d read failed", idx);
        }
        (*self)(idx + 1);
      }, 5000);
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
    // from_epoch_**utc**, and the distinction is the whole trap of issue #289.
    //
    // Event-log timestamps come off the wire RAW: EventLogEntry::from_bytes()
    // reads the four bytes and nothing converts them, because they are the
    // pump's OWN clock, which runs on local time. They are not UTC epochs in
    // our domain the way cached single events are (those go through
    // local_unix_to_utc_resolved() on read).
    //
    // So the right rendering is to take the value's fields verbatim -- which is
    // what from_epoch_utc() does -- and NOT to shift it again. from_epoch_local()
    // here would treat the pump's local wall clock as though it were UTC and
    // move it by the offset a second time.
    //
    // The old localtime_r() here shifted, and was wrong on BOTH targets -- on
    // the device too, because ESPHome overrides localtime_r() to use its parsed
    // zone (posix_tz.cpp), so it really did move these values. An earlier
    // version of this comment said the device was right by accident; it was
    // not. This is one of the two sites issue #289 genuinely fixed.
    const ESPTime lt = ESPTime::from_epoch_utc(static_cast<time_t>(e.timestamp));

    char buf[80];
    snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d %02d:%02d",
             e.event_type_str(),
             lt.year, lt.month, lt.day_of_month, lt.hour, lt.minute);

    if (!result.empty()) result += "\n";
    result += buf;
  }
  return result;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
