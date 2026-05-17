/**
 * Schedule Service for Grundfos ALPHA HWR Pump
 *
 * Manages pump schedule operations including reading, writing,
 * enabling/disabling, and validation. The pump supports up to 5 schedule layers
 * (0-4), with each layer containing one time interval per day of the week.
 *
 * Based on: reference/alpha-hwr/src/alpha_hwr/services/schedule.py
 *
 * Protocol Details:
 * -----------------
 * - Schedule state: Class 10, Object 84, SubID 1 (ClockProgramOverview)
 *   - Byte 7 = enabled flag (0x01=enabled, 0x00=disabled)
 *
 * - Schedule entries: Class 10, Object 84
 *   - SubID: 1000 + layer (1000-1004 for layers 0-4)
 *   - Format: 3-byte header + (7 days × 6 bytes) = 45 bytes total
 *   - Each entry: [Enabled][Action][StartH][StartM][EndH][EndM]
 *
 * - Enable/Disable: Class 10, OpSpec 0x93 (OpSpec 4, Length 19)
 *   - Writes to Object 84, SubID 1
 *   - Modifies complete 10-byte ClockProgramOverview structure
 *
 * - Write entries: Class 10, OpSpec 0xB3 (OpSpec 5, Length varies)
 *   - Writes to Object 84, SubID 1000-1004
 *   - Payload: 42 bytes (7 days × 6 bytes)
 */

#pragma once

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "schedule_entry.h"
#include <functional>
#include <vector>

namespace esphome {
namespace alpha_hwr {

// Forward declarations
namespace core {
class Transport;
class Session;
} // namespace core

namespace services {

/**
 * Single event entry (non-recurring, timestamp-based).
 * Protocol: Object 84, SubIDs 900-999, Type 220 (10 bytes)
 *
 * Byte layout:
 *   [0]   enable (bool)
 *   [1]   action (SchedulingActionType, 0x02=RUN)
 *   [2-5] begin (uint32 BE, Unix timestamp)
 *   [6-9] end (uint32 BE, Unix timestamp)
 *
 * Single events override the weekly schedule when active.
 * In case of overlapping events, the one with lowest SubID wins.
 */
struct SingleEvent {
  bool enabled{false};
  uint8_t action{0x02};
  uint32_t begin_timestamp{0};
  uint32_t end_timestamp{0};
  uint8_t index{0}; // 0-99 (SubID = 900 + index)

  bool is_valid() const { return enabled && end_timestamp > begin_timestamp; }

  void to_bytes(uint8_t *buf) const {
    buf[0] = enabled ? 0x01 : 0x00;
    buf[1] = action;
    buf[2] = (begin_timestamp >> 24) & 0xFF;
    buf[3] = (begin_timestamp >> 16) & 0xFF;
    buf[4] = (begin_timestamp >> 8) & 0xFF;
    buf[5] = begin_timestamp & 0xFF;
    buf[6] = (end_timestamp >> 24) & 0xFF;
    buf[7] = (end_timestamp >> 16) & 0xFF;
    buf[8] = (end_timestamp >> 8) & 0xFF;
    buf[9] = end_timestamp & 0xFF;
  }

