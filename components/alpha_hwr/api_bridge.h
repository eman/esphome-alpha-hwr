#pragma once

#include "esphome/core/defines.h"

// The bridge needs both CustomAPIDevice features: register_service()
// (custom_services: true) and fire_homeassistant_event()
// (homeassistant_services: true). The shipped packages enable both in their
// api: blocks; without them the bridge compiles out and the entity interface
// keeps working.
#if defined(USE_API) && defined(USE_API_CUSTOM_SERVICES) && defined(USE_API_HOMEASSISTANT_SERVICES)
#define ALPHA_HWR_HAS_API_BRIDGE
#endif

#ifdef ALPHA_HWR_HAS_API_BRIDGE

#include <string>
#include "esphome/components/api/custom_api_device.h"
#include "write_operation_service.h"

namespace esphome {
namespace alpha_hwr {

class AlphaHwrComponent;

/**
 * Home Assistant surface of the programmatic write interface (issue #92).
 *
 * Registers the pump_* services (surfacing in HA as
 * esphome.<node>_pump_set_enabled etc.) and fires the terminal
 * esphome.alpha_hwr_write_settled event for every write operation.
 *
 * The bridge is the single owner of the service/event contract: argument
 * parse failures become an immediate terminal `rejected` event here, before
 * the operation layer is involved, so a client waiting on its op_id can never
 * hang on a malformed call. Everything else — sequencing, confirm readbacks,
 * clamp/reject detection, watchdogs — lives in WriteOperationService; this
 * class only translates between HA strings and typed submissions.
 *
 * Compiled only when the `api:` component is configured (USE_API); every
 * shipped package already configures it. Service arguments deliberately use
 * only bool/float/string — int-typed service variables hit an ESP32-C3
 * RISC-V linker bug (see packages/alpha_hwr_schedule_editor.yaml).
 */
class AlphaHwrApiBridge : public api::CustomAPIDevice {
 public:
  /** Register the services and hook the component's write-result callback. */
  void setup(AlphaHwrComponent *component);

  /** Fire esphome.alpha_hwr_write_settled with the result's settled values. */
  void fire_write_settled(const services::WriteResult &result);

 private:
  void on_set_enabled(bool enabled, std::string op_id);
  void on_set_mode(std::string mode, std::string op_id);
  void on_set_setpoint(std::string mode, float value, std::string op_id);
  void on_set_temperature_range(float min_c, float max_c, bool autoadapt, std::string op_id);
  void on_set_cycle_times(float on_minutes, float off_minutes, float flow, std::string op_id);
  void on_set_pump_state(std::string state, std::string op_id);  // "off" | "engaged" | "scheduled"

  // Schedule services, migrated from packages/alpha_hwr_schedule_editor.yaml
  // with their names and single data-string formats unchanged (the string
  // format is the ESP32-C3 int32 linker-bug workaround); op_id is a new,
  // optional second argument. Parse failures used to silently return in the
  // YAML lambdas — here they become an immediate terminal `rejected` event.
  void on_upload_schedule(std::string data, std::string op_id);       // v1 bulk payload
  void on_set_schedule_entry(std::string data, std::string op_id);    // "layer,day,sh,sm,eh,em"
  void on_clear_schedule_entry(std::string data, std::string op_id);  // "layer,day"
  void on_set_schedule_enabled(std::string data, std::string op_id);  // "0" | "1"
  void on_refresh_schedule(std::string op_id);
  void on_set_single_event(std::string data, std::string op_id);      // "begin_ts,end_ts"
  void on_clear_single_event(std::string data, std::string op_id);    // "slot"
  void on_refresh_single_events(std::string op_id);
  void on_set_vacation(std::string data, std::string op_id);          // "begin_ts,end_ts" (Stop event)
  void on_clear_vacation(std::string op_id);

  /** Immediate terminal rejection for arguments the bridge cannot parse. */
  void reject_(services::WriteCommand command, const std::string &op_id, const std::string &detail);

  AlphaHwrComponent *component_{nullptr};
};

}  // namespace alpha_hwr
}  // namespace esphome

#endif  // ALPHA_HWR_HAS_API_BRIDGE
