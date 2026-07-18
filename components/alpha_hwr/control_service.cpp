#include "control_service.h"
#include "transport.h"
#include "frame_builder.h"
#include "esphome/core/log.h"
#include <cstring>
#include <cmath>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char *TAG = "alpha_hwr.control";

// Class 10 Control Mode Mapping
// Maps ControlMode values to (ModeByte, SuffixBytes)
// Based on protocol specification in control.py lines 137-145
// Class 10 Control Mode Mapping
// Maps ControlMode enum values (array index) to (ModeByte, SuffixBytes).
// Reference: control.py _MODE_BYTE_MAP (lines 145-151) and _MODE_SUFFIX_MAP (lines 156-163)
//
// The 4-byte suffix is the set_point field written into the start/stop control
// object (overall_operation_local_request, Obj 0x0601) when no explicit setpoint
// is supplied. For the scalar-setpoint modes this suffix (45 65 70 00 = 3671.0)
// is NOT inert -- bench-confirmed that writing it durably overwrites the pump's
// stored setpoint. send_control_request() therefore sends a NaN "keep existing"
// sentinel for those modes instead of this default (issue #83); the suffix here
// is only used for modes that do not carry a scalar setpoint (DHW / temperature
// range), whose values were taken from GO app captures.
const ControlService::ControlModeMapping ControlService::CLASS10_CONTROL_MAP[] = {
    {0x00, {0x45, 0x65, 0x70, 0x00}},  // CONSTANT_PRESSURE (0)
    {0x01, {0x45, 0x65, 0x70, 0x00}},  // PROPORTIONAL_PRESSURE (1) - mode_byte 0x01, suffix from _MODE_SUFFIX_MAP
    {0x02, {0x45, 0x65, 0x70, 0x00}},  // CONSTANT_SPEED (2)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (3) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (4) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // AUTO_ADAPT (5) - not in _MODE_BYTE_MAP
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (6) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (7) - unused
    {0x08, {0x45, 0x65, 0x70, 0x00}},  // CONSTANT_FLOW (8) - mode_byte 0x08, suffix same as pressure
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (9) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (10) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (11) - unused
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (12) - unused
    {0x0D, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_RADIATOR (13)
    {0x0E, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_UNDERFLOOR (14)
    {0x0F, {0x45, 0x65, 0x70, 0x00}},  // AUTO_ADAPT_COMBINED (15)
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (16-24) - unused entries
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x00, {0x00, 0x00, 0x00, 0x00}},
    {0x19, {0x38, 0xC6, 0x76, 0xEF}},  // DHW_ON_OFF (25) - suffix from app capture
    {0x00, {0x00, 0x00, 0x00, 0x00}},  // (26) - unused
    {0x1B, {0x39, 0x67, 0x70, 0x00}},  // TEMPERATURE_RANGE (27)
};

ControlService::ControlService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {
  ESP_LOGI(TAG, "ControlService initialized");
}

void ControlService::set_schedule_callback(std::function<void(std::function<void()>, uint32_t)> callback) {
   schedule_callback_ = callback;
 }

void ControlService::set_mode_change_callback(std::function<void(ControlMode, uint8_t, float)> callback) {
   mode_change_callback_ = callback;
 }

void ControlService::update_mode_from_notification(uint8_t mode, uint8_t operation_mode, float setpoint,
                                                   uint8_t control_source) {
  // Update internal state
  ControlMode reported = static_cast<ControlMode>(mode);
  cached_operation_mode_ = operation_mode;

  // The setpoint belongs to the mode the pump is actually reporting, so cache it
  // into that mode's own slot regardless of the pending guard below (issue #51/#91).
  // Reference: control.py::get_mode() lines 428-434 (Pa → meters for pressure modes).
  cache_setpoint_for_mode(reported, setpoint);

  // Issue #91: don't let a passive notification reporting the OLD mode overwrite
  // an in-flight mode command before the pump has applied it. Keep the commanded
  // mode until the notification reports it. (Notifications are rare, but this
  // keeps every current_mode_ writer consistent.)
  if (mode_command_pending_ && reported != commanded_mode_) {
    ESP_LOGD(TAG, "Ignoring notification mode %s while %s is pending confirmation",
             get_mode_name(reported), get_mode_name(commanded_mode_));
  } else {
    mode_command_pending_ = false;
    current_mode_ = reported;
    mode_valid_ = true;  // Mark mode as valid - we received it from the pump
  }

  // Derive pump enabled state from operation_mode:
  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled
  pump_enabled_ = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
  pump_enabled_valid_ = true;

  // Fix #53: update remote mode state from the pump's control_source byte.
  // Reference: Python control.py — is_remote = (control_source == 2).
  //   2 = Remote/Digital (BMS or ESPHome controlling the pump)
  //   1 = Local/Panel    (physical panel is in control)
  // Only update on the two known values; treat 0 or any unexpected byte as
  // "unknown" and leave the current state unchanged so a stale reading from
  // the pump cannot incorrectly clear a state that was confirmed via ACK.
  if (control_source == 2) {
    is_remote_mode_enabled_ = true;
    ESP_LOGD(TAG, "Remote mode: pump reports control_source=2 (Remote/Digital) → enabled");
  } else if (control_source == 1) {
    is_remote_mode_enabled_ = false;
    ESP_LOGD(TAG, "Remote mode: pump reports control_source=1 (Local/Panel) → disabled");
  } else if (control_source != 0xFF) {
    // 0xFF is our own sentinel meaning "caller didn't pass control_source".
    // Any other unexpected value is logged but does not change the cached state.
    ESP_LOGD(TAG, "Remote mode: control_source=0x%02X unrecognized — state unchanged (was %s)",
             control_source, is_remote_mode_enabled_ ? "enabled" : "disabled");
  }
  
  float display_setpoint = get_setpoint_for_mode(current_mode_);
  ESP_LOGI(TAG, "Control mode from notification: %s (op_mode=%d, setpoint=%.4f, raw=%.4f, enabled=%s, remote=%s)",
           get_mode_name(current_mode_), operation_mode, display_setpoint, setpoint,
           pump_enabled_ ? "YES" : "NO", is_remote_mode_enabled_ ? "YES" : "NO");
  
  // Notify callback if set
  if (mode_change_callback_) {
    mode_change_callback_(current_mode_, operation_mode, display_setpoint);
  }
}

void ControlService::cache_setpoint_for_mode(ControlMode mode, float raw_setpoint) {
  // Store the pump-native setpoint into the mode's own per-mode cache, converted
  // to display units (issue #51). Keyed on the given mode -- not current_mode_ --
  // so a readback can populate the reported mode's cache even while current_mode_
  // is being held optimistically (issue #91). Modes without a scalar setpoint
  // (DHW / temperature range / auto adapt) have no slot and are left untouched.
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:     cached_pressure_setpoint_ = raw_setpoint / 9806.65f; break;
    case ControlMode::PROPORTIONAL_PRESSURE: cached_proportional_setpoint_ = raw_setpoint / 9806.65f; break;
    case ControlMode::CONSTANT_FLOW:         cached_flow_setpoint_ = raw_setpoint * 3600.0f; break;
    case ControlMode::CONSTANT_SPEED:        cached_speed_setpoint_ = raw_setpoint; break;
    default: break;
  }
}

