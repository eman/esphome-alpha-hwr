/**
 * Schedule Service Implementation for Grundfos ALPHA HWR Pump
 *
 * Implements schedule management operations including reading, writing,
 * enabling/disabling, and validation of weekly pump schedules.
 *
 * Based on: reference/alpha-hwr/src/alpha_hwr/services/schedule.py (931 lines)
 */

#include "schedule_service.h"
#include "codec.h"
#include "schedule_codec.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "frame_builder.h"
#include "session.h"
#include "transport.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>

namespace esphome {
namespace alpha_hwr {
namespace services {

// Single-event timestamps cross the wire in the pump's LOCAL-Unix clock domain
// while SingleEvent itself always holds UTC, so both directions shift by the
// local UTC offset -- but they do NOT share one offset, and the description
// that used to sit here (a single "now" offset applied to both, exact by
// construction) has not been true since issue #179's companion fix.
//
// Writes resolve the offset at the event's own UTC instant, which they have.
// Reads have only the wire-local value, so they refine: an approximate offset
// from the local value, then the real one from the approximate UTC. See
// local_unix_to_utc_resolved() in schedule_service.h, which also records what
// the round trip does and does not guarantee across a DST boundary.
//
// local_utc_offset_seconds() lives in that header too, beside the conversions
// that use it, so the host tests can reach it directly.

// Day names for parsing schedule entries
static const char *DAY_NAMES[7] = {"Monday",   "Tuesday", "Wednesday",
                                   "Thursday", "Friday",  "Saturday",
                                   "Sunday"};

// -------------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------------

ScheduleService::ScheduleService(core::Transport &transport,
                                 core::Session &session)
    : transport_(transport), session_(session), schedule_callback_(nullptr),
      write_callback_(nullptr), schedule_state_cached_(false),
      schedule_enabled_(false), last_state_poll_ms_(0),
      overview_cached_(false) {
  memset(overview_structure_, 0, sizeof(overview_structure_));
  memset(layer_cached_, 0, sizeof(layer_cached_));
  memset(cached_layer_data_, 0, sizeof(cached_layer_data_));
  ESP_LOGD(TAG, "ScheduleService initialized");
}

// -------------------------------------------------------------------------
// Schedule State Operations
// -------------------------------------------------------------------------

bool ScheduleService::get_state(bool *result) {
  if (!result) {
    ESP_LOGE(TAG, "get_state() called with null result pointer");
    return false;
  }

  if (!this->schedule_state_cached_) {
    ESP_LOGV(TAG, "Schedule state not cached yet");
    return false;
  }

  *result = this->schedule_enabled_;
  return true;
}

bool ScheduleService::poll_state_async(std::function<void(bool)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGV(TAG, "Cannot poll schedule state: session not ready");
    if (on_complete)
      on_complete(false);
    return false;
  }

  // Runs on the telemetry cadence; at DEBUG this line alone is one API frame
  // per subscriber every poll, so keep the routine case at VERBOSE (#127).
  ESP_LOGV(TAG, "Polling schedule state (Object 84, SubID 1)...");

  // Build Class 10 READ request for Object 84, SubID 1
  uint8_t apdu[5];
  apdu[0] = 0x0A;
  apdu[1] = 0x03;
  apdu[2] = 84;
  apdu[3] = 0x00;
  apdu[4] = 0x01;

  // IMPORTANT: Pump responds with SubID 0, not SubID 1 that we requested!
  this->transport_.send_apdu_command(
      apdu, 5, 0xDA01, 0,
      [this, on_complete](bool success, const uint8_t *payload, size_t payload_len) {
        if (!success) {
          ESP_LOGW(TAG, "Failed to poll schedule state (timeout)");
          if (on_complete)
            on_complete(false);
          return;
        }

        if (payload_len >= 13) {
          // This poll runs on the telemetry cadence, so it is mostly a no-op
          // confirmation. Only announce an actual transition (or the first
          // cached value): the callback republishes the schedule text sensors,
          // and an unconditional republish of identical state is an API frame
          // per subscriber every poll for nothing (issue #127).
          const bool changed = !this->schedule_state_cached_ ||
                               this->schedule_enabled_ != (payload[7] != 0);

          this->schedule_enabled_ = (payload[7] != 0);
          this->schedule_state_cached_ = true;
          memcpy(this->overview_structure_, payload + 3, 10);
          this->overview_cached_ = true;

          if (changed) {
            ESP_LOGD(TAG, "Schedule state updated: %s",
                     this->schedule_enabled_ ? "enabled" : "disabled");
          } else {
            ESP_LOGV(TAG, "Schedule state unchanged: %s",
                     this->schedule_enabled_ ? "enabled" : "disabled");
          }

          if (changed && this->state_change_callback_) {
            this->state_change_callback_(this->schedule_enabled_);
          }
          if (on_complete)
            on_complete(true);
        } else {
          ESP_LOGW(TAG, "Schedule state response too short (%zu bytes)",
                   payload_len);
          if (on_complete)
            on_complete(false);
        }
      });

  return true;
}

void ScheduleService::set_state_async(bool enable, std::function<void(bool)> on_sent) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot set schedule state: session not ready");
    if (on_sent)
      on_sent(false);
    return;
  }

