/**
 * Sensor Publisher for ESPHome Integration
 * 
 * This module handles publishing decoded telemetry data to ESPHome sensor components.
 * It separates the concern of "what data we have" (protocol layer) from "where to
 * publish it" (ESPHome integration layer).
 * 
 * Architecture:
 * - Protocol Layer: Decodes raw bytes into typed telemetry structs
 * - Service Layer: Manages telemetry operations and state
 * - Publisher Layer (this): Maps telemetry structs to ESPHome sensors
 * 
 * This separation enables:
 * 1. Testing protocol/service layers without ESPHome dependencies
 * 2. Reusing protocol/service layers in non-ESPHome contexts
 * 3. Clear single-responsibility design
 * 
 * Reference: In Python, the client owns the data model and external code reads from it.
 *            In ESPHome, we must push to sensors, so this publisher handles that mapping.
 * 
 * Example Usage:
 * ```cpp
 * // In AlphaHwrComponent::setup():
 * sensor_publisher_.set_voltage_sensor(voltage_sensor_);
 * sensor_publisher_.set_flow_sensor(flow_sensor_);
 * // ... set all sensors
 * 
 * telemetry_service_.set_sensor_publisher(&sensor_publisher_);
 * 
 * // Later, in TelemetryService::handle_motor_state_response():
 * auto motor = protocol::decode_motor_state_response(data, len);
 * sensor_publisher_->publish_motor_state(motor);
 * ```
 */

#pragma once

#include "esphome/core/log.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "telemetry_decoder.h"
#include <vector>
#include <string>

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * Sensor Publisher
 * 
 * Maps decoded telemetry structs to ESPHome sensor components.
 * Handles validation, range checking, and state publishing.
 * 
 * Design Notes:
 * - All sensor pointers are optional (nullptr-safe)
 * - Validation happens before publishing (NaN, range checks)
 * - Logging uses alpha_hwr.publisher tag for filtering
 */
class SensorPublisher {
 public:
  /**
   * Constructor
   */
  SensorPublisher();

  /**
   * Destructor
   */
  ~SensorPublisher() = default;

  // ============================================================================
  // Sensor Setters (called from AlphaHwrComponent during setup)
  // ============================================================================

  void set_flow_sensor(sensor::Sensor *sensor) { flow_sensor_ = sensor; }
  void set_head_sensor(sensor::Sensor *sensor) { head_sensor_ = sensor; }
  void set_power_sensor(sensor::Sensor *sensor) { power_sensor_ = sensor; }
  void set_rpm_sensor(sensor::Sensor *sensor) { rpm_sensor_ = sensor; }
  void set_temp_media_sensor(sensor::Sensor *sensor) { temp_media_sensor_ = sensor; }
  void set_temp_pcb_sensor(sensor::Sensor *sensor) { temp_pcb_sensor_ = sensor; }
  void set_temp_control_box_sensor(sensor::Sensor *sensor) { temp_control_box_sensor_ = sensor; }
  void set_voltage_sensor(sensor::Sensor *sensor) { voltage_sensor_ = sensor; }
  void set_voltage_dc_sensor(sensor::Sensor *sensor) { voltage_dc_sensor_ = sensor; }
  void set_current_sensor(sensor::Sensor *sensor) { current_sensor_ = sensor; }
  void set_inlet_pressure_sensor(sensor::Sensor *sensor) { inlet_pressure_sensor_ = sensor; }
  static void set_outlet_pressure_sensor(sensor::Sensor * /*sensor*/) { /* Removed: HWR pump lacks this sensor */ }
  void set_head_rate_sensor(sensor::Sensor *sensor);
  void setup_head_rate_callback();

#ifdef USE_TEXT_SENSOR
  void set_alarms_text_sensor(text_sensor::TextSensor *sensor) { alarms_sensor_ = sensor; }
  void set_warnings_text_sensor(text_sensor::TextSensor *sensor) { warnings_sensor_ = sensor; }
#endif

  // ============================================================================
  // Publishing Methods (called from TelemetryService handlers)
  // ============================================================================

  /**
   * Publish motor state telemetry.
   * 
   * Updates voltage (AC/DC), current, power, RPM, and converter temperature sensors.
   * 
   * @param motor Decoded motor state telemetry
   */
  void publish_motor_state(const protocol::MotorStateTelemetry& motor);

  /**
   * Publish flow and pressure telemetry.
   * 
   * Updates flow rate, head pressure, inlet pressure, and outlet pressure sensors.
   * 
   * @param flow Decoded flow/pressure telemetry
   */
  void publish_flow_pressure(const protocol::FlowPressureTelemetry& flow);

  /**
   * Publish temperature telemetry.
   * 
   * Updates media, PCB, and control box temperature sensors.
   * 
   * @param temp Decoded temperature telemetry
   */
  void publish_temperature(const protocol::TemperatureTelemetry& temp);

  /**
   * Publish alarm codes.
   * 
   * Updates alarms text sensor with comma-separated list of active alarm codes.
   * If no alarms, publishes "None".
   * 
   * @param codes List of active alarm codes
   */
  void publish_alarms(const std::vector<uint16_t>& codes);

  /**
   * Publish warning codes.
   * 
   * Updates warnings text sensor with comma-separated list of active warning codes.
   * If no warnings, publishes "None".
   * 
   * @param codes List of active warning codes
   */
  void publish_warnings(const std::vector<uint16_t>& codes);

 private:
  // Sensor pointers (all optional)
  sensor::Sensor *flow_sensor_{nullptr};
  sensor::Sensor *head_sensor_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *rpm_sensor_{nullptr};
  sensor::Sensor *temp_media_sensor_{nullptr};
  sensor::Sensor *temp_pcb_sensor_{nullptr};
  sensor::Sensor *temp_control_box_sensor_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *voltage_dc_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *inlet_pressure_sensor_{nullptr};
  sensor::Sensor *head_rate_sensor_{nullptr};

  // State for head derivative (callback-based, captures `this`)
  float prev_head_m_{NAN};
  uint32_t prev_head_time_ms_{0};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *alarms_sensor_{nullptr};
  text_sensor::TextSensor *warnings_sensor_{nullptr};
#endif

  // Helper to format code list as comma-separated string
  static std::string format_codes(const std::vector<uint16_t>& codes);

  // Logging tag
  static constexpr const char* TAG = "alpha_hwr.publisher";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
