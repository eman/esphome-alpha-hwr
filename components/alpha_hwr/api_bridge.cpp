#include "api_bridge.h"
#ifdef ALPHA_HWR_HAS_API_BRIDGE

#include <cmath>
#include <cstdio>
#include <map>
#include "alpha_hwr.h"
#include "esphome/core/log.h"

namespace esphome {
namespace alpha_hwr {

static const char *const BRIDGE_TAG = "alpha_hwr.api";
static const char *const WRITE_SETTLED_EVENT = "esphome.alpha_hwr_write_settled";

using services::ControlMode;
using services::ControlService;
using services::WriteCommand;
using services::WriteResult;
using services::WriteStatus;

void AlphaHwrApiBridge::setup(AlphaHwrComponent *component) {
  component_ = component;

  register_service(&AlphaHwrApiBridge::on_set_enabled, "pump_set_enabled", {"enabled", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_mode, "pump_set_mode", {"mode", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_setpoint, "pump_set_setpoint",
                   {"mode", "value", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_temperature_range, "pump_set_temperature_range",
                   {"min_c", "max_c", "autoadapt", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_cycle_times, "pump_set_cycle_times",
                   {"on_minutes", "off_minutes", "op_id"});

  register_service(&AlphaHwrApiBridge::on_set_schedule_entry, "set_schedule_entry",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_clear_schedule_entry, "clear_schedule_entry",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_schedule_enabled, "set_schedule_enabled",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_refresh_schedule, "refresh_schedule", {"op_id"});
  register_service(&AlphaHwrApiBridge::on_set_single_event, "set_single_event",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_clear_single_event, "clear_single_event",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_refresh_single_events, "refresh_single_events",
                   {"op_id"});

  // Terminal results reach fire_write_settled() through the component's
  // central write-result hook (see AlphaHwrComponent::setup()), which also
  // refreshes the schedule displays.
  ESP_LOGI(BRIDGE_TAG, "Programmatic write services registered");
}

void AlphaHwrApiBridge::fire_write_settled(const WriteResult &result) {
  std::map<std::string, std::string> data;
  data["op_id"] = result.op_id;
  data["command"] = services::write_command_to_string(result.command);
  data["status"] = services::write_status_to_string(result.status);
  data["detail"] = result.detail;
  data["origin"] = result.origin == services::WriteOrigin::ENTITY ? "entity" : "service";
  data["seq"] = std::to_string(result.seq);
  // Echo of the original request, so the event is self-contained for
  // logging/retry (review feedback on #92). Only populated fields are sent.
  if (result.requested_mode != ControlMode::NONE) {
    data["requested_mode"] = ControlService::mode_to_string(result.requested_mode);
  }
  if (!std::isnan(result.requested_value)) {
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "%.4g", result.requested_value);
    data["requested_value"] = rbuf;
  }
  if (!std::isnan(result.requested_temp_min)) {
    char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "%.1f", result.requested_temp_min);
    data["requested_temp_min"] = rbuf;
    snprintf(rbuf, sizeof(rbuf), "%.1f", result.requested_temp_max);
    data["requested_temp_max"] = rbuf;
  }
  if (result.requested_on_minutes >= 0) {
    data["requested_on_minutes"] = std::to_string(result.requested_on_minutes);
    data["requested_off_minutes"] = std::to_string(result.requested_off_minutes);
  }
  if (!result.requested_begin_hhmm.empty()) {
    data["requested_begin"] = result.requested_begin_hhmm;
    data["requested_end"] = result.requested_end_hhmm;
  }

  char buf[32];
  auto put_float = [&](const char *key, float value, const char *fmt) {
    if (std::isnan(value)) return;
    snprintf(buf, sizeof(buf), fmt, value);
    data[key] = buf;
  };
  auto put_bool = [&](const char *key, int8_t value) {
    if (value < 0) return;
    data[key] = value ? "true" : "false";
  };

  switch (result.command) {
    case WriteCommand::SET_PUMP_ENABLED:
      put_bool("enabled", result.enabled);
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      put_float("value", result.value, "%.4g");
      break;
    case WriteCommand::SET_MODE:
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      break;
    case WriteCommand::SET_SETPOINT:
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      put_float("value", result.value, "%.4g");
      put_bool("enabled", result.enabled);
      break;
    case WriteCommand::SET_TEMPERATURE_RANGE:
      put_float("temp_min", result.temp_min, "%.1f");
      put_float("temp_max", result.temp_max, "%.1f");
      put_bool("autoadapt", result.autoadapt);
      break;
    case WriteCommand::SET_CYCLE_TIMES:
      if (result.on_minutes >= 0) data["on_minutes"] = std::to_string(result.on_minutes);
      if (result.off_minutes >= 0) data["off_minutes"] = std::to_string(result.off_minutes);
      break;
    default: {
      // Schedule commands. Settled values come from the verify readbacks.
      static const char *DAY_NAMES[7] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                         "Friday",   "Saturday", "Sunday"};
      if (result.layer >= 0) data["layer"] = std::to_string(result.layer);
      if (result.day >= 0 && result.day <= 6) {
        data["day"] = std::to_string(result.day);
        data["day_name"] = DAY_NAMES[result.day];
      }
      if (result.slot >= 0) data["slot"] = std::to_string(result.slot);
      if (!result.begin_hhmm.empty()) data["begin"] = result.begin_hhmm;
      if (!result.end_hhmm.empty()) data["end"] = result.end_hhmm;
      if (result.begin_ts > 0) data["begin_ts"] = std::to_string(result.begin_ts);
      if (result.end_ts > 0) data["end_ts"] = std::to_string(result.end_ts);
      put_bool("enabled", result.sched_enabled);
      if (result.event_count >= 0) data["event_count"] = std::to_string(result.event_count);
      break;
    }
  }

  fire_homeassistant_event(WRITE_SETTLED_EVENT, data);
}

void AlphaHwrApiBridge::reject_(WriteCommand command, const std::string &op_id,
                                const std::string &detail) {
  // Bridge-level failures are malformed requests (unparsable data, unknown
  // mode names): deterministic, so they settle `invalid`, never `rejected`.
  ESP_LOGW(BRIDGE_TAG, "%s (op_id='%s') invalid at the bridge: %s",
           services::write_command_to_string(command), op_id.c_str(), detail.c_str());
  WriteResult result;
  result.op_id = op_id;
  result.command = command;
  result.status = WriteStatus::INVALID;
  result.detail = detail;
  fire_write_settled(result);
}

void AlphaHwrApiBridge::on_set_enabled(bool enabled, std::string op_id) {
  component_->submit_set_enabled(enabled, op_id);
}

void AlphaHwrApiBridge::on_set_mode(std::string mode, std::string op_id) {
  ControlMode parsed;
  if (!ControlService::mode_from_string(mode.c_str(), parsed)) {
    reject_(WriteCommand::SET_MODE, op_id, "unknown mode '" + mode + "'");
    return;
  }
  component_->submit_set_mode(parsed, op_id);
}

void AlphaHwrApiBridge::on_set_setpoint(std::string mode, float value, std::string op_id) {
  ControlMode parsed;
  if (!ControlService::mode_from_string(mode.c_str(), parsed)) {
    reject_(WriteCommand::SET_SETPOINT, op_id, "unknown mode '" + mode + "'");
    return;
  }
  component_->submit_set_setpoint(parsed, value, op_id);
}

void AlphaHwrApiBridge::on_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                                 std::string op_id) {
  component_->submit_set_temperature_range(min_c, max_c, autoadapt, op_id);
}

void AlphaHwrApiBridge::on_set_cycle_times(float on_minutes, float off_minutes, std::string op_id) {
  // Minutes arrive as float only because int-typed service variables hit the
  // ESP32-C3 linker bug; the operation layer validates the 1-60 range.
  if (on_minutes < 0 || on_minutes > 255 || off_minutes < 0 || off_minutes > 255) {
    reject_(WriteCommand::SET_CYCLE_TIMES, op_id, "cycle times must be 1-60 minutes");
    return;
  }
  component_->submit_set_cycle_times(static_cast<uint8_t>(on_minutes),
                                     static_cast<uint8_t>(off_minutes), op_id);
}

// ---------------------------------------------------------------------------
// Schedule services (data-string formats preserved from the YAML originals)
// ---------------------------------------------------------------------------

void AlphaHwrApiBridge::on_set_schedule_entry(std::string data, std::string op_id) {
  int vals[6];
  int n = sscanf(data.c_str(), "%d,%d,%d,%d,%d,%d",
                 &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]);
  if (n != 6 || vals[0] < 0 || vals[0] > 4 || vals[1] < 0 || vals[1] > 6 ||
      vals[2] < 0 || vals[2] > 23 || vals[3] < 0 || vals[3] > 59 ||
      vals[4] < 0 || vals[4] > 23 || vals[5] < 0 || vals[5] > 59) {
    reject_(WriteCommand::SET_SCHEDULE_ENTRY, op_id, "parse error: " + data);
    return;
  }
  component_->submit_set_schedule_entry(
      static_cast<uint8_t>(vals[0]), static_cast<uint8_t>(vals[1]), static_cast<uint8_t>(vals[2]),
      static_cast<uint8_t>(vals[3]), static_cast<uint8_t>(vals[4]), static_cast<uint8_t>(vals[5]),
      op_id);
}

void AlphaHwrApiBridge::on_clear_schedule_entry(std::string data, std::string op_id) {
  int l, d;
  if (sscanf(data.c_str(), "%d,%d", &l, &d) != 2 || l < 0 || l > 4 || d < 0 || d > 6) {
    reject_(WriteCommand::CLEAR_SCHEDULE_ENTRY, op_id, "parse error: " + data);
    return;
  }
  component_->submit_clear_schedule_entry(static_cast<uint8_t>(l), static_cast<uint8_t>(d), op_id);
}

void AlphaHwrApiBridge::on_set_schedule_enabled(std::string data, std::string op_id) {
  if (data != "0" && data != "1") {
    reject_(WriteCommand::SET_SCHEDULE_ENABLED, op_id, "parse error: " + data);
    return;
  }
  component_->submit_set_schedule_enabled(data == "1", op_id);
}

void AlphaHwrApiBridge::on_refresh_schedule(std::string op_id) {
  component_->submit_refresh_schedule(op_id);
}

void AlphaHwrApiBridge::on_set_single_event(std::string data, std::string op_id) {
  unsigned long begin_ts = 0, end_ts = 0;
  if (sscanf(data.c_str(), "%lu,%lu", &begin_ts, &end_ts) != 2 || begin_ts >= end_ts) {
    reject_(WriteCommand::SET_SINGLE_EVENT, op_id, "parse error: " + data);
    return;
  }
  component_->submit_set_single_event(static_cast<uint32_t>(begin_ts),
                                      static_cast<uint32_t>(end_ts), op_id);
}

void AlphaHwrApiBridge::on_clear_single_event(std::string data, std::string op_id) {
  int idx = 0;
  if (sscanf(data.c_str(), "%d", &idx) != 1 || idx < 0 || idx > 99) {
    reject_(WriteCommand::CLEAR_SINGLE_EVENT, op_id, "parse error: " + data);
    return;
  }
  component_->submit_clear_single_event(static_cast<uint8_t>(idx), op_id);
}

void AlphaHwrApiBridge::on_refresh_single_events(std::string op_id) {
  component_->submit_refresh_single_events(op_id);
}

}  // namespace alpha_hwr
}  // namespace esphome

#endif  // ALPHA_HWR_HAS_API_BRIDGE
