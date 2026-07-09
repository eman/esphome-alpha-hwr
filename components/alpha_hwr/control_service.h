#pragma once

#include <cstdint>
#include <functional>
#include "transport.h"
#include "session.h"
#include "codec.h"
#include "frame_builder.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * Control Mode Enumeration.
 * 
 * Matches ControlMode from GENI Profile (id=112) used by ALPHA HWR pumps.
 * Each mode defines how the pump regulates its operation.
 * 
 * Most Common Modes for ALPHA HWR:
 * - CONSTANT_PRESSURE (0): Maintains constant outlet pressure
 * - CONSTANT_SPEED (2): Runs at fixed RPM
 * - AUTO_ADAPT_* (13-15): Automatically adapts to system demand
 * 
 * Reference: alpha_hwr/constants.py::ControlMode
 */
enum class ControlMode : uint8_t {
  CONSTANT_PRESSURE = 0,              // Constant outlet pressure (meters)
  PROPORTIONAL_PRESSURE = 1,          // Pressure varies linearly with flow
  CONSTANT_SPEED = 2,                 // Fixed RPM operation
  AUTO_ADAPT = 5,                     // Generic AutoAdapt (limited support)
  CONSTANT_FLOW = 8,                  // Constant flow rate (m³/h)
  AUTO_ADAPT_RADIATOR = 13,           // AutoAdapt for radiator systems
  AUTO_ADAPT_UNDERFLOOR = 14,         // AutoAdapt for underfloor heating
  AUTO_ADAPT_COMBINED = 15,           // AutoAdapt for mixed systems
  DHW_ON_OFF = 25,                    // Domestic hot water on/off control
  TEMPERATURE_RANGE = 27,             // Temperature range control (min/max)
  NONE = 254,                         // No control mode active
};

/**
 * Operation Mode Enumeration.
 * 
 * Defines the pump's operational state (running, stopped, etc.).
 * 
 * Reference: alpha_hwr/constants.py::OperationMode
 */
enum class OperationMode : uint8_t {
  AUTO = 0,           // Automatic operation (normal mode)
  STOP = 1,           // Pump stopped
  USER_DEFINED = 4,   // User-defined operation
};

/**
 * Control Service for pump start/stop and mode management.
 * 
 * This service handles all pump control operations:
 * - Starting and stopping the pump
 * - Changing control modes (constant pressure, speed, flow, etc.)
 * - Setting setpoints for each mode
 * - Reading current control mode and operation state
 * 
 * The service abstracts the complexity of Class 10 and Class 3 protocol
 * operations, providing a clean API for pump control.
 * 
 * Architecture:
 * - Uses Transport layer for BLE packet I/O
 * - Validates session state before operations
 * - Builds control packets using FrameBuilder
 * - Sends configuration commits after state changes
 * 
 * Example Usage:
 *   ControlService control(transport, session);
 *   
 *   // Start pump with current mode
 *   control.start();
 *   
 *   // Change to constant pressure mode
 *   control.set_mode(ControlMode::CONSTANT_PRESSURE);
 *   
 *   // Stop pump
 *   control.stop();
 * 
 * Protocol Reference:
 * - Control commands use Class 10 Sub 0x5600, Obj 0x0601
 * - Payload format: [Header][Flag][Mode][Suffix]
 * - Flag: 0x00 = Start/Run, 0x01 = Stop
 * - Configuration commit required after state changes
 * 
 * Python Reference: alpha_hwr/services/control.py
 */
class ControlService {
 public:
  /**
   * Initialize control service.
   * 
   * @param transport BLE transport layer for packet I/O
   * @param session Session manager for state tracking
   */
  ControlService(core::Transport &transport, core::Session &session);
  
   /**
    * Get the current control mode from the pump.
    * 
    * Reads Class 10 Object 86, Sub-ID 6 to get the pump's current control mode,
    * operation mode, and setpoint. Updates internal state with the control mode.
    * 
    * @param on_complete Callback function(bool success, ControlMode mode)
    * @return True if read request was sent successfully
    * 
    * Protocol Notes:
    * - Uses Class 10 Object 86, Sub-ID 6 (overall_operation_local_request_obj)
    * - Response format: [00 00 XX][control_source][operation_mode][control_mode][setpoint(4 bytes)]
    * - Control mode is at offset 2 (byte 5 in response)
    * 
    * Reference: control.py::get_mode() lines 224-294
    */
   bool get_mode_async(std::function<void(bool, ControlMode)> on_complete);
   
