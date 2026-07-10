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
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "frame_builder.h"
#include "session.h"
#include "transport.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char *TAG = "schedule_service";

// Day names for parsing schedule entries
static const char *DAY_NAMES[7] = {"Monday",   "Tuesday", "Wednesday",
                                   "Thursday", "Friday",  "Saturday",
                                   "Sunday"};

namespace {

// Builds the 53-byte Class 10 SET APDU (11-byte header + 42-byte payload)
// used to write a full week of schedule entries to one layer. Shared by
// write_entries() and write_entries_async() so the encoding logic only
// lives in one place.
void build_schedule_apdu(const std::vector<ScheduleEntry> &entries,
                         uint8_t layer, uint8_t apdu_out[53]) {
  // Prepare 42-byte payload (7 days × 6 bytes)
  uint8_t payload_data[42];
  memset(payload_data, 0, 42); // Initialize with zeros (disabled entries)

  // Fill payload with entries
  for (const auto &entry : entries) {
    int day_idx = entry.get_day_index();
    if (day_idx < 0) {
      ESP_LOGW(TAG, "Invalid day name in entry (day_index=%d), skipping", day_idx);
      continue;
    }

    size_t offset = day_idx * 6;

    // Fill 6-byte entry
    entry.to_bytes(payload_data + offset);

    ESP_LOGV(TAG,
             "Added entry for %s at offset %zu: %02X %02X %02X %02X %02X %02X",
             entry.get_day(), offset, payload_data[offset],
             payload_data[offset + 1], payload_data[offset + 2],
             payload_data[offset + 3], payload_data[offset + 4],
             payload_data[offset + 5]);
  }

  // Build APDU
  uint16_t sub_id = 1000 + layer;
  uint8_t sub_h = (sub_id >> 8) & 0xFF;
  uint8_t sub_l = sub_id & 0xFF;

  apdu_out[0] = 0x0A;  // Class 10
  apdu_out[1] = 0xB3;  // OpSpec 5
  apdu_out[2] = 84;    // Object ID 84
  apdu_out[3] = sub_h; // SubID high byte
  apdu_out[4] = sub_l; // SubID low byte
  apdu_out[5] = 0x00;  // Reserved
  apdu_out[6] = 0xDE;  // Type 222 header
  apdu_out[7] = 0x01;
  apdu_out[8] = 0x00;
  apdu_out[9] = 0x00;  // Size high byte
  apdu_out[10] = 0x2A; // Size low byte (42 bytes)

  // Append payload
  memcpy(apdu_out + 11, payload_data, 42);
}

}  // namespace

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

bool ScheduleService::poll_state() {
  if (!this->session_.is_ready()) {
    ESP_LOGV(TAG, "Cannot poll schedule state: session not ready");
    return false;
  }

  ESP_LOGD(TAG, "Polling schedule state (Object 84, SubID 1)...");

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
      [this](bool success, const uint8_t *payload, size_t payload_len) {
        if (!success) {
          ESP_LOGW(TAG, "Failed to poll schedule state (timeout)");
          return;
        }

        if (payload_len >= 13) {
          this->schedule_enabled_ = (payload[7] != 0);
          this->schedule_state_cached_ = true;
          memcpy(this->overview_structure_, payload + 3, 10);
          this->overview_cached_ = true;

          ESP_LOGD(TAG, "Schedule state updated: %s",
                   this->schedule_enabled_ ? "enabled" : "disabled");

          if (this->state_change_callback_) {
            this->state_change_callback_(this->schedule_enabled_);
          }
        } else {
          ESP_LOGW(TAG, "Schedule state response too short (%zu bytes)",
                   payload_len);
        }
      });

  return true;
}

bool ScheduleService::enable() { return this->set_state(true); }

bool ScheduleService::disable() { return this->set_state(false); }