void ControlService::sync_cache_async(std::function<void(bool)> callback) {
  ESP_LOGD(TAG, "Syncing full control cache...");
  ControlMode expected_mode = mode_valid_ ? current_mode_ : ControlMode::NONE;
  
  get_mode_async([this, expected_mode, callback](bool success, ControlMode mode) {
    if (!success) {
      ESP_LOGW(TAG, "Failed to sync control state (get_mode failed)");
      if (callback) callback(false);
      return;
    }
    
    if (expected_mode != ControlMode::NONE && mode != expected_mode) {
      // The pump reports a different mode than we expect. If a mode command is
      // still in flight, get_mode_async() has already kept current_mode_ at the
      // commanded value (no clobber, and no NaN of the setpoint cache -- issue
      // #91). Give the pump a bounded number of 2s retries to apply the command.
      if (mode_command_pending_ && mode_confirm_attempts_ < MAX_MODE_CONFIRM_ATTEMPTS) {
        mode_confirm_attempts_++;
        ESP_LOGW(TAG, "Pump still reports %s (expected %s), retry %u/%u in 2s...",
                 get_mode_name(mode), get_mode_name(expected_mode),
                 static_cast<unsigned>(mode_confirm_attempts_),
                 static_cast<unsigned>(MAX_MODE_CONFIRM_ATTEMPTS));
        if (schedule_callback_) {
          schedule_callback_([this, callback]() { this->sync_cache_async(callback); }, 2000);
        } else if (callback) {
          callback(false);
        }
        return; // Stop here, retry will continue
      }

      // Either no command is pending (a genuine out-of-band mode change we should
      // adopt) or we've exhausted the retries (the pump never applied our
      // command). Accept what the pump reports so state reflects reality instead
      // of endlessly forcing the commanded mode.
      ESP_LOGW(TAG, "Accepting pump-reported mode %s (expected %s) after %u attempt(s)",
               get_mode_name(mode), get_mode_name(expected_mode),
               static_cast<unsigned>(mode_confirm_attempts_));
      mode_command_pending_ = false;
      mode_confirm_attempts_ = 0;
      current_mode_ = mode;
      mode_valid_ = true;
      if (mode_change_callback_) {
        mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
      }
      // Fall through to read Obj 91 config bounds for the accepted mode.
    }

    // Now read Obj 91 Sub 430 for Temp Range, AutoAdapt, and Cycle Time
    read_obj91_config([this, callback](bool ok) {
      if (!ok) {
        if (callback) callback(false);
        return;
      }
      if (callback) {
        bool valid = is_cache_valid();
        if (!valid) {
          ESP_LOGW(TAG, "Cache sync completed but required fields are missing (autoadapt=%d, temp_min=%.1f, temp_max=%.1f)",
                   cached_autoadapt_, cached_temp_min_, cached_temp_max_);
        }
        callback(valid);
      }
    });
  });
}