   /**
    * Set callback for scheduling delayed operations.
    * 
    * The service needs to schedule configuration commits and retries.
    * The main component provides this callback to delegate scheduling.
    * 
    * @param callback Function to call set_timeout on the component
    */
   void set_schedule_callback(std::function<void(std::function<void()>, uint32_t)> callback);
    
    /**
     * Set callback for configuration commits.
     * Delegates to ScheduleService::send_configuration_commit() which
     * preserves the cached ClockProgramOverview (including schedule_enabled).
     */
    void set_config_commit_callback(std::function<void()> callback) { config_commit_callback_ = callback; }
   
   /**
    * Set callback for control mode change notifications.
    * 
    * Called whenever the control mode changes (from passive notification or command).
    * 
    * @param callback Function(ControlMode, operation_mode, setpoint) to call on mode change
    */
   void set_mode_change_callback(std::function<void(ControlMode, uint8_t, float)> callback);
  
  /**
   * Start the pump.
   * 
   * Sends the start command using Class 10 DataObject method.
   * Optionally switches to a different mode before starting.
   * 
   * @param mode Optional control mode (255 = use current mode)
   * @return True if start command sent successfully
   * 
   * Protocol Notes:
   * - Uses Class 10 Sub 0x5600, Obj 0x0601
   * - Flag byte = 0x00 for start operation
   * - Sends configuration commit after start
   * - Requires authenticated session
   * - When called with mode=255 (use current mode) and a setpoint has
   *   already been cached for CONSTANT_PRESSURE, PROPORTIONAL_PRESSURE,
   *   CONSTANT_SPEED, or CONSTANT_FLOW, that cached setpoint is resent
   *   instead of the mode's hardcoded default suffix (fixes #43: enabling
   *   the pump no longer forces it to a fixed ~3671 RPM). If no cached
   *   setpoint is available yet, or an explicit mode override is passed,
   *   the default suffix is used (unchanged behavior).
   *
   * Example:
   *   control.start();  // Start with current mode
   *   control.start(static_cast<uint8_t>(ControlMode::CONSTANT_PRESSURE));  // Start with specific mode
   *
   * Reference: control.py::start() lines 165-234
   */
  bool start(uint8_t mode = 255);
  
  /**
   * Stop the pump.
   * 
   * Sends the stop command using Class 10 DataObject method.
   * 
   * @param mode Optional control mode (255 = use current mode)
   * @return True if stop command sent successfully
   * 
   * Protocol Notes:
   * - Uses Class 10 Sub 0x5600, Obj 0x0601
   * - Flag byte = 0x01 for stop operation
   * - Sends configuration commit after stop
   * - Telemetry stream may pause when stopped
   * 
   * Example:
   *   control.stop();
   * 
   * Reference: control.py::stop() lines 236-303
   */
  bool stop(uint8_t mode = 255);
  
  /**
   * Set control mode.
   * 
   * Changes the pump's control mode without changing the setpoint.
   * Tries Class 10 method first, falls back to Class 3 if needed.
   * 
   * @param mode Control mode to set
   * @return True if mode set successfully
   * 
   * Example:
   *   control.set_mode(ControlMode::CONSTANT_PRESSURE);
   *   control.set_mode(ControlMode::AUTO_ADAPT_RADIATOR);
   * 
   * Reference: control.py::set_mode() lines 364-436
   */
  bool set_mode(ControlMode mode);
  
  /**
   * Enable remote control mode.
   * 
   * Enables remote control (Class 3 command ID 7), allowing external
   * control of the pump via BLE/API. When enabled, pump ignores local controls.
   * 
   * @return True once the command has been queued (the actual enabled
   *   state is confirmed asynchronously by handle_remote_mode_ack() once
   *   the pump's ACK arrives -- see get_remote_enabled())
   * 
   * Protocol Notes:
   * - Uses Class 3 command: [0x03, 0x81, 0x07] (OpSpec 0x81 = SET). Fixed
   *   from 0xC1 (INFO) in issue #46 -- bench-verified against a real pump
   *   that 0xC1 always produces a "rejected" ACK ([03 01 xx]), while 0x81
   *   produces the clean success ACK ([03 00]).
   * - is_remote_mode_enabled_ is only updated once the pump's ACK confirms
   *   success (ack byte 0x00); a rejected or missing ACK leaves the
   *   previous state unchanged.
   * 
   * Reference: control.py::enable_remote_mode() lines 305-333 (note: the
   * Python reference has the same 0xC1 bug, not yet fixed there)
   */
  bool enable_remote_mode();
  
