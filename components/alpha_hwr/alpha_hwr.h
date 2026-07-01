#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "auth.h"
#include "ble_connection_manager.h"
#include "codec.h"
#include "control_service.h"
#include "device_info_service.h"
#include "esphome/core/log.h"
#include "event_log_service.h"
#include "frame_builder.h"
#include "history_service.h"
#include "schedule_entry.h"
#include "schedule_service.h"
#include "sensor_publisher.h"
#include "session.h"
#include "telemetry_service.h"
#include "time_service.h"
#include "transport.h"
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

namespace esphome {
namespace alpha_hwr {

static const char *TAG = "alpha_hwr";

// ============================================================================
// DISCOVERY METHODS FOR GRUNDFOS ALPHA HWR PUMPS
// ============================================================================
//
// Primary Discovery Method (Most Reliable):
//   Match by Grundfos Company ID (0xFE5D) in BLE manufacturer service data.
//   This is GUARANTEED to be present on all ALPHA HWR pumps.
//
//   Manufacturer Data Structure:
//     Byte 0-1: Frame header
//     Byte 2:   Product Family (0x34 = ALPHA)
//     Byte 3:   Product Type (0x07 = HWR)
//     Byte 4+:  Additional data
//
// Secondary Discovery Method (Fallback):
//   Check for GENI service UUID in advertised services.
//   UUID: 0000fdd0-0000-1000-8000-00805f9b34fb
//   May not always be advertised depending on firmware version.
//
// Tertiary Discovery Method (User-Friendly):
//   Device name pattern: ALPHA_<serial_number>
//   Most user-friendly but not guaranteed to be present.
//
// Implementation Note:
//   The is_alpha_hwr_device() method implements the primary and secondary
//   methods. When using ble_client with a specific MAC address, the device
//   will be validated upon connection.
//
// ============================================================================

// Grundfos Company ID for BLE manufacturer data (most reliable discovery
// method)
static const uint16_t GRUNDFOS_COMPANY_ID = 0xFE5D;

// Product identification bytes in manufacturer data
static const uint8_t PRODUCT_FAMILY_ALPHA = 0x34;
static const uint8_t PRODUCT_TYPE_HWR = 0x07;

// GENI Protocol UUIDs - Single bidirectional characteristic
// NOTE: The GENI characteristic is inside the Grundfos service (0xFE5D), not a
// separate service!
static const esp32_ble_tracker::ESPBTUUID GRUNDFOS_SERVICE_UUID =
    esp32_ble_tracker::ESPBTUUID::from_uint16(0xFE5D);
static const esp32_ble_tracker::ESPBTUUID GENI_CHAR_UUID =
    esp32_ble_tracker::ESPBTUUID::from_raw(
        "859cffd1-036e-432a-aa28-1a0085b87ba9");

class AlphaHwrComponent : public PollingComponent,
                          public ble_client::BLEClientNode {
public:
  explicit AlphaHwrComponent(ble_client::BLEClient *parent)
      : PollingComponent(10000), auth_(transport_),
        telemetry_service_(transport_), control_service_(transport_, session_),
        schedule_service_(transport_, session_),
        device_info_service_(transport_, session_), time_service_(&transport_),
        event_log_service_(transport_, session_),
        history_service_(transport_, session_) {
    parent->register_ble_node(this);
    parent_ = parent;
    ESP_LOGI(TAG, "AlphaHwrComponent constructor");
  }

  void set_flow_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_flow_sensor(sensor);
  }
  void set_head_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_head_sensor(sensor);
  }
  void set_power_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_power_sensor(sensor);
  }
  void set_rpm_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_rpm_sensor(sensor);
  }
  void set_temp_media_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_temp_media_sensor(sensor);
  }
  void set_temp_pcb_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_temp_pcb_sensor(sensor);
  }
  void set_temp_control_box_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_temp_control_box_sensor(sensor);
  }
  void set_voltage_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_voltage_sensor(sensor);
  }
  void set_voltage_dc_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_voltage_dc_sensor(sensor);
  }
  void set_current_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_current_sensor(sensor);
  }
  void set_inlet_pressure_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_inlet_pressure_sensor(sensor);
  }
  void set_head_rate_sensor(sensor::Sensor *sensor) {
    sensor_publisher_.set_head_rate_sensor(sensor);
  }
  static void set_outlet_pressure_sensor(
      sensor::Sensor * /*sensor*/) { /* Removed: HWR pump lacks this sensor */ }
  void set_pairing_status_binary_sensor(binary_sensor::BinarySensor *sensor) {
    pairing_status_sensor_ = sensor;
  }
