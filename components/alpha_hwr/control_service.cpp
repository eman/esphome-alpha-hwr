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

void ControlService::check_flow_setpoint_scale(float previous_setpoint, float new_setpoint, float raw_register_value) {
  // See issue #44: bench-verified on 2026-07-08 — Object 86/Sub 6 in Constant
  // Flow mode returns the exact same raw value (~0.000694 m³/h) regardless of
  // the actual commanded setpoint (tested 0.2/2.0/8.0 m³/h, all read back
  // identically). This register is confirmed NOT a reliable source of truth
  // for this mode's setpoint, so update_mode_from_notification()/get_mode_async()
  // no longer apply its value to cached_setpoint_ for CONSTANT_FLOW — this is
  // now purely a diagnostic log (kept in case a future pump/firmware revision
  // behaves differently) rather than something that gates a behavior change.
  if (std::isnan(previous_setpoint) || std::isnan(new_setpoint) || previous_setpoint == 0.0f) {
    return;
  }
  float ratio = new_setpoint / previous_setpoint;
  if (ratio > 10.0f || ratio < 0.1f) {
    ESP_LOGW(TAG,
      "Constant Flow setpoint readback changed by %.1fx (last known=%.4f m³/h, raw register=%.6f m³/h) — "
      "Object 86/Sub 6 is known-unreliable for this mode (see #44); ignoring and keeping last "
      "client-commanded value",
      ratio, previous_setpoint, raw_register_value);
  }
}