  static SingleEvent from_bytes(const uint8_t *data, uint8_t idx) {
    SingleEvent e;
    e.index = idx;
    e.enabled = data[0] != 0;
    e.action = data[1];
    e.begin_timestamp = ((uint32_t)data[2] << 24) | ((uint32_t)data[3] << 16) |
                        ((uint32_t)data[4] << 8) | data[5];
    e.end_timestamp = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                      ((uint32_t)data[8] << 8) | data[9];
    return e;
  }
};

/**
 * Schedule Service - Manages pump weekly schedules.
 *
 * Provides methods to:
 * - Read schedule entries from pump (by layer or all layers)
 * - Write schedule entries to pump (with validation)
 * - Enable/disable the internal schedule
 * - Validate entries for overlaps and time range errors
 * - Clear individual day entries
 *
 * Example Usage (from lambda in YAML):
 *
 * ```cpp
 * // Enable schedule
 * id(pump).enable_schedule();
 *
 * // Read current schedule
 * std::vector<ScheduleEntry> entries = id(pump).read_schedule_entries();
 * for (const auto &entry : entries) {
 *   ESP_LOGI("schedule", "%s", entry.to_string().c_str());
 * }
 *
 * // Write new schedule
 * std::vector<ScheduleEntry> new_schedule;
 * new_schedule.push_back(ScheduleEntry("Monday", 6, 0, 8, 0));
 * new_schedule.push_back(ScheduleEntry("Tuesday", 6, 0, 8, 0));
 * id(pump).write_schedule_entries(new_schedule, 0);  // Write to layer 0
 * ```
 */
class ScheduleService {
public:
  /**
   * Callback for scheduling delayed operations.
   * Parameters: (callback_function, delay_ms)
   */
  using ScheduleCallback = std::function<void(std::function<void()>, uint32_t)>;

  /**
   * Callback for writing BLE packets.
   * Parameters: (handle, data, length) -> bool (success)
   */
  using WriteCallback =
      std::function<bool(uint16_t, const uint8_t *, uint16_t)>;

  /**
   * Callback for schedule state changes.
   * Parameters: (enabled)
   */
  using StateChangeCallback = std::function<void(bool)>;

  // -------------------------------------------------------------------------
  // Constructor
  // -------------------------------------------------------------------------

  ScheduleService(core::Transport &transport, core::Session &session);

  // -------------------------------------------------------------------------
  // Callback Setters (called by main component during setup)
  // -------------------------------------------------------------------------

  /**
   * Set callback for scheduling delayed operations.
   * Required for async write operations and verification delays.
   */
  void set_schedule_callback(ScheduleCallback callback) {
    this->schedule_callback_ = callback;
  }

  /**
   * Set callback for writing to BLE characteristic.
   * Required for sending commands to pump.
   */
  void set_write_callback(WriteCallback callback) {
    this->write_callback_ = callback;
  }

  /**
   * Set callback for timeout operations (ESPHome's set_timeout).
   * Required for async write operations with completion callbacks.
   *
   * @param callback Function that takes (lambda, delay_ms) and schedules lambda
   * to run after delay
   */
  void set_timeout_callback(
      std::function<void(std::function<void()>, uint32_t)> callback) {
    this->set_timeout_callback_ = callback;
  }

  /**
   * Set callback for schedule state changes.
   */
  void set_state_change_callback(StateChangeCallback callback) {
    this->state_change_callback_ = callback;
  }

  // -------------------------------------------------------------------------
  // Schedule State Operations
  // -------------------------------------------------------------------------

  /**
   * Get the current schedule state (enabled/disabled).
   *
   * Returns the cached schedule state. The state is automatically updated
   * by calling poll_state() periodically from the main component update() loop.
   *
   * @param result Output parameter - set to true if enabled, false if disabled
   * @return True if cached state is valid, false if no state has been read yet
   *
   * Protocol: Class 10, Object 84, SubID 1
   * - Response format:
   * [Header(3)][Capabilities(4)][Enabled(1)][DefaultAction(1)][BaseSetpoint(4)]
   * - Byte 4 (payload index) is the enabled flag (0x01=enabled, 0x00=disabled)
   *
   * Note: This method returns the cached state immediately. Call poll_state()
   * to trigger an async read operation to update the cache.
   */
  bool get_state(bool *result);

  /**
   * Poll schedule state from pump (async operation).
   *
   * Triggers an async read operation to fetch the current schedule state
   * from the pump. The result is cached and can be retrieved via get_state().
   *
   * This method should be called periodically (e.g., every 30s) from the
   * main component update() loop to keep the cached state fresh.
   *
   * @return True if read request was sent successfully, false otherwise
   */
  bool poll_state();

