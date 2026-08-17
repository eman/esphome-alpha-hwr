#pragma once

#include <cmath>
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
  
  // NOTE (issue #92): the high-level start/stop, set_mode and setpoint
  // setters that used to live here moved into WriteOperationService, which
  // sequences their wire steps (built from this service's primitives below)
  // and reports a terminal settle result per write. This service keeps the
  // pump-state cache, the readback/coordination guards, and the wire
  // primitives.

  // Remote-mode enable/disable used to live here as two standalone
  // enable_remote_mode()/disable_remote_mode() entry points that talked to the
  // transport directly. That was the last write in the component that bypassed
  // the operation layer, against AGENTS §6's "one write path" and §8.4: it
  // emitted no settle event, had no watchdog, could not be superseded, and
  // confirmed itself from the command ACK alone rather than from a readback.
  // It now goes through WriteOperationService::submit_set_remote_mode(); the
  // wire primitive is send_remote_mode_command() below.
   
   /**
    * Get current control mode name as string.
    *
    * @param mode Control mode value
    * @return Human-readable mode name
    */
   static const char *get_mode_name(ControlMode mode);

   /**
    * Machine-readable mode identifier (snake_case), used by the programmatic
    * service/event interface (issue #92). Distinct from get_mode_name(), which
    * is the human-facing display string shown in entities.
    */
   static const char *mode_to_string(ControlMode mode);

   /**
    * Parse a machine-readable mode identifier (see mode_to_string()).
    * @return True and sets out on a recognized identifier, false otherwise.
    */
   static bool mode_from_string(const char *str, ControlMode &out);
   
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
    * the current state unchanged -- there is no ACK-derived fallback, because
    * a Class 3 ACK only says the pump accepted the command, not what state it
    * ended up in. SET_REMOTE_MODE confirms from this readback instead.
    * 
    * @return True if remote control is enabled, false if in auto/local mode
    */
   bool get_remote_enabled() const { return is_remote_mode_enabled_; }

   /**
    * Has a recognized control_source (Remote/Digital or Local/Panel) ever been
    * observed? False means get_remote_enabled()'s `false` is a default, not a
    * reading -- entity lambdas should report unknown rather than "off".
    */
   bool is_remote_state_valid() const { return remote_state_valid_; }
   
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
   /** Get cached cycle time ON minutes (-1 = not yet read; Obj 91 Sub 421, issue #106). */
   int8_t get_cached_cycle_time_on() const { return cached_cycle_time_on_; }
   /** Get cached cycle time OFF minutes (-1 = not yet read). */
   int8_t get_cached_cycle_time_off() const { return cached_cycle_time_off_; }
   /**
    * Get the cycle-mode stored flow setpoint in m³/h (Obj 91 Sub 421; NAN until
    * the DHW config has been read). The wire float is SI m³/s (issue #107).
    */
   float get_cached_cycle_flow() const {
     return dhw_config_valid_ ? protocol::decode_float_be(cached_dhw_setpoint_raw_) * 3600.0f : NAN;
   }

   /**
    * Decode a cycle-time minutes byte read from the pump (Object 91). Valid range
    * is 1-60 (matches set_cycle_time_control_async); any other byte is treated as
    * explicitly "unknown" and returned as the -1 sentinel. This range check is
    * what makes the raw uint8_t safe to store in the int8_t field: without it,
    * assigning the byte directly let a 0 pass as a bogus valid value, mapped bytes
    * 128-254 to negative minutes, and relied on signed truncation for the 0xFF
    * "unset" case. See issue #94.
    */
   static int8_t parse_cycle_time_minutes(uint8_t raw) {
     return (raw >= 1 && raw <= 60) ? static_cast<int8_t>(raw) : -1;
   }

   /**
    * Synchronize all cached control data from the pump.
    * Queries Object 86 Sub 7 (Mode) and Object 91 Sub 430 (Temp range/cycle time).
    * Results are cached and accessible via getters.
    */
   void sync_cache_async(std::function<void(bool)> callback = nullptr);

   /**
    * Check if all required control cache values are valid.
    */
   bool is_cache_valid() const {
     // Cycle-time config (DHW_ON_OFF mode, Object 91) is intentionally NOT
     // required here (issue #94): it is not displayed anywhere, and requiring it
     // could permanently block every command (this gate feeds check_ready) if the
     // pump ever returns a short/unusual Object 91 payload that leaves the
     // cycle-time fields at their -1 "unknown" sentinel. Same rationale as
     // excluding the mode-specific setpoints from readiness.
     return mode_valid_ &&
            pump_enabled_valid_ &&
            cached_autoadapt_ != -1 &&
            !std::isnan(cached_temp_min_) &&
            !std::isnan(cached_temp_max_);
   }

   /**
    * Invalidate all cached control data.
    */
   void invalidate_cache() {
     mode_valid_ = false;
     pump_enabled_valid_ = false;
     remote_state_valid_ = false;
     cached_autoadapt_ = -1;
     cached_temp_min_ = NAN;
     cached_temp_max_ = NAN;
     cached_cycle_time_on_ = -1;
     cached_cycle_time_off_ = -1;
     dhw_config_valid_ = false;
     temp_limits_tail_valid_ = false;
     // Drop any in-flight mode command (issue #91): a command issued on a prior
     // connection must not be "confirmed" by a read on the next connection.
     mode_command_pending_ = false;
     mode_confirm_attempts_ = 0;
   }

   private:
    // The write-operation layer (issue #92) sequences multi-step writes and
    // terminal settle events on top of this service's wire primitives
    // (send_control_request, send_set_mode_request, set_class10_setpoint,
    // write_temp_range_config, write_cycle_config, read_obj91_config) and its
    // command-coordination state (commanded_mode_/mode_command_pending_). It is
    // deliberately a friend rather than widening the public API: the primitives
    // are unsafe to call without the sequencing the op layer provides.
    friend class WriteOperationService;

    core::Transport &transport_;
    core::Session &session_;
    ControlMode current_mode_{ControlMode::NONE};
    bool mode_valid_{false};  // Track if we've received a real mode from the pump

    // Mode-command coordination (issue #91). current_mode_ has several writers:
    // the optimistic setters (set_mode/start) and the readback writers
    // (get_mode_async, called directly by the #54 poll and post-command
    // reconciles, plus update_mode_from_notification). Without coordination, a
    // readback that lands after a mode command is issued but before the pump has
    // applied it reports the OLD mode and silently overwrites the optimistic new
    // mode. To prevent that, set_mode/start record the commanded mode as
    // "pending", and every readback writer keeps the commanded mode until a
    // readback confirms it (reports == commanded).
    ControlMode commanded_mode_{ControlMode::NONE};  // last mode we told the pump to enter
    bool mode_command_pending_{false};               // true until a readback confirms commanded_mode_
    uint8_t mode_confirm_attempts_{0};               // bounds the sync_cache_async retry loop
    // Max mismatch retries before we stop forcing the commanded mode and accept
    // whatever the pump reports (recovers from a command the pump never applied).
    static constexpr uint8_t MAX_MODE_CONFIRM_ATTEMPTS = 4;

    bool is_remote_mode_enabled_{false};  // Track remote mode state
    // Has a recognized control_source (1 or 2) ever been seen? Mirrors
    // pump_enabled_valid_. Without it the {false} default above is
    // indistinguishable from an observed Local/Panel, and SET_REMOTE_MODE's
    // confirm would settle a disable ACCEPTED on a cold cache having read
    // nothing -- the same false-confirm the SET_PUMP_ENABLED rollback fixed.
    bool remote_state_valid_{false};
    // Bumped alongside remote_state_valid_, i.e. only when a control_source
    // byte the profile defines as Remote(2) or Local(1) is actually seen.
    // remote_state_valid_ alone is not enough to confirm a write against:
    // it is sticky, so once ANY recognized source has been observed it stays
    // true through readbacks that carry a source we cannot interpret, and
    // SET_REMOTE_MODE's confirm would then settle against the value the
    // PREVIOUS observation left behind. Object 86 Sub 7 is the prioritized
    // status after remote/local/alarm arbitration and the profile defines
    // ~40 sources for it, so a reply that is neither 1 nor 2 is a shape the
    // pump can really produce. SET_REMOTE_MODE's confirm snapshots this
    // counter once the Class 3 command has been answered -- acked, or its ACK
    // window closed -- and requires it to have moved by confirm time. Both of
    // those points are after the pump has had the command, so anything
    // counted from there on describes the post-command pump.
    uint32_t remote_source_observations_{0};
    bool pump_enabled_{false};       // Pump enabled (AUTO/USER_DEFINED) vs stopped (STOP)
    bool pump_enabled_valid_{false}; // Whether pump_enabled_ has been determined
    std::function<void(std::function<void()>, uint32_t)> schedule_callback_;
    std::function<void(ControlMode, uint8_t, float)> mode_change_callback_;
    std::function<void()> config_commit_callback_;
    
    // Per-mode setpoint cache (issue #51): each mode stores only its own value,
    // eliminating cross-mode contamination bugs that arose from the old shared
    // cached_setpoint_ field. NAN = not yet read from pump.
    //
    // NOTE: cached_flow_setpoint_ reads back from the pump like the other modes.
    // Object 86 only *appeared* unreliable for CONSTANT_FLOW (issue #44) because
    // the component wrote the setpoint in m³/h; the pump interpreted that as m³/s,
    // rejected it as out of range, and kept its old value, so the register looked
    // static. Once the write uses the pump's native m³/s (investigated in #88,
    // fixed in PR #90), the pump stores the value and reports it back, and
    // set_constant_flow_async() performs the same 1.2s post-write readback as the
    // other setters (issue #96).
    float cached_pressure_setpoint_{NAN};       // CONSTANT_PRESSURE: meters of water column
    float cached_proportional_setpoint_{NAN};   // PROPORTIONAL_PRESSURE: meters
    float cached_speed_setpoint_{NAN};           // CONSTANT_SPEED: RPM
    float cached_flow_setpoint_{NAN};            // CONSTANT_FLOW: m³/h
    float cached_temp_min_{NAN};           // Temperature range min (Object 91 Sub 430)
    float cached_temp_max_{NAN};           // Temperature range max (Object 91 Sub 430)
    uint8_t cached_operation_mode_{0xFF};  // Operation mode from notification
    int8_t cached_autoadapt_{-1};           // AutoAdapt state (-1=unknown, 0=off, 1=on)
    int8_t cached_cycle_time_on_{-1};       // Cycle time ON minutes (Obj 91 Sub 421, issue #106)
    int8_t cached_cycle_time_off_{-1};      // Cycle time OFF minutes (Obj 91 Sub 421)
    // DHW config (Obj 91 Sub 421, type 985 DHWOnOffControlConfiguration): the
    // stored per-mode flow setpoint, kept as the raw wire bytes so a cycle-time
    // write can echo it back verbatim (read-modify-write) without a float
    // round trip. Valid only after a successful read_dhw_config().
    uint8_t cached_dhw_setpoint_raw_[4]{0, 0, 0, 0};
    bool dhw_config_valid_{false};
    // Trailing bytes of the Sub 430 TemperatureRangeControlUserSettings struct
    // (the pump's min/max on/off-time LIMITS + version tail, issue #106): kept
    // verbatim from the last read so write_temp_range_config() preserves them
    // instead of zeroing the pump's limits. Defaults to the byte pattern the
    // component historically sent, used only before the first read.
    uint8_t cached_temp_limits_tail_[5]{0x00, 0x00, 0x00, 0x16, 0x00};
    bool temp_limits_tail_valid_{false};

   public:
    /// Has the pump's own on/off-time LIMITS block been read back yet?
    ///
    /// write_temp_range_config() echoes those five bytes verbatim so a
    /// temperature write does not zero them (issue #106). They arrive only
    /// with an Obj 91 Sub 430 reply; until one lands, the array below holds
    /// this class's historical constants, which are not the pump's limits.
    ///
    /// This is cleared by invalidate_cache() on every disconnect, so it is
    /// false for a window on every reconnect -- and the HA service path reaches
    /// the write without check_ready() (the entity path is gated,
    /// api_bridge.cpp is not). A service call in that window would otherwise
    /// send the constants as if they were the pump's own.
    ///
    /// Note what it does NOT catch: the reply's declared size (payload[2]) is
    /// ignored, so a pump whose type-1012 struct is shorter inside a
    /// full-length frame would have five bytes of padding captured as limits
    /// and this would read true. Deriving the tail bound from the declared
    /// size is the check for that, and is not made here.
    bool temp_limits_known() const { return temp_limits_tail_valid_; }

   private:
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
   * Send the Class 3 remote-mode command: enable (0x07) or disable/Auto
   * (0x08) as a SET (`[0x03, 0x81, <id>]`). Same ACK shapes as
   * send_run_command(): a clean `[03 00]` is an ack, the `[03 01 xx]`
   * descriptor reply is a rejection, and a closed window is neither.
   *
   * Opcode 0x81 (SET) rather than 0xC1 (INFO) is issue #46, bench-verified:
   * 0xC1 always produced the rejection shape and remote mode never took
   * effect. The Python reference (control.py::enable_remote_mode) still has
   * the 0xC1 bug.
   *
   * Like the run-state commands this produces no unsolicited notification,
   * so the ack is not the verdict -- the caller reads control_source back
   * (Object 86 Sub 7) and that readback decides.
   */
  void send_remote_mode_command(bool enable, std::function<void(bool acked, bool rejected)> on_result);

  /**
   * Send the pump's unfused Class 3 run-state command: START (0x06) or STOP
   * (0x05) as a SET (`[0x03, 0x81, <id>]`). Command ids and ACK behavior
   * bench-verified by jfriend00 on a real pump (issue #92, 2026-07-19):
   * success is the clean `[03 00]` ACK (empty data), a rejection is the
   * `[03 01 xx]` descriptor reply (the same shapes as the remote-mode
   * commands, #46). Unlike the 0x0601 control object, this carries no mode
   * and no setpoint, so it cannot clobber either — but it also produces no
   * unsolicited notification, so callers must read the run state back to
   * confirm it actually changed.
   *
   * @param start_pump True for START, false for STOP
   * @param on_result Called once: (acked, rejected). (false, false) = the
   *   ACK window closed without a match (the command may still have applied;
   *   decide via readback).
   */
  void send_run_command(bool start_pump, std::function<void(bool acked, bool rejected)> on_result);

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
  bool send_control_request(ControlMode mode, bool start_pump, float setpoint = NAN,
                            bool queue_commit = true);

  /**
   * Change the control mode without altering the mode's stored setpoint or the
   * pump's enabled state. Sends a Class 10 SET with APDU Sub=0x5600, Obj=0x0A01
   * (this is the GENI overall_control_mode_local_request object -- id 86,
   * sub-id 10 -- addressed with sub-id 10 in the high byte of the Obj field).
   * Per its GENI profile that object ignores control_source, operation_mode and
   * set_point, so the payload fills those with no-op sentinels
   * (Undefined / NoCmd / NaN) and only control_mode is applied -- matching the
   * Grundfos GO app. Unlike send_control_request()'s start/stop object
   * (Sub=0x5600, Obj=0x0601), this never overwrites the pump's setpoint
   * (issue #97/#83) and never force-enables the pump (issue #45).
   *
   * @param mode Control mode to switch to
   * @return True if the command was queued
   */
  bool send_set_mode_request(ControlMode mode);

  /**
   * Record a just-sent mode command as "commanded but unconfirmed" (issue #91):
   * sets commanded_mode_/mode_command_pending_, resets the confirm-attempt
   * counter, optimistically adopts the mode, and notifies the mode-change
   * callback. Shared by set_mode(), start(mode) and the write-operation layer.
   */
  void note_mode_commanded(ControlMode mode);

  /**
   * Record a just-sent start/stop command's optimistic enabled state.
   * Shared by start()/stop() and the write-operation layer.
   */
  void note_enabled_commanded(bool enabled);

  /**
   * Write the temperature-range configuration struct (Object 91 Sub 430,
   * OpSpec 0x97, matching the Grundfos GO app capture) and report the pump's
   * ACK. Does NOT switch mode or commit; callers sequence that around it.
   *
   * @param on_ack Called with true on the pump's ACK, false on timeout.
   */
  void write_temp_range_config(float min_temp, float max_temp, bool autoadapt_enabled,
                               std::function<void(bool)> on_ack);

  /**
   * Read the DHW on/off configuration (Object 91 Sub 421, type 985
   * DHWOnOffControlConfiguration: [setpoint f32][on u8][off u8]) and refresh
   * cached_cycle_time_on_/off_ plus the raw setpoint bytes. This is the
   * object that actually holds the live cycle times; the Sub 430 block the
   * component previously read is the temperature-range user settings, whose
   * trailing bytes are on/off-time LIMITS and invariant to the cycle
   * configuration (issue #106, GENI profile 52/7 + GO-app captures).
   *
   * @param callback Called with true when the read parsed and caches updated.
   */
  void read_dhw_config(std::function<void(bool)> callback);

  /**
   * Write the DHW on/off configuration (Object 91 Sub 421, OpSpec 0x8F,
   * obj-first addressing, mirroring the GO app's capture frame byte for
   * byte). Read-modify-write: callers MUST read first — returns false
   * without sending when no config is cached. No configuration commit is
   * needed (capture-verified: the GO app sends none and the value persists).
   *
   * @param on_ack Called with the pump's short ACK result.
   * @param setpoint_be4 Optional new flow setpoint as 4 wire bytes (f32 BE,
   *   m³/s; issue #107). nullptr echoes the stored setpoint bytes verbatim
   *   from the last read_dhw_config().
   */
  bool write_dhw_config(uint8_t on_minutes, uint8_t off_minutes,
                        std::function<void(bool)> on_ack,
                        const uint8_t *setpoint_be4 = nullptr);

  /**
   * Read Object 91 Sub 430 (temperature range, AutoAdapt, cycle times) and
   * refresh the corresponding caches. Extracted from sync_cache_async() so the
   * write-operation layer can confirm config writes without re-reading the mode.
   *
   * @param callback Called with true when the read parsed and caches updated.
   */
  void read_obj91_config(std::function<void(bool)> callback);

  /**
   * Store a pump-native setpoint into the given mode's per-mode cache (issue #51),
   * converting to display units. Keyed on the passed mode rather than
   * current_mode_ so a readback can refresh the pump's actually-reported mode even
   * while current_mode_ is held optimistically (issue #91).
   */
  void cache_setpoint_for_mode(ControlMode mode, float raw_setpoint);

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
