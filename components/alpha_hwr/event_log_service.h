/**
 * Event Log Service for Grundfos ALPHA HWR Pumps
 *
 * Reads the pump's event log (start/stop events with timestamps).
 * Object 88, SubID 10199 (metadata), SubIDs 10200-10219 (entries).
 *
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/event_log.py
 */

#pragma once

#include "esphome/core/log.h"
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace core {
class Transport;
class Session;
}

namespace services {

/**
 * Event log entry parsed from pump response.
 *
 * Entry format (16 bytes):
 *   Bytes 0-1:   header (uint16 BE)
 *   Bytes 2-3:   field (uint16 BE)
 *   Byte  4:     cycle_counter (uint8)
 *   Byte  5:     unknown (uint8)
 *   Byte  6:     mode_byte (uint8)
 *   Bytes 7-8:   constants (uint16 BE)
 *   Byte  9:     event_type (0x01=start, 0x02=stop)
 *   Bytes 10-13: timestamp (uint32 BE Unix)
 *   Bytes 14-15: trailing (uint16 BE)
 */
struct EventLogEntry {
  uint8_t cycle_counter{0};
  uint8_t mode_byte{0};
  uint8_t event_type{0};   // 0x01=start, 0x02=stop
  uint32_t timestamp{0};

  static EventLogEntry from_bytes(const uint8_t *data) {
    EventLogEntry e;
    e.cycle_counter = data[4];
    e.mode_byte = data[6];
    e.event_type = data[9];
    e.timestamp = ((uint32_t)data[10] << 24) | ((uint32_t)data[11] << 16) |
                  ((uint32_t)data[12] << 8) | data[13];
    return e;
  }

  const char *event_type_str() const {
    switch (event_type) {
      case 0x01: return "Start";
      case 0x02: return "Stop";
      default: return "Unknown";
    }
  }
};

/**
 * Entries the event-log object can actually address (issue #284).
 *
 * Derived from the address map, not chosen: `geni_profile_52_7.xml` gives
 * `event_log_obj` as
 *
 *     <sub-id>10200</sub-id> <last-sub-id>11200</last-sub-id>
 *     <sub-ids-per-object>1</sub-ids-per-object>
 *
 * so entries live at 10200..11200 inclusive -- 1001 of them -- and 11201 onward
 * is a different part of the object. Reading past that is meaningless whatever
 * the pump claims, and there is no "is 1001 enough?" conversation to have,
 * which is the value of taking the number from the map rather than from a
 * judgement about what is reasonable.
 *
 * Recorded even though it is not the binding limit, because the two ceilings
 * below say different things and only one of them moves if the transport
 * changes.
 */
constexpr uint16_t EVENT_LOG_ADDRESSABLE_ENTRIES = 1001;

/**
 * Entries this component will actually walk.
 *
 * The binding ceiling, and it is NOT the address map's: it is
 * `core::Transport::MAX_ABANDON_STEPS`, the number of commands
 * `abandon_queue_()` will unwind in one call. Past that the remainder is
 * dropped silently and its callers are stranded without a terminal callback --
 * exactly the hazard issue #259 closed -- so a chain longer than the unwind cap
 * reintroduces it. `read_entries_async()` static_asserts the two agree.
 *
 * What this costs: on a pump reporting more than 512 entries the log is read
 * short, and the clamp says so at WARN rather than truncating silently. No
 * observed pump comes near it -- both this bench unit and the sibling client's
 * report 20 -- so this bound is unfalsifiable against real hardware, and its
 * whole value is in the shape. Before it, `count` could be 65,535: roughly 1.9
 * hours of reads at the corpus's reply latency, 91 hours if they go unanswered,
 * ~512 KB accumulated in `cached_entries_` on a part with a fraction of that
 * free, and a `uint16_t` sub-id that wraps from idx 55336 into a completely
 * different part of object 88's address space.
 */
constexpr uint16_t EVENT_LOG_MAX_CHAIN_ENTRIES = 256;

/**
 * Queue headroom the chain leaves under `Transport::MAX_ABANDON_STEPS`.
 *
 * The first cut of this set the chain to exactly MAX_ABANDON_STEPS, which was
 * wrong and was caught in review: that cap bounds the WHOLE drain, not this
 * chain's share of it. The chain is issued one command at a time, but each
 * failure callback queues the next and `abandon_queue_()` pulls re-queued
 * commands into the same drain -- so an abandon walks the entire remaining
 * chain, and anything else already queued walks with it. `TelemetryService::
 * poll()` alone appends five callback-less reads.
 *
 * At exactly the cap, a chain plus that traffic exhausts the drain before the
 * chain's terminal callback, which strands the caller -- the hazard #259 closed
 * -- and, since #284 added an in-flight flag, leaves `entries_reading_` set so
 * the event log is never read again for the life of the boot. That second
 * consequence is why the headroom is generous rather than exact: the cost of
 * being wrong went up when the flag arrived.
 */
constexpr uint16_t EVENT_LOG_ABANDON_HEADROOM = 64;

struct EventLogMetadata {
  uint16_t cycle_counter{0};
  uint16_t available_entries{0};
  uint16_t max_entries{20};
};

class EventLogService {
 public:
  EventLogService(core::Transport &transport, core::Session &session);
  ~EventLogService() = default;

  /**
   * Read event log metadata (Object 88, SubID 10199).
   * Returns cycle counter and number of available entries.
   */
  void read_metadata_async(std::function<void(bool, const EventLogMetadata &)> on_complete);

  /**
   * Read all event log entries from pump.
   * Reads metadata first, then entries sequentially.
   */
  void read_entries_async(std::function<void(bool, const std::vector<EventLogEntry> &)> on_complete);

  /**
   * Format event log as display string.
   */
  std::string format_display() const;

  const std::vector<EventLogEntry> &get_cached_entries() const { return cached_entries_; }
  bool is_cached() const { return entries_cached_; }

  /**
   * Release any in-flight chain state (issue #284).
   *
   * Belt and braces for `entries_reading_`. The flag is cleared on every
   * terminal path the chain has, including the abandoned one -- but the whole
   * point of #259 is that a drain CAN run out of steps and drop a callback, and
   * the headroom above makes that unlikely rather than impossible. A leaked
   * flag means the event log is never read again for the life of the boot,
   * which is a worse failure than the duplicate chain the flag prevents.
   *
   * A disconnect is the one moment we know nothing is in flight, so it is the
   * natural place to be sure. Called from the component's disconnect handler
   * alongside the other services' cache invalidation.
   */
  void on_disconnect() { entries_reading_ = false; }

 private:
  core::Transport &transport_;
  core::Session &session_;

  std::vector<EventLogEntry> cached_entries_;
  EventLogMetadata metadata_;
  bool entries_cached_{false};
  /// A chain is on the transport queue right now (issue #284).
  ///
  /// A clamp bounds ONE chain; it does nothing about N of them. The re-arm
  /// backoff bumps the read generation, which retires the timers but does not
  /// stop work already queued in the transport -- so a slow chain could have a
  /// second copy queued behind it, turning a slow read into an unbounded one.
  /// Same shape as `ControlService::limiters_reading_`, and the same remedy.
  ///
  /// Cleared on EVERY terminal path, the abandoned one included. A flag that
  /// leaks means the event log is never read again for the life of the boot,
  /// which is a worse failure than the one it prevents.
  bool entries_reading_{false};

  static constexpr const char *TAG = "alpha_hwr.event_log";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