  /**
   * Disable remote control mode.
   * 
   * Returns pump to automatic operation based on internal logic.
   * 
   * @return True once the command has been queued (see enable_remote_mode()
   *   for how the confirmed state is determined)
   * 
   * Protocol Notes:
   * - Uses Class 3 command: [0x03, 0x81, 0x06] (OpSpec 0x81 = SET). Fixed
   *   from 0xC1 (INFO) -- see enable_remote_mode().
   * 
   * Reference: control.py::disable_remote_mode() lines 335-362 (note: the
   * Python reference has the same 0xC1 bug, not yet fixed there)
   */
  bool disable_remote_mode();
   
   /**
    * Get current control mode name as string.
    * 
    * @param mode Control mode value
    * @return Human-readable mode name
    */
   static const char *get_mode_name(ControlMode mode);
   
   /**
    * Get the current control mode.
    * 
    * @return Current ControlMode value
    */
   ControlMode get_current_mode() const { return current_mode_; }
   
   /**
    * Check if the control mode is valid (received from pump).
    * 
    * @return True if we've received a real mode from the pump, false otherwise
    */
   bool is_mode_valid() const { return mode_valid_; }
   
   /**
    * Update control mode from passive notification.
    * 
    * Called by TelemetryService when it receives a passive notification
    * (OpSpec 0x0E, Object 0x2F01, Sub 1) containing control mode data.
    * The pump sends these notifications automatically during/after authentication.
    * 
    * Updates is_remote_mode_enabled_ when control_source == 2 (Remote/Digital)
    * or control_source == 1 (Local/Panel). Unknown values (e.g. 0) are ignored
    * so a stale or unrecognized byte can't incorrectly clear a confirmed remote state.
    * Reference: Python control.py — is_remote = (control_source == 2).
    * 
    * @param mode Control mode byte from passive notification
    * @param operation_mode Operation mode byte (AUTO/STOP/USER_DEFINED)
    * @param setpoint Setpoint value from notification
    * @param control_source Source byte from pump (2=Remote/Digital, 1=Local/Panel, 0=unknown)
    */
  void update_mode_from_notification(uint8_t mode, uint8_t operation_mode, float setpoint,
                                     uint8_t control_source = 0xFF);
   
   /**
    * Get whether remote mode is enabled.
    * 
    * Reflects the pump's actual control_source state when available. Updated
    * from two code paths, both of which carry the same payload format
    * ([control_source][operation_mode][control_mode][setpoint]):
    *   1. Passive Control Mode Status notifications (Obj 0x2F01 / Sub 0x0001,
    *      OpSpec 0x0E) — received automatically after authentication.
    *   2. Explicit Object 86 / Sub 6 read callback — triggered by the periodic
    *      control-state poll (issue #54).
    * When control_source == 2 (Remote/Digital) the flag is set true; when
    * control_source == 1 (Local/Panel) it is set false. Unknown values leave
    * the current state unchanged. Falls back to command-ACK tracking
    * (handle_remote_mode_ack) when control_source is unrecognized.
    * 
    * @return True if remote control is enabled, false if in auto/local mode
    */
   bool get_remote_enabled() const { return is_remote_mode_enabled_; }
   
   /**
    * Check if pump enabled state has been determined.
    * @return True if we've received operation_mode from pump or a user command
    */
   bool is_pump_enabled_valid() const { return pump_enabled_valid_; }
   
   /**
    * Get whether the pump is enabled (operating in its configured mode).
    * Enabled means the pump will operate per its control mode (e.g., cycling
    * the motor on/off in temperature range mode). Disabled means explicitly stopped.
    * Distinct from motor running (RPM > 0): enabled pump may have idle motor.
    * @return True if pump is enabled, false if explicitly stopped/disabled
    */
   bool is_pump_enabled() const { return pump_enabled_; }