  // Require the real overview: blind RMW of the
  // 10-byte ClockProgramOverview from hardcoded defaults could overwrite pump
  // settings we never read (the issue-#92 clobber class). The operation layer
  // polls the overview first, so this is only a safety net.
  if (!this->overview_cached_) {
    ESP_LOGE(TAG, "Cannot set schedule state: ClockProgramOverview not cached");
    if (on_sent)
      on_sent(false);
    return;
  }

  ESP_LOGI(TAG, "%s schedule (verified path)...", enable ? "Enabling" : "Disabling");

  uint8_t structure_bytes[10];
  memcpy(structure_bytes, this->overview_structure_, 10);
  structure_bytes[4] = enable ? 0x01 : 0x00;
  // Force default_action = Stop (SchedulingActionType 0x01), matching the
  // Grundfos app, instead of preserving whatever is on the pump. The pump idles
  // between windows and runs during the Auto windows we write. A stale
  // default_action of Auto (0x02) makes the app render the whole schedule as
  // "pump will be idle" (bench-confirmed 2026-07-22). Keep the cache in sync
  // with what we write so later cache reuses stay consistent.
  //
  // This note used to live in set_state(), which both overview-write paths
  // pointed at. That method is gone as dead code, so the explanation lives here
  // now -- at the first of the two sites that actually writes the byte, rather
  // than in a footnote to something deleted.
  structure_bytes[5] = 0x01;
  this->overview_structure_[5] = 0x01;  // keep cache consistent

  // Class 10 OpSpec 0x93, Object 84, SubID 1,
  // Type 218 (ClockProgramOverview) — sent with a callback so the caller gets a
  // completion signal.
  uint8_t apdu[21];
  apdu[0] = 0x0A;
  apdu[1] = 0x93;
  apdu[2] = 84;
  apdu[3] = 0x00;
  apdu[4] = 0x01;
  apdu[5] = 0x00;
  apdu[6] = 0xDA;
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;
  apdu[10] = 0x0A;
  memcpy(apdu + 11, structure_bytes, 10);

  // Awaited as the short Class 10 ACK it actually gets (issue #253).
  //
  // This asked for a reply carrying type 0xDA01, which a SET reply cannot carry
  // -- "the SET operation never returns anything but the APDU Head" (GENIbus
  // App. Prog. Manual fig 3.5 note 1) -- so it timed out at 3 s on every write
  // and the note below was written to explain the silence. The captures say the
  // opposite: 34 writes to this address in resources/traffic_capture, every one
  // answered in 50-193 ms with the ordinary short ACK.
  //
  // The verdict is unchanged and still comes from the readback. What changes is
  // that the acknowledgement is consumed by the write that earned it rather than
  // left for the next Class 10 write, and that the write settles in tens of
  // milliseconds rather than three seconds.
  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0, 0,
      [enable, on_sent](bool acked, const uint8_t * /*data*/, size_t /*len*/) {
        // The authoritative confirm is still the caller's poll_state_async()
        // readback, so this reports "sent" whether or not the pump answered.
        ESP_LOGD(TAG, "Schedule %s write %s", enable ? "enable" : "disable",
                 acked ? "ACKed" : "window closed (verify via readback)");
        // Deliberately do not touch overview_structure_[4] either. Operations
        // are serialized, so no commit can run between this write and the
        // caller's confirm poll -- but if every confirm poll times out, the
        // cached overview would keep an unverified byte and the next unrelated
        // schedule commit would silently re-apply the timed-out enable/disable.
        // poll_state_async() copies the whole overview back authoritatively.
        // Deliberately do NOT assert schedule_enabled_/schedule_state_cached_
        // from the *request*. This callback runs whether or not the write was
        // ACKed, so recording the requested value as cached truth made the
        // cache claim a state the pump may never have taken -- and callers gate
        // on that cache. Concretely: a retry of a failed enable then saw
        // "already in the requested state", skipped the write entirely, and
        // still settled ACCEPTED, so the documented idempotent recovery path
        // was a no-op exactly when it was needed. current_hash() folds the same
        // flag in, so the settle event's hash agreed with the lie. The
        // authoritative update is the caller's poll_state_async() readback.
        if (on_sent)
          on_sent(true);
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);
}

void ScheduleService::read_single_event_async(
    uint8_t index, std::function<void(bool, const SingleEvent &)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read single event: session not ready");
    if (on_complete)
      on_complete(false, SingleEvent{});
    return;
  }

  uint16_t sub_id = 900 + index;
  uint8_t apdu[5];
  apdu[0] = 0x0A;
  apdu[1] = 0x03;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;

  this->transport_.send_apdu_command(
      apdu, 5, 0xDC01, 0,
      [index, on_complete](bool success, const uint8_t *payload, size_t payload_len) {
        if (!success || payload_len < 13) {
          if (on_complete)
            on_complete(false, SingleEvent{});
          return;
        }
        // Shift pump LOCAL-Unix -> UTC so callers (confirm, cache, display)
        // always see UTC (see utc_to_local_unix).
        SingleEvent ev = SingleEvent::from_bytes(payload + 3, index);
        ev.begin_timestamp = local_unix_to_utc_resolved(ev.begin_timestamp);
        ev.end_timestamp = local_unix_to_utc_resolved(ev.end_timestamp);
        if (on_complete)
          on_complete(true, ev);
      },
      3000);
}