void ControlService::read_obj91_config(std::function<void(bool)> callback) {
  uint8_t apdu[5] = {0x0A, 0x03, 91, 0x01, 0xAE};
  this->transport_.send_apdu_command(apdu, 5, 91, 430,
    [this, callback](bool ok, const uint8_t* payload, size_t payload_len) {
      if (!ok || payload_len < 12) {
        ESP_LOGW(TAG, "Failed to sync config bounds (success=%d, len=%zu)", ok, payload_len);
        if (callback) callback(false);
        return;
      }

      int offset = (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) ? 3 : 0;

      if (payload_len >= (size_t)(offset + 9)) {
        cached_autoadapt_ = payload[offset] ? 1 : 0;
        cached_temp_min_ = protocol::decode_float_be(&payload[offset + 1]);
        cached_temp_max_ = protocol::decode_float_be(&payload[offset + 5]);

        // Cycle-time config is optional for readiness (issue #94) and each byte
        // is range-validated (1-60) so a 0xFF/0/out-of-range value maps to the
        // -1 "unknown" sentinel instead of truncating into it. Reset to unknown
        // first so a short payload that omits these bytes reports "unknown"
        // rather than leaving stale values from a previous read.
        cached_cycle_time_off_ = -1;
        cached_cycle_time_on_ = -1;
        if (payload_len >= (size_t)(offset + 10)) {
          cached_cycle_time_off_ = parse_cycle_time_minutes(payload[offset + 9]);
          if (payload_len >= (size_t)(offset + 13)) {
            cached_cycle_time_on_ = parse_cycle_time_minutes(payload[offset + 12]);
          }
        }
      }

      if (mode_change_callback_) {
        mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
      }

      // Cycle times are not part of is_cache_valid() (issue #94), so log them
      // separately as informational -- they may legitimately be -1 (unknown).
      ESP_LOGD(TAG, "Config bounds synced (cycle_on=%d, cycle_off=%d)",
               cached_cycle_time_on_, cached_cycle_time_off_);

      if (callback) callback(true);
    }, 5000);
}

