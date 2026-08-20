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
    remote_state_valid_ = true;
    remote_source_observations_++;
    ESP_LOGD(TAG, "Remote mode: pump reports control_source=2 (Remote/Digital) → enabled");
  } else if (control_source == 1) {
    is_remote_mode_enabled_ = false;
    remote_state_valid_ = true;
    remote_source_observations_++;
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

    // Now read Obj 91 Sub 430 for Temp Range and AutoAdapt (the live cycle
    // times come from Sub 421 below -- issue #106)
    read_obj91_config([this, callback](bool ok) {
      if (!ok) {
        if (callback) callback(false);
        return;
      }
      // Chain the DHW on/off configuration read (Obj 91 Sub 421). Its result
      // does not gate readiness (same rationale as issue #94: cycle times are
      // not required fields), so the sync verdict ignores its success flag.
      read_dhw_config([this, callback](bool /*dhw_ok*/) {
        if (callback) {
          bool valid = is_cache_valid();
          if (!valid) {
            ESP_LOGW(TAG, "Cache sync completed but required fields are missing (autoadapt=%d, temp_min=%.1f, temp_max=%.1f)",
                     cached_autoadapt_, cached_temp_min_, cached_temp_max_);
          }
          callback(valid);
        }
        // The per-mode setpoint ranges (issue #273), AFTER the verdict rather
        // than before it. They are four more reads and they gate nothing: a
        // pump that will not answer leaves the write layer on its fallback
        // bounds, which is a worse validation but not a broken component.
        // Putting them ahead of the callback would have added their timeouts to
        // the time-to-ready for no gain -- and on a pump that answers, they
        // land a few hundred milliseconds later anyway.
        read_setpoint_ranges(nullptr);
      });
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

        // The trailing bytes of this struct are the pump's min/max on/off-time
        // LIMITS (+ version tail), NOT the live cycle times the old parser
        // read from here (issue #106; the live values are Obj 91 Sub 421, see
        // read_dhw_config()). Keep them verbatim so write_temp_range_config()
        // can echo them back instead of zeroing the pump's limits.
        if (payload_len >= (size_t)(offset + 14)) {
          memcpy(cached_temp_limits_tail_, &payload[offset + 9], 5);
          temp_limits_tail_valid_ = true;
        }
      }

      if (mode_change_callback_) {
        mode_change_callback_(current_mode_, cached_operation_mode_, get_setpoint_for_mode(current_mode_));
      }

      if (callback) callback(true);
    }, 5000);
}

void ControlService::read_dhw_config(std::function<void(bool)> callback) {
  // Obj 91 Sub 421 (dhw_on_off_control_configuration_obj, type 985):
  // [setpoint f32 BE][on_period u8][off_period u8]. The pump replies with
  // OpSpec 0x0D and a [00 00 06] size header before the struct
  // (capture-verified; matched via the transport's Obj-91 workaround).
  uint8_t apdu[5] = {0x0A, 0x03, 91, 0x01, 0xA5};
  this->transport_.send_apdu_command(apdu, 5, 91, 421,
    [this, callback](bool ok, const uint8_t* payload, size_t payload_len) {
      if (!ok || payload_len < 9) {
        ESP_LOGW(TAG, "Failed to read DHW config (success=%d, len=%zu)", ok, payload_len);
        if (callback) callback(false);
        return;
      }

      int offset = (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) ? 3 : 0;
      if (payload_len < (size_t)(offset + 6)) {
        ESP_LOGW(TAG, "DHW config payload too short (len=%zu)", payload_len);
        if (callback) callback(false);
        return;
      }

      memcpy(cached_dhw_setpoint_raw_, &payload[offset], 4);
      dhw_config_valid_ = true;
      cached_cycle_time_on_ = parse_cycle_time_minutes(payload[offset + 4]);
      cached_cycle_time_off_ = parse_cycle_time_minutes(payload[offset + 5]);

      ESP_LOGD(TAG, "DHW config synced: on=%d min, off=%d min (setpoint %.3g native)",
               cached_cycle_time_on_, cached_cycle_time_off_,
               protocol::decode_float_be(cached_dhw_setpoint_raw_));

      if (callback) callback(true);
    }, 5000);
}

