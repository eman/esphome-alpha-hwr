/**
 * Session State Management for Alpha HWR Connections
 * 
 * This module provides explicit state tracking for BLE connections,
 * making it easier to understand connection lifecycle and implement
 * proper error handling.
 * 
 * States:
 * -------
 * IDLE             : Initial state, BLE connection not yet established
 * SERVICE_DISCOVERY: Searching for Grundfos GENI service
 * SUBSCRIBING      : Enabling notifications on GENI characteristic
 * AUTHENTICATING   : Authentication handshake in progress
 * READY            : Fully operational (authenticated + subscribed)
 * ERROR            : Error state requiring reconnection
 * 
 * State Transitions:
 * ------------------
 * IDLE -> SERVICE_DISCOVERY    : BLE connection opened
 * SERVICE_DISCOVERY -> SUBSCRIBING : GENI service found
 * SUBSCRIBING -> AUTHENTICATING : Notifications enabled
 * AUTHENTICATING -> READY       : Authentication complete
 * * -> ERROR                    : Any operation fails critically
 * * -> IDLE                     : Disconnect
 * 
 * Architecture Note:
 * ------------------
 * ESPHome's ble_client already manages the underlying BLE connection
 * state (connecting, connected, disconnected). This Session class
 * tracks the HIGHER-LEVEL application state specific to the GENI
 * protocol handshake sequence.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/session.py
 */

#pragma once

#include "esphome/core/component.h"
#include <cstdint>
#include <string>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * Connection session states.
 * 
 * These states track the Alpha HWR GENI protocol handshake sequence,
 * NOT the underlying BLE connection state (which is managed by
 * ESPHome's ble_client).
 */
enum class SessionState : uint8_t {
  /**
   * Initial state. BLE not connected or just connected but not yet
   * started service discovery.
   */
  IDLE = 0,
  
  /**
   * Searching for Grundfos GENI service (0xFE5D) and GENI characteristic.
   * BLE is connected but we haven't found the required services yet.
   */
  SERVICE_DISCOVERY = 1,
  
  /**
   * Subscribing to notifications on GENI characteristic.
   * Service/characteristic found, now enabling notifications.
   */
  SUBSCRIBING = 2,
  
  /**
   * Authentication handshake in progress.
   * Notifications enabled, now sending auth packets.
   */
  AUTHENTICATING = 3,
  
  /**
   * Fully operational. Auth complete, notifications enabled.
   * All operations (read, write, control) are permitted.
   */
  READY = 4,
  
  /**
   * Error state. Something failed and requires reconnection.
   */
  ERROR = 5
};

/**
 * Manages connection session state and lifecycle.
 * 
 * This class provides explicit state tracking and validation for
 * Alpha HWR pump connections. It ensures operations are only
 * attempted in appropriate states and provides clear error messages.
 * 
 * State Machine:
 * --------------
 * ```
 *       ┌────────┐
 *       │  IDLE  │◄───────────────────┐
 *       └───┬────┘                    │
 *           │ on_connected()          │
 *           ▼                         │
 *    ┌──────────────────┐             │
 *    │SERVICE_DISCOVERY │             │
 *    └──────┬───────────┘             │
 *           │ on_service_found()      │
 *           ▼                         │
 *    ┌──────────────┐                 │
 *    │ SUBSCRIBING  │                 │
 *    └──────┬───────┘                 │
 *           │ on_subscribed()         │
 *           ▼                         │
 *    ┌────────────────┐               │
 *    │AUTHENTICATING  │               │
 *    └──────┬─────────┘               │
 *           │ on_authenticated()      │
 *           ▼                         │
 *    ┌──────────┐                     │
 *    │  READY   │─────────────────────┘
 *    └──────────┘  on_disconnected()
 *           
 *    Any state can transition to ERROR on failure
 * ```
 * 
 * Usage Example:
 * --------------
 * ```cpp
 * Session session;
 * 
 * // BLE connection opened
 * session.on_connected();
 * 
 * // GENI service found
 * session.on_service_found();
 * 
 * // Notifications enabled
 * session.on_subscribed();
 * 
 * // Start authentication
 * session.on_authenticating();
 * 
 * // Auth complete
 * session.on_authenticated();
 * 
 * // Check state before operations
 * if (session.is_ready()) {
 *   // Send control commands
 * }
 * ```
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/session.py
 */
class Session {
 public:
  Session();
  
  /**
   * Transition to SERVICE_DISCOVERY state.
   * 
   * Called when BLE GATT connection is established.
   * Begins the sequence to find GENI service.
   */
  void on_connected();
  
  /**
   * Transition to SUBSCRIBING state.
   * 
   * Called when GENI service and characteristic are found.
   */
  void on_service_found();
  
  /**
   * Transition to AUTHENTICATING state.
   * 
   * Called when notifications are successfully enabled.
   */
  void on_subscribed();
  
  /**
   * Remain in AUTHENTICATING state (or transition from READY).
   * 
   * Called when authentication handshake begins.
   * Can be called from SUBSCRIBING (first auth) or READY (re-auth).
   */
  void on_authenticating();
  
  /**
   * Transition to READY state.
   * 
   * Called when authentication handshake completes successfully.
   */
  void on_authenticated();
  
  /**
   * Transition to IDLE state.
   * 
   * Called when BLE connection is closed (graceful or error).
   * Clears all state.
   */
  void on_disconnected();
  
  /**
   * Transition to ERROR state.
   * 
   * Called when a critical error occurs that requires reconnection.
   * 
   * @param error_message Description of the error
   */
  void on_error(const char* error_message);
  
  /**
   * Get current state.
   * 
   * @return Current SessionState
   */
  SessionState get_state() const { return state_; }
  
  /**
   * Get human-readable state name.
   * 
   * @return State name string (e.g., "READY")
   */
  const char* get_state_name() const;
  
  /**
   * Check if service discovery is in progress.
   * 
   * @return true if in SERVICE_DISCOVERY state
   */
  bool is_discovering() const { return state_ == SessionState::SERVICE_DISCOVERY; }
  
  /**
   * Check if subscribing to notifications.
   * 
   * @return true if in SUBSCRIBING state
   */
  bool is_subscribing() const { return state_ == SessionState::SUBSCRIBING; }
  
  /**
   * Check if authentication is in progress.
   * 
   * @return true if in AUTHENTICATING state
   */
  bool is_authenticating() const { return state_ == SessionState::AUTHENTICATING; }
  
  /**
   * Check if session is ready for operations.
   * 
   * @return true if in READY state
   */
  bool is_ready() const { return state_ == SessionState::READY; }
  
  /**
   * Check if session is in error state.
   * 
   * @return true if in ERROR state
   */
  bool is_error() const { return state_ == SessionState::ERROR; }
  
  /**
   * Check if connected (any operational state).
   * 
   * Connected means: not IDLE and not ERROR.
   * 
   * @return true if state is SERVICE_DISCOVERY, SUBSCRIBING, AUTHENTICATING, or READY
   */
  bool is_connected() const;
  
  /**
   * Get last error message (if in ERROR state).
   * 
   * @return Error message or nullptr if no error
   */
  const char* get_last_error() const { return last_error_.empty() ? nullptr : last_error_.c_str(); }
  
  /**
   * Reset to IDLE state.
   * 
   * Clears error state and prepares for reconnection.
   */
  void reset();
  
 private:
  SessionState state_;
  std::string last_error_;
  
  /**
   * Internal helper to transition state with logging.
   * 
   * @param new_state Target state
   * @param reason Reason for transition (for logging)
   */
  void transition_to(SessionState new_state, const char* reason);
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