bool ControlService::get_mode_async(std::function<void(bool, ControlMode)> on_complete) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot get mode: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    if (on_complete) {
      on_complete(false, current_mode_);
    }
    return false;
  }

  ESP_LOGD(TAG, "Reading current control mode from pump (Object 86, SubID 7)...");

   // Build Class 10 READ request: [0x0A][0x03][Obj][Sub-H][Sub-L]
   // Object 86 (0x56), SubID 7 (0x0007) - overall_operation_prioritized_request_obj
   // Reference: GENI profile - this is the read-only prioritized status after remote/local/alarm logic
   // Reference: control.py::_read_class10_object() lines 76-85
   //
   // IMPORTANT: The pump responds with a PASSIVE NOTIFICATION (OpSpec 0x0E), not a direct response!
   // The Python reference accepts ANY Class 10 packet as the response (see base.py::match_class10_response).
   // We need to do the same here - accept any Class 10 packet, not just exact Object/Sub ID matches.
   //
   // Response format: [00 00 XX][control_source][operation_mode][control_mode][setpoint(4 bytes)]
   // Where control_source now reflects the prioritized state (remote vs local control).
   uint8_t apdu[5];
   apdu[0] = 0x0A;  // Class 10
   apdu[1] = 0x03;  // OpSpec: READ (INFO)
   apdu[2] = 0x56;  // Object 86 (1 byte!)
   apdu[3] = 0x00;  // SubID 7 high byte
   apdu[4] = 0x07;  // SubID 7 low byte
   
  // Send with response matching for Object 86 Sub 7 response
  // The pump responds with OpSpec 0x0E notification: bytes 6-7 = Sub 0x0001, bytes 8-9 = Obj 0x2F01
    this->transport_.send_apdu_command(
      apdu, 5, 
      0x0001,  // Match Sub-ID at bytes 6-7 of response frame
      0x2F01,  // Match Obj-ID at bytes 8-9 of response frame
      [this, on_complete](bool success, const uint8_t* payload, size_t payload_len) {
        if (!success) {
          ESP_LOGW(TAG, "Failed to read control mode (timeout)");
          if (on_complete) {
            on_complete(false, current_mode_);
          }
          return;
        }

        if (payload_len >= 10) {
          // Response format: [00 00 XX][control_source][operation_mode][control_mode][setpoint(4 bytes)]
          // Determine offset: check for 3-byte header [00 00 XX]
          int offset = 0;
          if (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) {
            offset = 3;
          }

          if (payload_len >= offset + 7) {
            uint8_t control_source = payload[offset];
            uint8_t operation_mode = payload[offset + 1];
            uint8_t control_mode_byte = payload[offset + 2];

            ESP_LOGD(TAG, 
              "Parsed control mode: mode=%d, op_mode=%d, source=%d (raw payload_len=%zu)", 
              control_mode_byte, operation_mode, control_source, payload_len);

            // Validate control mode value
            if (control_mode_byte <= static_cast<uint8_t>(ControlMode::NONE)) {
              ControlMode reported = static_cast<ControlMode>(control_mode_byte);

              // The pump's run state and remote/local source reflect its actual
              // reality independent of which control mode we expect, so update
              // them unconditionally.
              cached_operation_mode_ = operation_mode;
              pump_enabled_ = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
              pump_enabled_valid_ = true;

              // Fix #53: Update remote mode state from control_source.
              // Sub 7 is the prioritized status object that reflects the actual remote/local state
              // after evaluation of remote/local/alarm influence.
              // Reference: GENI profile - control_source: 2 = Remote/Digital, 1 = Local/Panel.
              if (control_source == 2) {
                is_remote_mode_enabled_ = true;
                ESP_LOGD(TAG, "Remote mode: Sub 7 prioritized read control_source=2 (Remote/Digital) → enabled");
              } else if (control_source == 1) {
                is_remote_mode_enabled_ = false;
                ESP_LOGD(TAG, "Remote mode: Sub 7 prioritized read control_source=1 (Local/Panel) → disabled");
              } else {
                ESP_LOGD(TAG, "Remote mode: Sub 7 prioritized read control_source=0x%02X unrecognized — state unchanged",
                         control_source);
              }

              // The reported setpoint belongs to whatever mode the pump is
              // ACTUALLY in right now, so cache it into that mode's own slot
              // regardless of whether we keep current_mode_ optimistic below. This
              // keeps every mode's cache fresh and means bounded-retry recovery
              // (sync_cache_async accepting the reported mode) never lands on an
              // un-populated setpoint (issue #51/#91).
              if (payload_len >= (size_t)(offset + 7)) {
                cache_setpoint_for_mode(reported, protocol::decode_float_be(&payload[offset + 3]));
              }

              // Issue #91: if a mode command is in flight and the pump is still
              // reporting the OLD mode, this read landed before the pump applied
              // our command (or is a stale in-flight read). Keep the commanded
              // mode rather than letting the stale report overwrite it.
              // sync_cache_async() will retry, and once the pump reports the
              // commanded mode the block below confirms it. This is what makes the
              // #54 poll and the post-command reconciles safe against an in-flight
              // mode change.
              if (mode_command_pending_ && reported != commanded_mode_) {
                ESP_LOGD(TAG, "Ignoring readback mode %s while %s is pending confirmation",
                         get_mode_name(reported), get_mode_name(commanded_mode_));
                // Report the pump's ACTUAL mode to the caller (not the kept
                // commanded mode) so sync_cache_async() can see the mismatch and
                // drive its bounded retry. current_mode_ itself is left untouched.
                if (on_complete) {
                  on_complete(true, reported);
                }
                return;
              }

              // Readback confirms the commanded mode, or no command is pending
              // (out-of-band change, issue #54): trust the reported mode.
              mode_command_pending_ = false;
              mode_confirm_attempts_ = 0;
              current_mode_ = reported;
              mode_valid_ = true;

              ESP_LOGI(TAG, "Control mode updated to %d (%s), setpoint=%.2f",
                control_mode_byte, get_mode_name(current_mode_), get_setpoint_for_mode(current_mode_));

              // Notify mode change
              if (mode_change_callback_) {
                mode_change_callback_(current_mode_, operation_mode, get_setpoint_for_mode(current_mode_));
              }

              if (on_complete) {
                on_complete(true, current_mode_);
              }
              return;
            } else {
              ESP_LOGW(TAG, "Invalid control mode value: %d", control_mode_byte);
            }
          } else {
            ESP_LOGW(TAG, "Response too short: expected >= %d bytes, got %zu", offset + 7, payload_len);
          }
        } else {
          ESP_LOGW(TAG, "Response payload too short: expected >= 10 bytes, got %zu", payload_len);
        }

        if (on_complete) {
          on_complete(false, current_mode_);
        }
      },
      5000);  // 5-second timeout for Object 86 read

  return true;
}

void ControlService::handle_remote_mode_ack(bool enabling, bool got_response, const uint8_t* data, size_t len) {
  // Class 3 command ACK format: [Start][Len][Dest][SvcH][Class=0x03][Ack]...[CRC]
  // Ack byte 0x00 = clean/success ACK; 0x01 (with a trailing descriptor byte)
  // = rejected/no-op. Bench-verified against a real pump (2026-07-08, #46):
  // opcode 0x81 (SET) reliably produces the 0x00 ack; the previous 0xC1
  // (INFO) opcode always produced the 0x01 "rejected" ack, which is why
  // remote mode never actually took effect before this fix.
  if (!got_response || len < 6) {
    ESP_LOGW(TAG, "Remote mode %s: no ACK received (timeout) -- state left unchanged",
             enabling ? "enable" : "disable");
    return;
  }
  uint8_t ack_byte = data[5];
  if (ack_byte == 0x00) {
    this->is_remote_mode_enabled_ = enabling;
    ESP_LOGI(TAG, "Remote mode %s (confirmed: clean ACK from pump)", enabling ? "enabled" : "disabled (Auto)");
  } else {
    ESP_LOGW(TAG, "Remote mode %s command was rejected by pump (ack=0x%02X, expected 0x00) -- state left unchanged",
             enabling ? "enable" : "disable", ack_byte);
  }
}