  /**
   * Enable the internal schedule.
   *
   * Activates the pump's built-in schedule functionality. When enabled,
   * the pump will automatically start/stop according to the programmed
   * schedule entries.
   *
   * @return True if successfully enabled, false otherwise
   *
   * Protocol: Class 10, OpSpec 0x93, Object 84, SubID 1
   * - Reads current ClockProgramOverview structure
   * - Modifies byte 4 (clock_program_enabled) to 0x01
   * - Writes back complete 10-byte structure
   */
  bool enable();

  /**
   * Disable the internal schedule.
   *
   * Deactivates the pump's built-in schedule functionality. The pump
   * will continue operating according to its current mode, but will
   * not automatically start/stop based on the schedule.
   *
   * @return True if successfully disabled, false otherwise
   *
   * Protocol: Same as enable() but sets byte 4 to 0x00
   */
  bool disable();

  // -------------------------------------------------------------------------
  // Schedule Entry Operations
  // -------------------------------------------------------------------------

  /**
   * Read schedule entries from the pump.
   *
   * Retrieves the current weekly schedule from one or all layers.
   * Each layer can contain up to 7 entries (one per day of the week).
   *
   * @param entries Output vector - populated with schedule entries
   * @param layer Optional specific layer (0-4) to read. If -1, reads all
   * layers.
   * @return True if read succeeded, false on failure
   *
   * Protocol: Class 10, Object 84
   * - SubID: 1000 + layer (1000-1004 for layers 0-4)
   * - Response format: [Header 3 bytes] + [7 days × 6 bytes] = 45 bytes
   * - Each 6-byte entry: [Enabled][Action][StartH][StartM][EndH][EndM]
   * - Days in order: Mon, Tue, Wed, Thu, Fri, Sat, Sun
   * - Only enabled entries are included in output
   */
  bool read_entries(std::vector<ScheduleEntry> *entries, int layer = -1);

  /**
   * Read schedule entries from the pump (async with callback).
   *
   * @param layer Layer to read (0-4)
   * @param on_complete Callback invoked with success status and entries
   * @return True if read request sent successfully
   */
  bool read_entries_async(
      int layer, std::function<void(bool success,
                                    const std::vector<ScheduleEntry> &entries)>
                     on_complete);

  /**
   * Write schedule entries to the pump (synchronous, fire-and-forget).
   *
   * Writes a complete weekly schedule to the specified layer. Each layer
   * can contain up to 7 entries (one per day). Entries are validated before
   * writing to ensure no overlaps or invalid time ranges.
   *
   * Note: This is fire-and-forget mode - returns immediately without waiting
   * for acknowledgment. Use write_entries_async() for proper transaction
   * handling.
   *
   * @param entries Vector of ScheduleEntry objects to write
   * @param layer Schedule layer to write to (0-4)
   * @return True if successfully written, false otherwise
   */
  bool write_entries(const std::vector<ScheduleEntry> &entries,
                     uint8_t layer = 0);

  /**
   * Write schedule entries to the pump (async with completion callback).
   *
   * Writes a complete weekly schedule and waits for acknowledgment from pump.
   * Uses ESPHome's set_timeout() to check for response after write completes.
   *
   * @param entries Vector of ScheduleEntry objects to write
   * @param layer Schedule layer to write to (0-4)
   * @param on_complete Callback invoked when write completes (with success
   * status)
   * @return True if write packet sent successfully, false on validation/send
   * error
   *
   * Protocol: Class 10, OpSpec 0xB3 (OpSpec 5), Object 84
   * - SubID: 1000 + layer
   * - Payload: 42 bytes (7 days × 6 bytes, no header for writes)
   * - APDU format:
   *   [0x0A]              # Class 10
   *   [0xB3]              # OpSpec 5
   *   [84]                # Object ID
   *   [SubH][SubL]        # SubID (big-endian)
   *   [0x00]              # Reserved
   *   [0xDE][0x01][0x00]  # Type 222 header
   *   [0x00][0x2A]        # Size (42 bytes)
   *   [42 bytes data]     # Schedule entries
   *
   * Validation:
   * - Layer must be 0-4
   * - Time ranges must be valid (non-zero duration)
   * - No overlaps allowed within same day/layer
   * - Only one entry per day per layer (last one wins)
   */
  bool write_entries_async(const std::vector<ScheduleEntry> &entries,
                           uint8_t layer,
                           std::function<void(bool success)> on_complete);