   /** Get cached setpoints per mode (NAN if not yet read from pump) — issue #51. */
   float get_cached_pressure_setpoint() const { return cached_pressure_setpoint_; }
   float get_cached_proportional_setpoint() const { return cached_proportional_setpoint_; }
   float get_cached_speed_setpoint() const { return cached_speed_setpoint_; }
   float get_cached_flow_setpoint() const { return cached_flow_setpoint_; }
   /** Get cached temperature range min (NAN if not yet read). */
   float get_cached_temp_min() const { return cached_temp_min_; }
   /** Get cached temperature range max (NAN if not yet read). */
   float get_cached_temp_max() const { return cached_temp_max_; }
   /** Get cached operation mode (0xFF if not yet read). */
   uint8_t get_cached_operation_mode() const { return cached_operation_mode_; }
   /** Get cached autoadapt enabled state (-1=unknown, 0=off, 1=on). */
   int8_t get_cached_autoadapt() const { return cached_autoadapt_; }
   
   /**
    * Read current mode, setpoint, and temperature range from pump.
    * Queries Object 86 Sub 6 and (for temp range) Object 91 Sub 430.
    * Results are cached and accessible via getters.
    */
   void read_setpoints_from_pump();

   // ========== Setpoint Configuration Methods ==========
   
   /**
    * Set constant pressure mode with setpoint.
    * 
    * Switches to CONSTANT_PRESSURE mode (Mode 0) and sets the pressure setpoint.
    * 
    * @param value_m Pressure setpoint in meters of water column (0.5 - 10.0 m)
    * @param callback Callback function(bool success)
    * 
    * Protocol Notes:
    * - Sets mode first using set_mode()
    * - Writes setpoint using Class 3 register 0x18
    * - Value stored as 32-bit float big-endian
    * - Sends configuration commit after write
    * 
    * Reference: control.py::set_constant_pressure() lines 591-629
    */
   void set_constant_pressure_async(float value_m, std::function<void(bool)> callback);
   
   /**
    * Set constant speed mode with setpoint.
    * 
    * Switches to CONSTANT_SPEED mode (Mode 2) and sets the RPM setpoint.
    * 
    * @param value_rpm Speed setpoint in RPM (500 - 4500 RPM)
    * @param callback Callback function(bool success)
    * 
    * Protocol Notes:
    * - Sets mode first using set_mode()
    * - Writes setpoint using Class 3 register 0x04
    * - Value stored as 32-bit float big-endian
    * - Sends configuration commit after write
    * 
    * Reference: control.py::set_constant_speed() lines 631-664
    */
   void set_constant_speed_async(float value_rpm, std::function<void(bool)> callback);
   
   /**
    * Set constant flow mode with setpoint.
    * 
    * Switches to CONSTANT_FLOW mode (Mode 8) and sets the flow rate setpoint.
    * 
    * @param value_m3h Flow rate setpoint in cubic meters per hour
    * @param callback Callback function(bool success)
    * 
    * Protocol Notes:
    * - Sets mode first using set_mode()
    * - Writes setpoint using Class 3 register 0x15
    * - Value stored as 32-bit float big-endian
    * - Sends configuration commit after write
    * 
    * Reference: control.py::set_constant_flow() lines 666-699
    */
   void set_constant_flow_async(float value_m3h, std::function<void(bool)> callback);
   
   /**
    * Set temperature range control mode with min/max setpoints and AutoAdapt flag.
    * 
    * Switches to TEMPERATURE_RANGE mode (Mode 27) and configures temperature range.
    * 
    * @param min_temp Minimum temperature in Celsius
    * @param max_temp Maximum temperature in Celsius
    * @param autoadapt_enabled Enable AutoAdapt (DeltaTempEnabled flag)
    * @param callback Callback function(bool success)
    * 
    * Protocol Notes:
    * - Sets mode first using set_mode()
    * - Writes config to Object 91, Sub-ID 430 (Type 1012)
    * - Payload: [DeltaTempEnabled(1)][MinTemp(4BE)][MaxTemp(4BE)][TimeLimits(4)]
    * - Total APDU: 19 bytes (OpSpec 0xB3 + 13-byte payload)
    * - Sends configuration commit after write
    * 
    * Reference: control.py::set_temperature_range_control() lines 919-987
    */
   void set_temperature_range_async(float min_temp, float max_temp, bool autoadapt_enabled,
                                     std::function<void(bool)> callback);
   