bool ControlService::enable_remote_mode() {
   // Verify session is authenticated
   if (session_.get_state() != core::SessionState::READY) {
     ESP_LOGW(TAG, "Cannot enable remote mode: session not ready");
     return false;
   }

   ESP_LOGI(TAG, "Enabling remote mode...");

   // Class 3: 03 81 07 (OpSpec 0x81 = SET; command ID 7 = enable remote).
   // Fix #46: was 0xC1 (INFO), which the pump always rejected with a
   // "descriptor-only" ACK ([03 01 xx]) rather than the clean [03 00]
   // success ACK -- bench-verified against a real pump.
   const uint8_t apdu[3] = {0x03, 0x81, 0x07};
   
   // Send command and wait for the ACK (wildcard match: any Class 3 response)
   // so we only update is_remote_mode_enabled_ once the pump actually
   // confirms the command, instead of assuming success.
   this->transport_.send_apdu_command(apdu, 3, 0, 0,
     [this](bool success, const uint8_t* data, size_t len) {
       this->handle_remote_mode_ack(true, success, data, len);
     });
   
   ESP_LOGI(TAG, "Enable remote mode command sent, awaiting ACK...");
   return true;
 }
 
 bool ControlService::disable_remote_mode() {
   // Verify session is authenticated
   if (session_.get_state() != core::SessionState::READY) {
     ESP_LOGW(TAG, "Cannot disable remote mode: session not ready");
     return false;
   }

   ESP_LOGI(TAG, "Disabling remote mode (Auto)...");

   // Class 3: 03 81 08 (OpSpec 0x81 = SET; command ID 8 = disable remote / Auto).
   // Fix #46: was 0xC1 (INFO) -- see enable_remote_mode() for details.
   const uint8_t apdu[3] = {0x03, 0x81, 0x08};
   
   // Send command and wait for the ACK -- see enable_remote_mode().
   this->transport_.send_apdu_command(apdu, 3, 0, 0,
     [this](bool success, const uint8_t* data, size_t len) {
       this->handle_remote_mode_ack(false, success, data, len);
     });
   
   ESP_LOGI(TAG, "Disable remote mode command sent, awaiting ACK...");
   return true;
 }

void ControlService::with_resolved_enabled_state(std::function<void(bool resolved, bool enabled)> on_resolved) {
  if (pump_enabled_valid_) {
    on_resolved(true, pump_enabled_);
    return;
  }

  // Fix #45: don't guess "true" (start) when we don't actually know the
  // pump's current on/off state — read it back first.
  ESP_LOGD(TAG, "Pump enabled state unknown; reading from pump before sending control request...");
  get_mode_async([this, on_resolved](bool success, ControlMode /*mode*/) {
    // get_mode_async() updates pump_enabled_/pump_enabled_valid_ internally
    // on success (see its response handler above).
    if (!success || !pump_enabled_valid_) {
      // Genuinely unknown: neither "true" nor "false" is safe to guess here
      // (either could force-enable or force-disable the pump), so abort
      // the control request entirely instead of picking one.
      ESP_LOGW(TAG, "Could not determine pump enabled state before control request; "
                    "aborting rather than guessing (could force-enable or force-disable the pump)");
      on_resolved(false, false);
      return;
    }
    on_resolved(true, pump_enabled_);
  });
}

const char *ControlService::get_mode_name(ControlMode mode) {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:
      return "Constant Pressure";
    case ControlMode::PROPORTIONAL_PRESSURE:
      return "Proportional Pressure";
    case ControlMode::CONSTANT_SPEED:
      return "Constant Speed";
    case ControlMode::AUTO_ADAPT:
      return "Auto Adapt";
    case ControlMode::CONSTANT_FLOW:
      return "Constant Flow";
    case ControlMode::AUTO_ADAPT_RADIATOR:
      return "Auto Adapt Radiator";
    case ControlMode::AUTO_ADAPT_UNDERFLOOR:
      return "Auto Adapt Underfloor";
    case ControlMode::AUTO_ADAPT_COMBINED:
      return "Auto Adapt Combined";
    case ControlMode::DHW_ON_OFF:
      return "Cycle Time Control";
    case ControlMode::TEMPERATURE_RANGE:
      return "Temperature Control";
    case ControlMode::NONE:
      return "Unknown";
    default:
      return "Unknown";
  }
}

float ControlService::get_setpoint_for_mode(ControlMode mode) const {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:     return cached_pressure_setpoint_;
    case ControlMode::PROPORTIONAL_PRESSURE: return cached_proportional_setpoint_;
    case ControlMode::CONSTANT_SPEED:        return cached_speed_setpoint_;
    case ControlMode::CONSTANT_FLOW:         return cached_flow_setpoint_;
    default:                                 return NAN;
  }
}

void ControlService::note_mode_commanded(ControlMode mode) {
  commanded_mode_ = mode;
  mode_command_pending_ = true;
  mode_confirm_attempts_ = 0;
  current_mode_ = mode;
  mode_valid_ = true;
  if (mode_change_callback_) {
    mode_change_callback_(current_mode_, 0, 0.0f);
  }
}