bool ScheduleService::set_state(bool enable) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot set schedule state: session not ready");
    return false;
  }

  ESP_LOGI(TAG, "%s schedule...", enable ? "Enabling" : "Disabling");

  // Read-modify-write implementation (matches Python reference
  // schedule.py:724-767) Use cached ClockProgramOverview structure if
  // available, otherwise use defaults
  uint8_t structure_bytes[10];

  if (this->overview_cached_) {
    // Use cached structure from last poll (preserves all pump settings)
    memcpy(structure_bytes, this->overview_structure_, 10);
    ESP_LOGD(TAG, "Using cached ClockProgramOverview structure");
  } else {
    // Fallback to default values (for first-time use before any poll)
    // These are typical ALPHA HWR default values
    ESP_LOGW(TAG, "No cached overview - using default values (consider calling "
                  "poll_state() first)");
    structure_bytes[0] = 0x8C; // max_nof_actions = 140
    structure_bytes[1] = 0x23; // max_nof_single_events = 35
    structure_bytes[2] = 0x05; // max_nof_alternative_events_per_day = 5
    structure_bytes[3] = 0x05; // max_nof_events_per_day = 5
    structure_bytes[4] = 0x00; // clock_program_enabled (will be set below)
    structure_bytes[5] = 0x01; // default_action = START
    structure_bytes[6] = 0x00; // base_set_point (float32 = 0.0)
    structure_bytes[7] = 0x00;
    structure_bytes[8] = 0x00;
    structure_bytes[9] = 0x00;
  }

  // Modify only the enable flag (byte 4)
  structure_bytes[4] = enable ? 0x01 : 0x00;

  // Build APDU: Class 10 SET command for Object 84, SubID 1
  // OpSpec 0x93 = OpSpec 4 (SET), Length 19
  uint8_t apdu[21];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x93;  // OpSpec 4, Length 19
  apdu[2] = 84;    // Object 84 (decimal)
  apdu[3] = 0x00;  // SubID high byte
  apdu[4] = 0x01;  // SubID low byte (SubID = 1)
  apdu[5] = 0x00;  // Reserved
  apdu[6] = 0xDA;  // Type 218 (ClockProgramOverview)
  apdu[7] = 0x01;  // Type continued
  apdu[8] = 0x00;  // Type continued
  apdu[9] = 0x00;  // Size high byte
  apdu[10] = 0x0A; // Size low byte (10 bytes)
  memcpy(apdu + 11, structure_bytes, 10);

  // Write command
  if (!this->write_class10_command(apdu, 21)) {
    ESP_LOGE(TAG, "Failed to write schedule enable/disable command");
    return false;
  }

  ESP_LOGI(TAG, "Schedule %s command sent", enable ? "enable" : "disable");

  // Update cached state optimistically (will be verified on next poll)
  this->schedule_enabled_ = enable;
  this->schedule_state_cached_ = true;

  return true;
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
    ESP_LOGD(TAG, "Using cached ClockProgramOverview structure for commit");
  } else {
    ESP_LOGE(TAG, "Cannot commit configuration: ClockProgramOverview not yet cached. Ignoring commit to prevent corruption.");
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

  // Send configuration commit (fire-and-forget, no response expected)
  if (!this->write_class10_command(apdu, 21)) {
    ESP_LOGE(TAG, "Failed to send configuration commit");
    return false;
  }

  ESP_LOGD(TAG, "Configuration commit sent successfully");
  return true;
}

// -------------------------------------------------------------------------
// Schedule Entry Operations
// -------------------------------------------------------------------------

