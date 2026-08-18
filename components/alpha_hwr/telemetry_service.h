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
   * 
   * This should be called periodically (e.g., every 10 seconds) by the
   * main component's update() method.
   */
  void poll();

  /**
   * Process incoming packet.
   * 
   * Routes the packet to the appropriate handler based on OpSpec:
   * - 0x30: Motor state response
   * - 0x2B: Flow/pressure response
   * - 0x14: Temperature response
   * - 0x13: Alarms/warnings response
   * - 0x0E: Passive notification
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

  // Handler methods for different telemetry types
  void handle_motor_state_response(const uint8_t* data, size_t len);
  void handle_flow_pressure_response(const uint8_t* data, size_t len);
  void handle_temperature_response(const uint8_t* data, size_t len);
  void handle_alarms_response(const uint8_t* data, size_t len, uint8_t opspec);
  void handle_warnings_response(const uint8_t* data, size_t len, uint8_t opspec);
  void handle_passive_notification(const uint8_t* data, size_t len);

  // Helper to send read requests
  void send_read_request(uint32_t register_addr);

  // Track 0x09 alarm/warning response ordering (alarms polled before warnings)
  bool next_09_is_warnings_{false};

  // Logging tag
  static constexpr const char* TAG = "alpha_hwr.telemetry";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