namespace {
/// Pump-native -> display units, matching cache_setpoint_for_mode().
float native_to_display(ControlMode mode, float native) {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:
    case ControlMode::PROPORTIONAL_PRESSURE:
      return native / 9806.65f;  // Pascals -> meters
    case ControlMode::CONSTANT_FLOW:
      return native * 3600.0f;   // m³/s -> m³/h
    default:
      return native;             // RPM is native
  }
}
}  // namespace

bool ControlService::get_setpoint_range(ControlMode mode, float &min_out, float &max_out) const {
  const SetpointRange *r = nullptr;
  switch (mode) {
    case ControlMode::CONSTANT_SPEED:        r = &cs_range_; break;
    case ControlMode::CONSTANT_PRESSURE:     r = &cp_range_; break;
    case ControlMode::PROPORTIONAL_PRESSURE: r = &pp_range_; break;
    case ControlMode::CONSTANT_FLOW:         r = &cf_range_; break;
    default: return false;  // no scalar setpoint, so no range
  }
  // Hoisted, and spelled without a '||', so mutation_check.sh has a pipe-free
  // line to anchor to -- its entries are split on that character.
  const bool usable = !std::isnan(r->min) && !std::isnan(r->max) && r->max > r->min;
  if (!usable) return false;
  min_out = r->min;
  max_out = r->max;
  return true;
}

void ControlService::read_one_setpoint_range_(ControlMode mode, uint16_t sub,
                                              std::function<void(bool)> callback) {
  uint8_t apdu[5] = {0x0A, 0x03, 86, static_cast<uint8_t>(sub >> 8),
                     static_cast<uint8_t>(sub & 0xFF)};
  // Type 301 version 1: reply header bytes 6-9 are `00 01 2D 01`, so TypeH is
  // 0x0001 and (TypeL << 8) | Version is 0x2D01. Real type expectations, unlike
  // the Obj 91 reads -- so this takes the ordinary exact-match path and needs
  // none of the workaround in transport.cpp.
  this->transport_.send_apdu_command(
      apdu, 5, 0x2D01, 0x0001,
      [this, mode, sub, callback](bool ok, const uint8_t *payload, size_t payload_len) {
        if (!ok) {
          ESP_LOGW(TAG, "Setpoint range read failed for Obj 86 Sub %u", static_cast<unsigned>(sub));
          if (callback) callback(false);
          return;
        }
        const int offset =
            (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) ? 3 : 0;
        // default_set_point, min_set_point, max_set_point: three floats in.
        if (payload_len < static_cast<size_t>(offset + 12)) {
          ESP_LOGW(TAG, "Setpoint range reply too short for Sub %u (len=%zu)",
                   static_cast<unsigned>(sub), payload_len);
          if (callback) callback(false);
          return;
        }
        const float lo = native_to_display(mode, protocol::decode_float_be(&payload[offset + 4]));
        const float hi = native_to_display(mode, protocol::decode_float_be(&payload[offset + 8]));
        // A pump that answers with a degenerate range is not a pump to validate
        // against. Refusing it here is what keeps setpoint_ranges_known() from
        // claiming a complete set; get_setpoint_range() re-checks max > min on
        // every call, so a cached one would fall back rather than bound
        // anything, and the two together are belt and braces.
        const bool usable = !std::isnan(lo) && !std::isnan(hi) && hi > lo;
        if (!usable) {
          ESP_LOGW(TAG, "Setpoint range for Sub %u is not usable (%.4f-%.4f)",
                   static_cast<unsigned>(sub), lo, hi);
          if (callback) callback(false);
          return;
        }
        SetpointRange r{lo, hi};
        switch (mode) {
          case ControlMode::CONSTANT_SPEED:        cs_range_ = r; break;
          case ControlMode::CONSTANT_PRESSURE:     cp_range_ = r; break;
          case ControlMode::PROPORTIONAL_PRESSURE: pp_range_ = r; break;
          case ControlMode::CONSTANT_FLOW:         cf_range_ = r; break;
          default: break;
        }
        ESP_LOGD(TAG, "Setpoint range for %s: %.4f - %.4f", get_mode_name(mode), lo, hi);
        if (callback) callback(true);
      },
      // Shorter than the 5 s config reads. Nothing waits on these, and four of
      // them chained is four timeouts on a pump that does not answer them.
      3000);
}