  /**
   * Clear (disable) a schedule entry for a specific day (async).
   *
   * This disables the schedule for a specific day on the specified layer,
   * but does not affect other days or layers.
   *
   * Implementation: Reads current schedule for layer, removes entry for
   * specified day, and writes back the filtered schedule asynchronously.
   *
   * @param day Day name (Monday-Sunday)
   * @param layer Schedule layer (0-4)
   * @param on_complete Optional callback invoked with success/failure when
   *                    the full read-filter-write sequence completes.
   *                    If nullptr, failures are logged but not propagated.
   */
  void clear_entry(const std::string &day, uint8_t layer = 0,
                   std::function<void(bool)> on_complete = nullptr);

  // -------------------------------------------------------------------------
  // Single Event Operations (One-Time Schedules)
  // -------------------------------------------------------------------------

  /**
   * Read all active single events from the pump.
   * Reads SubIDs 900 to 900+max_nof_single_events-1 from Object 84.
   *
   * @param on_complete Callback with success status and vector of events
   */
  void read_single_events_async(
      std::function<void(bool, const std::vector<SingleEvent> &)> on_complete);

  /**
   * Write a single event to a specific slot.
   *
   * @param event The event to write (event.index determines SubID = 900 +
   * index)
   * @param on_complete Callback with success status
   */
  void write_single_event_async(const SingleEvent &event,
                                std::function<void(bool)> on_complete);

  /**
   * Clear (disable) a single event at a specific index.
   *
   * @param index Event slot (0 to max_nof_single_events-1)
   * @param on_complete Callback with success status
   */
  void clear_single_event_async(uint8_t index,
                                std::function<void(bool)> on_complete);

  /**
   * Find the first free (disabled) single event slot.
   * @return Index of free slot, or -1 if all slots are full
   */
  int find_free_single_event_slot() const;

  /**
   * Get cached single events.
   */
  const std::vector<SingleEvent> &get_cached_single_events() const {
    return cached_single_events_;
  }

  /**
   * Get max number of single events from ClockProgramOverview.
   */
  uint8_t get_max_single_events() const {
    return overview_cached_ ? overview_structure_[1] : 35;
  }

  /**
   * Format single events as display string.
   */
  std::string format_single_events_display() const;

  // -------------------------------------------------------------------------
  // Validation Methods
  // -------------------------------------------------------------------------

  /**
   * Validate a list of schedule entries for conflicts and errors.
   *
   * Performs comprehensive validation including:
   * - Time range validity (not zero duration)
   * - No overlaps within same day/layer
   * - Valid day names
   * - Valid layer values (0-4)
   *
   * @param entries Vector of ScheduleEntry instances to validate
   * @param errors Output vector - populated with error messages if validation
   * fails
   * @return True if all entries are valid, false if any errors found
   *
   * Example error messages:
   * - "Entry 2 (Monday 06:00-06:00): Invalid time range: begin and end times
   * are identical (06:00)"
   * - "Overlap detected: Monday layer 0: 06:00-08:00 overlaps with 07:00-09:00"
   */
  static bool validate_entries(const std::vector<ScheduleEntry> &entries,
                        std::vector<std::string> *errors);

  // -------------------------------------------------------------------------
  // Display & Formatting Methods
  // -------------------------------------------------------------------------