void ControlService::note_enabled_commanded(bool enabled) {
  pump_enabled_ = enabled;
  pump_enabled_valid_ = true;
}

const char *ControlService::mode_to_string(ControlMode mode) {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:     return "constant_pressure";
    case ControlMode::PROPORTIONAL_PRESSURE: return "proportional_pressure";
    case ControlMode::CONSTANT_SPEED:        return "constant_speed";
    case ControlMode::AUTO_ADAPT:            return "auto_adapt";
    case ControlMode::CONSTANT_FLOW:         return "constant_flow";
    case ControlMode::AUTO_ADAPT_RADIATOR:   return "auto_adapt_radiator";
    case ControlMode::AUTO_ADAPT_UNDERFLOOR: return "auto_adapt_underfloor";
    case ControlMode::AUTO_ADAPT_COMBINED:   return "auto_adapt_combined";
    case ControlMode::DHW_ON_OFF:            return "cycle_time";
    case ControlMode::TEMPERATURE_RANGE:     return "temperature_range";
    default:                                 return "unknown";
  }
}

bool ControlService::mode_from_string(const char *str, ControlMode &out) {
  static const ControlMode MODES[] = {
      ControlMode::CONSTANT_PRESSURE,   ControlMode::PROPORTIONAL_PRESSURE,
      ControlMode::CONSTANT_SPEED,      ControlMode::AUTO_ADAPT,
      ControlMode::CONSTANT_FLOW,       ControlMode::AUTO_ADAPT_RADIATOR,
      ControlMode::AUTO_ADAPT_UNDERFLOOR, ControlMode::AUTO_ADAPT_COMBINED,
      ControlMode::DHW_ON_OFF,          ControlMode::TEMPERATURE_RANGE,
  };
  if (str == nullptr) return false;
  for (ControlMode m : MODES) {
    if (strcmp(str, mode_to_string(m)) == 0) {
      out = m;
      return true;
    }
  }
  return false;
}

void ControlService::send_configuration_commit() {
  // Delegate to the external callback (ScheduleService::send_configuration_commit)
  // which preserves the cached ClockProgramOverview structure including
  // the schedule_enabled flag. The previous hardcoded implementation had
  // clock_program_enabled=0x00 which silently disabled the schedule.
  if (config_commit_callback_) {
    ESP_LOGD(TAG, "Delegating configuration commit to ScheduleService...");
    config_commit_callback_();
  } else {
    ESP_LOGW(TAG, "No configuration commit callback set - skipping commit");
  }
}


bool ControlService::send_control_request(ControlMode mode, bool start_pump, float setpoint,
                                          bool queue_commit) {
  // Reference: control.py::_send_control_request() lines 233-284
  // Payload: [2F 01 00 00 07 00][Flag][Mode][Suffix/Setpoint(4)]

  ControlModeMapping mapping;
  if (!get_class10_mapping(mode, mapping)) {
    // Default to mode_byte 0x02 (CONSTANT_SPEED) like Python does
    mapping.mode_byte = 0x02;
    memcpy(mapping.suffix, "\x45\x65\x70\x00", 4);
  }

  uint8_t payload[12];
  payload[0] = 0x2F;
  payload[1] = 0x01;
  payload[2] = 0x00;
  payload[3] = 0x00;
  payload[4] = 0x07;
  payload[5] = 0x00;
  payload[6] = start_pump ? 0x00 : 0x01;  // 0=Start, 1=Stop
  payload[7] = mapping.mode_byte;

  if (!std::isnan(setpoint)) {
    // Encode setpoint as float32 BE in the set_point field
    protocol::encode_float_be(setpoint, &payload[8]);
  } else if (mode == ControlMode::CONSTANT_PRESSURE || mode == ControlMode::PROPORTIONAL_PRESSURE ||
             mode == ControlMode::CONSTANT_SPEED || mode == ControlMode::CONSTANT_FLOW) {
    // Scalar-setpoint mode with no value to send (e.g. start/stop before the
    // cache has synced). Send a NaN set_point so the pump keeps the mode's stored
    // setpoint instead of overwriting it with the map default (~3671), which is a
    // durable clobber (issue #83). NaN = "no change", matching the Grundfos GO app.
    payload[8] = 0x7F;
    payload[9] = 0xFF;
    payload[10] = 0xFF;
    payload[11] = 0xFF;
  } else {
    // Non-scalar modes (DHW / temperature range): use the captured default suffix.
    memcpy(&payload[8], mapping.suffix, 4);
  }

  // OpSpec 0x90 = SET + 16 bytes (4 IDs + 12 payload)
  uint8_t apdu[18];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x90;  // OpSpec: SET with length 16
  apdu[2] = 0x56;  // Sub ID high (SUB_CONTROL = 0x5600)
  apdu[3] = 0x00;  // Sub ID low
  apdu[4] = 0x06;  // Obj ID high (OBJ_CONTROL = 0x0601)
  apdu[5] = 0x01;  // Obj ID low
  memcpy(&apdu[6], payload, 12);

  // Send command and optionally schedule configuration commit.
  // Multi-step setters (mode switch + dedicated write) disable this here and
  // issue a single commit after the step-2 write.
  this->transport_.send_apdu_command(apdu, 18);

  if (queue_commit && schedule_callback_) {
    schedule_callback_([this]() { this->send_configuration_commit(); }, 200);
  }

  return true;
}