   /**
    * Set proportional pressure mode with setpoint.
    * Converts meters to Pascals (m × 9806.65) before sending.
    * Two-step pattern: send_control_request + set_class10_setpoint(Sub 15).
    * Reference: control.py::set_proportional_pressure() lines 635-668
    */
   void set_proportional_pressure_async(float value_m, std::function<void(bool)> callback);
   
   /**
    * Set cycle time control mode (Mode 25 / DHW_ON_OFF_CONTROL).
    * Configures pump to cycle on/off at specified intervals.
    * Writes config to Object 91, Sub-ID 430.
    * Reference: control.py::set_cycle_time_control() lines 982-1061
    */
   void set_cycle_time_control_async(uint8_t on_minutes, uint8_t off_minutes,
                                      std::function<void(bool)> callback);
   
   private:
    core::Transport &transport_;
    core::Session &session_;
    ControlMode current_mode_{ControlMode::NONE};
    bool mode_valid_{false};  // Track if we've received a real mode from the pump
    bool is_remote_mode_enabled_{false};  // Track remote mode state
    bool pump_enabled_{false};       // Pump enabled (AUTO/USER_DEFINED) vs stopped (STOP)
    bool pump_enabled_valid_{false}; // Whether pump_enabled_ has been determined
    std::function<void(std::function<void()>, uint32_t)> schedule_callback_;
    std::function<void(ControlMode, uint8_t, float)> mode_change_callback_;
    std::function<void()> config_commit_callback_;
    
    // Per-mode setpoint cache (issue #51): each mode stores only its own value,
    // eliminating cross-mode contamination bugs that arose from the old shared
    // cached_setpoint_ field. NAN = not yet read from pump.
    //
    // NOTE: cached_flow_setpoint_ is populated ONLY by set_constant_flow_async()'s
    // optimistic client-side write — Object 86/Sub 6 is confirmed unreliable for
    // CONSTANT_FLOW (bench-tested, issue #44).
    float cached_pressure_setpoint_{NAN};       // CONSTANT_PRESSURE: meters of water column
    float cached_proportional_setpoint_{NAN};   // PROPORTIONAL_PRESSURE: meters
    float cached_speed_setpoint_{NAN};           // CONSTANT_SPEED: RPM
    float cached_flow_setpoint_{NAN};            // CONSTANT_FLOW: m³/h (client-write only)
    float cached_temp_min_{NAN};           // Temperature range min (Object 91 Sub 430)
    float cached_temp_max_{NAN};           // Temperature range max (Object 91 Sub 430)
    uint8_t cached_operation_mode_{0xFF};  // Operation mode from notification
    int8_t cached_autoadapt_{-1};           // AutoAdapt state (-1=unknown, 0=off, 1=on)
  
  // Sub-ID constants for setpoint registers (Reference: control.py lines 137-141)
  static constexpr uint16_t SUB_SPEED_SETPOINT = 13;
  static constexpr uint16_t SUB_PRESSURE_SETPOINT = 15;
  static constexpr uint16_t SUB_FLOW_SETPOINT = 39;
    /**
    * Get the cached setpoint for a specific mode.
    * Used internally for callbacks and log messages (issue #51).
    * Returns NAN for modes without a scalar setpoint (AUTO_ADAPT_*, TEMPERATURE_RANGE, etc.).
    */
   float get_setpoint_for_mode(ControlMode mode) const;

  /**
   * Handle the ACK response for enable_remote_mode()/disable_remote_mode()
   * (fixes #46). Only updates is_remote_mode_enabled_ when the pump's Class 3
   * ACK confirms success (ack byte 0x00); leaves the state unchanged on a
   * rejected ACK (0x01) or a timeout, rather than assuming success.
   *
   * @param enabling True if this was an enable_remote_mode() call, false for disable
   * @param got_response True if the transport got a matching response before timeout
   * @param data Raw response bytes (only valid when got_response is true)
   * @param len Length of data
   */
  void handle_remote_mode_ack(bool enabling, bool got_response, const uint8_t* data, size_t len);