bool ScheduleService::send_configuration_commit() {
  // Send configuration commit packet to persist schedule changes
  // This MUST be called after any schedule write operation (OpSpec 0xB3)
  //
  // Protocol: Class 10, OpSpec 0x93 (SET, Length 19), Object 84, SubID 1
  // Payload: 10-byte ClockProgramOverview structure
  //
  // Reference: reference/alpha-hwr/src/alpha_hwr/services/control.py:1038
  // Hex: 0A9354000100DA0100000A[10 bytes structure]

  ESP_LOGD(TAG, "Sending configuration commit...");

  // Use cached ClockProgramOverview structure if available, otherwise use
  // defaults This preserves all pump settings while committing schedule changes
  uint8_t structure_bytes[10];

  if (this->overview_cached_) {
    memcpy(structure_bytes, this->overview_structure_, 10);
    // default_action = Stop; see the note in set_state_async() for why this is
    // forced rather than preserved.
    structure_bytes[5] = 0x01;
    this->overview_structure_[5] = 0x01;  // keep cache consistent
    ESP_LOGD(TAG, "Using cached ClockProgramOverview structure for commit");
  } else {
    ESP_LOGW(TAG, "Cannot commit configuration: ClockProgramOverview not yet cached. Ignoring commit to prevent corruption.");
    return false;
  }

  // Build APDU: Class 10 SET command for Object 84, SubID 1
  uint8_t apdu[21];
  apdu[0] = 0x0A; // Class 10
  apdu[1] = 0x93; // OpSpec 0x93 (SET, Length 19)
  apdu[2] = 84;   // Object 84 (schedule object)
  apdu[3] = 0x00; // SubID high byte
  apdu[4] = 0x01; // SubID low byte (SubID = 1)
  apdu[5] = 0x00; // Reserved
  apdu[6] = 0xDA; // Type 218 (0xDA01 = ClockProgramOverview)
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;  // Size high byte
  apdu[10] = 0x0A; // Size low byte (10 bytes)

  // Append the 10-byte structure
  memcpy(apdu + 11, structure_bytes, 10);

  // Write configuration commit
  this->write_class10_command(apdu, 21);

  ESP_LOGD(TAG, "Configuration commit sent successfully");
  return true;
}

// -------------------------------------------------------------------------
// Schedule Entry Operations
// -------------------------------------------------------------------------


bool ScheduleService::read_entries_async(
    int layer,
    std::function<void(bool success, const std::vector<ScheduleEntry> &entries)>
        on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read schedule entries: session not ready");
    return false;
  }

  // Special handling for layer=-1: read all layers
  if (layer == -1) {
    ESP_LOGD(TAG, "Reading schedule entries from all layers (async)...");

    struct ReadAllState {
      ScheduleService *service;
      uint8_t current_layer = 0;
      // Counted explicitly rather than inferred from all_entries.size():
      // a layer that reads back cleanly but holds no enabled entries is a
      // legitimate success with an empty contribution, so emptiness says
      // nothing about whether the read worked.
      uint8_t layers_ok = 0;
      uint8_t layers_failed = 0;
      // Guards the terminal callback. Both the completion path and the
      // queue-failure path below can fire on_complete, and callers settle a
      // write operation from it; firing twice would settle twice.
      bool terminated = false;
      std::vector<ScheduleEntry> all_entries;
      std::function<void(bool success, const std::vector<ScheduleEntry> &)> on_complete;
    };

    auto state = std::make_shared<ReadAllState>();
    state->service = this;
    state->on_complete = on_complete;

    auto read_next_layer = [](auto& self, std::shared_ptr<ReadAllState> st) -> void {
      if (st->current_layer > 4) {
        // All-or-nothing: callers treat this boolean as "the pump's schedule
        // is now known" and act on it -- publishing the schedule hash,
        // settling refresh_schedule as ACCEPTED. A partially-read grid does
        // not support either claim, so any failed layer fails the whole read.
        // Entries from the layers that did succeed are still passed through
        // for logging.
        bool all_ok = st->layers_failed == 0;
        ESP_LOGD(TAG,
                 "Read %zu total schedule entries from all layers "
                 "(%u/5 layers OK)",
                 st->all_entries.size(), st->layers_ok);
        if (!all_ok)
          ESP_LOGW(TAG, "%u of 5 schedule layers failed to read; "
                        "reporting the read as failed",
                   st->layers_failed);
        if (st->on_complete && !st->terminated) {
          st->terminated = true;
          st->on_complete(all_ok, st->all_entries);
        }
        return;
      }

      int layer_to_read = st->current_layer;
      bool queued = st->service->read_entries_async(
          layer_to_read,
          [st, self](bool success, const std::vector<ScheduleEntry> &entries) {
            if (success) {
              st->layers_ok++;
              for (const auto &entry : entries) {
                st->all_entries.push_back(entry);
              }
              ESP_LOGV(TAG, "Layer %d contributed %zu entries", st->current_layer,
                       entries.size());
            } else {
              st->layers_failed++;
              ESP_LOGW(TAG,
                       "Failed to read layer %d (continuing with other layers)",
                       st->current_layer);
            }

            st->current_layer++;
            self(self, st);
          });

      if (!queued) {
        // read_entries_async() returns false without invoking the callback
        // (e.g. session not ready), which would otherwise stall this chain
        // forever and never call the original on_complete. Fail the whole
        // read explicitly instead.
        ESP_LOGW(TAG, "Failed to queue read for layer %d - aborting read-all-layers",
                 layer_to_read);
        if (st->on_complete && !st->terminated) {
          st->terminated = true;
          st->on_complete(false, st->all_entries);
        }
      }
    };

    read_next_layer(read_next_layer, state);
    return true;
  }

  if (layer < 0 || layer > 4) {
    ESP_LOGE(TAG, "Invalid layer %d (must be 0-4 or -1 for all)", layer);
    return false;
  }

  ESP_LOGD(TAG, "Reading schedule entries for layer %d (async)...", layer);

  uint16_t sub_id = 1000 + layer;

  uint8_t apdu[5];
  apdu[0] = 0x0A;
  apdu[1] = 0x03;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;

  this->transport_.send_apdu_command(
      apdu, 5, 0xDE01, 0,
      [this, on_complete, layer](bool success, const uint8_t *payload,
                                 size_t payload_len) {
        ESP_LOGD(TAG,
                 "Schedule entry read callback: success=%d, payload_len=%zu",
                 success, payload_len);
        std::vector<ScheduleEntry> entries;
        if (!success) {
          ESP_LOGW(TAG,
                   "Failed to read schedule entries for layer %d (timeout)",
                   layer);
          if (on_complete)
            on_complete(false, entries);
          return;
        }

        if (payload_len < 45) {
          ESP_LOGW(TAG, "Schedule entries response too short (%zu bytes)",
                   payload_len);
          if (on_complete)
            on_complete(false, entries);
          return;
        }

        // Cache raw layer data (42 bytes starting after 3-byte header)
        memcpy(this->cached_layer_data_[layer], payload + 3, 42);
        this->layer_cached_[layer] = true;

        const uint8_t *entry_data = payload + 3;
        for (int day_idx = 0; day_idx < 7; day_idx++) {
          size_t offset = day_idx * 6;
          const uint8_t *entry_bytes = entry_data + offset;
          if (entry_bytes[0] != 0) {
            entries.push_back(ScheduleEntry::from_bytes(
                entry_bytes, DAY_NAMES[day_idx], layer));
          }
        }

        ESP_LOGD(TAG, "Read %d enabled entries from layer %d (async)",
                 (int)entries.size(), layer);
        if (on_complete)
          on_complete(true, entries);
      });

  return true;
}