#ifdef USE_TEXT_SENSOR
  void set_alarms_text_sensor(text_sensor::TextSensor *sensor) {
    sensor_publisher_.set_alarms_text_sensor(sensor);
  }
  void set_warnings_text_sensor(text_sensor::TextSensor *sensor) {
    sensor_publisher_.set_warnings_text_sensor(sensor);
  }
  void set_schedule_text_sensor(text_sensor::TextSensor *sensor) {
    schedule_text_sensor_ = sensor;
  }
  void set_control_mode_text_sensor(text_sensor::TextSensor *sensor) {
    control_mode_sensor_ = sensor;
  }
  void set_serial_number_text_sensor(text_sensor::TextSensor *sensor) {
    serial_number_sensor_ = sensor;
  }
  void set_software_version_text_sensor(text_sensor::TextSensor *sensor) {
    software_version_sensor_ = sensor;
  }
  void set_hardware_version_text_sensor(text_sensor::TextSensor *sensor) {
    hardware_version_sensor_ = sensor;
  }
  void set_ble_version_text_sensor(text_sensor::TextSensor *sensor) {
    ble_version_sensor_ = sensor;
  }
  void set_product_name_text_sensor(text_sensor::TextSensor *sensor) {
    product_name_sensor_ = sensor;
  }
  void set_product_version_text_sensor(text_sensor::TextSensor *sensor) {
    product_version_sensor_ = sensor;
  }
  void set_single_events_text_sensor(text_sensor::TextSensor *sensor) {
    single_events_text_sensor_ = sensor;
  }
  void set_event_log_text_sensor(text_sensor::TextSensor *sensor) {
    event_log_text_sensor_ = sensor;
  }
  void set_history_text_sensor(text_sensor::TextSensor *sensor) {
    history_text_sensor_ = sensor;
  }
  void set_cycle_timestamps_text_sensor(text_sensor::TextSensor *sensor) {
    cycle_timestamps_text_sensor_ = sensor;
  }
  void set_last_clock_sync_sensor(text_sensor::TextSensor *sensor) {
    last_clock_sync_sensor_ = sensor;
  }
#endif
  // Numeric sensor setters for operating statistics
  void set_start_count_sensor(sensor::Sensor *sensor) {
    start_count_sensor_ = sensor;
  }
  void set_operating_hours_sensor(sensor::Sensor *sensor) {
    operating_hours_sensor_ = sensor;
  }
  void set_clock_diff_sensor(sensor::Sensor *sensor) {
    clock_diff_sensor_ = sensor;
  }
  void set_pairing_enabled(bool enabled) { pairing_enabled_ = enabled; }

  void setup() override;
  void loop() override;
  void update() override; // Called every 10 seconds (PollingComponent interval)
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param) override;

  // Static helper: validate if a device is an ALPHA HWR pump (discovery mode).
  // Returns true if the device matches the Grundfos ALPHA HWR product signature.
  static bool is_alpha_hwr_device(const esp32_ble_tracker::ESPBTDevice &device);

  // Called from on_ble_service_data_advertise / on_ble_advertise triggers for
  // the Grundfos service UUID.  Caches product_family/type/version from the
  // service data payload so they are available before the GATT connection opens.
  // The esp32_ble_tracker strips the 2-byte UUID from the payload, so:
  //   x[0..2] = 3-byte frame header
  //   x[3]    = product_family  (0x34 = ALPHA)
  //   x[4]    = product_type    (0x07 = HWR)
  //   x[5]    = product_version
  void on_ble_service_data_seen(const std::vector<uint8_t> &x) {
    ble_manager_.cache_adv_info_from_service_data(x);
  }

