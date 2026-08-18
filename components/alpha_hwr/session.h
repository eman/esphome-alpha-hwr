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
 * STABILIZING      : Notifications enabled; settling before the session is
 *                    declared ready. Nothing is sent during this window.
 * READY            : Fully operational (subscribed + stabilize window elapsed)
 * 
 * State Transitions:
 * ------------------
 * IDLE -> SERVICE_DISCOVERY    : BLE connection opened
 * SERVICE_DISCOVERY -> SUBSCRIBING : GENI service found
 * SUBSCRIBING -> STABILIZING    : Notifications enabled
 * STABILIZING -> READY          : Stabilize window elapsed
 * * -> IDLE                     : Disconnect
 *
 * There is no ERROR state, and this is deliberate rather than an omission.
 * One was declared here for a long time, documented as "any operation fails
 * critically" -- but nothing ever entered it: `on_error()` and `reset()` had no
 * caller anywhere in the component, so the transition the diagram promised
 * could not occur (issue #174 audit).
 *
 * `on_authenticating()` went the same way, and for the same reason. It existed
 * to re-enter this state from READY for a re-authentication that no caller ever
 * performed, and its first-entry case duplicated what `on_subscribed()` already
 * does. It was removed with the opening sequence itself (issue #174): there is
 * no handshake left to begin, so nothing can be in the middle of one.
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
   * Notifications enabled, settling before the session is declared ready.
   * Nothing is on the wire during this state: it is a fixed delay separating
   * the CCCD write and the encryption negotiation behind it from the first
   * GENI traffic.
   *
   * It was called AUTHENTICATING while this component opened a connection with
   * four GENIbus reads it called a handshake. Those are gone (issue #174); the
   * numbering is kept because get_state() is logged as an integer.
   */
  STABILIZING = 3,
  
  /**
   * Fully operational: notifications enabled and the stabilize window elapsed.
   * All operations (read, write, control) are permitted.
   *
   * Reaching it proves only that a timer fired. Not one frame has been
   * exchanged with the pump, so a deaf pump arrives here exactly as a live one
   * does -- which is what the inbound-data watchdog exists to notice.
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
 *    │  STABILIZING   │               │
 *    └──────┬─────────┘               │
 *           │ on_ready()              │
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
 * // Stabilize window elapsed
 * session.on_ready();
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
   * Transition to STABILIZING state.
   * 
   * Called when notifications are successfully enabled.
   */
  void on_subscribed();
  
  /**
   * Transition to READY state.
   * 
   * Called when the stabilize window has elapsed. Nothing has been exchanged
   * with the pump by this point -- see the arming site in AlphaHwrComponent for
   * what that window is and is not for.
   */
  void on_ready();
  
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
   * Check if the session is in its post-subscribe stabilize window.
   * 
   * @return true if in STABILIZING state
   */
  bool is_stabilizing() const { return state_ == SessionState::STABILIZING; }
  
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
   * @return true if state is SERVICE_DISCOVERY, SUBSCRIBING, STABILIZING, or READY
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