// -------------------------------------------------------------------------
// Validation Methods
// -------------------------------------------------------------------------

bool ScheduleService::validate_entries(
    const std::vector<ScheduleEntry> &entries,
    std::vector<std::string> *errors) {
  errors->clear();

  // Validate each entry's time range
  for (size_t i = 0; i < entries.size(); i++) {
    const auto &entry = entries[i];

    // Check valid day name
    bool valid_day = entry.get_day_index() >= 0;
    if (!valid_day) {
      char buf[128];
      snprintf(buf, sizeof(buf), "Entry %zu: Invalid day name '%s'", i,
               entry.get_day());
      errors->push_back(std::string(buf));
    }

    // Check valid layer
    if (entry.get_layer() > 4) {
      char buf[128];
      snprintf(buf, sizeof(buf),
               "Entry %zu (%s): Invalid layer %d (must be 0-4)", i,
               entry.get_day(), entry.get_layer());
      errors->push_back(std::string(buf));
    }

    // Check valid time range
    std::string error_msg;
    if (!entry.is_valid_time_range(&error_msg)) {
      char buf[256];
      snprintf(buf, sizeof(buf), "Entry %zu (%s %s-%s): %s", i,
               entry.get_day(), entry.get_begin_time().c_str(),
               entry.get_end_time().c_str(), error_msg.c_str());
      errors->push_back(std::string(buf));
    }
  }

  // Check for overlaps - only compare enabled entries
  for (size_t i = 0; i < entries.size(); i++) {
    const auto &entry1 = entries[i];
    if (!entry1.is_enabled())
      continue;

    for (size_t j = i + 1; j < entries.size(); j++) {
      const auto &entry2 = entries[j];
      if (!entry2.is_enabled())
        continue;

      if (entry1.overlaps_with(entry2)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Overlap detected: %s layer %d: %s-%s overlaps with %s-%s",
                 entry1.get_day(), entry1.get_layer(),
                 entry1.get_begin_time().c_str(), entry1.get_end_time().c_str(),
                 entry2.get_begin_time().c_str(),
                 entry2.get_end_time().c_str());
        errors->push_back(std::string(buf));
      }
    }
  }

  // Check layer counts (warn if too many entries per day/layer)
  int day_layer_counts[7][5] = {}; // [day_index][layer] -> enabled entry count
  for (const auto &entry : entries) {
    if (!entry.is_enabled())
      continue;
    int day_idx = entry.get_day_index();
    uint8_t layer = entry.get_layer();
    if (day_idx < 0 || layer > 4)
      continue; // Already reported as an error above
    day_layer_counts[day_idx][layer]++;
  }

  for (int day_idx = 0; day_idx < 7; day_idx++) {
    for (int layer = 0; layer < 5; layer++) {
      int count = day_layer_counts[day_idx][layer];
      if (count > 10) { // Arbitrary high limit
        char buf[256];
        snprintf(
            buf, sizeof(buf),
            "Warning: %s layer %d has %d entries. This may exceed pump capacity.",
            DAY_NAMES[day_idx], layer, count);
        errors->push_back(std::string(buf));
      }
    }
  }

  return errors->empty();
}