private:
  ble_client::BLEClient *parent_ = nullptr;

  bool pairing_enabled_ =
      false; // Controls whether to attempt BLE pairing/bonding

  void authenticate();
  void trigger_initial_data_reads();

  // BLE connection manager (handles all BLE operations)
  core::BLEConnectionManager ble_manager_;

  // BLE transport layer (handles packet reassembly)
  core::Transport transport_;

  // Session state management (handles connection state machine)
  core::Session session_;

  // Authentication module (handles 3-stage handshake)
  core::Authentication auth_;

  // Telemetry service (handles all telemetry operations)
  services::TelemetryService telemetry_service_;

  // Control service (handles pump start/stop and mode changes)
  services::ControlService control_service_;

  // Schedule service (handles weekly schedule management)
  services::ScheduleService schedule_service_;

  // Device information service (handles device identification strings)
  services::DeviceInfoService device_info_service_;

  // Time service (handles pump RTC management)
  services::TimeService time_service_;

  // Event log service (reads pump start/stop event history)
  services::EventLogService event_log_service_;

  // History service (reads trend data: flow, head, temp, power-on)
  services::HistoryService history_service_;

  // Sensor publisher (maps telemetry to ESPHome sensors)
  services::SensorPublisher sensor_publisher_;

  // Pairing status sensor (separate from telemetry)
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
#ifdef USE_TEXT_SENSOR
  // Schedule display sensor
  text_sensor::TextSensor *schedule_text_sensor_{nullptr};
  // Control mode display sensor
  text_sensor::TextSensor *control_mode_sensor_{nullptr};
  // Device information text sensors
  text_sensor::TextSensor *serial_number_sensor_{nullptr};
  text_sensor::TextSensor *software_version_sensor_{nullptr};
  text_sensor::TextSensor *hardware_version_sensor_{nullptr};
  text_sensor::TextSensor *ble_version_sensor_{nullptr};
  text_sensor::TextSensor *product_name_sensor_{nullptr};
  // product_version is decoded from the BLE advertisement at scan time
  // (before any connection) so it is available as a pre-connection discriminator.
  text_sensor::TextSensor *product_version_sensor_{nullptr};
  text_sensor::TextSensor *single_events_text_sensor_{nullptr};
  text_sensor::TextSensor *event_log_text_sensor_{nullptr};
  text_sensor::TextSensor *history_text_sensor_{nullptr};
  text_sensor::TextSensor *cycle_timestamps_text_sensor_{nullptr};
  text_sensor::TextSensor *last_clock_sync_sensor_{nullptr};
#endif

  // Operating statistics sensors
  sensor::Sensor *start_count_sensor_{nullptr};
  sensor::Sensor *operating_hours_sensor_{nullptr};
  sensor::Sensor *clock_diff_sensor_{nullptr};

  // Tracks whether the post-auth data read chain has been triggered.
  // Ensures device info, event log, history, etc. are read even when
  // the BLE connection persists through an ESP32 restart (no re-auth).
  bool initial_data_read_done_{false};

  // Time synchronization tracking
  uint32_t last_time_sync_timestamp_{0}; // millis() when last sync occurred
  static constexpr uint32_t TIME_SYNC_INTERVAL_MS =
      24 * 60 * 60 * 1000; // 24 hours in milliseconds

  /**
   * Check if daily time sync is due and perform it if needed.
   * Called from update() to sync pump RTC with system time once per day.
   */
  void check_and_sync_time();

