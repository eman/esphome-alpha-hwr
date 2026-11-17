/**
 * Telemetry Service for Grundfos ALPHA HWR Pumps
 * 
 * This service handles all telemetry operations including:
 * - Reading current telemetry snapshot
 * - Managing periodic telemetry polling
 * - Parsing Class 10 DataObject notifications
 * - Updating sensor values
 * 
 * The service coordinates between the transport layer (BLE communication)
 * and the protocol layer (frame parsing/telemetry decoding) to provide
 * telemetry management.
 * 
 * Architecture:
 * - Transport Layer: Handles BLE communication and raw packets
 * - Protocol Layer: Parses frames and decodes telemetry
 * - Service Layer (this): Coordinates operations and manages state
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#pragma once

#include "esphome/core/log.h"
#include <functional>
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace alpha_hwr {
namespace core {
class Transport;
}

namespace services {

// Forward declarations
class SensorPublisher;
class ControlService;

/**
 * Telemetry Service
 */
class TelemetryService {
 public:
  /**
   * Constructor
   */
  explicit TelemetryService(core::Transport &transport);

  /**
   * Destructor
   */
  ~TelemetryService() = default;

  /**
   * Set sensor publisher for updating ESPHome sensors.
   * 
   * @param publisher Pointer to SensorPublisher instance
   */
  void set_sensor_publisher(SensorPublisher* publisher);

  /**
   * Set control service for updating control mode from passive notifications.
   * 
   * @param control_service Pointer to ControlService instance
   */
  void set_control_service(ControlService* control_service);

  /**
   * Set the minimum interval between energy read attempts.
   *
   * @param interval_ms Minimum interval in milliseconds (default 60000)
   */
  void set_energy_update_interval(uint32_t interval_ms) { energy_update_interval_ms_ = interval_ms; }

  /**
   * Start telemetry service.
   * 
   * Call this when the session is ready for telemetry.
   */
  void start();

  /**
   * Stop telemetry service.
   * 
   * Call this when disconnecting or shutting down.
   */
  void stop();

  /**
   * Check if service is running.
   * 
   * @return true if service is active
   */
  bool is_running() const;

  /**
   * Poll telemetry data.
   * 
   * Sends read requests for all telemetry registers:
   * - Motor state (0x570045)
   * - Flow/pressure (0x5D0122)
   * - Temperature (0x5D012C)
   * - Alarms (0x580000)
   * - Warnings (0x58000B)
   * - Cumulative energy (0x570001), awaited on its separate configured cadence
   * 
   * This should be called periodically (e.g., every 10 seconds) by the
   * main component's update() method.
   */
  void poll();

  /**
   * Process incoming packet.
   * 
   * Routes validated object types and versions:
   * - 256 v1/v2/v3: Motor state
   * - 565 v2: Flow/pressure
   * - 534 v1/v2: Temperature
   * - 303 v1: Control status
   *
   * Fault arrays (570 v1) use request callbacks because alarms and warnings
   * share a type and their replies do not identify the requested instance.
   * Energy uses its awaited type-232/version-1 read callback.
   * 
   * @param data Packet data
   * @param len Length of packet data
   */
  void on_packet(const uint8_t* data, size_t len);

 private:
  core::Transport &transport_;
  
  // Sensor publisher (for publishing telemetry to ESPHome sensors)
  SensorPublisher* sensor_publisher_{nullptr};
  
  // Control service (for updating control mode from passive notifications)
  ControlService* control_service_{nullptr};

  // State
  bool running_ = false;

  // Energy polling state
  enum class EnergyReadState : uint8_t { UNKNOWN, AVAILABLE, FAILED, UNSUPPORTED };
  EnergyReadState energy_read_state_{EnergyReadState::UNKNOWN};
  uint32_t energy_update_interval_ms_{60000};
  uint32_t last_energy_attempt_ms_{0};
  uint32_t energy_generation_{0};
  bool energy_attempted_{false};
  bool energy_read_pending_{false};

  // Energy polling helpers
  void poll_energy_();
  void set_energy_read_state_(EnergyReadState state, const char* reason = "no valid response");

  // Handler methods for different telemetry types
  void handle_motor_state_response(const uint8_t* data, size_t len);
  void handle_flow_pressure_response(const uint8_t* data, size_t len);
  void handle_temperature_response(const uint8_t* data, size_t len);
  void handle_control_status(const uint8_t* body, size_t len);
  void send_fault_read_request(bool warnings);

  // Helper to send read requests
  void send_read_request(uint32_t register_addr);

  // Prevent overlapping fault polls; generation retires callbacks after stop().
  bool fault_poll_pending_{false};
  bool fault_poll_failed_{false};
  uint32_t fault_generation_{0};

  // Logging tag
  static constexpr const char* TAG = "alpha_hwr.telemetry";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