// -------------------------------------------------------------------------
// Internal Helper Methods
// -------------------------------------------------------------------------

void ScheduleService::write_class10_command(const uint8_t *apdu,
                                            size_t apdu_len) {
  // The ClockProgramOverview commit, and the only caller of this helper.
  //
  // Awaited since issue #253. The callback exists to make the transport wait --
  // a null one means the command is popped the moment its last chunk goes out --
  // and reports nothing onward, because this helper has no caller that could act
  // on it: send_configuration_commit() returns true for "sent" and its own
  // callers confirm by re-reading the schedule.
  //
  // The commit is the most frequent Class 10 SET this component makes: every
  // setpoint write, every control request and every layer write schedules one.
  // Leaving it unawaited meant one unclaimed acknowledgement per schedule
  // change, in the exact shape the next write's matcher accepts (issue #248).
  // 34 instances in resources/traffic_capture, every one answered in 50-193 ms.
  this->transport_.send_apdu_command(
      apdu, apdu_len, 0, 0,
      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {
        ESP_LOGV(TAG, "Configuration commit %s", success ? "acknowledged" : "unanswered");
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);
}

// -------------------------------------------------------------------------
// Display & Formatting Methods
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// Cached Entry Access
// -------------------------------------------------------------------------

bool ScheduleService::get_cached_entry(uint8_t layer, uint8_t day_index,
                                       ScheduleEntry *entry) const {
  if (layer > 4 || day_index > 6 || !entry)
    return false;
  if (!layer_cached_[layer])
    return false;

  const uint8_t *bytes = cached_layer_data_[layer] + (day_index * 6);
  *entry = ScheduleEntry::from_bytes(bytes, DAY_NAMES[day_index], layer);
  return true;
}

void ScheduleService::set_entry_async(uint8_t layer, uint8_t day_index,
                                      const ScheduleEntry &entry,
                                      std::function<void(bool)> on_complete) {
  if (layer > 4 || day_index > 6) {
    ESP_LOGE(TAG, "Invalid layer %d or day %d", layer, day_index);
    if (on_complete)
      on_complete(false);
    return;
  }

  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot set entry: session not ready");
    if (on_complete)
      on_complete(false);
    return;
  }

  if (layer_cached_[layer]) {
    // Cache hit: modify cached data and write immediately
    entry.to_bytes(cached_layer_data_[layer] + (day_index * 6));
    ESP_LOGI(TAG, "Setting %s on layer %d: %s-%s (%s)", DAY_NAMES[day_index],
             layer, entry.get_begin_time().c_str(),
             entry.get_end_time().c_str(),
             entry.is_enabled() ? "enabled" : "disabled");
    write_cached_layer_async(layer, on_complete);
  } else {
    // Cache miss: read layer first, then modify and write
    ESP_LOGD(TAG, "Layer %d not cached, reading first...", layer);
    read_entries_async(
        layer, [this, layer, day_index, entry,
                on_complete](bool success, const std::vector<ScheduleEntry> &) {
          if (!success || !layer_cached_[layer]) {
            ESP_LOGE(TAG, "Failed to read layer %d for set_entry", layer);
            if (on_complete)
              on_complete(false);
            return;
          }
          // Now cached; modify and write
          ScheduleEntry mutable_entry = entry;
          mutable_entry.to_bytes(cached_layer_data_[layer] + (day_index * 6));
          write_cached_layer_async(layer, on_complete);
        });
  }
}

void ScheduleService::write_layer_image_async(
    uint8_t layer, const uint8_t image[42],
    std::function<void(bool)> on_complete) {
  if (layer > 4 || !layer_cached_[layer]) {
    ESP_LOGE(TAG, "write_layer_image: layer %d not cached", layer);
    if (on_complete)
      on_complete(false);
    return;
  }
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "write_layer_image: session not ready");
    if (on_complete)
      on_complete(false);
    return;
  }
  memcpy(cached_layer_data_[layer], image, 42);
  write_cached_layer_async(layer, on_complete);
}