public:
  // Control service access methods (for ESPHome switches/buttons)
  bool pump_start() { return control_service_.start(); }
  bool pump_stop() { return control_service_.stop(); }
  bool set_control_mode(services::ControlMode mode) {
    return control_service_.set_mode(mode);
  }
  bool enable_remote() { return control_service_.enable_remote_mode(); }
  bool disable_remote() { return control_service_.disable_remote_mode(); }

  // Setpoint configuration methods (for ESPHome number entities)
  void set_constant_pressure(float value_m,
                             std::function<void(bool)> callback) {
    control_service_.set_constant_pressure_async(value_m, callback);
  }
  void set_constant_speed(float value_rpm, std::function<void(bool)> callback) {
    control_service_.set_constant_speed_async(value_rpm, callback);
  }
  void set_constant_flow(float value_m3h, std::function<void(bool)> callback) {
    control_service_.set_constant_flow_async(value_m3h, callback);
  }
  void set_temperature_range(float min_temp, float max_temp, bool autoadapt,
                             std::function<void(bool)> callback) {
    control_service_.set_temperature_range_async(min_temp, max_temp, autoadapt,
                                                 callback);
  }
  void set_proportional_pressure(float value_m,
                                 std::function<void(bool)> callback) {
    control_service_.set_proportional_pressure_async(value_m, callback);
  }
  void set_cycle_time_control(uint8_t on_minutes, uint8_t off_minutes,
                              std::function<void(bool)> callback) {
    control_service_.set_cycle_time_control_async(on_minutes, off_minutes,
                                                  callback);
  }

  // State tracking getters
  services::ControlMode get_control_mode() const {
    return control_service_.get_current_mode();
  }
  bool is_mode_valid() const { return control_service_.is_mode_valid(); }
  bool get_remote_enabled() const {
    return control_service_.get_remote_enabled();
  }
  bool is_pump_enabled_valid() const {
    return control_service_.is_pump_enabled_valid();
  }
  bool is_pump_enabled() const {
    return control_service_.is_pump_enabled();
  }
  static const char *get_control_mode_name(services::ControlMode mode) {
    return services::ControlService::get_mode_name(mode);
  }

  // Cached setpoint getters (NAN = not yet read from pump)
  float get_cached_setpoint() const {
    return control_service_.get_cached_setpoint();
  }
  float get_cached_temp_min() const {
    return control_service_.get_cached_temp_min();
  }
  float get_cached_temp_max() const {
    return control_service_.get_cached_temp_max();
  }
  int8_t get_cached_autoadapt() const {
    return control_service_.get_cached_autoadapt();
  }

  // Schedule service access methods (for ESPHome buttons/lambdas)
  bool enable_schedule() {
    bool success = schedule_service_.enable();
    if (success) {
      this->publish_schedule_json();
    }
    return success;
  }
  bool disable_schedule() {
    bool success = schedule_service_.disable();
    if (success) {
      this->publish_schedule_json();
    }
    return success;
  }
  bool get_schedule_state(bool *result) {
    return schedule_service_.get_state(result);
  }
  bool read_schedule_entries(std::vector<ScheduleEntry> *entries,
                             int layer = -1) {
    return schedule_service_.read_entries(entries, layer);
  }
  bool read_schedule_entries_async(
      int layer, std::function<void(bool, const std::vector<ScheduleEntry> &)>
                     on_complete) {
    return schedule_service_.read_entries_async(layer, on_complete);
  }
  bool write_schedule_entries(const std::vector<ScheduleEntry> &entries,
                              uint8_t layer = 0) {
    return schedule_service_.write_entries(entries, layer);
  }
  bool write_schedule_entries_async(const std::vector<ScheduleEntry> &entries,
                                    uint8_t layer,
                                    std::function<void(bool)> on_complete) {
    return schedule_service_.write_entries_async(entries, layer, on_complete);
  }
  void clear_schedule_entry(const std::string &day, uint8_t layer = 0,
                            std::function<void(bool)> on_complete = nullptr) {
    schedule_service_.clear_entry(day, layer, on_complete);
  }
  bool get_schedule_display_string(const std::vector<ScheduleEntry> &entries,
                                   std::string *result) {
    return schedule_service_.get_schedule_display_string(entries, result);
  }

  // Schedule editor helpers (for HA template entities)
  bool get_cached_schedule_entry(uint8_t layer, uint8_t day_index,
                                 ScheduleEntry *entry) {
    return schedule_service_.get_cached_entry(layer, day_index, entry);
  }
  void set_schedule_entry(uint8_t layer, uint8_t day_index,
                          const ScheduleEntry &entry,
                          std::function<void(bool)> on_complete) {
    schedule_service_.set_entry_async(
        layer, day_index, entry, [this, on_complete](bool success) {
          if (success) {
            // Refresh display after successful write
            this->set_timeout(500, [this]() { this->publish_schedule_json(); });
          }
          if (on_complete)
            on_complete(success);
        });
  }
  void clear_schedule_entry_async(uint8_t layer, uint8_t day_index,
                                  std::function<void(bool)> on_complete) {
    schedule_service_.clear_entry_async(
        layer, day_index, [this, on_complete](bool success) {
          if (success) {
            this->set_timeout(500, [this]() { this->publish_schedule_json(); });
          }
          if (on_complete)
            on_complete(success);
        });
  }
  bool is_schedule_layer_cached(uint8_t layer) const {
    return schedule_service_.is_layer_cached(layer);
  }

  // Single event (one-time schedule) methods
  void read_single_events(
      std::function<void(bool, const std::vector<services::SingleEvent> &)>
          on_complete) {
    schedule_service_.read_single_events_async(
        [this, on_complete](bool success,
                            const std::vector<services::SingleEvent> &events) {
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->single_events_text_sensor_) {
              this->single_events_text_sensor_->publish_state(
                  schedule_service_.format_single_events_display());
            }
#endif
          }
          if (on_complete)
            on_complete(success, events);
        });
  }
  void write_single_event(const services::SingleEvent &event,
                          std::function<void(bool)> on_complete) {
    schedule_service_.write_single_event_async(
        event, [this, on_complete](bool success) {
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->single_events_text_sensor_) {
              this->single_events_text_sensor_->publish_state(
                  schedule_service_.format_single_events_display());
            }
#endif
          }
          if (on_complete)
            on_complete(success);
        });
  }
  void clear_single_event(uint8_t index,
                          std::function<void(bool)> on_complete) {
    schedule_service_.clear_single_event_async(
        index, [this, on_complete](bool success) {
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->single_events_text_sensor_) {
              this->single_events_text_sensor_->publish_state(
                  schedule_service_.format_single_events_display());
            }
#endif
          }
          if (on_complete)
            on_complete(success);
        });
  }
  int find_free_single_event_slot() const {
    return schedule_service_.find_free_single_event_slot();
  }

  // Event log methods
  void read_event_log(std::function<void(bool)> on_complete) {
    event_log_service_.read_entries_async(
        [this, on_complete](
            bool success, const std::vector<services::EventLogEntry> &entries) {
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->event_log_text_sensor_) {
              std::string display = event_log_service_.format_display();
              if (display.size() > 255) {
                display.resize(252);
                display += "...";
              }
              this->event_log_text_sensor_->publish_state(display);
            }
#endif
          }
          if (on_complete)
            on_complete(success);
        });
  }

  // History methods
  void read_history(std::function<void(bool)> on_complete) {
    history_service_.read_trends_async(
        [this, on_complete](bool success,
                            const std::vector<services::TrendSeries> &trends) {
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->history_text_sensor_) {
              std::string display = history_service_.format_display();
              if (display.size() > 255) {
                display.resize(252);
                display += "...";
              }
              this->history_text_sensor_->publish_state(display);
            }
#endif
            // Chain: read cycle timestamps after trends
            this->read_cycle_timestamps();
          }
          if (on_complete)
            on_complete(success);
        });
  }

  /**
   * Read cycle timestamps (last 10 cycles) and publish to text sensor.
   */
  void read_cycle_timestamps() {
    history_service_.read_cycle_timestamps_async(
        10, [this](bool success, const std::vector<uint32_t> &timestamps) {
          if (success && !timestamps.empty()) {
#ifdef USE_TEXT_SENSOR
            if (this->cycle_timestamps_text_sensor_) {
              std::string display;
              for (const auto &ts : timestamps) {
                time_t t = ts;
                const struct tm *tm_info = localtime(&t);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
                if (!display.empty())
                  display += "\n";
                display += buf;
              }
              if (display.size() > 255) {
                display.resize(252);
                display += "...";
              }
              this->cycle_timestamps_text_sensor_->publish_state(display);
            }
#endif
            ESP_LOGI(TAG, "Read %zu cycle timestamps", timestamps.size());
          } else if (!success) {
            ESP_LOGW(TAG, "Failed to read cycle timestamps");
          }
        });
  }

  /**
   * Asynchronously read pump's real-time clock.
   *
   * Reads the current time from the pump's internal RTC (used for schedule
   * execution).
   *
   * @param callback Called with ESPTime representing pump time (invalid ESPTime
   * on failure)
   *
   * Usage:
   *   component->read_pump_clock([](ESPTime pump_time) {
   *     if (pump_time.is_valid()) {
   *       // Time read successfully
   *     }
   *   });
   */
  void read_pump_clock(std::function<void(ESPTime)> callback) {
    time_service_.get_clock_async(callback);
  }

  /**
   * Asynchronously synchronize pump's real-time clock with system time.
   *
   * Sets the pump's internal RTC to match the ESP32's current time (from SNTP).
   * The pump clock is used for schedule execution and event logging.
   *
   * @param callback Called with success status (true if clock was synchronized)
   *
   * Usage:
   *   component->sync_pump_clock([](bool success) {
   *     if (success) {
   *       // Clock synchronized successfully
   *     }
   *   });
   */
  /**
   * Asynchronously read device information and update text sensors.
   *
   * Reads device identification strings (serial, versions, product name) from
   * the pump and publishes them to configured text sensors.
   *
   * This is typically called once after authentication to populate device info.
   *
   * Usage:
   *   Called automatically in authenticate() after successful authentication
   */
  void read_device_info();

  /**
   * Read operating statistics (start count, operating hours) and publish to
   * sensors.
   */
  void read_statistics();

  /**
   * Read pump clock and publish drift sensor (manual trigger for testing).
   */
  void read_pump_clock();

  /**
   * Perform clock synchronization with drift calculation.
   */
  void perform_clock_sync();

  /**
   * Asynchronously read the pump schedule and update the text sensor display.
   *
   * This is a convenience method for displaying the current schedule in Home
   * Assistant. It reads all schedule layers from the pump and formats them into
   * a readable string, then publishes to the schedule_text_sensor if one is
   * configured.
   *
   * Usage in YAML button lambda:
   *   on_press:
   *     - lambda: id(pump).update_schedule_display();
   */
  /**
   * Publish schedule data as JSON to the text sensor for the Lovelace card.
   * Reads from the schedule cache (no BLE traffic). Call after reads/writes
   * complete.
   *
   * JSON format (compact, fits HA 255-char state limit for typical usage):
   *   {"e":1,"s":{"0":[[360,480],[360,480],0,0,0,0,0]}}
   *   - "e": schedule enabled (1/0)
   *   - "s": layers keyed by number, only non-empty layers included
   *   - Each layer: array of 7 entries (Mon=0..Sun=6)
   *   - Entry: [start_minutes, end_minutes] or 0 (disabled/empty)
   */
  void publish_schedule_json() {
#ifdef USE_TEXT_SENSOR
    if (!this->schedule_text_sensor_)
      return;

    bool enabled = false;
    schedule_service_.get_state(&enabled);

    std::string json = "{\"e\":";
    json += enabled ? "1" : "0";
    json += ",\"s\":{";

    bool first_layer = true;
    for (int layer = 0; layer < 5; layer++) {
      if (!schedule_service_.is_layer_cached(layer))
        continue;

      bool has_entries = false;
      std::string layer_json = "[";
      for (int day = 0; day < 7; day++) {
        if (day > 0)
          layer_json += ",";
        ScheduleEntry entry;
        if (get_cached_schedule_entry(layer, day, &entry) &&
            entry.is_enabled()) {
          int start = entry.get_begin_hour() * 60 + entry.get_begin_minute();
          int end = entry.get_end_hour() * 60 + entry.get_end_minute();
          layer_json += "[";
          layer_json += to_string(start);
          layer_json += ",";
          layer_json += to_string(end);
          layer_json += "]";
          has_entries = true;
        } else {
          layer_json += "0";
        }
      }
      layer_json += "]";

      if (has_entries) {
        if (!first_layer)
          json += ",";
        json += "\"";
        json += to_string(layer);
        json += "\":";
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

    this->schedule_text_sensor_->publish_state(json);
    ESP_LOGD(TAG, "Published schedule JSON (%zu chars)", json.size());
#endif
  }

  /**
   * Read schedule from pump and publish JSON. Called during initial setup and
   * refresh.
   */
  void update_schedule_display() {
    ESP_LOGD(TAG, "Refreshing schedule from pump...");
    this->read_schedule_entries_async(
        -1, [this](bool success, const std::vector<ScheduleEntry> &entries) {
          if (!success) {
            ESP_LOGW(TAG, "Failed to read schedule for display update");
            return;
          }
          this->publish_schedule_json();
        });
  }
};

} // namespace alpha_hwr
} // namespace esphome
