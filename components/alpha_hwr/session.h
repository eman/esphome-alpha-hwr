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
 * 
 * State Transitions:
 * ------------------
 * IDLE -> SERVICE_DISCOVERY    : BLE connection opened
 * SERVICE_DISCOVERY -> SUBSCRIBING : GENI service found
 * SUBSCRIBING -> AUTHENTICATING : Notifications enabled
 * AUTHENTICATING -> READY       : Authentication complete
 * * -> IDLE                     : Disconnect
 *
 * There is no ERROR state, and this is deliberate rather than an omission.
 * One was declared here for a long time, documented as "any operation fails
 * critically" -- but nothing ever entered it: `on_error()` and `reset()` had no
 * caller anywhere in the component, so the transition the diagram promised
 * could not occur (issue #174 audit).
 *
 * It is redundant with what the component actually does. A failure that matters
 * ends the BLE link, which lands here as `on_disconnected()` -> IDLE, and the
 * inbound-data watchdog recycles from there; the user-facing cause is reported
 * by the fault-string hold (`failure_hold.h`), which carries more than a state
 * could. The one predicate that ever inspected ERROR, `is_connected()`, treated
 * it exactly as IDLE -- which is the clearest statement that it was not a
 * distinct state. Re-add it if a caller ever genuinely needs one.
 * 
 * Architecture Note:
 * ------------------
 * ESPHome's ble_client already manages the underlying BLE connection
 * state (connecting, connected, disconnected). This Session class
 * tracks the HIGHER-LEVEL application state specific to the GENI
 * protocol handshake sequence.
 * 
 * Reference: https://github.com/eman/alpha-hwr (src/alpha_hwr/core/session.py)
 */

#pragma once

#include "esphome/core/component.h"
#include <cstdint>

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
  READY = 4
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
 *    Any state returns to IDLE on disconnect
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
 * Reference: https://github.com/eman/alpha-hwr (src/alpha_hwr/core/session.py)
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
   * Check if connected (any operational state).
   * 
   * Connected means: anything but IDLE.
   * 
   * @return true if state is SERVICE_DISCOVERY, SUBSCRIBING, AUTHENTICATING, or READY
   */
  bool is_connected() const;
  
 private:
  SessionState state_;
  
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