bool ScheduleService::read_entries(std::vector<ScheduleEntry> *entries,
                                   int layer) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read schedule entries: session not ready");
    return false;
  }

  if (layer < 0 || layer > 4) {
    ESP_LOGE(TAG, "Invalid layer %d (must be 0-4)", layer);
    return false;
  }

  ESP_LOGD(TAG, "Reading schedule entries for layer %d...", layer);

  uint16_t sub_id = 1000 + layer;

  uint8_t apdu[5];
  apdu[0] = 0x0A;
  apdu[1] = 0x03;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;

  this->transport_.send_apdu_command(
      apdu, 5, 0xDE01, 0,
      [this, entries, layer](bool success, const uint8_t *payload,
                             size_t payload_len) {
        if (!success) {
          ESP_LOGW(TAG,
                   "Failed to read schedule entries for layer %d (timeout)",
                   layer);
          return;
        }

        if (payload_len < 45) {
          ESP_LOGW(TAG, "Schedule entries response too short (%zu bytes)",
                   payload_len);
          entries->clear();
          return;
        }

        const uint8_t *entry_data = payload + 3;
        entries->clear();
        int enabled_count = 0;

        for (int day_idx = 0; day_idx < 7; day_idx++) {
          size_t offset = day_idx * 6;
          const uint8_t *entry_bytes = entry_data + offset;

          if (entry_bytes[0] != 0) {
            ScheduleEntry entry = ScheduleEntry::from_bytes(
                entry_bytes, DAY_NAMES[day_idx], layer);
            entries->push_back(entry);
            enabled_count++;
          }
        }

        ESP_LOGD(TAG, "Read %d enabled entries from layer %d", enabled_count,
                 layer);
      });

  return true;
}

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
      uint8_t current_layer;
      std::vector<ScheduleEntry> all_entries;
      std::function<void(bool success, const std::vector<ScheduleEntry> &)> on_complete;
    };

    auto state = std::make_shared<ReadAllState>();
    state->service = this;
    state->current_layer = 0;
    state->on_complete = on_complete;

    auto read_next_layer = [](auto& self, std::shared_ptr<ReadAllState> st) -> void {
      if (st->current_layer > 4) {
        ESP_LOGD(TAG, "Read %zu total schedule entries from all layers",
                 st->all_entries.size());
        if (st->on_complete)
          st->on_complete(true, st->all_entries);
        return;
      }

      int layer_to_read = st->current_layer;
      bool queued = st->service->read_entries_async(
          layer_to_read,
          [st, self](bool success, const std::vector<ScheduleEntry> &entries) {
            if (success) {
              for (const auto &entry : entries) {
                st->all_entries.push_back(entry);
              }
              ESP_LOGV(TAG, "Layer %d contributed %zu entries", st->current_layer,
                       entries.size());
            } else {
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
        if (st->on_complete)
          st->on_complete(false, st->all_entries);
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

bool ScheduleService::write_entries(const std::vector<ScheduleEntry> &entries,
                                    uint8_t layer) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot write schedule entries: session not ready");
    return false;
  }

  // Validate layer
  if (layer > 4) {
    ESP_LOGE(TAG, "Invalid layer: %d. Must be 0-4.", layer);
    return false;
  }

  // Validate entries
  std::vector<std::string> errors;
  if (!this->validate_entries(entries, &errors)) {
    ESP_LOGE(TAG, "Schedule validation failed:");
    for (const auto &error : errors) {
      ESP_LOGE(TAG, "  %s", error.c_str());
    }
    return false;
  }

  ESP_LOGI(TAG, "Writing %zu schedule entries to layer %d...", entries.size(),
           layer);

  uint8_t apdu[53]; // 11 header bytes + 42 data bytes
  build_schedule_apdu(entries, layer, apdu);

  // Send write command
  if (!this->write_class10_command(apdu, sizeof(apdu))) {
    ESP_LOGE(TAG, "Failed to write schedule to layer %d", layer);
    return false;
  }

  ESP_LOGI(TAG, "Schedule written successfully to layer %d", layer);
  return true;
}

bool ScheduleService::write_entries_async(
    const std::vector<ScheduleEntry> &entries, uint8_t layer,
    std::function<void(bool)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot write schedule entries: session not ready");
    if (on_complete)
      on_complete(false);
    return false;
  }

  if (!this->set_timeout_callback_) {
    ESP_LOGE(TAG,
             "Cannot use async write: set_timeout_callback not configured");
    if (on_complete)
      on_complete(false);
    return false;
  }

  // Validate layer
  if (layer > 4) {
    ESP_LOGE(TAG, "Invalid layer: %d. Must be 0-4.", layer);
    if (on_complete)
      on_complete(false);
    return false;
  }

  // Validate entries
  std::vector<std::string> errors;
  if (!this->validate_entries(entries, &errors)) {
    ESP_LOGE(TAG, "Schedule validation failed:");
    for (const auto &error : errors) {
      ESP_LOGE(TAG, "  %s", error.c_str());
    }
    if (on_complete)
      on_complete(false);
    return false;
  }

  ESP_LOGI(TAG, "Writing %zu schedule entries to layer %d (async mode)...",
           entries.size(), layer);

  uint8_t apdu[53]; // 11 header bytes + 42 data bytes
  build_schedule_apdu(entries, layer, apdu);

  ESP_LOGI(TAG, "Queueing async schedule write for layer %d...", layer);

  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0xDE01, 0,
      [on_complete, layer](bool success, const uint8_t *data, size_t len) {
        if (success) {
          ESP_LOGI(TAG, "Async write completed with ACK for layer %d", layer);
        } else {
          ESP_LOGW(TAG,
                   "Async write timeout/error for layer %d - treating as "
                   "success (per Python reference: pump commits on timeout)",
                   layer);
        }
        // NOTE: Always reports true to match Python reference behavior.
        // The pump's two-phase commit often times out even on success because
        // the ACK notification arrives outside the expected window.
        if (on_complete) {
          on_complete(true);
        }
      },
      3000);

  return true;
}