void ControlService::update_mode_from_notification(uint8_t mode, uint8_t operation_mode, float setpoint,
                                                   uint8_t control_source) {
  // Update internal state
  current_mode_ = static_cast<ControlMode>(mode);
  mode_valid_ = true;  // Mark mode as valid - we received it from the pump
  cached_operation_mode_ = operation_mode;
  
  // Cache setpoint from notification — write to the mode-specific field (issue #51).
  // Per-mode storage eliminates cross-mode contamination; no NAN-clearing needed.
  // Reference: control.py::get_mode() lines 428-434 (Pa → meters for pressure modes).
  if (current_mode_ == ControlMode::CONSTANT_PRESSURE) {
    cached_pressure_setpoint_ = setpoint / 9806.65f;
  } else if (current_mode_ == ControlMode::PROPORTIONAL_PRESSURE) {
    cached_proportional_setpoint_ = setpoint / 9806.65f;
  } else if (current_mode_ == ControlMode::CONSTANT_FLOW) {
    // Fix #44: Object 86/Sub 6 unreliable for CONSTANT_FLOW — do not update
    // cached_flow_setpoint_ here. Only client-side writes in set_constant_flow_async()
    // populate it. No cross-mode contamination possible (per-mode fields, issue #51).
    check_flow_setpoint_scale(cached_flow_setpoint_, setpoint, setpoint);
  } else if (current_mode_ == ControlMode::CONSTANT_SPEED) {
    cached_speed_setpoint_ = setpoint;
  }
  // Other modes (AUTO_ADAPT_*, DHW_ON_OFF, TEMPERATURE_RANGE, etc.) don't use
  // a scalar setpoint cache — temperature range uses cached_temp_min_/max_ instead.
  
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


void ControlService::read_setpoints_from_pump() {
  if (!mode_valid_) {
    ESP_LOGW(TAG, "Cannot read setpoints: mode not yet known");
    return;
  }
  
  ESP_LOGD(TAG, "Reading setpoints for mode: %s", get_mode_name(current_mode_));
  
  // For Temperature Range mode, read Object 91 Sub 430 for min/max temps + autoadapt
  if (current_mode_ == ControlMode::TEMPERATURE_RANGE) {
    ESP_LOGD(TAG, "Reading temperature range from Object 91 Sub 430...");
    
    uint8_t apdu[5];
    apdu[0] = 0x0A;  // Class 10
    apdu[1] = 0x03;  // OpSpec: READ
    apdu[2] = 91;    // Object 91
    apdu[3] = 0x01;  // Sub 430 high byte (430 = 0x01AE)
    apdu[4] = 0xAE;  // Sub 430 low byte
    
    // Use wildcard matching (0, 0) like Python reference's match_class10_response,
    // because the pump responds with OpSpec 0x15 whose frame layout doesn't follow
    // the standard [ObjH][ObjL][SubH][SubL] order that explicit matching expects.
    this->transport_.send_apdu_command(apdu, 5, 0, 0,
      [this](bool ok, const uint8_t* payload, size_t payload_len) {
        if (!ok || payload_len < 12) {
          ESP_LOGW(TAG, "Failed to read temp range (success=%d, len=%zu)", ok, payload_len);
          return;
        }
        
        // Skip 3-byte header [00 00 XX] if present
        int offset = 0;
        if (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) {
          offset = 3;
        }
        
        if (payload_len >= (size_t)(offset + 9)) {
          // [delta_enabled(1)][min_temp(4 float BE)][max_temp(4 float BE)]
          cached_autoadapt_ = payload[offset] ? 1 : 0;
          cached_temp_min_ = protocol::decode_float_be(&payload[offset + 1]);
          cached_temp_max_ = protocol::decode_float_be(&payload[offset + 5]);
          ESP_LOGI(TAG, "Temperature range: min=%.1f, max=%.1f, autoadapt=%s", 
                   cached_temp_min_, cached_temp_max_, cached_autoadapt_ ? "ON" : "OFF");
          
          // Trigger mode change callback to update UI
          if (mode_change_callback_) {
            mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
          }
        }
      }, 5000);
  } else {
    // For non-temperature modes, read Object 86 Sub 7 to get current state.
    // Sub 7 is the prioritized status object (after remote/local/alarm logic).
    // This is the same read Python's get_mode() uses.
    // Reference: control.py::get_mode() line 398
    // Use 3s delay to ensure stale passive notifications have been consumed
    ESP_LOGD(TAG, "Reading state via Object 86 Sub 7 (get_mode)...");
    ControlMode expected_mode = current_mode_;
    get_mode_async([this, expected_mode](bool success, ControlMode mode) {
      if (success) {
        // Check if pump returned a stale mode (not yet updated after set_mode)
        if (mode != expected_mode) {
          ESP_LOGW(TAG, "Pump returned stale mode %s (expected %s), retrying in 2s...",
                   get_mode_name(mode), get_mode_name(expected_mode));
          // Restore expected mode (don't let stale response overwrite our set_mode)
          current_mode_ = expected_mode;
          mode_valid_ = true;
          // NAN the expected mode's per-mode field to signal "not yet read"
          // (CONSTANT_FLOW excluded — client-write is the source of truth, issue #44)
          switch (expected_mode) {
            case ControlMode::CONSTANT_PRESSURE: cached_pressure_setpoint_ = NAN; break;
            case ControlMode::PROPORTIONAL_PRESSURE: cached_proportional_setpoint_ = NAN; break;
            case ControlMode::CONSTANT_SPEED: cached_speed_setpoint_ = NAN; break;
            default: break;
          }
          if (mode_change_callback_) {
            mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
          }
          // Retry after additional delay
          if (schedule_callback_) {
            schedule_callback_([this]() {
              get_mode_async([this](bool ok, ControlMode m) {
                if (ok) {
                  ESP_LOGD(TAG, "Retry setpoint read: %.4f (mode: %s)", get_setpoint_for_mode(m), get_mode_name(m));
                  if (mode_change_callback_) {
                    mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
                  }
                } else {
                  ESP_LOGW(TAG, "Retry setpoint read failed (timeout)");
                }
              });
            }, 5000);
          }
        } else {
          ESP_LOGI(TAG, "Setpoint read from pump: %.4f (mode: %s)", 
                   get_setpoint_for_mode(mode), get_mode_name(mode));
        }
      } else {
        ESP_LOGW(TAG, "Failed to read state via Object 86 Sub 7 (timeout)");
      }
    });
  }
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
              current_mode_ = static_cast<ControlMode>(control_mode_byte);
              mode_valid_ = true;
              cached_operation_mode_ = operation_mode;
              
              // Derive pump enabled state from operation_mode
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
              
              // Extract and cache setpoint into the mode-specific field (issue #51).
              // Per-mode storage eliminates cross-mode contamination.
              if (payload_len >= (size_t)(offset + 7)) {
                float raw_setpoint = protocol::decode_float_be(&payload[offset + 3]);
                
                if (current_mode_ == ControlMode::CONSTANT_PRESSURE) {
                  cached_pressure_setpoint_ = raw_setpoint / 9806.65f;
                } else if (current_mode_ == ControlMode::PROPORTIONAL_PRESSURE) {
                  cached_proportional_setpoint_ = raw_setpoint / 9806.65f;
                } else if (current_mode_ == ControlMode::CONSTANT_FLOW) {
                  // Fix #44: Object 86/Sub 6 unreliable for CONSTANT_FLOW — do not update
                  // cached_flow_setpoint_ here. Only client-side writes populate it.
                  // No cross-mode contamination possible (per-mode fields, issue #51).
                  check_flow_setpoint_scale(cached_flow_setpoint_, raw_setpoint, raw_setpoint);
                } else if (current_mode_ == ControlMode::CONSTANT_SPEED) {
                  cached_speed_setpoint_ = raw_setpoint;
                }
                // Other modes don't use a scalar setpoint cache.
              }
              
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

bool ControlService::start(uint8_t mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot start pump: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  ESP_LOGI(TAG, "Starting pump...");

  // Resolve target mode (255 = use current mode)
  // Reference: control.py::start() lines 183-206
  if (mode != 255) {
    current_mode_ = static_cast<ControlMode>(mode);
    // Per-mode fields (issue #51): no NAN-clearing needed — each mode has its
    // own storage, so a previous mode's value can never contaminate this mode.
  }
  
  ControlMode target = current_mode_;

  // Resolve the setpoint to send with the start command.
  //
  // Fix #43: send_control_request() falls back to CLASS10_CONTROL_MAP's
  // default suffix when no setpoint is given, which decodes to a hardcoded
  // ~3671.0 for CONSTANT_PRESSURE/PROPORTIONAL_PRESSURE/CONSTANT_SPEED/
  // CONSTANT_FLOW. Reuse the pump's actual last-known setpoint instead, so
  // enabling the pump doesn't clobber the user's configured setpoint.
  //
  // Issue #51: read from the mode-specific field — no cross-mode contamination
  // possible. DHW_ON_OFF, TEMPERATURE_RANGE, and AUTO_ADAPT_* keep using the
  // default suffix (unchanged behavior; those modes don't have a scalar setpoint cache).
  float start_setpoint = NAN;
  if (mode == 255) {
    switch (target) {
      case ControlMode::CONSTANT_PRESSURE:     start_setpoint = cached_pressure_setpoint_; break;
      case ControlMode::PROPORTIONAL_PRESSURE: start_setpoint = cached_proportional_setpoint_; break;
      case ControlMode::CONSTANT_SPEED:        start_setpoint = cached_speed_setpoint_; break;
      case ControlMode::CONSTANT_FLOW:         start_setpoint = cached_flow_setpoint_; break;
      default: break;
    }
    if (!std::isnan(start_setpoint)) {
      // Stored in display units (meters for pressure); convert to pump-native units (Pascals)
      if (target == ControlMode::CONSTANT_PRESSURE || target == ControlMode::PROPORTIONAL_PRESSURE) {
        start_setpoint *= 9806.65f;
      }
      ESP_LOGD(TAG, "Reusing cached setpoint on start: %.4f (raw units)", start_setpoint);
    } else {
      ESP_LOGD(TAG, "No cached setpoint to reuse on start; using mode default suffix");
    }
  }

  if (!send_control_request(target, true, start_setpoint)) {
    ESP_LOGE(TAG, "Failed to send start command");
    return false;
  }

  // Update mode state if a specific mode was requested
  if (mode != 255) {
    mode_valid_ = true;
    if (mode_change_callback_) {
      mode_change_callback_(current_mode_, 0, 0.0f);
    }
  }

  // Pump is now enabled (user started it)
  pump_enabled_ = true;
  pump_enabled_valid_ = true;

  // Schedule a post-command readback after ~500ms to ensure cached state is
  // synchronized with pump's actual state (fixes #52). The pump does not send
  // unsolicited notifications after start/stop commands, so the non-optimistic
  // Pump Enabled switch won't update from cache until the next telemetry poll
  // or a readback occurs. Reporter bench-tested ~500ms delay and confirmed it
  // works reliably.
  if (schedule_callback_) {
    schedule_callback_([this]() {
      ESP_LOGD(TAG, "Post-command readback after start (issue #52)");
      get_mode_async([](bool success, ControlMode /*mode*/) {
        if (!success) {
          ESP_LOGW(TAG, "Post-command readback failed after start");
        }
      });
    }, 500);
  }

  ESP_LOGI(TAG, "Pump start command sent (mode=%d)", static_cast<uint8_t>(target));
  return true;
}

bool ControlService::stop(uint8_t mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot stop pump: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  ESP_LOGI(TAG, "Stopping pump...");

  // Resolve target mode (255 = use current mode)
  // Reference: control.py::stop() lines 208-231
  ControlMode target = current_mode_;
  if (mode != 255) {
    target = static_cast<ControlMode>(mode);
  }
  
  if (!send_control_request(target, false)) {
    ESP_LOGE(TAG, "Failed to send stop command");
    return false;
  }

  // NOTE: Python reference does NOT update _current_mode in stop()
  // Pump is now disabled (user stopped it)
  pump_enabled_ = false;
  pump_enabled_valid_ = true;

  // Schedule a post-command readback after ~500ms to ensure cached state is
  // synchronized with pump's actual state (fixes #52). The pump does not send
  // unsolicited notifications after start/stop commands, so the non-optimistic
  // Pump Enabled switch won't update from cache until the next telemetry poll
  // or a readback occurs. Reporter bench-tested ~500ms delay and confirmed it
  // works reliably.
  if (schedule_callback_) {
    schedule_callback_([this]() {
      ESP_LOGD(TAG, "Post-command readback after stop (issue #52)");
      get_mode_async([](bool success, ControlMode /*mode*/) {
        if (!success) {
          ESP_LOGW(TAG, "Post-command readback failed after stop");
        }
      });
    }, 500);
  }

  ESP_LOGI(TAG, "Pump stop command sent (mode=%d)", static_cast<uint8_t>(target));
  return true;
}

bool ControlService::set_mode(ControlMode mode) {
  // Verify session is authenticated
  if (session_.get_state() != core::SessionState::READY) {
    ESP_LOGW(TAG, "Cannot set mode: session not ready (state=%d)", static_cast<int>(session_.get_state()));
    return false;
  }

  uint8_t mode_val = static_cast<uint8_t>(mode);
  ESP_LOGI(TAG, "Setting control mode to %d (%s)...", mode_val, get_mode_name(mode));

  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true" (start), so switching modes never implicitly
  // force-enables the pump while it's off.
  //
  // NOTE: with_resolved_enabled_state() usually resolves synchronously (when
  // pump_enabled_valid_ is already known), but can fall back to an async
  // get_mode_async() read-back. Since this function's bool return can't
  // reflect a result that isn't known yet, we return true optimistically
  // here (consistent with start()/stop(), which already return true after
  // just queueing a transport command) and log any failure from within the
  // callback instead.
  with_resolved_enabled_state([this, mode, mode_val](bool resolved, bool enabled) {
    if (!resolved) {
      // Enabled state genuinely unknown even after a read-back attempt --
      // abort rather than guessing (either guess risks force-enabling or
      // force-disabling the pump).
      ESP_LOGE(TAG, "Aborting mode change to %d: could not determine pump enabled state", mode_val);
      return;
    }
    // Always use send_control_request() which handles all modes via Class 10
    // (defaults to mode_byte 0x02 for modes not in CLASS10_CONTROL_MAP)
    // Reference: control.py::set_mode() lines 345-366
    if (!send_control_request(mode, enabled)) {
      ESP_LOGW(TAG, "Failed to send control request for mode %d", mode_val);
      return;
    }

    current_mode_ = mode;
    mode_valid_ = true;
    // Per-mode fields (issue #51): no NAN-clearing needed — each mode retains
    // its own cached value independently.

    if (mode_change_callback_) {
      mode_change_callback_(current_mode_, 0, 0.0f);
    }

    // Read setpoints for the new mode after delay (pump needs time to update passive notifications)
    if (schedule_callback_) {
      schedule_callback_([this]() { read_setpoints_from_pump(); }, 5000);
    }

    // Log what flag was sent, not an assertion about the resulting pump
    // state (the pump's actual state could differ if it changes elsewhere
    // between this send and any later read-back).
    ESP_LOGI(TAG, "Mode set to %s (sent enabled=%s)", get_mode_name(mode), enabled ? "true" : "false");
  });

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

   // Class 3: 03 81 06 (OpSpec 0x81 = SET; command ID 6 = disable remote / Auto).
   // Fix #46: was 0xC1 (INFO) -- see enable_remote_mode() for details.
   const uint8_t apdu[3] = {0x03, 0x81, 0x06};
   
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


bool ControlService::send_control_request(ControlMode mode, bool start, float setpoint) {
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
  payload[6] = start ? 0x00 : 0x01;  // 0=Start, 1=Stop
  payload[7] = mapping.mode_byte;

  if (!std::isnan(setpoint)) {
    // Encode setpoint as float32 BE in suffix position
    protocol::encode_float_be(setpoint, &payload[8]);
  } else {
    // Use default suffix bytes from mode map
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

  // Send command and schedule configuration commit
  this->transport_.send_apdu_command(apdu, 18);

  if (schedule_callback_) {
    schedule_callback_([this]() { this->send_configuration_commit(); }, 200);
  }

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

// ========== Setpoint Configuration Implementation ==========
// All setpoint methods follow the Python reference two-step pattern:
// 1. send_control_request(mode, setpoint=value) - Class 10 SET with float in suffix
// 2. set_class10_setpoint(value, sub_id) - Class 10 SET to specific sub-ID

void ControlService::set_constant_pressure_async(float value_m, std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting constant pressure to %.2f m...", value_m);
  
  // Validate setpoint (0.5m to 10.0m)
  // Reference: control.py::set_constant_pressure() lines 550-555
  if (value_m < 0.5f || value_m > 10.0f) {
    ESP_LOGE(TAG, "Setpoint %.2f m is outside valid range (0.5-10.0 m)", value_m);
    if (callback) callback(false);
    return;
  }
  
  // Convert meters to Pascals (Python reference line 558)
  float value_pa = value_m * 9806.65f;
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so writing a setpoint never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, value_pa, value_m, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting constant pressure setpoint write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Update overall operation request (Sub 6)
    if (!send_control_request(ControlMode::CONSTANT_PRESSURE, enabled, value_pa)) {
      ESP_LOGE(TAG, "Failed to send control request for constant pressure");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Update specific pressure setpoint (Sub 15)
    // Schedule after 400ms to allow control request + commit to complete
    if (schedule_callback_) {
      schedule_callback_([this, value_pa, value_m, callback]() {
        set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
        cached_pressure_setpoint_ = value_m;
        ESP_LOGI(TAG, "✓ Constant pressure set to %.2f m (%.0f Pa)", value_m, value_pa);
        if (callback) callback(true);
      }, 400);
    } else {
      set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
      cached_pressure_setpoint_ = value_m;
      if (callback) callback(true);
    }
  });
}

void ControlService::set_constant_speed_async(float value_rpm, std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting constant speed to %.0f RPM...", value_rpm);
  
  // Validate setpoint (500 to 4500 RPM)
  // Reference: control.py::set_constant_speed() lines 586-590
  if (value_rpm < 500.0f || value_rpm > 4500.0f) {
    ESP_LOGE(TAG, "Setpoint %.0f RPM is outside valid range (500-4500 RPM)", value_rpm);
    if (callback) callback(false);
    return;
  }
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so writing a setpoint never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, value_rpm, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting constant speed setpoint write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Update overall operation request (Sub 6)
    if (!send_control_request(ControlMode::CONSTANT_SPEED, enabled, value_rpm)) {
      ESP_LOGE(TAG, "Failed to send control request for constant speed");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Update specific speed setpoint (Sub 13)
    if (schedule_callback_) {
      schedule_callback_([this, value_rpm, callback]() {
        set_class10_setpoint(value_rpm, SUB_SPEED_SETPOINT);
        cached_speed_setpoint_ = value_rpm;
        ESP_LOGI(TAG, "✓ Constant speed set to %.0f RPM", value_rpm);
        if (callback) callback(true);
      }, 400);
    } else {
      set_class10_setpoint(value_rpm, SUB_SPEED_SETPOINT);
      cached_speed_setpoint_ = value_rpm;
      if (callback) callback(true);
    }
  });
}

void ControlService::set_constant_flow_async(float value_m3h, std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting constant flow to %.2f m³/h...", value_m3h);
  
  // Validate setpoint (0.1 to 10.0 m³/h)
  // Reference: control.py::set_constant_flow() lines 618-622
  if (value_m3h < 0.1f || value_m3h > 10.0f) {
    ESP_LOGE(TAG, "Setpoint %.2f m³/h is outside valid range (0.1-10.0 m³/h)", value_m3h);
    if (callback) callback(false);
    return;
  }
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so writing a setpoint never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, value_m3h, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting constant flow setpoint write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Update overall operation request (Sub 6)
    if (!send_control_request(ControlMode::CONSTANT_FLOW, enabled, value_m3h)) {
      ESP_LOGE(TAG, "Failed to send control request for constant flow");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Update specific flow setpoint (Sub 39)
    if (schedule_callback_) {
      schedule_callback_([this, value_m3h, callback]() {
        set_class10_setpoint(value_m3h, SUB_FLOW_SETPOINT);
        cached_flow_setpoint_ = value_m3h;
        ESP_LOGI(TAG, "✓ Constant flow set to %.2f m³/h", value_m3h);
        if (callback) callback(true);
      }, 400);
    } else {
      set_class10_setpoint(value_m3h, SUB_FLOW_SETPOINT);
      cached_flow_setpoint_ = value_m3h;
      if (callback) callback(true);
    }
  });
}

void ControlService::set_temperature_range_async(float min_temp, float max_temp, bool autoadapt_enabled,
                                                  std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting Temperature Range Control: %.1f°C - %.1f°C (AutoAdapt: %s)...",
           min_temp, max_temp, autoadapt_enabled ? "ON" : "OFF");
  
  // Validate temperature range (20°C to 70°C)
  if (min_temp < 20.0f || min_temp > 70.0f || max_temp < 20.0f || max_temp > 70.0f) {
    ESP_LOGE(TAG, "Temperature range (%.1f-%.1f°C) is outside valid range (20-70°C)", min_temp, max_temp);
    if (callback) callback(false);
    return;
  }
  
  if (min_temp >= max_temp) {
    ESP_LOGE(TAG, "Min temp (%.1f°C) must be less than max temp (%.1f°C)", min_temp, max_temp);
    if (callback) callback(false);
    return;
  }
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so writing a setpoint never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, min_temp, max_temp, autoadapt_enabled, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting temperature range write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Switch mode and set baseline (Sub 6) with min_temp as setpoint
    // Reference: control.py::set_temperature_range_control() line 924
    if (!send_control_request(ControlMode::TEMPERATURE_RANGE, enabled, min_temp)) {
      ESP_LOGE(TAG, "Failed to switch to TEMPERATURE_RANGE mode");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Write temperature range to Object 91, Sub-ID 430
    // Reference: control.py::set_temperature_range_control() lines 930-955
    // Payload format (Type 1012): [DeltaTempEnabled(1)][MinTemp(4)][MaxTemp(4)][TimeLimits(4)]
    auto write_temp_range = [this, min_temp, max_temp, autoadapt_enabled, callback]() {
      uint8_t struct_data[13];
      struct_data[0] = autoadapt_enabled ? 0x01 : 0x00;
      protocol::encode_float_be(min_temp, &struct_data[1]);
      protocol::encode_float_be(max_temp, &struct_data[5]);
      struct_data[9] = 0x05;
      struct_data[10] = 0x3C;
      struct_data[11] = 0x01;
      struct_data[12] = 0x1E;
      
      // APDU: [Class][OpSpec][ObjID][SubH][SubL][Reserved][Type(3)][Size(2)][Data(13)]
      uint8_t apdu[24];
      apdu[0] = 0x0A;     // Class 10
      apdu[1] = 0xB3;     // OpSpec 0xB3
      apdu[2] = 91;       // Object 91
      apdu[3] = 0x01;     // Sub-ID high (0x01AE = 430)
      apdu[4] = 0xAE;     // Sub-ID low
      apdu[5] = 0x00;     // Reserved
      apdu[6] = 0xF4;     // Type 1012 byte 1
      apdu[7] = 0x03;     // Type 1012 byte 2
      apdu[8] = 0x00;     // Type 1012 byte 3
      apdu[9] = 0x00;     // Size high byte
      apdu[10] = 0x0D;    // Size low byte (13 bytes)
      memcpy(&apdu[11], struct_data, 13);
      
      this->transport_.send_apdu_command(apdu, 24);
      
      // Cache temperature range values (no per-mode scalar setpoint for TEMPERATURE_RANGE;
      // cached_temp_min_/max_ serve that role — issue #51)
      cached_temp_min_ = min_temp;
      cached_temp_max_ = max_temp;
      cached_autoadapt_ = autoadapt_enabled ? 1 : 0;
      
      // Send configuration commit (transport queue handles ordering)
      this->send_configuration_commit();
      if (callback) callback(true);
    };
    
    ESP_LOGI(TAG, "Temperature range write queued: %.1f-%.1f°C (AutoAdapt: %s)",
             min_temp, max_temp, autoadapt_enabled ? "ON" : "OFF");
    
    // Schedule step 2 after step 1 completes
    if (schedule_callback_) {
      schedule_callback_(write_temp_range, 400);
    } else {
      write_temp_range();
    }
  });
}