void ScheduleService::write_cached_layer_async(
    uint8_t layer, std::function<void(bool)> on_complete) {
  if (layer > 4 || !layer_cached_[layer]) {
    if (on_complete)
      on_complete(false);
    return;
  }

  uint16_t sub_id = 1000 + layer;
  uint8_t apdu[53];
  apdu[0] = 0x0A;
  apdu[1] = 0xB3;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;
  apdu[5] = 0x00;
  apdu[6] = 0xDE;
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;
  apdu[10] = 0x2A;
  memcpy(apdu + 11, cached_layer_data_[layer], 42);

  ESP_LOGI(TAG, "Writing cached layer %d to pump...", layer);

  // This write DOES get an answer, and it always did (issue #253).
  //
  // What stood here was `0xDE01, 0` -- wait for a reply carrying type 0xDE01,
  // the ClockProgramLayer object -- with a 3 s window and quiet_timeout set,
  // explained as "fire-and-forget write (pump commits on timeout)". That
  // explanation was built backwards from a symptom. The reply never came
  // because a SET reply cannot carry a type: "the SET operation never returns
  // anything but the APDU Head" (GENIbus App. Prog. Manual fig 3.5 note 1). So
  // the command waited out its full 3 s every time, the timeout was hidden at
  // DEBUG by quiet_timeout, and the callback below reported success from the
  // timeout path -- which is why it looked like it worked.
  //
  // resources/traffic_capture settles it directly: 20 layer writes across the
  // five layers, every one answered in 36-142 ms with the ordinary short Class
  // 10 ACK, `24 05 F8 E7 0A 01 00 AE A2` -- the same nine bytes every other SET
  // is answered with.
  //
  // Two things follow. A full schedule write settles ~3 s per layer sooner, five
  // layers at a time. And since issue #254 those bogus timeouts were recording a
  // reply debt each, so a write that was answered promptly was also, on paper,
  // owed a reply -- and the next Class 10 write within STALE_REPLY_WINDOW_MS
  // paid for it.
  //
  // on_complete(true) unconditionally is DELIBERATELY unchanged. `success` now
  // means "answered", not "accepted", and this write's callers confirm by
  // re-reading the layer; turning silence into a failure verdict here is the
  // #234 mistake and belongs to whoever changes that contract on purpose.
  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0, 0,
      [this, on_complete, layer](bool success, const uint8_t * /*data*/,
                                 size_t /*len*/) {
        ESP_LOGV(TAG, "Layer %d write %s", layer, success ? "acknowledged" : "unanswered");
        // Send configuration commit after write
        this->send_configuration_commit();
        ESP_LOGI(TAG, "Layer %d write + config commit sent", layer);
        if (on_complete)
          on_complete(true);
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);
}

// -------------------------------------------------------------------------
// Single Event Operations
// -------------------------------------------------------------------------

void ScheduleService::read_single_events_async(
    std::function<void(bool, const std::vector<SingleEvent> &)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read single events: session not ready");
    if (on_complete)
      on_complete(false, std::vector<SingleEvent>{});
    return;
  }

  uint8_t max_events = get_max_single_events();
  ESP_LOGD(TAG, "Reading up to %d single events from pump...", max_events);

  // Read single events sequentially to avoid flooding the transport queue
  auto events = std::make_shared<std::vector<SingleEvent>>();
  // Counted explicitly: an empty result is legitimate for a pump with no
  // active events, so emptiness cannot stand in for "the read failed".
  auto slots_failed = std::make_shared<uint8_t>(0);
  auto read_next = std::make_shared<std::function<void(uint8_t)>>();

  // See the note in HistoryService::read_trends_async(): capturing `read_next`
  // inside the closure it owns is a self-reference cycle that leaks the chain
  // once per invocation. This is the one of the three that also repeats without
  // a reconnect -- refresh_single_events is a public HA service.
  std::weak_ptr<std::function<void(uint8_t)>> read_next_weak = read_next;

  *read_next = [this, events, slots_failed, on_complete, max_events,
                read_next_weak](uint8_t idx) {
    auto self = read_next_weak.lock();
    if (!self)
      return;  // chain abandoned (disconnect)
    if (idx >= max_events) {
      // All-or-nothing. single_events_cached_ is what
      // find_free_single_event_slot() gates on, and an unread slot looks
      // free -- so caching a partial read lets a later set_single_event
      // overwrite a live event that was simply never read back. That is the
      // clobber class issue #92 exists to prevent, so a partial read must
      // leave the previous cache (and its cached flag) untouched.
      bool all_ok = *slots_failed == 0;
      if (all_ok) {
        this->cached_single_events_ = *events;
        this->single_events_cached_ = true;
        ESP_LOGD(TAG, "Read %zu active single events (of %d slots)",
                 events->size(), max_events);
      } else {
        ESP_LOGW(TAG,
                 "%u of %d single event slots failed to read; leaving the "
                 "cache untouched and reporting the read as failed",
                 *slots_failed, max_events);
      }
      if (on_complete)
        on_complete(all_ok, *events);
      return;
    }

    uint16_t sub_id = 900 + idx;
    uint8_t apdu[5];
    apdu[0] = 0x0A;
    apdu[1] = 0x03;
    apdu[2] = 84;
    apdu[3] = (sub_id >> 8) & 0xFF;
    apdu[4] = sub_id & 0xFF;

    this->transport_.send_apdu_command(
      apdu, 5, 0xDC01, 0,
        [idx, events, slots_failed, on_complete,
         self](bool success, const uint8_t *payload, size_t payload_len) {
          if (!success || payload_len < 13) {
            (*slots_failed)++;
            ESP_LOGW(TAG, "Failed to read single event slot %d "
                          "(continuing with other slots)",
                     idx);
          } else {
            SingleEvent ev = SingleEvent::from_bytes(payload + 3, idx);
            // pump LOCAL-Unix -> UTC so the cache/display stay UTC.
            ev.begin_timestamp = local_unix_to_utc_resolved(ev.begin_timestamp);
            ev.end_timestamp = local_unix_to_utc_resolved(ev.end_timestamp);
            if (ev.enabled) {
              events->push_back(ev);
              ESP_LOGD(TAG, "Single event slot %d: active (%" PRIu32 " - %" PRIu32 ")", idx,
                       ev.begin_timestamp, ev.end_timestamp);
            }
          }
          // Read next slot
          (*self)(idx + 1);
        },
        3000);
  };

  (*read_next)(0);
}