void ScheduleService::clear_entry(const std::string &day, uint8_t layer,
                                   std::function<void(bool)> on_complete) {
  if (!this->session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot clear schedule entry: session not ready");
    if (on_complete) on_complete(false);
    return;
  }

  // Validate day
  bool valid_day = false;
  for (const char *valid : DAY_NAMES) {
    if (day == valid) {
      valid_day = true;
      break;
    }
  }
  if (!valid_day) {
    ESP_LOGE(TAG, "Invalid day name: %s", day.c_str());
    if (on_complete) on_complete(false);
    return;
  }

  // Validate layer
  if (layer > 4) {
    ESP_LOGE(TAG, "Invalid layer: %d. Must be 0-4.", layer);
    if (on_complete) on_complete(false);
    return;
  }

  ESP_LOGI(TAG, "Clearing schedule entry for %s on layer %d...", day.c_str(),
           layer);

  // Fully async: read → filter → write, with completion propagation.
  this->read_entries_async(layer,
    [this, day, layer, on_complete](bool success, const std::vector<ScheduleEntry> &entries) {
      if (!success) {
        ESP_LOGE(TAG, "Failed to read current schedule for layer %d", layer);
        if (on_complete) on_complete(false);
        return;
      }

      // Filter out the entry for the specified day
      std::vector<ScheduleEntry> filtered_entries;
      for (const auto &entry : entries) {
        if (entry.get_day() != day) {
          filtered_entries.push_back(entry);
        }
      }

      // Write back the filtered entries asynchronously
      this->write_entries_async(filtered_entries, layer,
        [on_complete, day, layer](bool write_success) {
          if (write_success) {
            ESP_LOGI(TAG, "Cleared schedule entry for %s on layer %d", day.c_str(), layer);
          } else {
            ESP_LOGE(TAG, "Failed to write filtered schedule for layer %d", layer);
          }
          if (on_complete) on_complete(write_success);
        });
    });
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

// NOTE: read_class10_object() removed - requires async implementation
// For Phase 7, only write operations (enable/disable/write_entries) are
// supported Read operations (get_state/read_entries) will be added in Phase 8

bool ScheduleService::write_class10_command(const uint8_t *apdu,
                                            size_t apdu_len) {
  // Build GENI frame and queue the packet with pacing and non-blocking wait
  this->transport_.send_apdu_command(apdu, apdu_len);

  return true;
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

void ScheduleService::clear_entry_async(uint8_t layer, uint8_t day_index,
                                        std::function<void(bool)> on_complete) {
  // Create a disabled entry
  ScheduleEntry disabled;
  disabled.set_enabled(false);
  disabled.set_action(0x02);
  disabled.set_begin_hour(0);
  disabled.set_begin_minute(0);
  disabled.set_end_hour(0);
  disabled.set_end_minute(0);
  disabled.set_day(DAY_NAMES[day_index]);
  disabled.set_layer(layer);

  set_entry_async(layer, day_index, disabled, on_complete);
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

  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0xDE01, 0,
      [this, on_complete, layer](bool success, const uint8_t *data,
                                 size_t len) {
        // Send configuration commit after write
        this->send_configuration_commit();
        ESP_LOGI(TAG, "Layer %d write + config commit sent", layer);
        if (on_complete)
          on_complete(true);
      },
      3000);
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
  auto read_next = std::make_shared<std::function<void(uint8_t)>>();

  *read_next = [this, events, on_complete, max_events, read_next](uint8_t idx) {
    if (idx >= max_events) {
      // All done
      this->cached_single_events_ = *events;
      this->single_events_cached_ = true;
      ESP_LOGD(TAG, "Read %zu active single events (of %d slots)",
               events->size(), max_events);
      if (on_complete)
        on_complete(true, *events);
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
        [this, idx, events, on_complete, max_events,
         read_next](bool success, const uint8_t *payload, size_t payload_len) {
          if (success && payload_len >= 13) {
            SingleEvent ev = SingleEvent::from_bytes(payload + 3, idx);
            if (ev.enabled) {
              events->push_back(ev);
              ESP_LOGD(TAG, "Single event slot %d: active (%u - %u)", idx,
                       ev.begin_timestamp, ev.end_timestamp);
            }
          }
          // Read next slot
          (*read_next)(idx + 1);
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
  ESP_LOGI(TAG, "Writing single event to slot %d (SubID %d): %s %u-%u",
           event.index, sub_id, event.enabled ? "enabled" : "disabled",
           event.begin_timestamp, event.end_timestamp);

  uint8_t apdu[21]; // 11 header + 10 data
  apdu[0] = 0x0A;
  apdu[1] = 0xB3;
  apdu[2] = 84;
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;
  apdu[5] = 0x00;
  apdu[6] = 0xDC; // Type 220
  apdu[7] = 0x01;
  apdu[8] = 0x00;
  apdu[9] = 0x00;
  apdu[10] = 0x0A; // Size: 10 bytes
  event.to_bytes(apdu + 11);

  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0xDC01, 0,
      [this, on_complete, event](bool success, const uint8_t *data,
                                 size_t len) {
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
      3000);
}

void ScheduleService::clear_single_event_async(
    uint8_t index, std::function<void(bool)> on_complete) {
  SingleEvent disabled;
  disabled.index = index;
  disabled.enabled = false;
  disabled.action = 0x02;
  disabled.begin_timestamp = 0;
  disabled.end_timestamp = 0;
  write_single_event_async(disabled, on_complete);
}

int ScheduleService::find_free_single_event_slot() const {
  uint8_t max_events = overview_cached_ ? overview_structure_[1] : 35;
  if (!single_events_cached_)
    return 0;

  std::set<uint8_t> used;
  for (const auto &ev : cached_single_events_) {
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

    char buf[80];
    snprintf(buf, sizeof(buf), "[%d] %04d-%02d-%02d %02d:%02d - %02d:%02d",
             ev.index, begin_tm.tm_year + 1900, begin_tm.tm_mon + 1,
             begin_tm.tm_mday, begin_tm.tm_hour, begin_tm.tm_min,
             end_tm.tm_hour, end_tm.tm_min);

    if (!result.empty())
      result += "\n";
    result += buf;
  }
  return result;
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

std::string ScheduleService::generate_json() const {
  bool enabled = this->schedule_enabled_;

  std::string json;
  json.reserve(256);
  json += "{\"e\":";
  json += enabled ? "1" : "0";
  json += ",\"s\":{";

  char buf[32];
  bool first_layer = true;
  for (int layer = 0; layer < 5; layer++) {
    if (!is_layer_cached(layer))
      continue;

    bool has_entries = false;
    std::string layer_json;
    layer_json.reserve(100);
    layer_json += "[";
    for (int day = 0; day < 7; day++) {
      if (day > 0)
        layer_json += ",";
      ScheduleEntry entry;
      if (get_cached_entry(layer, day, &entry) && entry.is_enabled()) {
        int start = entry.get_begin_hour() * 60 + entry.get_begin_minute();
        int end = entry.get_end_hour() * 60 + entry.get_end_minute();
        snprintf(buf, sizeof(buf), "[%d,%d]", start, end);
        layer_json += buf;
        has_entries = true;
      } else {
        layer_json += "0";
      }
    }
    layer_json += "]";

    if (has_entries) {
      if (!first_layer)
        json += ",";
      snprintf(buf, sizeof(buf), "\"%d\":", layer);
      json += buf;
      json += layer_json;
      first_layer = false;
    }
  }

  json += "}}";

  // Safety: HA limits entity state to 255 characters
  if (json.size() > 255) {
    json.resize(252);
    json += "...";
    ESP_LOGW(TAG, "Schedule JSON truncated to 255 chars");
  }

  return json;
}

} // namespace services
} // namespace alpha_hwr
} // namespace esphome