void ControlService::set_proportional_pressure_async(float value_m, std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting proportional pressure to %.2f m...", value_m);
  
  // Validate setpoint (0.5 to 10.0 m)
  // Reference: control.py::set_proportional_pressure() lines 649-654
  if (value_m < 0.5f || value_m > 10.0f) {
    ESP_LOGE(TAG, "Setpoint %.2f m is outside valid range (0.5-10.0 m)", value_m);
    if (callback) callback(false);
    return;
  }
  
  // Convert meters to Pascals (same conversion as constant_pressure)
  // Reference: control.py::set_proportional_pressure() line 657
  float value_pa = value_m * 9806.65f;
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so writing a setpoint never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, value_pa, value_m, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting proportional pressure setpoint write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Update overall operation request (Sub 6) with Pa value
    if (!send_control_request(ControlMode::PROPORTIONAL_PRESSURE, enabled, value_pa)) {
      ESP_LOGE(TAG, "Failed to send control request for proportional pressure");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Update specific pressure setpoint (Sub 15)
    // Reference: control.py::set_proportional_pressure() lines 665-667
    if (schedule_callback_) {
      schedule_callback_([this, value_pa, value_m, callback]() {
        set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
        cached_proportional_setpoint_ = value_m;
        ESP_LOGI(TAG, "✓ Proportional pressure set to %.2f m (%.0f Pa)", value_pa / 9806.65f, value_pa);
        if (callback) callback(true);
      }, 400);
    } else {
      set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
      cached_proportional_setpoint_ = value_m;
      if (callback) callback(true);
    }
  });
}

