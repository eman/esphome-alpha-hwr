#pragma once

#include <functional>
#include <memory>
#include <string>

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
#include "ble_connection_manager.h"
#include "codec.h"
#include "control_service.h"
#include "device_info_service.h"
#include "esphome/core/log.h"
#include "event_log_service.h"
#include "frame_builder.h"
#include "history_service.h"
#include "initial_read_retry.h"  // re-arm predicate for the one-shot initial read chain
#include "link_watchdog.h"       // inbound-data watchdog predicate for a deaf-but-open link
#include "readiness_watchdog.h"  // progress watchdog for a link that is open but never usable
#include "clock_sync_gate.h"  // whether a clock sync can run, and whether to say so
#include "publish_gate.h"  // publish-on-change gates for the YAML control entities (#127)
#include "pump_schedule_ux.h"
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
      : PollingComponent(10000), telemetry_service_(transport_), control_service_(transport_, session_),
        schedule_service_(transport_, session_),
        device_info_service_(transport_), time_service_(&transport_),
        event_log_service_(transport_, session_),
        history_service_(transport_, session_),
        write_op_service_(control_service_, schedule_service_, time_service_) {
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
  // Source revision of this component, resolved by `git describe` at codegen
  // (see __init__.py). Published with the firmware build timestamp as
  // "Component Build" — the release version only changes at a release, so it
  // cannot tell two builds apart (issue #124). "unknown" when git is absent.
  void set_build_revision(const char *revision) { build_revision_ = revision; }
  // Diagnostic "problem" sensor for the dead schedule (issue #124): schedule
  // enabled while the pump is STOP, so no window can run.
  void set_schedule_stalled_binary_sensor(binary_sensor::BinarySensor *sensor) {
    schedule_stalled_sensor_ = sensor;
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
  // "Pump Run State": off / engaged / scheduled / stalled — the one entity that
  // separates AUTO from STOP once the schedule is on (issue #124).
  void set_pump_run_state_text_sensor(text_sensor::TextSensor *sensor) {
    pump_run_state_sensor_ = sensor;
  }
  // "Component Build": which build of this component is running (issue #124).
  void set_component_build_text_sensor(text_sensor::TextSensor *sensor) {
    component_build_sensor_ = sensor;
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
  // Link diagnostics (issue #176). Both are gated on change before publishing:
  // sensor::Sensor::publish_state() does not dedup, so an ungated republish on
  // the ~1 s link tick would cost a frame per API subscriber per second for a
  // value that changes at most once per recycle.
  void set_link_recycles_sensor(sensor::Sensor *sensor) {
    link_recycles_sensor_ = sensor;
  }
  void set_link_max_gap_sensor(sensor::Sensor *sensor) {
    link_max_gap_sensor_ = sensor;
  }
  // The tail histogram (issue #176 part 1). One setter per rung rather than one
  // taking an index, and each one static_asserts that the index it writes holds
  // the threshold its own name claims. The name is the only thing telling an
  // operator what a counter means, and nothing in a reading would reveal a
  // mislabelled one -- so reordering LINK_GAP_THRESHOLDS_MS becomes a compile
  // error here, and a config key in __init__.py that drifts out of step calls a
  // method that does not exist rather than quietly wiring the wrong rung.
  void set_link_gaps_over_15s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[0] == 15000u, "rung 0 is not 15s");
    link_gap_over_sensors_[0] = sensor;
  }
  void set_link_gaps_over_20s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[1] == 20000u, "rung 1 is not 20s");
    link_gap_over_sensors_[1] = sensor;
  }
  void set_link_gaps_over_30s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[2] == 30000u, "rung 2 is not 30s");
    link_gap_over_sensors_[2] = sensor;
  }
  void set_link_gaps_over_45s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[3] == 45000u, "rung 3 is not 45s");
    link_gap_over_sensors_[3] = sensor;
  }
  void set_link_gaps_over_60s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[4] == 60000u, "rung 4 is not 60s");
    link_gap_over_sensors_[4] = sensor;
  }
  void set_link_gaps_over_90s_sensor(sensor::Sensor *sensor) {
    static_assert(LINK_GAP_THRESHOLDS_MS[5] == 90000u, "rung 5 is not 90s");
    link_gap_over_sensors_[5] = sensor;
  }
  void set_link_gaps_truncated_sensor(sensor::Sensor *sensor) {
    link_gaps_truncated_sensor_ = sensor;
  }
  void set_link_watch_time_sensor(sensor::Sensor *sensor) {
    link_watch_time_sensor_ = sensor;
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
  // Budget (ms) the inbound-data watchdog allows between received
  // notifications before it tears the link down; timed from connection-open
  // while nothing has arrived yet. 0 = disabled. See link_watchdog.h for why
  // this is a liveness check rather than a gate on READY.
  void set_data_timeout(uint32_t ms) {
    this->link_data_timeout_ms_ = ms;
    // The in-force window starts at the configured budget. Set here rather
    // than in setup() so it cannot be left at the member default by a config
    // that sets the option -- which would run the first window at 60 s
    // regardless of what was asked for.
    this->link_data_timeout_current_ms_ = ms;
  }
  // Budget (ms) the readiness watchdog allows between connection-open and the
  // pump becoming usable, before it tears the link down. 0 = disabled. See
  // readiness_watchdog.h: this watches PROGRESS where the one above watches
  // liveness, and the two are not interchangeable -- a session stuck with the
  // pump still volunteering telemetry re-arms the liveness watchdog on every
  // frame and is invisible to it (issue #211).
  // Whether an expired readiness budget also tears the link down, or only says
  // so. Defaults to false, and the asymmetry is the whole reason the two are
  // separate options: naming the fault costs nothing, while recycling takes
  // another run at the encryption-on-open window that can erase a bond
  // (issue #14) -- and on a configuration nobody has yet observed (issue #244)
  // it may do that forever. So the diagnosis ships on and the remedy is opt-in.
  void set_ready_recycle(bool enabled) { link_ready_recycle_ = enabled; }
  void set_ready_timeout(uint32_t ms) {
    this->link_ready_timeout_ms_ = ms;
    this->link_ready_timeout_current_ms_ = ms;
  }
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
  // Converge the pump to a Run/Schedule target, writing only the fields that
  // differ from the current cached state (both writes if a field is unknown).
  // Ordering avoids a transient dead/gated state: disable the schedule before
  // touching run state when turning it off; set AUTO before enabling the
  // schedule when turning it on (so there is never a STOP+schedule moment).
  // origin/op_id let the autonomous dead-schedule repair (issue #124) report
  // itself as INTERNAL with a stable op_id, so a client watching write_settled
  // can tell a self-repair from a switch toggle. The switches keep the
  // defaults.
  void apply_pump_schedule_target_(const ux::PumpScheduleTarget &target,
                                   services::WriteOrigin origin = services::WriteOrigin::ENTITY,
                                   const std::string &op_id = "") {
    bool cur_schedule = false;
    bool schedule_known = schedule_service_.get_state(&cur_schedule);
    bool pump_known = control_service_.is_pump_enabled_valid();
    bool cur_pump = control_service_.is_pump_enabled();

    bool write_schedule = !schedule_known || cur_schedule != target.schedule_enabled;
    bool write_pump = !pump_known || cur_pump != target.pump_enabled;

    if (write_schedule && !target.schedule_enabled) {
      write_op_service_.submit_set_schedule_enabled(false, op_id, nullptr, origin);
    }
    if (write_pump) {
      write_op_service_.submit_set_enabled(target.pump_enabled, op_id, nullptr, origin);
    }
    if (write_schedule && target.schedule_enabled) {
      write_op_service_.submit_set_schedule_enabled(true, op_id, nullptr, origin);
    }
  }

  // Publishes the run-state diagnostics and repairs a dead schedule (issue
  // #124). Called from update() once the caches are synchronized. Defined in
  // alpha_hwr.cpp.
  void reconcile_run_state_();
  // True while an enabled Stop single-event (vacation) covers now — a commanded
  // stop, so a stopped pump there is expected rather than a dead schedule.
  bool stop_single_event_active_() const;

  // `git describe` of the component source, filled in at codegen (issue #124).
  const char *build_revision_{"unknown"};

  // Dead-schedule repair throttle (issue #124). A repair normally follows an
  // *external* write that created the stall, so the component can never spin on
  // its own. But a pump that reverts to STOP by itself (an alarm forcing it,
  // say) would otherwise draw one write every poll forever, so attempts are
  // spaced by at least this interval — measured across stall episodes and
  // across reconnects, so neither a relapse nor a flapping BLE link can
  // multiply them. The first attempt after boot is immediate.
  static constexpr uint32_t DEAD_SCHEDULE_REPAIR_MIN_INTERVAL_MS = 300000;  // 5 min
  uint32_t last_repair_attempt_ms_{0};
  bool repair_attempted_{false};
  // Last published run-state / stalled values, so both entities publish only on
  // change (update() runs every 10s).
  const char *run_state_published_{nullptr};
  int8_t stalled_published_{-1};  // -1 = never published

  // Helper for retrying the initial cache sync
  void do_control_cache_sync(uint32_t gen);

  ble_client::BLEClient *parent_ = nullptr;

  bool pairing_enabled_ =
      false; // Controls whether to attempt BLE pairing/bonding

  uint32_t reconnect_settle_ms_{0};   // Post-disconnect reconnect hold-off (ms)
  bool reconnect_settling_{false};    // True while holding off reconnect after a disconnect
  bool reconnect_timer_armed_{false}; // True once the settle timer has started this episode

  uint32_t link_data_timeout_ms_{60000};  // Inbound-data watchdog budget (ms); 0 = disabled
  // 300000, matching the schema default. Kept in step deliberately: while these
  // and the schema disagreed, every host test inherited a budget no shipped
  // config produced.
  uint32_t link_ready_timeout_ms_{300000};  // Readiness watchdog budget (ms); 0 = disabled
  bool link_ready_recycle_{false};          // ...and whether expiry also recycles the link

  uint32_t control_state_poll_interval_ms_{30000};  // Control state poll interval (ms); default 30s (fixes #54)
  uint32_t last_control_state_poll_time_{0};        // Timestamp of last control state poll

  // How long after notifications are enabled the session waits before it is
  // declared ready. Nothing is sent during this window -- see the arming site
  // in setup() for what it separates and why it is not simply removed.
  static constexpr uint32_t SESSION_STABILIZE_MS = 2000;

  // The name of that timer. A named constant because the arm and the cancel are
  // ~80 lines apart and a typo in either is silent: an uncancelled timer
  // declares the NEXT connection ready before it has stabilized, which is
  // issue #15 in a new costume.
  static constexpr const char *SESSION_READY_TIMER = "hwr_session_ready";

  // Runs once per connection, SESSION_STABILIZE_MS after notifications are
  // enabled: declares the session ready and starts everything downstream of
  // that. Replaces authenticate() plus Authentication's completion callback,
  // which between them did exactly this and sent four GENIbus reads whose
  // replies this component discarded (issue #174).
  void on_session_stabilized_();
  void trigger_initial_data_reads();

  // BLE connection manager (handles all BLE operations)
  core::BLEConnectionManager ble_manager_;

  // BLE transport layer (handles packet reassembly)
  core::Transport transport_;

  // Session state management (handles connection state machine)
  core::Session session_;

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
  binary_sensor::BinarySensor *schedule_stalled_sensor_{nullptr};
#ifdef USE_TEXT_SENSOR
  // Schedule display sensors
  text_sensor::TextSensor *schedule_hash_text_sensor_{nullptr};
  text_sensor::TextSensor *schedule_layer_sensors_[5] = {nullptr, nullptr,
                                                         nullptr, nullptr,
                                                         nullptr};
  // Control mode display sensor
  text_sensor::TextSensor *control_mode_sensor_{nullptr};
  // Run state (off/engaged/scheduled/stalled) — see reconcile_run_state_()
  text_sensor::TextSensor *pump_run_state_sensor_{nullptr};
  // Build identity, published once in setup()
  text_sensor::TextSensor *component_build_sensor_{nullptr};
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
  // Link diagnostics (issue #176) — not operating statistics; the comment above
  // labels start_count_/operating_hours_ further down.
  sensor::Sensor *link_recycles_sensor_{nullptr};
  sensor::Sensor *link_max_gap_sensor_{nullptr};
  // Last values published to the two above, for the change gate. Sentinels
  // rather than 0, so the first publish always goes out -- a counter that is
  // genuinely 0 still has to reach Home Assistant once to be thresholdable.
  uint32_t link_recycles_published_{0xFFFFFFFFu};
  uint32_t link_max_gap_published_{0xFFFFFFFFu};
  // The tail histogram, its trust check, and the time they were drawn from
  // (issue #176 part 1). The counters move at most once per quiet interval past
  // 15 s -- effectively never on a healthy link -- so the change gate above is
  // the whole story for them.
  sensor::Sensor *link_gap_over_sensors_[LINK_GAP_BUCKETS]{};
  sensor::Sensor *link_gaps_truncated_sensor_{nullptr};
  // One sentinel per rung, spelled out because there is no aggregate
  // initialiser for "fill". A rung added without extending this list would be
  // value-initialised to 0, which equals its starting count -- so the change
  // gate would never fire and that counter's zero baseline would never reach
  // Home Assistant, breaking exactly the total_increasing reset accounting the
  // sentinels exist for. Silent, and only on the new rung, so:
  static_assert(LINK_GAP_BUCKETS == 6,
                "add a sentinel below for every rung in LINK_GAP_THRESHOLDS_MS");
  uint32_t link_gap_over_published_[LINK_GAP_BUCKETS]{
      0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
      0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint32_t link_gaps_truncated_published_{0xFFFFFFFFu};
  // Watched time is the exception: it advances on every notification, so a
  // change gate alone would publish it every 10 s forever. Throttled to
  // LINK_GAP_WATCH_PUBLISH_MS as well, with the first publish exempt so the
  // zero baseline reaches Home Assistant at boot -- its total_increasing reset
  // accounting needs to see that baseline or it charges the new run's counts to
  // the old one.
  sensor::Sensor *link_watch_time_sensor_{nullptr};
  uint32_t link_watch_time_published_{0xFFFFFFFFu};
  uint32_t link_watch_time_publish_ms_{0};
  void publish_link_diagnostics_(uint32_t now_ms);
  /// The largest threshold that actually has a sensor attached, in ms; 0 when
  /// no rung is configured.
  ///
  /// What the censoring warning is gated on, rather than "any histogram entity
  /// exists". Every rung is independently optional, so the top of the ladder is
  /// not the top of a given config: declaring only `link_gaps_over_15s` under
  /// the 60 s default is a perfectly sound setup, and warning it about the 90 s
  /// rung it never asked for would be noise. Declaring only `link_watch_time`
  /// has no rung to censor at all.
  ///
  /// Relies on LINK_GAP_THRESHOLDS_MS being ascending, which
  /// test_gap_thresholds_are_the_documented_set() pins.
  uint32_t link_gap_top_configured_rung_ms_() const {
    uint32_t top = 0;
    for (size_t i = 0; i < LINK_GAP_BUCKETS; i++) {
      if (this->link_gap_over_sensors_[i] != nullptr)
        top = LINK_GAP_THRESHOLDS_MS[i];
    }
    return top;
  }

  sensor::Sensor *start_count_sensor_{nullptr};
  sensor::Sensor *operating_hours_sensor_{nullptr};
  sensor::Sensor *clock_diff_sensor_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_id_{nullptr};
#endif

  // Tracks whether the post-ready data read chain has been triggered.
  // Ensures device info, event log, history, etc. are read even when
  // the BLE connection persists through an ESP32 restart (the session was
  // already ready, so on_session_stabilized_() never ran).
  bool initial_data_read_done_{false};

  // millis() when the current initial-read attempt was triggered. The chain is
  // a one-shot, so an attempt whose reads never land would otherwise leave the
  // device half-initialised until the next disconnect; this timestamp is what
  // lets update() notice and re-arm. See initial_read_retry.h.
  uint32_t initial_read_started_ms_{0};

  // How long the first attempt gets before update() re-arms the chain. A
  // healthy link lands everything in ~20 s.
  static constexpr uint32_t INITIAL_READ_SYNC_TIMEOUT_MS = 60000;

  // Ceiling for the doubling backoff between re-arms. A pump that never
  // returns one of these values settles here rather than being re-read at a
  // fixed rate forever.
  static constexpr uint32_t INITIAL_READ_RETRY_MAX_MS = 600000;

  // Current re-arm interval; doubles on each retry, resets when the chain
  // finally lands. See initial_read_retry.h.
  uint32_t initial_read_retry_interval_ms_{INITIAL_READ_SYNC_TIMEOUT_MS};

  // What the chain itself produces, as opposed to what heals on its own. The
  // control and schedule caches are refreshed by update()'s own polls, so they
  // cannot tell whether the chain landed; these can. Reset on disconnect and on
  // each re-arm so a retry re-proves them rather than inheriting the verdict.
  bool device_info_read_ok_{false};
  bool statistics_read_ok_{false};

  // The chain's clock leg publishes the pre-sync drift -- how far out the pump
  // was when we found it. Re-running it after a sync has corrected the pump
  // would replace that figure with a meaningless zero, so it runs on the first
  // attempt of a connection only. It no longer writes anything; the write is
  // check_and_sync_time()'s (see the leg's own comment for why it could never
  // have written from there).
  bool initial_clock_sync_started_{false};

  /// True once the reads that only the chain performs have landed.
  bool chain_products_complete_() const {
    return device_info_read_ok_ && statistics_read_ok_;
  }

  // Generation counter for the initial-read chain timers. Bumped on
  // disconnect so pending set_timeout lambdas from a previous connection
  // self-invalidate instead of firing reads against the next connection
  // (same pattern as scheduler_sequence_). See issue #18.
  uint32_t read_chain_gen_{0};

  // Time synchronization tracking.
  //
  // The stamp is taken when a sync is SUBMITTED, not when one succeeds. That
  // matters now that a sync can honestly fail: update() runs every 10 s and
  // the operation itself can take 25 s, so throttling on success alone would
  // let a pump whose clock cannot be confirmed collect a fresh write every
  // 10 s, several of them in flight at once. Stamping at submission makes the
  // interval below a floor on ATTEMPTS, which is what bounds the traffic.
  //
  // 0 means never attempted, so the first update() after boot syncs.
  uint32_t last_time_sync_timestamp_{0};
  // How long to wait before the next attempt: a day after a confirmed sync,
  // 15 minutes after one that did not confirm. Retrying a failed sync a day
  // later would leave the pump running its schedule off a wrong clock for a
  // day; retrying it every 10 s is the write storm above.
  //
  // Only a submitted sync sets this. When the pump is not yet synchronized no
  // attempt is stamped and the next update() retries in 10 s, which costs
  // nothing because that path never reaches the wire.
  //
  // The other two ways to have nothing to write -- no time_id, and a time
  // source that never answers -- no longer reach here at all: they are
  // permanent, and check_and_sync_time() turns them away at the gate rather
  // than re-arming a retry that can only fail (clock_sync_gate.h).
  uint32_t time_sync_interval_ms_{0};
  static constexpr uint32_t TIME_SYNC_INTERVAL_MS =
      24 * 60 * 60 * 1000; // 24 hours in milliseconds
  static constexpr uint32_t TIME_SYNC_RETRY_MS = 15 * 60 * 1000;  // 15 minutes

  /**
   * Submit a pump clock sync and record its outcome (the "Last Clock Sync"
   * text sensor and the retry interval above). The single place the RTC is
   * written from. Called only by check_and_sync_time(); the boot read-chain leg
   * reads the pump's clock for the drift figure and does not write.
   *
   * @param reason Short label for the logs.
   * @return False when nothing was submitted. Callers reach this only with a
   *   usable wall clock in hand (check_and_sync_time() gates on it), so in
   *   practice that means the pump's state cache is not synchronized yet.
   */
  bool submit_clock_sync_(const char *reason);

  /**
   * @brief Publish the pump-vs-node clock drift, or say why it cannot be had.
   *
   * Both callers -- the read chain's pre-sync measurement and the manual "Read
   * Pump Clock" button -- ask the same question, and until issue #270 each
   * answered it with its own copy of the clock read and its own copy of the
   * literal 1609459200. They still differ in one way, so this reports rather
   * than decides what to publish on failure: the read chain leaves the last
   * good reading alone (overwriting "how far out was the pump when we found
   * it" with NAN is the defect #259 fixed), while the manual read publishes
   * NAN, because someone who pressed the button is owed an answer and
   * "unknown" is the true one.
   *
   * @param pump_time A pump clock already known to be valid.
   * @param context Placed in the log line, e.g. "before sync".
   * @return True when a drift was computed and published.
   */
  bool publish_clock_drift_(const ESPTime &pump_time, const char *context);


  /**
   * Check if daily time sync is due and perform it if needed.
   * Called from update() to sync pump RTC with system time once per day.
   */
  void check_and_sync_time();

  /// Report, at most hourly, that the pump's clock is not being kept.
  /// @param why short phrase completing "Pump clock is not being synced: ..."
  void warn_clock_not_syncing_(const char *why);

  uint32_t clock_warn_last_ms_{0};
  static constexpr uint32_t CLOCK_WARN_INTERVAL_MS = 60 * 60 * 1000;  // 1 hour

  /// True when this build has a wall clock the pump can be synced from.
  ///
  /// Two distinct ways to have none, and they behave identically: `time_id` was
  /// not configured, or the firmware was built with no ESPHome time component
  /// at all. Both are permanent for the life of the run, which is what
  /// separates them from "SNTP has not answered yet" -- that one resolves on
  /// its own; the gate answers WAIT for it and returns before the retry loop,
  /// which now serves only a pump whose state cache is not synchronized yet.
  /// Keeping the USE_TIME guard here means the callers read as plain questions.
  bool has_wall_clock_() const {
#ifdef USE_TIME
    return time_id_ != nullptr;
#else
    return false;
#endif
  }

  // Pump Link Status evaluator: coarse link-health enum from session/bond/timing,
  // published (plus the latched last-failure string) on change. Driven by the
  // connection/disconnection/ready callbacks and a periodic check in loop().
  void evaluate_link_status();

  // Inbound-data watchdog (link_watchdog.h): recycles a link that is open and
  // apparently healthy but delivers no notifications. Timed from
  // connection-open and refreshed on every received notification, so it covers
  // both a connection that never produced data and a session that goes deaf.
  /// @return true if it tore the link down, so the caller can skip the
  ///         readiness check rather than disconnecting twice in one tick.
  bool check_link_liveness_();
  uint32_t link_last_inbound_ms_{0};

  // Backoff state for that watchdog (issue #176). link_data_timeout_ms_ is the
  // configured budget and never changes; this is the window currently in force.
  // It doubles on every recycle that produced no data and resets to the
  // configured value on a notification received while the session is READY (a
  // deaf pump still volunteers operation-status notifications of its own
  // accord, so resetting on those would mean the
  // backoff never engages), so a link that can recover is unaffected while a
  // permanently deaf one stops being recycled ~1,300 times a day. Not reset at
  // connection-open: a widened window governs the next connection's opening
  // too. Initialised from the configured value in setup().
  uint32_t link_data_timeout_current_ms_{60000};

  // Readiness watchdog (issue #211). Its observable is progress, not data: the
  // link has been open this long and the pump still is not usable.
  //
  // The arming rule is the whole design and it is one line: link_open_ms_ is
  // stamped at connection-open and by NOTHING else. Not by data, not by a
  // session transition, not by a cache filling partway. A check re-armed by
  // something on the way to the state it is waiting for feeds itself and can
  // never fire -- which is exactly what the Pump Link Status ladder did to
  // link_last_open_ms_, documented in link_watchdog.h, and what the reporter of
  // #211 predicted for this timer before it existed.
  void check_link_readiness_();
  uint32_t link_ready_since_ms_{0};
  // Whether THIS connection ever reached the usable state.
  //
  // Not read back off ready_sensor_, and that is a correctness fix rather than
  // a style choice: `ready_status` is cv.Optional, so a hand-written config
  // that omits the entity would leave the watchdog's notion of readiness
  // permanently false and recycle a perfectly healthy pump every five minutes,
  // then hourly, forever -- each recycle re-entering the encryption-on-open
  // path that can erase the bond (issue #14). A diagnostic that depends on a
  // diagnostic being declared is not a diagnostic.
  //
  // Latched rather than recomputed, because is_state_synchronized() can go
  // false again if a cache is invalidated mid-session, and the timer must not
  // re-arm against a connection that already proved itself. Cleared only at
  // connection-open, with link_ready_since_ms_.
  bool link_pump_ready_seen_{false};
  // Backoff, sharing the data watchdog's doubling and ceiling. Reset when the
  // pump actually becomes ready, which is the only evidence that the previous
  // window was merely too short rather than the link being stuck.
  uint32_t link_ready_timeout_current_ms_{300000};
  // Consecutive recycles that never reached readiness.
  //
  // Added INTO the published link_recycles rather than exposed separately. The
  // first version left it out of the published value entirely, on the theory
  // that link_recycles already covered it -- it did not, because a readiness
  // recycle never touches the data counter, so the one number issue #211's
  // reporter named as their outside-the-component signal ("can see Pump Link
  // Recycles climbing") stayed at zero through exactly the failure this change
  // exists for. Each counter is reset by its own evidence; the sum is what an
  // automation thresholds on, and "this link is not working" is one question.
  uint32_t link_recycles_without_ready_{0};

  // Consecutive recycles with no data in between. Reset by a notification
  // received while the session is READY -- the same gate as the backoff window
  // above, and for the same reason -- so it reads 0 in normal operation and an
  // automation can threshold on it instead of having to detect a flap cadence
  // live.
  uint32_t link_recycles_without_data_{0};

  // Longest quiet interval observed since boot, for choosing the data_timeout
  // default from what actually happens on real installations rather than from a
  // constants calculation. Samples exactly the intervals the watchdog above is
  // timed over — including the open-to-first-notification one, and including an
  // interval that ends in a recycle rather than a notification; see
  // LinkGapSampler for why both of those are load-bearing. Stamped alongside
  // link_last_inbound_ms_ at all three of its call sites.
  LinkGapSampler link_gap_;

  uint32_t link_boot_ms_{0};
  uint32_t link_last_open_ms_{0};
  uint32_t link_last_eval_ms_{0};
  uint16_t link_consecutive_failures_{0};
  /// Diagnostic suspend (issue #243): the link is down because someone asked for
  /// it, not because anything failed.
  ///
  /// The pump accepts ONE BLE connection at a time, so a bonded, connected node
  /// owns it and nothing else can have it. Two things want it:
  ///
  ///   - the Grundfos GO app, which is how you unlock the pump's front panel,
  ///     which is how you re-pair. The reporter logged six power cycles in one
  ///     night to free the pump for it, with outages from 44 seconds to 40
  ///     minutes;
  ///   - another CLIENT, for bench work -- the Python implementation in the
  ///     sibling alpha-hwr repo, or a throwaway probe. Protocol discovery and
  ///     wire-level reads of objects this component does not implement yet both
  ///     need something else on the link, and comparing the two implementations
  ///     against the same pump needs to go back and forth. Before this, that
  ///     meant flashing probe firmware and then flashing this back.
  ///
  /// Neither needs the bond cleared, which is the trap this replaces: clearing
  /// the ESP's bond strands the pump until someone re-pairs at the pump itself.
  bool suspended_{false};

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

  // Coupled `set_pump_state` service: a single selector over the three-state
  // machine (off / engaged / scheduled), the programmatic sibling of the two
  // switches. Composed from the raw SET_PUMP_ENABLED + SET_SCHEDULE_ENABLED
  // writes (so each keeps its own pump-verified readback), then reports ONE
  // aggregate outcome via on_complete(ok, actual_engaged, actual_scheduled,
  // detail). on_complete fires exactly once: immediately for a no-op (already
  // in the target state), otherwise after every issued sub-write settles. `ok`
  // is false if any sub-write was not accepted; actual_* are read back from the
  // pump so a partial failure (e.g. the second write fails) reports the real
  // end state. The underlying flag writes surface as their own op_id="" settle
  // events (like entity writes); this call adds the one event under the op_id.
  //
  // The two flags reach `on_complete` as TRI-STATE int8_t -- 1, 0, or -1 for
  // "not known". Both caches can be invalid (a disconnect clears them, and the
  // run-state cache also goes invalid after an unconfirmed write), and the two
  // reads here return that validity: is_pump_enabled_valid(), and get_state()'s
  // own return. Both used to be discarded, and the flags were passed as plain
  // bools, so a rejected call reported a CONCRETE state -- typically
  // `enabled: false, schedule_enabled: false, state: "off"` -- for a pump
  // nothing had been written to and whose real state was unknown. An automation
  // reading `state` concluded the pump was off. The bridge already had the
  // right encoding for unknown and a guard to use it; nothing could ever
  // produce it, so the guard was dead.
  void submit_set_pump_state(
      ux::PumpScheduleTarget target,
      std::function<void(services::WriteStatus, int8_t, int8_t, const std::string &)> on_complete) {
    using services::WriteStatus;
    auto tri = [](bool known, bool value) -> int8_t { return known ? (value ? 1 : 0) : -1; };
    if (!check_ready("set_pump_state")) {
      bool cs = false;
      const bool cs_known = schedule_service_.get_state(&cs);
      if (on_complete) {
        on_complete(WriteStatus::REJECTED,
                    tri(control_service_.is_pump_enabled_valid(), control_service_.is_pump_enabled()),
                    tri(cs_known, cs), "pump not connected/synchronized");
      }
      return;
    }

    bool cur_engaged = control_service_.is_pump_enabled();
    bool engaged_known = control_service_.is_pump_enabled_valid();
    bool cur_scheduled = false;
    bool sched_known = schedule_service_.get_state(&cur_scheduled);

    bool need_engaged = !engaged_known || cur_engaged != target.pump_enabled;
    bool need_scheduled = !sched_known || cur_scheduled != target.schedule_enabled;

    if (!need_engaged && !need_scheduled) {
      // No-op: already in the requested state. Still fire exactly one terminal
      // result so a client waiting on this op_id never hangs.
      // Both flags are known here by construction: need_* is true whenever
      // either is unknown, so reaching this branch means both reads were valid.
      if (on_complete) on_complete(WriteStatus::ACCEPTED, cur_engaged ? 1 : 0,
                                   cur_scheduled ? 1 : 0, "no change");
      return;
    }

    struct Agg {
      int pending{0};
      WriteStatus worst{WriteStatus::ACCEPTED};
      std::function<void(WriteStatus, int8_t, int8_t, const std::string &)> on_complete;
    };
    auto agg = std::make_shared<Agg>();
    agg->pending = (need_engaged ? 1 : 0) + (need_scheduled ? 1 : 0);
    agg->on_complete = std::move(on_complete);

    auto *self = this;
    // Each sub-write reports its real terminal WriteStatus; keep the most severe
    // across them (a coupled call is only as good as its worst leg), so
    // TIMEOUT / SUPERSEDED / REJECTED stay distinguishable for retry logic.
    auto step = [self, agg](WriteStatus st) {
      if (services::write_status_severity(st) > services::write_status_severity(agg->worst)) {
        agg->worst = st;
      }
      if (--agg->pending > 0) return;
      bool as = false;
      const bool as_known = self->schedule_service_.get_state(&as);
      const int8_t ae = self->control_service_.is_pump_enabled_valid()
                            ? (self->control_service_.is_pump_enabled() ? 1 : 0)
                            : -1;
      const int8_t asx = as_known ? (as ? 1 : 0) : -1;
      bool ok = agg->worst == WriteStatus::ACCEPTED || agg->worst == WriteStatus::CLAMPED;
      std::string detail =
          ok ? "" : std::string("a sub-write settled ") + services::write_status_to_string(agg->worst);
      if (agg->on_complete) agg->on_complete(agg->worst, ae, asx, detail);
    };

    // Order avoids a transient dead/gated state: disable the schedule before
    // touching run state; enable it only after AUTO is set. The status callback
    // is the 5th arg (bool `done` stays null — we only need the full status).
    if (need_scheduled && !target.schedule_enabled) {
      write_op_service_.submit_set_schedule_enabled(false, "", nullptr, services::WriteOrigin::SERVICE, step);
    }
    if (need_engaged) {
      write_op_service_.submit_set_enabled(target.pump_enabled, "", nullptr, services::WriteOrigin::SERVICE, step);
    }
    if (need_scheduled && target.schedule_enabled) {
      write_op_service_.submit_set_schedule_enabled(true, "", nullptr, services::WriteOrigin::SERVICE, step);
    }
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
    write_op_service_.submit_set_remote_mode(true, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
  }
  bool disable_remote() {
    if (!check_ready("disable_remote")) return false;
    write_op_service_.submit_set_remote_mode(false, "", nullptr, services::WriteOrigin::ENTITY);
    return true;
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
  /**
   * Remote-mode state with a "not read yet" signal, matching
   * get_schedule_state(). The bare getter above returns false both for an
   * observed Local/Panel and for a cache nothing has been read into, so an
   * entity using it shows OFF on a fresh connect -- and a scene reasserting
   * "off" against that then issues a write that settles TIMEOUT.
   */
  bool get_remote_state(bool *result) {
    if (!control_service_.is_remote_state_valid()) return false;
    if (result) *result = control_service_.get_remote_enabled();
    return true;
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

  // ---- "Engage Pump" vs "Schedule Enabled" reconciliation (issue: pump/
  // schedule switch reconciliation). These back the two UI switches and keep
  // them mutually exclusive like the Grundfos GO app, without ever creating a
  // dead schedule (STOP + schedule enabled never runs — bench-proven). The
  // three states are Off (STOP), Engaged (AUTO + schedule off), Scheduled
  // (AUTO + schedule on). "Engage Pump" engages the pump's mode (operation_mode
  // AUTO); whether the motor spins is mode-dependent (continuous in constant
  // modes, cycling in temperature/cycle-time). Pure target/display logic lives
  // in pump_schedule_ux.h; the programmatic services (submit_set_enabled /
  // submit_set_schedule_enabled) stay raw and uncoupled for automations.

  /// Drop the BLE link and stop reconnecting until released (issue #243).
  ///
  /// Not persisted: the flag initialises false, so a node coming back from a
  /// power cut is connected. A node that refuses to talk to the pump because
  /// someone flipped a switch a week ago is a worse failure than the
  /// inconvenience this solves -- and it is also the only way out if the API is
  /// unavailable, since there is no timeout and no auto-release.
  ///
  /// Suspending is NOT idempotent, deliberately: the teardown is fire-and-forget
  /// and can fail to take, so a second call retries it. Releasing is, because
  /// its side effects are destructive on repeat. See the guard.
  void set_suspended(bool suspended);
  bool is_suspended() const { return suspended_; }

  // "Engage Pump" switch: mode engaged continuously *now* = AUTO and not
  // schedule-gated. Returns false when either input is not yet cached (switch
  // shows unknown).
  bool get_engage_pump_state(bool *result) {
    bool schedule_on = false;
    if (!control_service_.is_pump_enabled_valid() ||
        !schedule_service_.get_state(&schedule_on)) {
      return false;
    }
    *result = ux::engage_pump_display(control_service_.is_pump_enabled(), schedule_on);
    return true;
  }

  // Toggle "Engage Pump": ON = engage continuously (AUTO + schedule off),
  // OFF = Off (STOP).
  void set_engage_pump(bool on) {
    if (!check_ready(on ? "engage_pump_on" : "engage_pump_off")) return;
    apply_pump_schedule_target_(on ? ux::engage_pump_on_target()
                                   : ux::engage_pump_off_target());
  }

  // Toggle "Schedule Enabled": ON = Scheduled (AUTO so it can actually run +
  // schedule on), OFF = Off (stop the pump + schedule off).
  void set_schedule(bool on) {
    if (!check_ready(on ? "schedule_on" : "schedule_off")) return;
    apply_pump_schedule_target_(on ? ux::schedule_on_target()
                                   : ux::schedule_off_target());
  }

  bool read_schedule_entries_async(
      int layer, std::function<void(bool, const std::vector<ScheduleEntry> &)>
                     on_complete) {
    return schedule_service_.read_entries_async(layer, on_complete);
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
            // Gated: this runs on every refresh_single_events service call --
            // which the schedule card fires after each save -- and again on
            // every reconnect, almost always with an identical string. An
            // ungated publish is an API frame per subscriber each time for no
            // change (issue #127 / AGENTS §4).
            if (this->single_events_text_sensor_) {
              publish_text_sensor_if_changed(
                  this->single_events_text_sensor_,
                  schedule_service_.format_single_events_display());
            }
            if (this->vacation_text_sensor_) {
              publish_text_sensor_if_changed(
                  this->vacation_text_sensor_,
                  schedule_service_.format_vacation_display(
                      time_service_.now_unix()));
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
  // A free-slot accessor used to live here, and the schedule editor's "Add
  // Single Event" button called it and then wrote to the slot it returned by
  // index. Nothing closed the gap between the two: a service call resolving in
  // that gap takes the same slot, writes a live event to it, and the button's
  // write then overwrites it. The picker is not the problem -- picking and
  // writing as separate steps is -- so the accessor is gone and the button
  // submits with no slot, letting the write-operation layer resolve one at the
  // moment it writes (issue #262). Callers that genuinely know their slot use
  // write_single_event() above; callers that do not use
  // submit_set_single_event() and read the slot off the settle event.

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
   * @return true with `*begin_ts` and `*end_ts` set on success; false (with a reason
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

    // The node's clock, through the one accessor, at the one floor (issue
    // #270). This used to read ::time(nullptr) and localtime() directly with a
    // floor of its own (year 2020), which made it the loosest of the four
    // notions of "now" in the component: it accepted a clock TimeService would
    // have refused, and it accepted one in a build with no time component at
    // all -- where libc has no zone loaded, so the anchoring year and every
    // mktime() below it were resolved against UTC while the pump runs local.
    ESPTime local_now;
    if (!time_service_.current_time(local_now)) {
      ESP_LOGW(tag, "System time not synced yet — cannot set a dated event");
      return false;
    }
    int year = static_cast<int>(local_now.year) - 1900;  // years since 1900

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
          (void) entries;  // display comes from the service's own formatter
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->event_log_text_sensor_) {
              std::string display = event_log_service_.format_display();
              if (display.size() > 255) {
                display.resize(252);
                display += "...";
              }
              publish_text_sensor_if_changed(this->event_log_text_sensor_, display);
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
          (void) trends;  // display comes from the service's own formatter
          if (success) {
#ifdef USE_TEXT_SENSOR
            if (this->history_text_sensor_) {
              std::string display = history_service_.format_display();
              if (display.size() > 255) {
                display.resize(252);
                display += "...";
              }
              publish_text_sensor_if_changed(this->history_text_sensor_, display);
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
              publish_text_sensor_if_changed(this->cycle_timestamps_text_sensor_, display);
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
   * Synchronize the pump's real-time clock with the node's (SNTP) time.
   *
   * Goes through the write-operation layer like every other pump write
   * (AGENTS §6): serialized against other writes, watchdogged, and settled
   * with one `set_clock` write_settled event carrying the confirmed offset.
   *
   * @param op_id Optional identifier echoed in the settle event.
   * @param done Fires with the terminal result -- true only for a sync the
   *   pump's own clock readback confirmed.
   * @param origin Defaults to INTERNAL: the two shipped callers are the boot
   *   read chain and the daily check, and nobody asked for either.
   * @return False when the write was not submitted at all -- the link is not
   *   ready, or there is no usable wall clock. Internal callers reach this only
   *   through check_and_sync_time(), which has already established a clock. `done`
   *   is NOT called in that case -- the return value is the answer, and it is
   *   already in the caller's hand. Calling it as well conflates "we did not
   *   try" with "we tried and the pump did not confirm", which are a 10-second
   *   retry and a 15-minute one respectively; the bench log said "retrying in
   *   15 min" twice at every boot while retrying in 10 s, because the two
   *   shared a callback.
   */
  bool sync_pump_clock(const std::string &op_id = "",
                       std::function<void(bool)> done = nullptr,
                       services::WriteOrigin origin = services::WriteOrigin::INTERNAL) {
    if (!check_ready("sync_pump_clock")) return false;
    ESPTime now;
    if (!time_service_.current_time(now)) {
      ESP_LOGD(TAG, "Skipping pump clock sync: no synced system time");
      return false;
    }
    write_op_service_.submit_set_clock(now, op_id, std::move(done), origin);
    return true;
  }

  /**
   * Asynchronously read device information and update text sensors.
   *
   * Reads device identification strings (serial, versions, product name) from
   * the pump and publishes them to configured text sensors.
   *
   * This is typically called once from the initial read chain to populate
   * device info.
   *
   * Usage:
   *   Called automatically from trigger_initial_data_reads(), which
   *   on_session_stabilized_() starts
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
   * Publish the canonical schedule hash (algorithm in schedule_codec.h) —
   * the sync-verification sensor an external scheduler polls — plus the
   * per-layer read-back sensors.
   * "unknown" until the full grid is cached.
   */
  void publish_schedule_hash() {
#ifdef USE_TEXT_SENSOR
    // Per-layer read-back sensors: each layer's
    // compact JSON always fits HA's 255-char state cap.
    // Gate every one of these: this runs on each write settle and again on each
    // reconnect, and the card issues one write per edited entry, so an ungated
    // republish costs 6 API frames per subscriber per settle for values that
    // almost never move (issue #127 / AGENTS §4).
    for (uint8_t layer = 0; layer < 5; layer++) {
      if (this->schedule_layer_sensors_[layer] != nullptr) {
        publish_text_sensor_if_changed(this->schedule_layer_sensors_[layer],
                                       schedule_service_.layer_json(layer));
      }
    }
    if (!this->schedule_hash_text_sensor_)
      return;
    std::string hash = schedule_service_.current_hash();
    if (publish_text_sensor_if_changed(this->schedule_hash_text_sensor_, hash)) {
      ESP_LOGD(TAG, "Published schedule hash %s", hash.c_str());
    }
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
          (void) entries;  // the cache is read back from the service instead
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