void ScheduleService::write_single_event_async(
    const SingleEvent &event, std::function<void(bool)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot write single event: session not ready");
    if (on_complete)
      on_complete(false);
    return;
  }

  uint16_t sub_id = 900 + event.index;
  ESP_LOGI(TAG, "Writing single event to slot %d (SubID %d): %s %" PRIu32 "-%" PRIu32,
           event.index, sub_id, event.enabled ? "enabled" : "disabled",
           event.begin_timestamp, event.end_timestamp);

  uint8_t apdu[21]; // 11 header + 10 data
  apdu[0] = 0x0A;
  // OpSpec 0x93 = SET + 19 payload bytes, which is what this frame carries
  // (1 obj + 2 sub + 2 type + 1 version + 3 size + 10 data). It sent 0xB3 --
  // SET + 51 -- copied from the layer write below, whose 53-byte APDU really
  // does carry 51. Bench-verified that this pump accepts either, so it was
  // never a visible failure; see the header note.
  apdu[1] = 0x93;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;
  apdu[5] = 0x00;
  apdu[6] = 0xDC; // Type 220
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;
  apdu[10] = 0x0A; // Size: 10 bytes
  // Shift UTC -> pump LOCAL-Unix before serializing (the pump's RTC is local
  // Unix time). Serialize a copy so the caller's event stays UTC. ts 0
  // (disabled/cleared) is left untouched by utc_to_local_unix.
  SingleEvent wire = event;
  wire.begin_timestamp = utc_to_local_unix(
      event.begin_timestamp, local_utc_offset_seconds((time_t) event.begin_timestamp));
  wire.end_timestamp = utc_to_local_unix(
      event.end_timestamp, local_utc_offset_seconds((time_t) event.end_timestamp));
  wire.to_bytes(apdu + 11);

  // Awaited as the short Class 10 ACK it actually gets (issue #253) -- the same
  // correction as set_state_async() and the layer write above. This asked for
  // type 0xDC01, which a SET reply cannot carry, so every single-event write
  // burned a full 3 s window. With up to 35 slots that is the dominant cost of a
  // single-event sweep, and WATCHDOG_SINGLE_EVENT_MS was sized around it.
  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0, 0,
      [this, on_complete, event](bool /*success*/, const uint8_t * /*data*/,
                                 size_t /*len*/) {
        this->send_configuration_commit();
        ESP_LOGI(TAG, "Single event slot %d write + commit sent", event.index);

        // Update cache
        if (single_events_cached_) {
          auto &cache = cached_single_events_;
          cache.erase(std::remove_if(cache.begin(), cache.end(),
                                     [&event](const SingleEvent &e) {
                                       return e.index == event.index;
                                     }),
                      cache.end());
          if (event.enabled) {
            cache.push_back(event);
          }
        }

        if (on_complete)
          on_complete(true);
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);
}

int ScheduleService::find_free_single_event_slot(
    uint32_t reusable_before_ts) const {
  uint8_t max_events = overview_cached_ ? overview_structure_[1] : 35;
  // A cold cache means every slot "looks free", so answering 0 here hands the
  // caller a live slot to overwrite -- the clobber class issue #92 exists to
  // prevent. -1 is this function's own "none available" answer; use it, and let
  // the caller warm the cache first.
  if (!single_events_cached_)
    return -1;

  std::set<uint8_t> used;
  for (const auto &ev : cached_single_events_) {
    if (reusable_before_ts > 0 &&
        (!ev.enabled || ev.end_timestamp < reusable_before_ts)) {
      continue;  // disabled or expired — reusable
    }
    used.insert(ev.index);
  }
  for (uint8_t i = 0; i < max_events; i++) {
    if (used.find(i) == used.end())
      return i;
  }
  return -1;
}

std::string ScheduleService::format_single_events_display() const {
  if (!single_events_cached_ || cached_single_events_.empty()) {
    return "No single events";
  }

  std::string result;
  for (const auto &ev : cached_single_events_) {
    if (!ev.enabled)
      continue;
    time_t begin_t = (time_t)ev.begin_timestamp;
    time_t end_t = (time_t)ev.end_timestamp;
    struct tm begin_tm, end_tm;
    localtime_r(&begin_t, &begin_tm);
    localtime_r(&end_t, &end_tm);

    // action 0x01 = Stop (pump off / vacation), 0x02 = Auto (one-time run).
    const char *action = ev.action == 0x01 ? "off" : "run";
    char buf[96];
    snprintf(buf, sizeof(buf), "[%d] %04d-%02d-%02d %02d:%02d - %02d:%02d (%s)",
             ev.index, begin_tm.tm_year + 1900, begin_tm.tm_mon + 1,
             begin_tm.tm_mday, begin_tm.tm_hour, begin_tm.tm_min,
             end_tm.tm_hour, end_tm.tm_min, action);

    if (!result.empty())
      result += "\n";
    result += buf;
  }
  return result;
}