void ControlService::set_cycle_time_control_async(uint8_t on_minutes, uint8_t off_minutes,
                                                    std::function<void(bool)> callback) {
  ESP_LOGI(TAG, "Setting Cycle Time Control: %d min on, %d min off...", on_minutes, off_minutes);
  
  // Validate ranges (1-60 minutes)
  // Reference: control.py::set_cycle_time_control() lines 1001-1003
  if (on_minutes < 1 || on_minutes > 60 || off_minutes < 1 || off_minutes > 60) {
    ESP_LOGE(TAG, "Cycle times must be between 1 and 60 minutes (got on=%d, off=%d)", on_minutes, off_minutes);
    if (callback) callback(false);
    return;
  }
  
  // Fix #45: send the pump's actual current enabled state instead of
  // hardcoding "true", so switching mode never implicitly force-enables
  // the pump while it's off.
  with_resolved_enabled_state([this, on_minutes, off_minutes, callback](bool resolved, bool enabled) {
    if (!resolved) {
      ESP_LOGE(TAG, "Aborting cycle time control write: could not determine pump enabled state");
      if (callback) callback(false);
      return;
    }
    // Step 1: Switch mode via send_control_request(DHW_ON_OFF)
    // Reference: control.py::set_cycle_time_control() lines 1006-1007
    if (!send_control_request(ControlMode::DHW_ON_OFF, enabled)) {
      ESP_LOGE(TAG, "Failed to switch to DHW_ON_OFF mode");
      if (callback) callback(false);
      return;
    }
    
    // Step 2: Write cycle time configuration to Object 91, Sub-ID 430
    // Reference: control.py::set_cycle_time_control() lines 1010-1055
    auto write_cycle_config = [this, on_minutes, off_minutes, callback]() {
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
      this->send_configuration_commit();
      
      ESP_LOGI(TAG, "✓ Cycle time set: %d min ON, %d min OFF", on_minutes, off_minutes);
      if (callback) callback(true);
    };
    
    // Schedule step 2 after step 1 completes
    if (schedule_callback_) {
      schedule_callback_(write_cycle_config, 400);
    } else {
      write_cycle_config();
    }
  });
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