  /**
   * Resolve the pump's current enabled (on/off) state before sending a
   * setpoint or mode-change control request, so those writes never
   * implicitly force-enable or force-disable the pump (fixes #45).
   *
   * The GENIbus control frame fuses mode + setpoint + on/off into a single
   * write (see send_control_request()), so there's no way to write a
   * setpoint/mode without also specifying a start/stop flag — the fix is to
   * send the pump's actual last-known on/off state instead of hardcoding
   * "true" (start).
   *
   * If the state is already known (pump_enabled_valid_), invokes
   * on_resolved(true, enabled) synchronously. Otherwise performs a
   * get_mode_async() read-back first (get_mode_async() updates
   * pump_enabled_/pump_enabled_valid_ internally on success). If that
   * read-back also fails, the state genuinely cannot be determined --
   * invokes on_resolved(false, ...) so the caller can abort the control
   * request entirely, rather than guessing either "true" (which could
   * force-enable a stopped pump, the original bug) or "false" (which would
   * send an explicit STOP and could force-disable a running pump -- just as
   * bad, in the opposite direction).
   *
   * @param on_resolved Callback invoked with (resolved, enabled). When
   *   resolved is false, enabled is meaningless and the caller must not
   *   proceed with the control request.
   *
   * Reference: issue #45 suggested fix
   */
  void with_resolved_enabled_state(std::function<void(bool resolved, bool enabled)> on_resolved);

  /**
   * Diagnostic log for issue #44: bench-verified (2026-07-08) that Constant
   * Flow mode's Object 86/Sub 6 setpoint readback returns a fixed value
   * (~0.000694 m³/h) regardless of the actual commanded setpoint (tested
   * 0.2/2.0/8.0 m³/h — all identical). Because of this, cached_setpoint_ is no
   * longer updated from this register while in CONSTANT_FLOW mode (see call
   * sites); this function only logs when a large jump would have occurred, in
   * case a different pump/firmware revision behaves differently.
   *
   * @param previous_setpoint cached_setpoint_ before this update (display units)
   * @param new_setpoint value that would have been cached had the register been trusted
   * @param raw_register_value Raw float as read from Object 86/Sub 6, pre-conversion
   */
  void check_flow_setpoint_scale(float previous_setpoint, float new_setpoint, float raw_register_value);
  
  /**
   * Send configuration commit packet.
   * Required after control operations to persist state changes.
   * Reference: control.py::_send_configuration_commit() lines 1038-1048
   */
  void send_configuration_commit();
  
  /**
   * Send control request with optional setpoint (Class 10 method).
   * 
   * Payload: [2F 01 00 00 07 00][Flag][Mode][Suffix/Setpoint(4)]
   * When setpoint is provided, suffix carries the float32 value.
   * When not provided, uses default suffix from CLASS10_CONTROL_MAP.
   * 
   * @param mode Control mode
   * @param start True for start/run (flag=0x00), false for stop (flag=0x01)
   * @param setpoint Optional setpoint value (NAN = use default suffix)
   * @return True if command was queued
   * 
   * Reference: control.py::_send_control_request() lines 233-284
   */
  bool send_control_request(ControlMode mode, bool start, float setpoint = NAN);
  
  /**
   * Set a Class 10 setpoint value (OpSpec 0x84 = SET + 4 bytes).
   * 
   * APDU: [0x0A][0x84][SubH][SubL][ObjH][ObjL][Float32BE]
   * Sends configuration commit after success.
   * 
   * @param value Float value to write
   * @param sub_id Sub-ID to write to
   * @param obj_id Object ID (default 86)
   * 
   * Reference: control.py::_set_class10_setpoint() lines 1100-1132
   */
  void set_class10_setpoint(float value, uint16_t sub_id, uint16_t obj_id = 86);
  
  /**
   * Class 10 Control Mode Mapping.
   * 
   * Maps ControlMode values to (ModeByte, SuffixBytes) tuples.
   * Used for building Class 10 control packets.
   * 
   * Payload format: 2F 01 00 00 07 00 [Flag] [Mode] [Suffix bytes]
   * - Flag: 00=Start/Run, 01=Stop
   * - Suffix is invariant to Flag
   * 
   * Reference: control.py::_CLASS10_CONTROL_MAP lines 137-145
   */
  struct ControlModeMapping {
    uint8_t mode_byte;
    uint8_t suffix[4];
  };
  
  static const ControlModeMapping CLASS10_CONTROL_MAP[];
  
  /**
   * Get Class 10 control mapping for a mode.
   * 
   * @param mode Control mode
   * @param mapping Output mapping (mode byte + suffix)
   * @return True if mode supported in Class 10
   */
  static bool get_class10_mapping(ControlMode mode, ControlModeMapping &mapping);
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