int ScheduleService::find_vacation_slot() const {
  if (!single_events_cached_)
    return -1;
  for (const auto &ev : cached_single_events_) {
    if (ev.enabled && ev.action == 0x01)  // Stop = vacation
      return ev.index;
  }
  return -1;
}

std::string ScheduleService::format_vacation_display() const {
  if (!single_events_cached_)
    return "unknown";
  for (const auto &ev : cached_single_events_) {
    if (!ev.enabled || ev.action != 0x01)  // only Stop events are vacations
      continue;
    time_t begin_t = (time_t) ev.begin_timestamp;
    time_t end_t = (time_t) ev.end_timestamp;
    struct tm begin_tm, end_tm;
    localtime_r(&begin_t, &begin_tm);
    localtime_r(&end_t, &end_tm);
    char buf[96];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d - %04d-%02d-%02d %02d:%02d",
             begin_tm.tm_year + 1900, begin_tm.tm_mon + 1, begin_tm.tm_mday,
             begin_tm.tm_hour, begin_tm.tm_min,
             end_tm.tm_year + 1900, end_tm.tm_mon + 1, end_tm.tm_mday,
             end_tm.tm_hour, end_tm.tm_min);
    return std::string(buf);
  }
  return "No vacation";
}

// -------------------------------------------------------------------------
// Final Display & Formatting Methods
// -------------------------------------------------------------------------

bool ScheduleService::get_schedule_display_string(
    const std::vector<ScheduleEntry> &entries, std::string *result) {
  if (!result) {
    ESP_LOGE(TAG,
             "get_schedule_display_string() called with null result pointer");
    return false;
  }

  // Static day names in order
  static const char *DAYS[7] = {"Monday", "Tuesday",  "Wednesday", "Thursday",
                                "Friday", "Saturday", "Sunday"};

  // Build a map of day -> list of times
  // Each day contains a vector of [start_time, end_time] strings
  struct DaySchedule {
    std::vector<std::pair<std::string, std::string>> time_blocks;
  };
  DaySchedule day_schedules[7];

  // Collect all enabled entries and group by day
  for (const auto &entry : entries) {
    if (!entry.is_enabled()) {
      continue; // Skip disabled entries
    }

    int day_idx = entry.get_day_index();
    if (day_idx < 0 || day_idx >= 7) {
      continue; // Skip invalid days
    }

    day_schedules[day_idx].time_blocks.push_back(
        {entry.get_begin_time(), entry.get_end_time()});
  }

  // Sort each day's time blocks by start time
  for (int i = 0; i < 7; i++) {
    auto &blocks = day_schedules[i].time_blocks;
    std::sort(
        blocks.begin(), blocks.end(),
        [](const std::pair<std::string, std::string> &a,
           const std::pair<std::string, std::string> &b) {
          return a.first <
                 b.first; // Simple string comparison works for HH:MM format
        });
  }

  // Build output string
  std::string output;
  for (int i = 0; i < 7; i++) {
    output += DAYS[i];
    output += ": ";

    if (day_schedules[i].time_blocks.empty()) {
      output += "OFF";
    } else {
      for (size_t j = 0; j < day_schedules[i].time_blocks.size(); j++) {
        if (j > 0) {
          output += ", ";
        }
        output += day_schedules[i].time_blocks[j].first;
        output += "-";
        output += day_schedules[i].time_blocks[j].second;
      }
    }

    if (i < 6) {
      output += "\n";
    }
  }

  *result = output;
  return true;
}

bool ScheduleService::build_cached_layer_image(uint8_t layer,
                                               uint8_t out[42]) const {
  if (layer > 4 || !this->layer_cached_[layer])
    return false;
  memset(out, 0, 42);
  for (uint8_t day = 0; day < 7; day++) {
    ScheduleEntry entry;
    if (!this->get_cached_entry(layer, day, &entry))
      return false;
    uint8_t *cell = out + day * 6;
    if (entry.is_enabled()) {
      cell[0] = 0x01;
      cell[1] = 0x02;
      cell[2] = entry.get_begin_hour();
      cell[3] = entry.get_begin_minute();
      cell[4] = entry.get_end_hour();
      cell[5] = entry.get_end_minute();
    }
  }
  return true;
}

std::string ScheduleService::layer_json(uint8_t layer) const {
  uint8_t image[42];
  if (!this->build_cached_layer_image(layer, image))
    return "unknown";
  return codec::layer_image_to_json(image);
}

std::string ScheduleService::current_hash() const {
  if (!this->schedule_state_cached_)
    return "unknown";
  uint8_t images[codec::UPLOAD_LAYERS][codec::LAYER_IMAGE_BYTES];
  for (uint8_t layer = 0; layer < codec::UPLOAD_LAYERS; layer++) {
    if (!this->build_cached_layer_image(layer, images[layer]))
      return "unknown";
  }
  return codec::schedule_hash(images, this->schedule_enabled_);
}

} // namespace services
} // namespace alpha_hwr
} // namespace esphome