  /**
   * Get a human-readable schedule display string.
   *
   * Formats all schedule entries (across all layers) into a readable string
   * organized by day of week. Multiple time blocks per day are displayed on
   * the same line separated by commas. Days with no schedule show as "OFF".
   *
   * Format example:
   *   Monday: 06:00-08:00, 10:00-12:00, 15:00-17:00
   *   Tuesday: 06:00-08:00, 10:00-12:00
   *   ...
   *   Saturday: OFF
   *   Sunday: OFF
   *
   * @param entries Vector of all schedule entries (typically from read_entries
   * with layer=-1)
   * @param result Output string - populated with formatted schedule
   * @return True if formatting succeeded, false on error
   *
   * Note: Entries are automatically sorted by day and time. Only enabled
   * entries are displayed.
   */
  static bool get_schedule_display_string(const std::vector<ScheduleEntry> &entries,
                                   std::string *result);

  // -------------------------------------------------------------------------
  // Cached Entry Access (for Schedule Editor UI)
  // -------------------------------------------------------------------------

  /**
   * Get a cached schedule entry for a specific day on a layer.
   *
   * @param layer Schedule layer (0-4)
   * @param day_index Day index (0=Monday, 6=Sunday)
   * @param entry Output ScheduleEntry populated from cache
   * @return True if cache valid, false if layer not yet read
   */
  bool get_cached_entry(uint8_t layer, uint8_t day_index, ScheduleEntry *entry);

  /**
   * Set a single day's schedule entry on a layer (read-modify-write).
   *
   * If the layer is cached, modifies cache and writes immediately.
   * If not cached, reads layer first, then modifies and writes.
   * Automatically calls configuration commit after write.
   *
   * @param layer Schedule layer (0-4)
   * @param day_index Day index (0=Monday, 6=Sunday)
   * @param entry The entry to set
   * @param on_complete Callback with success status
   */
  void set_entry_async(uint8_t layer, uint8_t day_index,
                       const ScheduleEntry &entry,
                       std::function<void(bool)> on_complete);

  /**
   * Clear (disable) a single day's schedule entry on a layer.
   * Writes a disabled entry for the specified day.
   *
   * @param layer Schedule layer (0-4)
   * @param day_index Day index (0=Monday, 6=Sunday)
   * @param on_complete Callback with success status
   */
  void clear_entry_async(uint8_t layer, uint8_t day_index,
                         std::function<void(bool)> on_complete);

  /**
   * Check if a layer's entries are cached.
   */
  bool is_layer_cached(uint8_t layer) const {
    return layer <= 4 && layer_cached_[layer];
  }

  /**
   * Send configuration commit to persist schedule changes.
   * Also called by ControlService (via callback) to preserve schedule state.
   */
  bool send_configuration_commit();

protected:
  // -------------------------------------------------------------------------
  // Internal Helper Methods
  // -------------------------------------------------------------------------

  bool write_class10_command(const uint8_t *apdu, size_t apdu_len);
  bool set_state(bool enable);

  /**
   * Write a full layer from cached data and call config commit.
   */
  void write_cached_layer_async(uint8_t layer,
                                std::function<void(bool)> on_complete);

  // -------------------------------------------------------------------------
  // Member Variables
  // -------------------------------------------------------------------------

  core::Transport &transport_;
  core::Session &session_;

  ScheduleCallback schedule_callback_;
  WriteCallback write_callback_;
  StateChangeCallback state_change_callback_;

  std::function<void(std::function<void()>, uint32_t)> set_timeout_callback_;

  // Cached state
  bool schedule_state_cached_;
  bool schedule_enabled_;
  uint32_t last_state_poll_ms_;

  // Cached ClockProgramOverview (10 bytes)
  bool overview_cached_;
  uint8_t overview_structure_[10];

  // Cached layer data: raw 42 bytes per layer (7 days × 6 bytes)
  bool layer_cached_[5];
  uint8_t cached_layer_data_[5][42];

  // Cached single events
  std::vector<SingleEvent> cached_single_events_;
  bool single_events_cached_{false};

  static constexpr const char *TAG = "schedule_service";
};

} // namespace services
} // namespace alpha_hwr
} // namespace esphome