void ControlService::read_setpoint_ranges(std::function<void(bool)> callback) {
  // Four sequential reads, and the chain STOPS at the first failure. That is
  // not tidiness, it is the only safe order.
  //
  // All four objects are type 301 version 1, so all four reads declare the same
  // expectation and the transport cannot tell their replies apart -- it matches
  // on object TYPE, never on the instance, and says so at its parse site. A
  // chain that carried on after a timeout would therefore hand read N's late
  // reply to read N+1, shifting every remaining range by one slot: constant
  // pressure would end up bounded by constant speed's 1650-3671 read as
  // Pascals, i.e. 0.168-0.374 m, and an ordinary 1.5 m setpoint would be
  // refused as INVALID with a confident and false attribution to the pump. It
  // would persist for the whole connection, because nothing re-reads these.
  //
  // Stopping means a mode after the failure simply keeps its fallback bounds,
  // which is the honest answer and the one get_setpoint_range() is built for.
  //
  // Abandonment on disconnect is inherited rather than implemented: Transport's
  // reset() clears the command queue WITHOUT invoking callbacks, so a link drop
  // mid-chain kills the chain outright and no reply from the old connection can
  // reach this cache. That is a dependency worth naming -- if reset() ever
  // starts failing pending callbacks instead of dropping them, this chain would
  // run its remaining reads against the next connection and would need a
  // generation counter like the one in alpha_hwr.cpp.
  if (setpoint_ranges_reading_) {
    ESP_LOGD(TAG, "Setpoint range read already in flight; not starting a second");
    if (callback) callback(false);
    return;
  }
  setpoint_ranges_reading_ = true;
  auto finish = [this, callback](bool complete) {
    setpoint_ranges_reading_ = false;
    setpoint_ranges_valid_ = complete;
    if (!complete) {
      ESP_LOGW(TAG, "Setpoint ranges incomplete; the modes that did not answer "
                    "keep their fallback bounds until the next connection");
    }
    if (callback) callback(complete);
  };
  read_one_setpoint_range_(ControlMode::CONSTANT_SPEED, 13, [this, finish](bool a) {
    if (!a) { finish(false); return; }
    read_one_setpoint_range_(ControlMode::CONSTANT_PRESSURE, 15, [this, finish](bool b) {
      if (!b) { finish(false); return; }
      read_one_setpoint_range_(ControlMode::PROPORTIONAL_PRESSURE, 17, [this, finish](bool c) {
        if (!c) { finish(false); return; }
        read_one_setpoint_range_(ControlMode::CONSTANT_FLOW, 39, finish);
      });
    });
  });
}