bool ControlService::send_set_mode_request(ControlMode mode) {
  // Change the control mode WITHOUT touching the mode's stored setpoint.
  //
  // Writes GENI Class 10 object 86 / sub-id 10 = overall_control_mode_local_request_obj,
  // whose profile definition states it "only targets the control mode
  // (control_source, operation_mode and set_point are all ignored)". Addressed on
  // the wire as Obj 0x0A01 / Sub 0x5600 (the 0x0A high byte is sub-id 10), versus
  // the start/stop object 86 / sub-id 6 = overall_operation_local_request_obj
  // (Obj 0x0601), which DOES write set_point.
  //
  // The 12-byte payload is a type-303 OperationStatusRequest struct:
  //   [2F 01][00 00][07] = type 303 + 7-byte struct length, then the struct:
  //   [control_source=0x00 Undefined][operation_mode=0x06 NoCmd][control_mode=mode][set_point=NaN]
  // control_source/operation_mode/set_point are filled with no-op sentinels
  // (Undefined / NoCmd / NaN) so the pump changes ONLY the control mode, leaving
  // the run state and each mode's stored setpoint untouched. This is exactly how
  // the Grundfos GO app switches modes.
  //
  // Replaces the old set_mode path that wrote the start/stop object (0x0601) with
  // a default suffix, which durably overwrote the setpoint with ~3671 on a cold
  // cache (bench-confirmed). Bench-verified: switching into a cold-cache mode now
  // preserves the stored setpoint. Fixes #97 (all modes reachable) and #83 (no
  // clobber) together, and inherently avoids #45 (mode change is not start/stop).
  ControlModeMapping mapping;
  if (!get_class10_mapping(mode, mapping)) {
    mapping.mode_byte = 0x02;  // default CONSTANT_SPEED, matching send_control_request()
  }

  uint8_t payload[12];
  payload[0] = 0x2F;               // type 303 (0x012F, little-endian)
  payload[1] = 0x01;
  payload[2] = 0x00;
  payload[3] = 0x00;
  payload[4] = 0x07;               // 7-byte OperationStatusRequest struct follows
  payload[5] = 0x00;               // control_source = Undefined (ignored)
  payload[6] = 0x06;               // operation_mode = NoCmd (leave run state unchanged)
  payload[7] = mapping.mode_byte;  // control_mode = target mode (the only field applied)
  payload[8] = 0x7F;               // set_point = NaN -> keep the mode's stored setpoint
  payload[9] = 0xFF;
  payload[10] = 0xFF;
  payload[11] = 0xFF;

  // OpSpec 0x90 = SET + 16 bytes (4 IDs + 12 payload)
  uint8_t apdu[18];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x90;  // OpSpec: SET with length 16
  apdu[2] = 0x56;  // Sub ID high (SUB_CONTROL = 0x5600)
  apdu[3] = 0x00;  // Sub ID low
  apdu[4] = 0x0A;  // Obj ID high (0x0A = sub-id 10 -> overall_control_mode_local_request_obj)
  apdu[5] = 0x01;  // Obj ID low
  memcpy(&apdu[6], payload, 12);

  this->transport_.send_apdu_command(apdu, 18);

  // Intentionally NO configuration commit here. send_configuration_commit()
  // commits the ClockProgramOverview (the schedule), which is unrelated to the
  // control mode -- the old set_mode path only scheduled it as a side effect of
  // routing through send_control_request(). The mode change applies and persists
  // on its own: the Grundfos GO app sends this same frame with no commit after it
  // (BLE-capture confirmed), and on-device the new mode is reported back and
  // survives a reconnect without any commit. Adding one would just re-commit the
  // schedule on every mode switch.
  return true;
}

void ControlService::set_class10_setpoint(float value, uint16_t sub_id, uint16_t obj_id) {
  // Reference: control.py::_set_class10_setpoint() lines 1100-1132
  // OpSpec 0x84 = SET + 4 bytes
  // APDU: [0x0A][0x84][SubH][SubL][ObjH][ObjL][Float32BE]
  uint8_t apdu[10];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x84;  // OpSpec: SET + 4 bytes
  apdu[2] = (sub_id >> 8) & 0xFF;
  apdu[3] = sub_id & 0xFF;
  apdu[4] = (obj_id >> 8) & 0xFF;
  apdu[5] = obj_id & 0xFF;
  protocol::encode_float_be(value, &apdu[6]);

  this->transport_.send_apdu_command(apdu, 10);

  // Schedule configuration commit after setpoint write
  if (schedule_callback_) {
    schedule_callback_([this]() { this->send_configuration_commit(); }, 200);
  }
}

