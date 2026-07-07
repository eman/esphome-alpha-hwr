/**
 * Schedule Entry Model for Grundfos ALPHA HWR Pump
 *
 * Represents a single schedule entry with time window validation and overlap detection.
 * Based on: reference/alpha-hwr/src/alpha_hwr/models.py (ScheduleEntry class)
 *
 * Protocol Details:
 * -----------------
 * Binary format (6 bytes per entry):
 *   Byte 0: Enabled flag (0x01=enabled, 0x00=disabled)
 *   Byte 1: Action code (0x02=run pump)
 *   Byte 2: Begin hour (0-23)
 *   Byte 3: Begin minute (0-59)
 *   Byte 4: End hour (0-23)
 *   Byte 5: End minute (0-59)
 *
 * Storage format (Class 10, Object 84):
 * - Schedule state: SubID 1 (ClockProgramOverview) - byte 7 = enabled flag
 * - Schedule entries: SubID 1000-1004 (5 layers, 0-4)
 * - Each layer: 3-byte header + (7 days × 6 bytes) = 45 bytes total
 * - Days: Monday=0, Tuesday=1, ..., Sunday=6
 */

#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include <string>
#include <vector>

namespace esphome {
namespace alpha_hwr {

/**
 * Schedule entry representing a time window for pump operation.
 *
 * Validates time ranges and detects overlaps with other entries.
 * Supports midnight-crossing schedules (e.g., 22:00-02:00).
 */
class ScheduleEntry {
 public:
  // Valid day names for validation
  static constexpr const char *VALID_DAYS[7] = {
      "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

  // Sentinel for day_index_ meaning "not one of the 7 valid days".
  static constexpr uint8_t INVALID_DAY_INDEX = 7;

  // -------------------------------------------------------------------------
  // Constructors
  // -------------------------------------------------------------------------

  /**
   * Default constructor - creates disabled entry for Monday 00:00-00:00.
   */
  ScheduleEntry()
      : day_index_(0),
        begin_hour_(0),
        begin_minute_(0),
        end_hour_(0),
        end_minute_(0),
        action_(0x02),
        layer_(0),
        enabled_(false) {}

  /**
   * Create a schedule entry with full parameters.
   *
   * @param day Day name (Monday-Sunday)
   * @param begin_hour Start hour (0-23)
   * @param begin_minute Start minute (0-59)
   * @param end_hour End hour (0-23)
   * @param end_minute End minute (0-59)
   * @param action Action code (default 0x02 = run pump)
   * @param layer Schedule layer 0-4 (default 0)
   * @param enabled Whether entry is active (default true)
   */
  ScheduleEntry(const std::string &day, uint8_t begin_hour, uint8_t begin_minute, uint8_t end_hour,
                uint8_t end_minute, uint8_t action = 0x02, uint8_t layer = 0, bool enabled = true)
      : day_index_(INVALID_DAY_INDEX),
        begin_hour_(begin_hour),
        begin_minute_(begin_minute),
        end_hour_(end_hour),
        end_minute_(end_minute),
        action_(action),
        layer_(layer),
        enabled_(enabled) {
    this->set_day(day);
  }

  // -------------------------------------------------------------------------
  // Getters & Setters
  // -------------------------------------------------------------------------

  /**
   * Get day name. Returns "" if this entry's day is not one of the 7 valid
   * names (e.g. constructed from invalid input) - callers that validate day
   * names (see ScheduleService::validate_entries) rely on this not matching
   * any VALID_DAYS entry so invalid input is still reported as an error
   * instead of being silently coerced into a real day.
   */
  const char *get_day() const {
    return this->day_index_ < 7 ? VALID_DAYS[this->day_index_] : "";
  }

  /**
   * Set day by name. If `day` doesn't match any VALID_DAYS entry, the day is
   * left/marked invalid (see get_day()/get_day_index()) rather than silently
   * defaulting to a real day.
   */
  void set_day(const std::string &day) {
    this->day_index_ = INVALID_DAY_INDEX;
    for (uint8_t i = 0; i < 7; i++) {
      if (day == VALID_DAYS[i]) {
        this->day_index_ = i;
        break;
      }
    }
  }

  uint8_t get_begin_hour() const { return this->begin_hour_; }
  void set_begin_hour(uint8_t hour) { this->begin_hour_ = hour; }

  uint8_t get_begin_minute() const { return this->begin_minute_; }
  void set_begin_minute(uint8_t minute) { this->begin_minute_ = minute; }

  uint8_t get_end_hour() const { return this->end_hour_; }
  void set_end_hour(uint8_t hour) { this->end_hour_ = hour; }

  uint8_t get_end_minute() const { return this->end_minute_; }
  void set_end_minute(uint8_t minute) { this->end_minute_ = minute; }

  uint8_t get_action() const { return this->action_; }
  void set_action(uint8_t action) { this->action_ = action; }

  uint8_t get_layer() const { return this->layer_; }
  void set_layer(uint8_t layer) { this->layer_ = layer; }

  bool is_enabled() const { return this->enabled_; }
  void set_enabled(bool enabled) { this->enabled_ = enabled; }

  // -------------------------------------------------------------------------
  // Computed Properties
  // -------------------------------------------------------------------------

  /**
   * Get day index (0=Monday, 6=Sunday, -1 if invalid).
   */
  int get_day_index() const {
    return this->day_index_ < 7 ? static_cast<int>(this->day_index_) : -1;
  }

  /**
   * Get formatted begin time (HH:MM).
   */
  std::string get_begin_time() const {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", this->begin_hour_, this->begin_minute_);
    return std::string(buf);
  }

  /**
   * Get formatted end time (HH:MM).
   */
  std::string get_end_time() const {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", this->end_hour_, this->end_minute_);
    return std::string(buf);
  }

  /**
   * Calculate entry duration in minutes.
   *
   * If end time is before begin time, assumes schedule crosses midnight.
   *
   * @return Duration in minutes
   *
   * Examples:
   *   06:00-08:00 = 120 minutes
   *   22:00-02:00 = 240 minutes (crosses midnight)
   */
  int get_duration_minutes() const {
    int begin_mins = this->begin_hour_ * 60 + this->begin_minute_;
    int end_mins = this->end_hour_ * 60 + this->end_minute_;

    if (end_mins < begin_mins) {
      // Crosses midnight: time until midnight + time from midnight
      return (24 * 60 - begin_mins) + end_mins;
    }

    return end_mins - begin_mins;
  }

  /**
   * Check if this schedule entry crosses midnight.
   *
   * @return True if end time is before begin time
   */
  bool crosses_midnight() const {
    int begin_mins = this->begin_hour_ * 60 + this->begin_minute_;
    int end_mins = this->end_hour_ * 60 + this->end_minute_;
    return end_mins < begin_mins;
  }

  // -------------------------------------------------------------------------
  // Validation Methods
  // -------------------------------------------------------------------------

  /**
   * Validate day name is one of VALID_DAYS.
   */
  static bool is_valid_day(const std::string &day) {
    for (const char *valid_day : VALID_DAYS) {
      if (day == valid_day) {
        return true;
      }
    }
    return false;
  }

  /**
   * Validate that the time range is sensible.
   *
   * @param error_msg Output parameter for error message (if validation fails)
   * @return True if valid, false if invalid
   *
   * Checks:
   * - Duration is not zero
   * - Times are not identical
   */
  bool is_valid_time_range(std::string *error_msg = nullptr) const {
    int duration = this->get_duration_minutes();

    if (duration == 0) {
      if (error_msg) {
        *error_msg = "Invalid time range: begin and end times are identical (" + this->get_begin_time() + ")";
      }
      return false;
    }

    // All checks passed
    return true;
  }

  /**
   * Check if this entry overlaps with another entry.
   *
   * Only checks for overlap if both entries are:
   * - On the same day
   * - On the same layer
   * - Both enabled
   *
   * @param other Another ScheduleEntry to compare with
   * @return True if the entries overlap in time
   *
   * Examples:
   *   06:00-08:00 and 07:00-09:00 = True (overlap)
   *   06:00-08:00 and 08:00-10:00 = False (adjacent, no overlap)
   *   06:00-08:00 and 10:00-12:00 = False (separate)
   *   22:00-02:00 and 01:00-03:00 = True (both cross midnight, overlap)
   */
  bool overlaps_with(const ScheduleEntry &other) const {
    // Only check overlap if same day, same layer, and both enabled
    if (this->day_index_ != other.day_index_) {
      return false;
    }
    if (this->layer_ != other.layer_) {
      return false;
    }
    if (!this->enabled_ || !other.enabled_) {
      return false;
    }

    // Convert times to minutes since midnight for easier comparison
    int self_begin = this->begin_hour_ * 60 + this->begin_minute_;
    int self_end = this->end_hour_ * 60 + this->end_minute_;
    int other_begin = other.begin_hour_ * 60 + other.begin_minute_;
    int other_end = other.end_hour_ * 60 + other.end_minute_;

    // Handle midnight crossing
    bool self_crosses = this->crosses_midnight();
    bool other_crosses = other.crosses_midnight();

    if (!self_crosses && !other_crosses) {
      // Simple case: neither crosses midnight
      // Overlap if: self_begin < other_end AND other_begin < self_end
      return self_begin < other_end && other_begin < self_end;
    } else if (self_crosses && !other_crosses) {
      // Self crosses midnight: runs from [self_begin to 23:59] AND [00:00 to self_end]
      // Other is normal: [other_begin to other_end]
      bool in_evening_segment = other_begin >= self_begin;
      bool overlaps_morning_segment = (other_begin < self_end) || (other_end <= self_end);
      return in_evening_segment || overlaps_morning_segment;
    } else if (!self_crosses && other_crosses) {
      // Other crosses midnight, self doesn't
      bool in_evening_segment = self_begin >= other_begin;
      bool overlaps_morning_segment = (self_begin < other_end) || (self_end <= other_end);
      return in_evening_segment || overlaps_morning_segment;
    } else {
      // Both cross midnight - they definitely overlap somewhere
      return true;
    }
  }

  // -------------------------------------------------------------------------
  // Serialization Methods
  // -------------------------------------------------------------------------

  /**
   * Convert to 6-byte binary format for writing to pump.
   *
   * Format:
   *   Byte 0: Enabled flag (0x01 if enabled, 0x00 if disabled)
   *   Byte 1: Action code (0x02 for run)
   *   Byte 2: Start hour (0-23)
   *   Byte 3: Start minute (0-59)
   *   Byte 4: End hour (0-23)
   *   Byte 5: End minute (0-59)
   *
   * @param buffer Output buffer (must be at least 6 bytes)
   */
  void to_bytes(uint8_t *buffer) const {
    buffer[0] = this->enabled_ ? 0x01 : 0x00;
    buffer[1] = this->action_;
    buffer[2] = this->begin_hour_;
    buffer[3] = this->begin_minute_;
    buffer[4] = this->end_hour_;
    buffer[5] = this->end_minute_;
  }

  /**
   * Parse from 6-byte binary format.
   *
   * @param data 6-byte binary data
   * @param day Day name for this entry
   * @param layer Schedule layer (0-4)
   * @return ScheduleEntry instance
   */
  static ScheduleEntry from_bytes(const uint8_t *data, const std::string &day, uint8_t layer = 0) {
    return ScheduleEntry(day, data[2],  // begin_hour
                         data[3],        // begin_minute
                         data[4],        // end_hour
                         data[5],        // end_minute
                         data[1],        // action
                         layer, data[0] != 0  // enabled
    );
  }

  // -------------------------------------------------------------------------
  // String Representation
  // -------------------------------------------------------------------------

  /**
   * Get human-readable string representation.
   */
  std::string to_string() const {
    char buf[100];
    snprintf(buf, sizeof(buf), "%s L%d: %s-%s (%s, action=0x%02X)", this->get_day(), this->layer_,
             this->get_begin_time().c_str(), this->get_end_time().c_str(), this->enabled_ ? "enabled" : "disabled",
             this->action_);
    return std::string(buf);
  }

 protected:
  uint8_t day_index_;
  uint8_t begin_hour_;
  uint8_t begin_minute_;
  uint8_t end_hour_;
  uint8_t end_minute_;
  uint8_t action_;
  uint8_t layer_;
  bool enabled_;
};

}  // namespace alpha_hwr
}  // namespace esphome
