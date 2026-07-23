#pragma once

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include "api_bridge.h"
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
#include "write_operation_service.h"
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <ctime>

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
                          public ble_client::BLEClientNode,
                          public esp32_ble_tracker::ESPBTDeviceListener {
public:
  explicit AlphaHwrComponent(ble_client::BLEClient *parent)
      : PollingComponent(10000), auth_(transport_),
        telemetry_service_(transport_), control_service_(transport_, session_),
        schedule_service_(transport_, session_),
        device_info_service_(transport_, session_), time_service_(&transport_),
        event_log_service_(transport_, session_),
        history_service_(transport_, session_),
        write_op_service_(control_service_, schedule_service_) {
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
  void set_ready_binary_sensor(binary_sensor::BinarySensor *sensor) {
    ready_sensor_ = sensor;
  }
#ifdef USE_TEXT_SENSOR
  void set_alarms_text_sensor(text_sensor::TextSensor *sensor) {
    sensor_publisher_.set_alarms_text_sensor(sensor);
  }
  void set_warnings_text_sensor(text_sensor::TextSensor *sensor) {
    sensor_publisher_.set_warnings_text_sensor(sensor);
  }
  void set_schedule_hash_text_sensor(text_sensor::TextSensor *sensor) {
    schedule_hash_text_sensor_ = sensor;
  }
  void set_schedule_layer_text_sensor(uint8_t layer,
                                      text_sensor::TextSensor *sensor) {
    if (layer < 5) schedule_layer_sensors_[layer] = sensor;
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
  void set_vacation_text_sensor(text_sensor::TextSensor *sensor) {
    vacation_text_sensor_ = sensor;
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
  void set_pump_link_status_text_sensor(text_sensor::TextSensor *sensor) {
    pump_link_status_sensor_ = sensor;
  }
  void set_pump_last_link_failure_text_sensor(text_sensor::TextSensor *sensor) {
    pump_last_link_failure_sensor_ = sensor;
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
#ifdef USE_TIME
  void set_time_id(time::RealTimeClock *time_id) {
    time_id_ = time_id;
    time_service_.set_time_id(time_id);
  }
#endif
  void set_pairing_enabled(bool enabled) { pairing_enabled_ = enabled; }
  // Delay (ms) after a disconnect before allowing reconnection, so a
  // just-powered-up pump has time to be ready before encryption is requested.
  // 0 = disabled (immediate reconnect; the default/legacy behavior).
  void set_reconnect_settle_time(uint32_t ms) { this->reconnect_settle_ms_ = ms; }
  // Interval (ms) for periodic control state polling to detect out-of-band pump
  // state changes (e.g., internal schedule execution, manual button press).
  // 0 = disabled (default is 30 seconds; fixes issue #54).
  void set_control_state_poll_interval(uint32_t ms) { 
    control_state_poll_interval_ms_ = ms; 
  }

  void setup() override;
  void loop() override;
  void update() override; // Called every 10 seconds (PollingComponent interval)
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param) override;

  // ESPBTDeviceListener: observes scan results so the reconnect settle window
  // is timed from when the pump REAPPEARS (making it independent of how long
  // the pump was powered off). See reconnect_settle_time.
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

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
  // Helper for retrying the initial cache sync
  void do_control_cache_sync(uint32_t gen);

  ble_client::BLEClient *parent_ = nullptr;

  bool pairing_enabled_ =
      false; // Controls whether to attempt BLE pairing/bonding

  uint32_t reconnect_settle_ms_{0};   // Post-disconnect reconnect hold-off (ms)
  bool reconnect_settling_{false};    // True while holding off reconnect after a disconnect
  bool reconnect_timer_armed_{false}; // True once the settle timer has started this episode

  uint32_t control_state_poll_interval_ms_{30000};  // Control state poll interval (ms); default 30s (fixes #54)
  uint32_t last_control_state_poll_time_{0};        // Timestamp of last control state poll

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

  // Write-operation layer (issue #92): serializes every pump write, confirms
  // it against the pump, and reports one terminal result per operation.
  services::WriteOperationService write_op_service_;

#ifdef ALPHA_HWR_HAS_API_BRIDGE
  // Home Assistant services + write_settled event (issue #92).
  AlphaHwrApiBridge api_bridge_;
#endif

  // Sensor publisher (maps telemetry to ESPHome sensors)
  services::SensorPublisher sensor_publisher_;

  // Pairing status sensor (separate from telemetry)
  binary_sensor::BinarySensor *pairing_status_sensor_{nullptr};
  binary_sensor::BinarySensor *ready_sensor_{nullptr};
#ifdef USE_TEXT_SENSOR
  // Schedule display sensors
  text_sensor::TextSensor *schedule_hash_text_sensor_{nullptr};
  text_sensor::TextSensor *schedule_layer_sensors_[5] = {nullptr, nullptr,
                                                         nullptr, nullptr,
                                                         nullptr};
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
  text_sensor::TextSensor *vacation_text_sensor_{nullptr};
  text_sensor::TextSensor *event_log_text_sensor_{nullptr};
  text_sensor::TextSensor *history_text_sensor_{nullptr};
  text_sensor::TextSensor *cycle_timestamps_text_sensor_{nullptr};
  text_sensor::TextSensor *last_clock_sync_sensor_{nullptr};
  // Pump link status (coarse connection-health enum) + latched last failure
  text_sensor::TextSensor *pump_link_status_sensor_{nullptr};
  text_sensor::TextSensor *pump_last_link_failure_sensor_{nullptr};
#endif

  // Operating statistics sensors
  sensor::Sensor *start_count_sensor_{nullptr};
  sensor::Sensor *operating_hours_sensor_{nullptr};
  sensor::Sensor *clock_diff_sensor_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_id_{nullptr};
#endif

  // Tracks whether the post-auth data read chain has been triggered.
  // Ensures device info, event log, history, etc. are read even when
  // the BLE connection persists through an ESP32 restart (no re-auth).
  bool initial_data_read_done_{false};

  // Generation counter for the initial-read chain timers. Bumped on
  // disconnect so pending set_timeout lambdas from a previous connection
  // self-invalidate instead of firing reads against the next connection
  // (same pattern as auth_sequence_ / scheduler_sequence_). See issue #18.
  uint32_t read_chain_gen_{0};

  // Time synchronization tracking
  uint32_t last_time_sync_timestamp_{0}; // millis() when last sync occurred
  static constexpr uint32_t TIME_SYNC_INTERVAL_MS =
      24 * 60 * 60 * 1000; // 24 hours in milliseconds

  /**
   * Check if daily time sync is due and perform it if needed.
   * Called from update() to sync pump RTC with system time once per day.
   */
  void check_and_sync_time();

  // Pump Link Status evaluator: coarse link-health enum from session/bond/timing,
  // published (plus the latched last-failure string) on change. Driven by the
  // connection/disconnection/auth callbacks and a periodic check in loop().
  void evaluate_link_status();
  uint32_t link_boot_ms_{0};
  uint32_t link_last_open_ms_{0};
  uint32_t link_last_eval_ms_{0};
  uint16_t link_consecutive_failures_{0};
  bool link_ever_opened_{false};
  bool link_reached_ready_{false};
  std::string link_last_status_;
  std::string link_last_failure_published_;

public:
  // Cache coherence and gating
  bool is_state_synchronized() const;
  bool check_ready(const char *action_name) const {
    if (!this->is_state_synchronized()) {
      ESP_LOGW("alpha_hwr", "Command rejected: pump state is not yet fully synchronized (%s)", action_name);
      return false;
    }
    return true;
  }

  // ---- Programmatic write interface (issue #92). Submissions queue in the
  // write-operation layer; each ends in exactly one terminal WriteResult
  // delivered through the result callback (the api bridge turns that into the
  // esphome.alpha_hwr_write_settled Home Assistant event).
  void set_write_result_callback(std::function<void(const services::WriteResult &)> cb) {
    write_op_service_.set_result_callback(std::move(cb));
  }
  void submit_set_enabled(bool enabled, const std::string &op_id) {
    write_op_service_.submit_set_enabled(enabled, op_id);
  }
  void submit_set_mode(services::ControlMode mode, const std::string &op_id) {
    write_op_service_.submit_set_mode(mode, op_id);
  }
  void submit_set_setpoint(services::ControlMode mode, float value, const std::string &op_id) {
    write_op_service_.submit_set_setpoint(mode, value, op_id);
  }
  void submit_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                    const std::string &op_id) {
    write_op_service_.submit_set_temperature_range(min_c, max_c, autoadapt, op_id);
  }
  void submit_set_cycle_times(uint8_t on_minutes, uint8_t off_minutes, float flow,
                              const std::string &op_id) {
    write_op_service_.submit_set_cycle_times(on_minutes, off_minutes, flow, op_id);
  }
  void submit_set_schedule_entry(uint8_t layer, uint8_t day_index, uint8_t begin_hour,
                                 uint8_t begin_minute, uint8_t end_hour, uint8_t end_minute,
                                 const std::string &op_id) {
    write_op_service_.submit_set_schedule_entry(layer, day_index, begin_hour, begin_minute,
                                                end_hour, end_minute, op_id);
  }
  void submit_clear_schedule_entry(uint8_t layer, uint8_t day_index, const std::string &op_id) {
    write_op_service_.submit_clear_schedule_entry(layer, day_index, op_id);
  }
  void submit_set_schedule_enabled(bool enabled, const std::string &op_id) {
    write_op_service_.submit_set_schedule_enabled(enabled, op_id);
  }
  void submit_set_single_event(uint32_t begin_ts, uint32_t end_ts, const std::string &op_id) {
    write_op_service_.submit_set_single_event(begin_ts, end_ts, op_id);
  }
  void submit_clear_single_event(uint8_t slot, const std::string &op_id) {
    write_op_service_.submit_clear_single_event(slot, op_id);
  }
  void submit_set_vacation(uint32_t begin_ts, uint32_t end_ts, const std::string &op_id) {
    write_op_service_.submit_set_vacation(begin_ts, end_ts, op_id);
  }
  void submit_clear_vacation(const std::string &op_id) {
    write_op_service_.submit_clear_vacation(op_id);
  }
  void submit_refresh_schedule(const std::string &op_id) {
    write_op_service_.submit_refresh_schedule(op_id);
  }
  void submit_refresh_single_events(const std::string &op_id) {
    write_op_service_.submit_refresh_single_events(op_id);
  }
  void submit_upload_schedule(codec::UploadRequest request, const std::string &op_id) {
    write_op_service_.submit_upload_schedule(std::move(request), op_id);
  }

  // Control service access methods (for ESPHome switches/buttons).
  // Entity writes route through the write-operation layer (issue #92) with an
  // empty op_id: they get the same serialization, confirm readbacks, and
  // terminal settle events as the programmatic services — one write path.
  bool pump_start() {
    if (!check_ready("start")) return false;
    write_op_service_.submit_set_enabled(true, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
  }
  bool pump_stop() {
    if (!check_ready("stop")) return false;
    write_op_service_.submit_set_enabled(false, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
  }
  bool set_control_mode(services::ControlMode mode) {
    if (!check_ready("set_control_mode")) return false;
    write_op_service_.submit_set_mode(mode, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
  }
  bool enable_remote() { 
    if (!check_ready("enable_remote")) return false;
    return control_service_.enable_remote_mode(); 
  }
  bool disable_remote() { 
    if (!check_ready("disable_remote")) return false;
    return control_service_.disable_remote_mode(); 
  }

  // Setpoint configuration methods (for ESPHome number entities). The bool
  // callback fires with the operation's terminal result (true for
  // accepted/clamped), no longer with the send/ACK of the first wire step.
  void set_constant_pressure(float value_m,
                             std::function<void(bool)> callback) {
    if (!check_ready("set_constant_pressure")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_setpoint(services::ControlMode::CONSTANT_PRESSURE, value_m, "",
                                          callback, services::WriteOrigin::ENTITY);
  }
  void set_constant_speed(float value_rpm, std::function<void(bool)> callback) {
    if (!check_ready("set_constant_speed")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_setpoint(services::ControlMode::CONSTANT_SPEED, value_rpm, "",
                                          callback, services::WriteOrigin::ENTITY);
  }
  void set_constant_flow(float value_m3h, std::function<void(bool)> callback) {
    if (!check_ready("set_constant_flow")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_setpoint(services::ControlMode::CONSTANT_FLOW, value_m3h, "",
                                          callback, services::WriteOrigin::ENTITY);
  }
  void set_temperature_range(float min_temp, float max_temp, bool autoadapt,
                             std::function<void(bool)> callback) {
    if (!check_ready("set_temperature_range")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_temperature_range(min_temp, max_temp, autoadapt, "", callback,
                                                   services::WriteOrigin::ENTITY);
  }
  void set_proportional_pressure(float value_m,
                                 std::function<void(bool)> callback) {
    if (!check_ready("set_proportional_pressure")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_setpoint(services::ControlMode::PROPORTIONAL_PRESSURE, value_m,
                                          "", callback, services::WriteOrigin::ENTITY);
  }
  void set_cycle_time_control(uint8_t on_minutes, uint8_t off_minutes,
                              std::function<void(bool)> callback) {
    if (!check_ready("set_cycle_time_control")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_cycle_times(on_minutes, off_minutes, NAN, "", callback,
                                             services::WriteOrigin::ENTITY);
  }
  // Cycle-mode flow setpoint, m³/h (issue #107). Flow-only write: the op
  // layer resolves the on/off periods from its mandatory fresh read.
  void set_cycle_flow(float value_m3h, std::function<void(bool)> callback) {
    if (!check_ready("set_cycle_flow")) { if (callback) callback(false); return; }
    write_op_service_.submit_set_cycle_times(0, 0, value_m3h, "", callback,
                                             services::WriteOrigin::ENTITY);
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

  // Per-mode cached setpoint getters (NAN = not yet read from pump) — issue #51
  float get_cached_pressure_setpoint() const {
    return control_service_.get_cached_pressure_setpoint();
  }
  float get_cached_proportional_setpoint() const {
    return control_service_.get_cached_proportional_setpoint();
  }
  float get_cached_speed_setpoint() const {
    return control_service_.get_cached_speed_setpoint();
  }
  float get_cached_flow_setpoint() const {
    return control_service_.get_cached_flow_setpoint();
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
  int8_t get_cached_cycle_time_on() const {
    return control_service_.get_cached_cycle_time_on();
  }
  int8_t get_cached_cycle_time_off() const {
    return control_service_.get_cached_cycle_time_off();
  }
  float get_cached_cycle_flow() const {
    return control_service_.get_cached_cycle_flow();
  }

  // Schedule service access methods (for ESPHome buttons/lambdas). Writes
  // route through the write-operation layer (issue #92): verified against a
  // pump readback, serialized with every other write, and reported via the
  // terminal settle event. Display refreshes happen centrally in the
  // write-result hook (see setup()).
  bool enable_schedule() {
    write_op_service_.submit_set_schedule_enabled(true, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
  }
  bool disable_schedule() {
    write_op_service_.submit_set_schedule_enabled(false, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
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
    if (!check_ready("write_schedule_entries")) return false;
    return schedule_service_.write_entries(entries, layer);
  }
  bool write_schedule_entries_async(const std::vector<ScheduleEntry> &entries,
                                    uint8_t layer,
                                    std::function<void(bool)> on_complete) {
    if (!check_ready("write_schedule_entries_async")) { if (on_complete) on_complete(false); return false; }
    return schedule_service_.write_entries_async(entries, layer, on_complete);
  }
  void clear_schedule_entry(const std::string &day, uint8_t layer = 0,
                            std::function<void(bool)> on_complete = nullptr) {
    if (!check_ready("clear_schedule_entry")) { if (on_complete) on_complete(false); return; }
    ScheduleEntry probe;
    probe.set_day(day.c_str());
    int day_index = probe.get_day_index();
    if (day_index < 0) {
      if (on_complete) on_complete(false);
      return;
    }
    write_op_service_.submit_clear_schedule_entry(layer, static_cast<uint8_t>(day_index), "",
                                                  on_complete, services::WriteOrigin::ENTITY);
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
    if (!check_ready("set_schedule_entry")) { if (on_complete) on_complete(false); return; }
    write_op_service_.submit_set_schedule_entry(
        layer, day_index, entry.get_begin_hour(), entry.get_begin_minute(),
        entry.get_end_hour(), entry.get_end_minute(), "", on_complete,
        services::WriteOrigin::ENTITY);
  }
  void clear_schedule_entry_async(uint8_t layer, uint8_t day_index,
                                  std::function<void(bool)> on_complete) {
    if (!check_ready("clear_schedule_entry_async")) { if (on_complete) on_complete(false); return; }
    write_op_service_.submit_clear_schedule_entry(layer, day_index, "", on_complete,
                                                  services::WriteOrigin::ENTITY);
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
            if (this->vacation_text_sensor_) {
              this->vacation_text_sensor_->publish_state(
                  schedule_service_.format_vacation_display());
            }
#endif
          }
          if (on_complete)
            on_complete(success, events);
        });
  }
  void write_single_event(const services::SingleEvent &event,
                          std::function<void(bool)> on_complete) {
    write_op_service_.submit_set_single_event(event.begin_timestamp, event.end_timestamp, "",
                                              on_complete, event.index,
                                              services::WriteOrigin::ENTITY);
  }
  void clear_single_event(uint8_t index,
                          std::function<void(bool)> on_complete) {
    write_op_service_.submit_clear_single_event(index, "", on_complete,
                                                services::WriteOrigin::ENTITY);
  }
  int find_free_single_event_slot() const {
    return schedule_service_.find_free_single_event_slot();
  }

  /**
   * Build a begin/end Unix-timestamp pair from wall-clock month/day/hour/minute
   * fields, anchored to the current local year. Shared by the "Add Single Event"
   * and "Set Vacation" editor buttons (alpha_hwr_schedule_editor.yaml).
   *
   *  - Requires synced system time; refuses to build the pre-2020 timestamps an
   *    unsynced (epoch-1970) clock would otherwise produce.
   *  - Validates that both dates are real calendar dates (day-of-month, incl.
   *    leap years).
   *  - If the end falls strictly before the begin, rolls the end into the next
   *    year so ranges spanning New Year (e.g. Dec 28 -> Jan 3) work. A same-day
   *    range is fine as long as end > begin (e.g. 00:00 -> 23:59).
   *
   * @return true with *begin_ts/*end_ts set on success; false (with a reason
   *         logged under `tag`) otherwise — callers should abort the write.
   */
  bool build_event_window(const char *tag,
                          uint16_t begin_month, uint16_t begin_day,
                          uint16_t begin_hour, uint16_t begin_minute,
                          uint16_t end_month, uint16_t end_day,
                          uint16_t end_hour, uint16_t end_minute,
                          uint32_t *begin_ts, uint32_t *end_ts) const {
    auto valid_ymd = [](uint16_t month, uint16_t day, int tm_year) -> bool {
      if (month < 1 || month > 12 || day < 1)
        return false;
      static const uint8_t days_in_month[12] = {31, 28, 31, 30, 31, 30,
                                                31, 31, 30, 31, 30, 31};
      uint8_t max_day = days_in_month[month - 1];
      if (month == 2) {
        int full_year = tm_year + 1900;
        bool leap = (full_year % 4 == 0 && full_year % 100 != 0) ||
                    (full_year % 400 == 0);
        if (leap)
          max_day = 29;
      }
      return day <= max_day;
    };
    auto make_ts = [](int tm_year, uint16_t month, uint16_t day, uint16_t hour,
                      uint16_t minute) -> time_t {
      struct tm t = {};
      t.tm_year = tm_year;
      t.tm_mon = month - 1;
      t.tm_mday = day;
      t.tm_hour = hour;
      t.tm_min = minute;
      t.tm_sec = 0;
      t.tm_isdst = -1;  // let mktime resolve DST for the local zone
      return mktime(&t);
    };

    time_t now = ::time(nullptr);
    struct tm *lt = ::localtime(&now);
    if (lt == nullptr || lt->tm_year < 120 /* 2020 */) {
      ESP_LOGW(tag, "System time not synced yet — cannot set a dated event");
      return false;
    }
    int year = lt->tm_year;  // years since 1900

    if (!valid_ymd(begin_month, begin_day, year)) {
      ESP_LOGW(tag, "Invalid start date %02d/%02d", begin_month, begin_day);
      return false;
    }
    if (!valid_ymd(end_month, end_day, year)) {
      ESP_LOGW(tag, "Invalid end date %02d/%02d", end_month, end_day);
      return false;
    }

    time_t begin = make_ts(year, begin_month, begin_day, begin_hour, begin_minute);
    time_t end = make_ts(year, end_month, end_day, end_hour, end_minute);

    // End strictly before begin means the range wraps the New Year: re-anchor
    // the end to next year (re-validating day-of-month against the new year, in
    // case Feb 29 leap status differs).
    if (end < begin) {
      if (!valid_ymd(end_month, end_day, year + 1)) {
        ESP_LOGW(tag, "Invalid end date %02d/%02d", end_month, end_day);
        return false;
      }
      end = make_ts(year + 1, end_month, end_day, end_hour, end_minute);
    }

    if (end <= begin) {
      ESP_LOGW(tag, "Start must be before end");
      return false;
    }

    *begin_ts = (uint32_t) begin;
    *end_ts = (uint32_t) end;
    return true;
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
   * Publish the canonical schedule hash (RFC-005 §5.2) — the scheduler's
   * sync-verification sensor — plus the per-layer read-back sensors.
   * "unknown" until the full grid is cached.
   */
  void publish_schedule_hash() {
#ifdef USE_TEXT_SENSOR
    // Per-layer read-back sensors (dhw-sensor-apps issue #7): each layer's
    // compact JSON always fits HA's 255-char state cap.
    for (uint8_t layer = 0; layer < 5; layer++) {
      if (this->schedule_layer_sensors_[layer] != nullptr) {
        this->schedule_layer_sensors_[layer]->publish_state(
            schedule_service_.layer_json(layer));
      }
    }
    if (!this->schedule_hash_text_sensor_)
      return;
    std::string hash = schedule_service_.current_hash();
    this->schedule_hash_text_sensor_->publish_state(hash);
    ESP_LOGD(TAG, "Published schedule hash %s", hash.c_str());
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
          this->publish_schedule_hash();
        });
  }
};

} // namespace alpha_hwr
} // namespace esphome