bool ControlService::get_class10_mapping(ControlMode mode, ControlModeMapping &mapping) {
  uint8_t mode_val = static_cast<uint8_t>(mode);
  
  // Check if mode is in valid range and has non-zero mode byte
  if (mode_val >= sizeof(CLASS10_CONTROL_MAP) / sizeof(CLASS10_CONTROL_MAP[0])) {
    return false;
  }
  
  const ControlModeMapping &map_entry = CLASS10_CONTROL_MAP[mode_val];
  
  // Check if mode byte is non-zero (indicates supported mode)
  if (map_entry.mode_byte == 0x00 && mode_val != 0) {
    return false;
  }
  
  mapping = map_entry;
  return true;
}

void ControlService::write_temp_range_config(float min_temp, float max_temp, bool autoadapt_enabled,
                                             std::function<void(bool)> on_ack) {
  // Build APDU manually to match exactly what Grundfos GO app sends
  // App sends: [Class 10] [OpSpec 0x97] [ObjID 91] [SubH 01] [SubL AE] [TypeH 03] [TypeL F4] [Reserved 02] [Size 00 00 0E] [Data...]
  uint8_t apdu[25];
  apdu[0] = 0x0A;     // Class 10
  apdu[1] = 0x97;     // OpSpec 0x97 = SET + 23 bytes (1 Obj + 2 Sub + 2 Type + 1 Res + 3 Size + 14 Data)
  apdu[2] = 0x5B;     // Obj-ID (91 = 0x005B, encoded as 1 byte in this specific packet type)
  apdu[3] = 0x01;     // Sub-ID high (430 = 0x01AE)
  apdu[4] = 0xAE;     // Sub-ID low
  apdu[5] = 0x03;     // Type Code High (1012 = 0x03F4)
  apdu[6] = 0xF4;     // Type Code Low
  apdu[7] = 0x02;     // Reserved
  apdu[8] = 0x00;     // Size High
  apdu[9] = 0x00;     // Size Mid
  apdu[10] = 0x0E;    // Size Low (14 bytes)

  // Payload (14 bytes)
  apdu[11] = autoadapt_enabled ? 0x01 : 0x00; // DeltaTempEnabled
  protocol::encode_float_be(min_temp, &apdu[12]);
  protocol::encode_float_be(max_temp, &apdu[16]);

  // Default time limits / constants
  apdu[20] = 0x00;
  apdu[21] = 0x00;
  apdu[22] = 0x00;
  apdu[23] = 0x16;
  apdu[24] = 0x00;

  // Log the full APDU for debugging
  ESP_LOGI(TAG, "write_temp_range APDU: %s", format_hex_pretty(apdu, 25).c_str());

  // Pass expect_obj_id=0, expect_sub_id=0 for response matching because
  // the pump responds with a short ACK (OpSpec 0x01) without Obj/Sub fields
  this->transport_.send_apdu_command(
      apdu, 25, 0, 0,
      [on_ack](bool success, const uint8_t * /*data*/, size_t /*len*/) {
        if (on_ack) on_ack(success);
      },
      3000, false, true); // 3000ms timeout, no register read, expect short ACK
}

void ControlService::write_cycle_config(uint8_t on_minutes, uint8_t off_minutes) {
  // Payload: [00 00][OFF_min][01 42 02][ON_min][FB]  (8 bytes)
  const uint8_t struct_payload[8] = {
    0x00, 0x00,           // Header
    off_minutes,          // Byte 2: OFF time
    0x01, 0x42, 0x02,     // Fixed magic bytes
    on_minutes,           // Byte 6: ON time
    0xFB                  // Fixed suffix
  };

  // APDU: [Class][OpSpec][ObjID][SubH][SubL][Reserved][Type_H][Type_L][Size][Payload(8)]
  // Object 91, Sub-ID 430 (0x01AE), Type 1012 (0x03F4)
  // Using build_data_object_set equivalent format
  uint8_t data[11];  // Type(2) + Size(1) + Payload(8)
  data[0] = 0x03;    // Type high: 1012 = 0x03F4
  data[1] = 0xF4;    // Type low
  data[2] = 0x08;    // Size: 8 bytes
  memcpy(&data[3], struct_payload, 8);

  // Build full APDU matching build_data_object_set format:
  // [0x0A][OpSpec][SubH][SubL][ObjH][ObjL][data...]
  // sub_id=0x01AE (430), obj_id=91 (0x005B)
  // standard_len = 1(svc) + 1(src) + 1(class) + 1(opspec) + 4(IDs) + len(data) = 8 + 11 = 19
  // op_bits = 19 - 4 = 15 => OpSpec = 0x80 | 15 = 0x8F
  uint8_t apdu[17];  // class(1) + opspec(1) + IDs(4) + data(11)
  apdu[0] = 0x0A;    // Class 10
  apdu[1] = 0x8F;    // OpSpec: SET + 15 bytes (4 IDs + 11 data)
  apdu[2] = 0x01;    // Sub-ID high (0x01AE = 430)
  apdu[3] = 0xAE;    // Sub-ID low
  apdu[4] = 0x00;    // Obj-ID high (91 = 0x005B)
  apdu[5] = 0x5B;    // Obj-ID low
  memcpy(&apdu[6], data, 11);

  this->transport_.send_apdu_command(apdu, 17);
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
