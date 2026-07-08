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

void ControlService::update_mode_from_notification(uint8_t mode, uint8_t operation_mode, float setpoint) {
  // Update internal state
  current_mode_ = static_cast<ControlMode>(mode);
  mode_valid_ = true;  // Mark mode as valid - we received it from the pump
  cached_operation_mode_ = operation_mode;
  
  float previous_setpoint = cached_setpoint_;
  
  // Cache setpoint from notification — this is the same data Python's get_mode() reads
  // from Object 86 Sub 6. Apply unit conversion for pressure modes (Pa → meters).
  // Reference: control.py::get_mode() lines 428-434
  if (current_mode_ == ControlMode::CONSTANT_PRESSURE ||
      current_mode_ == ControlMode::PROPORTIONAL_PRESSURE) {
    cached_setpoint_ = setpoint / 9806.65f;
  } else if (current_mode_ == ControlMode::CONSTANT_FLOW) {
    // Fix #44: this register doesn't reliably carry the flow setpoint (see
    // check_flow_setpoint_scale() above), so don't let it clobber the last
    // known-good, client-commanded value. cached_setpoint_ is left as-is
    // (NAN until the user actually calls set_constant_flow_async(), which
    // sets it optimistically).
    check_flow_setpoint_scale(previous_setpoint, setpoint, setpoint);
  } else {
    cached_setpoint_ = setpoint;
  }
  
  // Derive pump enabled state from operation_mode:
  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled
  pump_enabled_ = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));
  pump_enabled_valid_ = true;
  
  ESP_LOGI(TAG, "Control mode from notification: %s (op_mode=%d, setpoint=%.4f, raw=%.4f, enabled=%s)",
           get_mode_name(current_mode_), operation_mode, cached_setpoint_, setpoint,
           pump_enabled_ ? "YES" : "NO");
  
  // Notify callback if set
  if (mode_change_callback_) {
    mode_change_callback_(current_mode_, operation_mode, cached_setpoint_);
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
            mode_change_callback_(current_mode_, cached_operation_mode_, cached_setpoint_);
          }
        }
      }, 5000);
  } else {
    // For non-temperature modes, read Object 86 Sub 6 to get current setpoint.
    // This is the same read Python's get_mode() uses.
    // Reference: control.py::get_mode() line 398
    // Use 3s delay to ensure stale passive notifications have been consumed
    ESP_LOGD(TAG, "Reading setpoint via Object 86 Sub 6 (get_mode)...");
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
          cached_setpoint_ = NAN;
          if (mode_change_callback_) {
            mode_change_callback_(current_mode_, cached_operation_mode_, cached_setpoint_);
          }
          // Retry after additional delay
          if (schedule_callback_) {
            schedule_callback_([this]() {
              get_mode_async([this](bool ok, ControlMode m) {
                if (ok) {
                  ESP_LOGD(TAG, "Retry setpoint read: %.4f (mode: %s)", cached_setpoint_, get_mode_name(m));
                  if (mode_change_callback_) {
                    mode_change_callback_(current_mode_, cached_operation_mode_, cached_setpoint_);
                  }
                } else {
                  ESP_LOGW(TAG, "Retry setpoint read failed (timeout)");
                }
              });
            }, 5000);
          }
        } else {
          ESP_LOGI(TAG, "Setpoint read from pump: %.4f (mode: %s)", 
                   cached_setpoint_, get_mode_name(mode));
        }
      } else {
        ESP_LOGW(TAG, "Failed to read setpoint via Object 86 Sub 6 (timeout)");
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

  ESP_LOGD(TAG, "Reading current control mode from pump (Object 86, SubID 6)...");

   // Build Class 10 READ request: [0x0A][0x03][Obj][Sub-H][Sub-L]
   // Object 86 (0x56), SubID 6 (0x0006)
   // Reference: control.py::_read_class10_object() lines 76-85
   //
   // IMPORTANT: The pump responds with a PASSIVE NOTIFICATION (OpSpec 0x0E), not a direct response!
   // The Python reference accepts ANY Class 10 packet as the response (see base.py::match_class10_response).
   // We need to do the same here - accept any Class 10 packet, not just exact Object/Sub ID matches.
   //
   // Response format: [00 00 XX][control_source][operation_mode][control_mode][setpoint(4 bytes)]
   uint8_t apdu[5];
   apdu[0] = 0x0A;  // Class 10
   apdu[1] = 0x03;  // OpSpec: READ (INFO)
   apdu[2] = 0x56;  // Object 86 (1 byte!)
   apdu[3] = 0x00;  // SubID 6 high byte
   apdu[4] = 0x06;  // SubID 6 low byte
   
  // Send with response matching for Object 86 Sub 6 response
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
              
              // Extract and cache setpoint (big-endian float at offset+3)
              if (payload_len >= (size_t)(offset + 7)) {
                float previous_setpoint = cached_setpoint_;
                float raw_setpoint = protocol::decode_float_be(&payload[offset + 3]);
                
                if (current_mode_ == ControlMode::CONSTANT_PRESSURE || 
                    current_mode_ == ControlMode::PROPORTIONAL_PRESSURE) {
                  // Convert pressure setpoints from Pascals to meters
                  cached_setpoint_ = raw_setpoint / 9806.65f;
                } else if (current_mode_ == ControlMode::CONSTANT_FLOW) {
                  // Fix #44: Object 86/Sub 6 doesn't reliably carry the flow
                  // setpoint (see check_flow_setpoint_scale()); keep the last
                  // known-good, client-commanded value instead of this readback.
                  check_flow_setpoint_scale(previous_setpoint, raw_setpoint, raw_setpoint);
                } else {
                  cached_setpoint_ = raw_setpoint;
                }
              }
              
              ESP_LOGI(TAG, "Control mode updated to %d (%s), setpoint=%.2f", 
                control_mode_byte, get_mode_name(current_mode_), cached_setpoint_);
              
              // Notify mode change
              if (mode_change_callback_) {
                mode_change_callback_(current_mode_, operation_mode, cached_setpoint_);
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
  }
  
  ControlMode target = current_mode_;
  
  if (!send_control_request(target, true)) {
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

  // Always use send_control_request() which handles all modes via Class 10
  // (defaults to mode_byte 0x02 for modes not in CLASS10_CONTROL_MAP)
  // Reference: control.py::set_mode() lines 345-366
  if (!send_control_request(mode, true)) {
    ESP_LOGW(TAG, "Failed to send control request for mode %d", mode_val);
    return false;
  }

  current_mode_ = mode;
  mode_valid_ = true;
  // Clear stale setpoint from previous mode
  cached_setpoint_ = NAN;

  if (mode_change_callback_) {
    mode_change_callback_(current_mode_, 0, 0.0f);
  }

  // Read setpoints for the new mode after delay (pump needs time to update passive notifications)
  if (schedule_callback_) {
    schedule_callback_([this]() { read_setpoints_from_pump(); }, 5000);
  }

  ESP_LOGI(TAG, "Mode set to %s", get_mode_name(mode));
  return true;
}

bool ControlService::enable_remote_mode() {
   // Verify session is authenticated
   if (session_.get_state() != core::SessionState::READY) {
     ESP_LOGW(TAG, "Cannot enable remote mode: session not ready");
     return false;
   }

   ESP_LOGI(TAG, "Enabling remote mode...");

   // Class 3: 03 C1 07
   // Reference: control.py lines 329-332
   const uint8_t apdu[3] = {0x03, 0xC1, 0x07};
   
   // Send command via transport queue
   this->transport_.send_apdu_command(apdu, 3);
   
   // Update state
   this->is_remote_mode_enabled_ = true;
   ESP_LOGI(TAG, "Remote mode enabled");
   return true;
 }
 
 bool ControlService::disable_remote_mode() {
   // Verify session is authenticated
   if (session_.get_state() != core::SessionState::READY) {
     ESP_LOGW(TAG, "Cannot disable remote mode: session not ready");
     return false;
   }

   ESP_LOGI(TAG, "Disabling remote mode (Auto)...");

   // Class 3: 03 C1 06
   // Reference: control.py lines 358-361
   const uint8_t apdu[3] = {0x03, 0xC1, 0x06};
   
   // Send command via transport queue
   this->transport_.send_apdu_command(apdu, 3);
   
   // Update state
   this->is_remote_mode_enabled_ = false;
   ESP_LOGI(TAG, "Remote mode disabled (Auto)");
   return true;
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
  
  // Step 1: Update overall operation request (Sub 6)
  if (!send_control_request(ControlMode::CONSTANT_PRESSURE, true, value_pa)) {
    ESP_LOGE(TAG, "Failed to send control request for constant pressure");
    if (callback) callback(false);
    return;
  }
  
  // Step 2: Update specific pressure setpoint (Sub 15)
  // Schedule after 400ms to allow control request + commit to complete
  if (schedule_callback_) {
    schedule_callback_([this, value_pa, value_m, callback]() {
      set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
      cached_setpoint_ = value_m;
      ESP_LOGI(TAG, "✓ Constant pressure set to %.2f m (%.0f Pa)", value_m, value_pa);
      if (callback) callback(true);
    }, 400);
  } else {
    set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
    cached_setpoint_ = value_m;
    if (callback) callback(true);
  }
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
  
  // Step 1: Update overall operation request (Sub 6)
  if (!send_control_request(ControlMode::CONSTANT_SPEED, true, value_rpm)) {
    ESP_LOGE(TAG, "Failed to send control request for constant speed");
    if (callback) callback(false);
    return;
  }
  
  // Step 2: Update specific speed setpoint (Sub 13)
  if (schedule_callback_) {
    schedule_callback_([this, value_rpm, callback]() {
      set_class10_setpoint(value_rpm, SUB_SPEED_SETPOINT);
      cached_setpoint_ = value_rpm;
      ESP_LOGI(TAG, "✓ Constant speed set to %.0f RPM", value_rpm);
      if (callback) callback(true);
    }, 400);
  } else {
    set_class10_setpoint(value_rpm, SUB_SPEED_SETPOINT);
    cached_setpoint_ = value_rpm;
    if (callback) callback(true);
  }
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
  
  // Step 1: Update overall operation request (Sub 6)
  if (!send_control_request(ControlMode::CONSTANT_FLOW, true, value_m3h)) {
    ESP_LOGE(TAG, "Failed to send control request for constant flow");
    if (callback) callback(false);
    return;
  }
  
  // Step 2: Update specific flow setpoint (Sub 39)
  if (schedule_callback_) {
    schedule_callback_([this, value_m3h, callback]() {
      set_class10_setpoint(value_m3h, SUB_FLOW_SETPOINT);
      cached_setpoint_ = value_m3h;
      ESP_LOGI(TAG, "✓ Constant flow set to %.2f m³/h", value_m3h);
      if (callback) callback(true);
    }, 400);
  } else {
    set_class10_setpoint(value_m3h, SUB_FLOW_SETPOINT);
    cached_setpoint_ = value_m3h;
    if (callback) callback(true);
  }
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
  
  // Step 1: Switch mode and set baseline (Sub 6) with min_temp as setpoint
  // Reference: control.py::set_temperature_range_control() line 924
  if (!send_control_request(ControlMode::TEMPERATURE_RANGE, true, min_temp)) {
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
    
    // Cache temperature range values
    cached_temp_min_ = min_temp;
    cached_temp_max_ = max_temp;
    cached_setpoint_ = min_temp;
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
  
  // Step 1: Update overall operation request (Sub 6) with Pa value
  if (!send_control_request(ControlMode::PROPORTIONAL_PRESSURE, true, value_pa)) {
    ESP_LOGE(TAG, "Failed to send control request for proportional pressure");
    if (callback) callback(false);
    return;
  }
  
  // Step 2: Update specific pressure setpoint (Sub 15)
  // Reference: control.py::set_proportional_pressure() lines 665-667
  if (schedule_callback_) {
    schedule_callback_([this, value_pa, value_m, callback]() {
      set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
      cached_setpoint_ = value_m;
      ESP_LOGI(TAG, "✓ Proportional pressure set to %.2f m (%.0f Pa)", value_pa / 9806.65f, value_pa);
      if (callback) callback(true);
    }, 400);
  } else {
    set_class10_setpoint(value_pa, SUB_PRESSURE_SETPOINT);
    cached_setpoint_ = value_m;
    if (callback) callback(true);
  }
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
  
  // Step 1: Switch mode via send_control_request(DHW_ON_OFF)
  // Reference: control.py::set_cycle_time_control() lines 1006-1007
  if (!send_control_request(ControlMode::DHW_ON_OFF, true)) {
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
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