bool ControlService::get_mode_async(std::function<void(bool, ControlMode)> on_complete) {
  // Verify the session is READY. Note what that does and does not mean: it is
  // reached on a timer, with no frame having been exchanged, so this rejects a
  // session that has not got that far and promises nothing about the pump.
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
   
  // Response matching for the Object 86 Sub 7 read.
  //
  // These two arguments used to be passed the other way round, and the whole
  // read only ever matched through Transport's "BACKUP MATCH" fallback as a
  // result -- the primary comparison never succeeded. The fallback has been
  // removed, so the order matters now.
  //
  // The pump's reply carries [00][TypeH][TypeL][Version] at bytes 6-9: Type 303
  // version 1, the mode/control object. Transport splits that at the wrong
  // boundary for historical reasons (see the note at its parse site), giving
  // bytes 6-7 = 0x0001 = TypeH and bytes 8-9 = 0x2F01 = (TypeL << 8) | Version.
    this->transport_.send_apdu_command(
      apdu, 5,
      0x2F01,  // expect_type_low_ver: bytes 8-9 of the response
      0x0001,  // expect_type_high:    bytes 6-7 of the response
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

          if (payload_len >= (size_t)(offset + 7)) {
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
                remote_state_valid_ = true;
                remote_source_observations_++;
                ESP_LOGD(TAG, "Remote mode: Sub 7 prioritized read control_source=2 (Remote/Digital) → enabled");
              } else if (control_source == 1) {
                is_remote_mode_enabled_ = false;
                remote_state_valid_ = true;
                remote_source_observations_++;
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

void ControlService::send_remote_mode_command(bool enable,
                                             std::function<void(bool acked, bool rejected)> on_result) {
  // Class 3 SET: [0x03, 0x81, 0x07 enable | 0x08 disable/Auto]. See the header
  // note for why this is 0x81 and not 0xC1 (issue #46).
  const uint8_t apdu[3] = {0x03, 0x81, static_cast<uint8_t>(enable ? 0x07 : 0x08)};

  ESP_LOGI(TAG, "Sending Class 3 remote-mode %s command...", enable ? "ENABLE" : "DISABLE");
  this->transport_.send_apdu_command(apdu, 3, 0, 0,
    [on_result](bool got_response, const uint8_t *data, size_t len) {
      if (!got_response || len < 6) {
        // Window closed without a matchable ACK; the pump may still have
        // applied the command -- the caller's readback decides.
        if (on_result) on_result(false, false);
        return;
      }
      uint8_t ack_byte = data[5];
      if (on_result) on_result(ack_byte == 0x00, ack_byte != 0x00);
    });
}

void ControlService::send_run_command(bool start_pump,
                                      std::function<void(bool acked, bool rejected)> on_result) {
  // Class 3 SET: [0x03, 0x81, 0x06 START | 0x05 STOP]. See the header note;
  // ids and ACK shapes from jfriend00's bench findings in issue #92.
  const uint8_t apdu[3] = {0x03, 0x81, static_cast<uint8_t>(start_pump ? 0x06 : 0x05)};

  ESP_LOGI(TAG, "Sending Class 3 %s command...", start_pump ? "START" : "STOP");
  this->transport_.send_apdu_command(apdu, 3, 0, 0,
    [on_result](bool got_response, const uint8_t *data, size_t len) {
      if (!got_response || len < 6) {
        // Window closed without a matchable ACK; the pump may still have
        // applied the command — the caller's readback decides.
        if (on_result) on_result(false, false);
        return;
      }
      uint8_t ack_byte = data[5];
      if (on_result) on_result(ack_byte == 0x00, ack_byte != 0x00);
    });
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
  //
  // Whole frame, after the class and OpSpec bytes:
  //   [56][00 06][01 2F][01][00 00 07][00][Flag][Mode][Setpoint(4)]
  //    obj   sub    type  ver   size    <-- the 7-byte type-303 struct -->
  // apdu[2..5] carry the object, the sub-id and the type's high byte; `payload`
  // below picks up at the type's LOW byte, which is why it starts 2F rather
  // than at a field boundary.

  ControlModeMapping mapping;
  if (!get_class10_mapping(mode, mapping)) {
    // Default to mode_byte 0x02 (CONSTANT_SPEED) like Python does
    mapping.mode_byte = 0x02;
    memcpy(mapping.suffix, "\x45\x65\x70\x00", 4);
  }

  uint8_t payload[12];
  payload[0] = 0x2F;  // type low (0x012F = 303, OperationStatusRequest)
  payload[1] = 0x01;  // object version
  payload[2] = 0x00;  // size, 3 bytes big-endian
  payload[3] = 0x00;
  payload[4] = 0x07;  // ...7, the struct's fixedSize
  payload[5] = 0x00;  // control_source = Undefined
  payload[6] = start_pump ? 0x00 : 0x01;  // operation_mode: 0=Start, 1=Stop
  payload[7] = mapping.mode_byte;         // control_mode

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

  // OpSpec 0x90 = SET + 16 bytes (3 address + 3 type/version + 10 struct)
  //
  // A Class 10 request is addressed OBJECT FIRST: [Obj][SubH][SubL], then
  // [TypeH][TypeL][Ver] and the object's own body. So these bytes are object 86
  // (0x56), sub-id 6, type 0x012F = 303 (OperationStatusRequest) -- the first
  // two bytes of `payload` finish that type word.
  //
  // They used to be labelled "Sub ID high" / "Obj ID high", which is backwards.
  // The bytes were right by coincidence: object 86 is 0x56 and sub-id 6 fits the
  // low byte either way round, so nothing came of it here. Something did come of
  // it elsewhere -- the deleted setpoint register write took the labels at their
  // word, laid its address out sub-first, and the pump refused it every single
  // time (issue #258).
  uint8_t apdu[18];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x90;  // OpSpec: SET with length 16
  apdu[2] = 0x56;  // Object id 86, the start/stop request
  apdu[3] = 0x00;  // Sub-id high
  apdu[4] = 0x06;  // Sub-id low -- 86/6, overall_operation_local_request_obj
  apdu[5] = 0x01;  // Type high; payload[0] is 0x2F, completing type 0x012F = 303
                   // (payload[1] is the object version, payload[2..4] the size)
  memcpy(&apdu[6], payload, 12);

  // Awaited (issue #253). Same arrangement as the mode write above: the
  // callback is what makes the transport wait, and it reports nothing onward --
  // the callers of this send confirm by reading the run state and setpoint back.
  //
  // This is the fused Obj 0601 write, and it is the one the Grundfos GO app uses
  // for start/stop and for setting a setpoint -- there is no separate setpoint
  // write, which is why there is no longer one here either (issue #258):
  // 25 instances in resources/traffic_capture
  // (`tools/geni_capture_scan.py sets`), each carrying a real set_point value,
  // each answered, and each followed immediately by an object 84 sub-id 1
  // overview commit (`... order`). Its address shape
  // was already listed in the short-ACK branch before anything could use it,
  // added on the reasoning that a writer giving it a callback would want it
  // back. That list is gone; the declaration below is what replaces it.
  //
  // Multi-step setters (mode switch + dedicated write) disable the commit here
  // and issue a single commit after the step-2 write.
  this->transport_.send_apdu_command(
      apdu, 18, 0, 0,
      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {
        ESP_LOGV(TAG, "Control request %s", success ? "acknowledged" : "unanswered");
      },
      core::Transport::SET_ACK_TIMEOUT_MS, /*allow_register_read=*/false,
      /*expect_short_ack=*/true, /*quiet_timeout=*/true);

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
  // (control_source, operation_mode and set_point are all ignored)". On the wire
  // that is `56 00 0A 01 2F 01 ...` -- object, sub-id, type -- versus the
  // start/stop object 86 / sub-id 6 = overall_operation_local_request_obj,
  // `56 00 06 01 2F ...`, which DOES write set_point.
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
  payload[0] = 0x2F;               // type low (apdu[5] holds the 0x01 high byte)
  payload[1] = 0x01;               // object version
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
  // Object first, as every accepted Class 10 SET is: [Obj][SubH][SubL], then
  // the type word. The old "Sub ID high / Obj ID high" labels here were
  // backwards and right only by coincidence -- see send_control_request() and
  // issue #258.
  apdu[2] = 0x56;  // Object id 86, the mode request
  apdu[3] = 0x00;  // Sub-id high
  apdu[4] = 0x0A;  // Sub-id low -- 86/10, overall_control_mode_local_request_obj
  apdu[5] = 0x01;  // Type high; payload[0] is 0x2F, completing type 0x012F = 303
  memcpy(&apdu[6], payload, 12);

  // Await the pump's short ACK rather than firing and forgetting (issue #248).
  //
  // GENIbus is interlocked: the reply follows its request within 50 ms and the
  // master idles before the next one (App. Prog. Manual fig 1), which is why a
  // reply carries no sequence number and no object echo -- "the Data Reply is
  // not self contained, meaning that the Data Request is necessary to process
  // it" (fig 3.5 note 3). A send nobody waits on breaks that: its reply arrives
  // with no owner, and the next command's matcher, which can only test the
  // frame's SHAPE, takes it for its own answer.
  //
  // Not hypothetical for this write. It is the Class 10 frame sent
  // CONFIG_STEP2_DELAY_MS before the temperature-range and DHW config writes, on
  // the same class, and its reply is the exact shape their matcher accepts.
  //
  // The callback exists to make the transport wait at all -- a null one means
  // fire-and-forget -- and deliberately reports nothing onward. An earlier
  // revision exposed it as an `on_ack` parameter that no caller ever passed,
  // promising a contract reset() does not honour: it clears the queue without
  // invoking callbacks, so the first caller to rely on it would wait forever.
  this->transport_.send_apdu_command(
      apdu, 18, 0, 0,
      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {
        ESP_LOGV(TAG, "Mode write %s", success ? "acknowledged" : "unanswered");
      },
      MODE_ACK_TIMEOUT_MS, false, true, /*quiet_timeout=*/true);

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

  // Trailing bytes: the pump's min/max on/off-time LIMITS + version tail
  // (type 1012 TemperatureRangeControlUserSettings). Echo the values read
  // from the pump so a temperature write doesn't zero its limits (issue
  // #106); before the first read this falls back to the historical constants.
  memcpy(&apdu[20], cached_temp_limits_tail_, 5);

  // Log the full APDU for debugging
  ESP_LOGI(TAG, "write_temp_range APDU: %s", format_hex_pretty(apdu, 25).c_str());

  // Pass expect_type_low_ver=0, expect_type_high=0 for response matching because
  // the pump responds with a short ACK (OpSpec 0x01) without Obj/Sub fields
  this->transport_.send_apdu_command(
      apdu, 25, 0, 0,
      [on_ack](bool success, const uint8_t *data, size_t /*len*/) {
        // What this callback reports is "the pump answered", not "the pump said
        // yes" -- and the distinction matters since issue #208 made refusals
        // visible.
        //
        // The caller short-circuits to REJECTED without a readback when this is
        // false, so it must mean silence and nothing else. A refusal is not
        // silence, and worse, it cannot be attributed with confidence: the
        // short-ACK branch in transport.cpp matches on "some queued write of
        // this shape", carrying no sequence number and no object echo, so the
        // mode write sent a few hundred ms earlier can be answered inside
        // this write's window. (That write is awaited since issue #248, so its
        // reply is normally consumed before this one is sent -- but "normally"
        // is a timing property, not a guarantee, which is why this callback
        // still does not treat a refusal as silence.) Reporting a refusal as silence
        // would turn that misattribution into a REJECTED verdict for a write
        // that landed, with no readback to catch it.
        //
        // So an answered-but-refused write reports true and is settled by the
        // readback, which is unambiguous. transport.cpp logs the refusal at
        // warning either way, so it is reported rather than swallowed.
        const bool answered = success or (data != nullptr);
        if (on_ack) on_ack(answered);
      },
      3000, false, true); // 3000ms timeout, no register read, expect short ACK
}

bool ControlService::write_dhw_config(uint8_t on_minutes, uint8_t off_minutes,
                                      std::function<void(bool)> on_ack,
                                      const uint8_t *setpoint_be4) {
  // Read-modify-write: the struct carries the mode's stored flow setpoint,
  // echoed back verbatim unless the caller asserts new setpoint bytes
  // (issue #107). The freshness guard applies either way: refuse to write
  // blind rather than clobber unread state (the class issue #92 bans).
  if (!dhw_config_valid_) {
    ESP_LOGW(TAG, "Cannot write DHW config: setpoint not read yet (call read_dhw_config first)");
    return false;
  }

  // Mirror the GO app's capture frame byte for byte (issue #106):
  //   [0A][8F][5B][01 A5][03 D9][01][00 00 06][setpoint f32][on][off]
  // Obj-first addressing with a 1-byte object id (like the Sub 430
  // temperature write), type 985 DHWOnOffControlConfiguration version 1,
  // 3-byte size = 6, then the struct. OpSpec 0x8F = SET + 15 bytes
  // (1 obj + 2 sub + 2 type + 1 version + 3 size + 6 data).
  uint8_t apdu[17];
  apdu[0] = 0x0A;   // Class 10
  apdu[1] = 0x8F;   // OpSpec: SET + 15 bytes
  apdu[2] = 0x5B;   // Obj-ID (91, 1 byte in this packet shape)
  apdu[3] = 0x01;   // Sub-ID high (421 = 0x01A5)
  apdu[4] = 0xA5;   // Sub-ID low
  apdu[5] = 0x03;   // Type high (985 = 0x03D9)
  apdu[6] = 0xD9;   // Type low
  apdu[7] = 0x01;   // Object version
  apdu[8] = 0x00;   // Size high
  apdu[9] = 0x00;   // Size mid
  apdu[10] = 0x06;  // Size low (6 bytes)
  memcpy(&apdu[11], setpoint_be4 != nullptr ? setpoint_be4 : cached_dhw_setpoint_raw_, 4);
  apdu[15] = on_minutes;
  apdu[16] = off_minutes;

  // The pump replies with the generic short Class 10 ACK (OpSpec 0x01, no
  // Obj/Sub fields), matched by the transport's short-ACK path.
  this->transport_.send_apdu_command(
      apdu, sizeof(apdu), 0, 0,
      [on_ack](bool success, const uint8_t *data, size_t /*len*/) {
        // "Answered", not "accepted" -- see write_temp_range_config() above for
        // why a refusal must not be reported as silence (issue #208).
        const bool answered = success or (data != nullptr);
        if (on_ack) on_ack(answered);
      },
      3000, false, true);
  return true;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
