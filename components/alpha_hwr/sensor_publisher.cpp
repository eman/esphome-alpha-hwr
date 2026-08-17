/**
 * Sensor Publisher Implementation
 * 
 * Maps decoded telemetry structs to ESPHome sensor components.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 *            (Python updates internal model, we publish to sensors)
 */

#include "sensor_publisher.h"
#include "esphome/core/hal.h"
#include <cmath>

namespace esphome {
namespace alpha_hwr {
namespace services {

SensorPublisher::SensorPublisher() {
}

void SensorPublisher::set_head_rate_sensor(sensor::Sensor *sensor) {
  head_rate_sensor_ = sensor;
}

// Called from AlphaHwrComponent::setup() after all sensors are wired.
// Attaches the derivative computation as a callback on head_sensor_ so it fires
// every time head pressure is published, regardless of which code path triggered it.
void SensorPublisher::setup_head_rate_callback() {
  if (head_sensor_ == nullptr || head_rate_sensor_ == nullptr) {
    return;
  }
  head_sensor_->add_on_state_callback([this](float head_m) {
    uint32_t now_ms = millis();
    if (!std::isnan(prev_head_m_) && prev_head_time_ms_ != 0) {
      float dt_s = static_cast<float>(now_ms - prev_head_time_ms_) / 1000.0f;
      if (dt_s > 30.0f) {
        // BLE reconnect gap: stale baseline — reset silently.
        ESP_LOGD(TAG, "Head rate: reset after %.1f s gap", dt_s);
      } else if (dt_s >= 0.1f) {
        float rate_m_s = (head_m - prev_head_m_) / dt_s;
        ESP_LOGD(TAG, "Head rate: %.4f m/s (dt=%.2f s)", rate_m_s, dt_s);
        head_rate_sensor_->publish_state(rate_m_s);
      }
    }
    prev_head_m_ = head_m;
    prev_head_time_ms_ = now_ms;
  });
}

void SensorPublisher::publish_motor_state(const protocol::MotorStateTelemetry& motor) {
  // Validate that we have at least some valid data
  if (!motor.has_power && !motor.has_speed) {
    ESP_LOGD(TAG, "Motor state has no valid data, skipping publish");
    return;
  }
  
  // Log summary
  ESP_LOGD(TAG, "Motor: AC=%.1fV, DC=%.1fV, %.2fA, %.1fW, %.0f RPM",
           motor.has_voltage_ac ? motor.voltage_ac_v : 0,
           motor.has_voltage_dc ? motor.voltage_dc_v : 0,
           motor.has_current ? motor.current_a : 0,
           motor.power_w, motor.speed_rpm);
  
  // Publish voltage AC
  if (motor.has_voltage_ac && voltage_sensor_ != nullptr) {
    voltage_sensor_->publish_state(motor.voltage_ac_v);
  }
  
  // Publish voltage DC
  if (motor.has_voltage_dc && voltage_dc_sensor_ != nullptr) {
    voltage_dc_sensor_->publish_state(motor.voltage_dc_v);
  }
  
  // Publish current
  if (motor.has_current && current_sensor_ != nullptr) {
    current_sensor_->publish_state(motor.current_a);
  }
  
  // Publish power
  if (motor.has_power && power_sensor_ != nullptr) {
    power_sensor_->publish_state(motor.power_w);
  }
  
  // Publish RPM
  if (motor.has_speed && rpm_sensor_ != nullptr) {
    rpm_sensor_->publish_state(motor.speed_rpm);
  }
}

void SensorPublisher::publish_flow_pressure(const protocol::FlowPressureTelemetry& flow) {
  // Validate that we have at least some valid data
  if (!flow.has_flow && !flow.has_head) {
    ESP_LOGD(TAG, "Flow/pressure has no valid data, skipping publish");
    return;
  }
  
  // Log summary
  ESP_LOGD(TAG, "Flow/Head: %.3f m³/h, %.2f m, P_in=%.2f bar",
           flow.flow_m3h, flow.head_m,
           flow.has_inlet_pressure ? flow.inlet_pressure_bar : NAN);

  // Publish flow rate
  if (flow.has_flow && flow_sensor_ != nullptr) {
    flow_sensor_->publish_state(flow.flow_m3h);
  }

  // Publish head in meters of head — the pump's native unit and the same unit
  // as the pressure setpoints (Grundfos GO app / datasheet convention).
  if (flow.has_head && head_sensor_ != nullptr) {
    head_sensor_->publish_state(flow.head_m);
  }

  // Head rate of change (m/s) is computed at notification rate via the
  // head_sensor_ callback registered in setup_head_rate_callback().

  // Publish inlet pressure (often NaN on HWR models)
  if (flow.has_inlet_pressure && inlet_pressure_sensor_ != nullptr) {
    if (!std::isnan(flow.inlet_pressure_bar)) {
      inlet_pressure_sensor_->publish_state(flow.inlet_pressure_bar);
    }
  }
}

void SensorPublisher::publish_temperature(const protocol::TemperatureTelemetry& temp) {
  // Log summary
  ESP_LOGD(TAG, "Temps: Media=%.1f°C, PCB=%.1f°C, Box=%.1f°C",
           temp.has_media_temp ? temp.media_temperature_c : NAN,
           temp.has_pcb_temp ? temp.pcb_temperature_c : NAN,
           temp.has_control_box_temp ? temp.control_box_temperature_c : NAN);
  
  // Publish media temperature (with range validation)
  if (temp.has_media_temp && temp_media_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 100°C)
    if (temp.media_temperature_c >= -20 && temp.media_temperature_c <= 100) {
      temp_media_sensor_->publish_state(temp.media_temperature_c);
    } else {
      ESP_LOGW(TAG, "Media temperature out of range: %.1f°C", temp.media_temperature_c);
    }
  }
  
  // Publish PCB temperature (with range validation)
  if (temp.has_pcb_temp && temp_pcb_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 150°C)
    if (temp.pcb_temperature_c >= -20 && temp.pcb_temperature_c <= 150) {
      temp_pcb_sensor_->publish_state(temp.pcb_temperature_c);
    } else {
      ESP_LOGW(TAG, "PCB temperature out of range: %.1f°C", temp.pcb_temperature_c);
    }
  }
  
  // Publish control box temperature (with range validation)
  if (temp.has_control_box_temp && temp_control_box_sensor_ != nullptr) {
    // Validate temperature is in reasonable range (-20°C to 150°C)
    if (temp.control_box_temperature_c >= -20 && temp.control_box_temperature_c <= 150) {
      temp_control_box_sensor_->publish_state(temp.control_box_temperature_c);
    } else {
      ESP_LOGW(TAG, "Control box temperature out of range: %.1f°C", temp.control_box_temperature_c);
    }
  }
}

void SensorPublisher::publish_alarms(const std::vector<uint16_t>& codes) {
  (void) codes;  // used only in the USE_TEXT_SENSOR build
#ifdef USE_TEXT_SENSOR
  if (alarms_sensor_ == nullptr) {
    return;
  }
  
  std::string codes_str = format_codes(codes);
  // Re-read on every telemetry poll and virtually always unchanged ("None").
  // TextSensor::publish_state() has no dedup, so republishing identical text is
  // an API state frame per subscriber per poll — as is the INFO line beside it
  // (issue #127). Announce transitions; stay quiet otherwise.
  if (alarms_sensor_->has_state() && alarms_sensor_->state == codes_str) {
    ESP_LOGV(TAG, "Alarms unchanged: %s", codes_str.c_str());
    return;
  }
  ESP_LOGI(TAG, "✓ Alarms: %s", codes_str.c_str());
  alarms_sensor_->publish_state(codes_str);
#endif
}

void SensorPublisher::publish_warnings(const std::vector<uint16_t>& codes) {
  (void) codes;  // used only in the USE_TEXT_SENSOR build
#ifdef USE_TEXT_SENSOR
  if (warnings_sensor_ == nullptr) {
    return;
  }
  
  std::string codes_str = format_codes(codes);
  // Same as publish_alarms(): transitions only (issue #127).
  if (warnings_sensor_->has_state() && warnings_sensor_->state == codes_str) {
    ESP_LOGV(TAG, "Warnings unchanged: %s", codes_str.c_str());
    return;
  }
  ESP_LOGI(TAG, "✓ Warnings: %s", codes_str.c_str());
  warnings_sensor_->publish_state(codes_str);
#endif
}

// Grundfos GENI error code descriptions (from GENI profile, 98+ codes)
// Used for both alarm (Object 88, Sub 0) and warning (Object 88, Sub 11) codes
static const char *get_alarm_description(uint16_t code) {
  switch (code) {
    case 0:   return "OK";
    case 1:   return "Leakage Current";
    case 2:   return "Motor Phase Missing";
    case 3:   return "External Alarm";
    case 4:   return "Max Number of Restarts";
    case 7:   return "Too Many Hardware Shutdowns";
    case 9:   return "Motor Phase Sequence Reversal";
    case 10:  return "Pump Communication";
    case 12:  return "Service Interval";
    case 14:  return "Electronic DC Link Protection";
    case 15:  return "Communication Fault Main System";
    case 16:  return "Other";
    case 17:  return "Low System Performance";
    case 19:  return "Diaphragm Break";
    case 21:  return "Too Many Starts Per Hour";
    case 22:  return "Motor Moisture";
    case 24:  return "Vibration";
    case 25:  return "User Wrong Configuration";
    case 26:  return "Welding Contactor";
    case 28:  return "Battery Low";
    case 29:  return "Impeller Forced Backwards";
    case 30:  return "Bearing Wear";
    case 31:  return "Varistor Change";
    case 32:  return "Over Voltage";
    case 33:  return "Service Soon";
    case 35:  return "Air in Pump Head";
    case 36:  return "Discharge Valve Leakage";
    case 37:  return "Suction Valve Leakage";
    case 39:  return "Mixing Loop Valve Stuck";
    case 40:  return "Motor Low Supply Voltage";
    case 41:  return "Motor Low Supply Voltage Transient";
    case 42:  return "Cut-In Fault";
    case 43:  return "Motor Positive Turbine Operation";
    case 44:  return "Under Temperature";
    case 45:  return "Voltage Asymmetry";
    case 46:  return "External Warning";
    case 48:  return "Motor Over Load";
    case 49:  return "Motor Over Current";
    case 50:  return "Motor Protection General Shut Down";
    case 51:  return "Motor Blocked";
    case 54:  return "Motor Protection 3s Limit";
    case 55:  return "Motor Current Protection";
    case 56:  return "Motor Under Load";
    case 57:  return "Dry Run";
    case 58:  return "Low Flow";
    case 59:  return "No Flow";
    case 60:  return "Low Input Power";
    case 61:  return "Water Hammer";
    case 63:  return "Media Temperature Frozen Protection";
    case 64:  return "Over Temperature";
    case 65:  return "Motor Over Temperature";
    case 66:  return "Control Electronics Over Temperature";
    case 67:  return "Power Converter Over Temperature";
    case 68:  return "High Media Temperature Protection";
    case 69:  return "Motor Over Heated";
    case 70:  return "Motor Over Heated Thermal Relay 2";
    case 72:  return "Motor Drive Unit Internal Fault";
    case 73:  return "Hardware Shut Down";
    case 74:  return "Motor High DC Link Voltage";
    case 75:  return "Motor Low DC Link Voltage";
    case 76:  return "PMCM Lost Node Internal";
    case 77:  return "Twin Pumps Communication Fault";
    case 80:  return "Hardware Fault Type 2";
    case 83:  return "FE Parameter Verification Error";
    case 84:  return "Persistence Break Down";
    case 85:  return "Motor Drive Unit Memory Fault";
    case 87:  return "Multi Sensor Limit Exceeded";
    case 88:  return "General Sensor Fault";
    case 89:  return "Primary Feedback Sensor Fault";
    case 90:  return "Speed Sensor Signal Fault";
    case 91:  return "Pump Flow Temperature Sensor";
    case 92:  return "Feedback Sensor Calibration Fault";
    case 93:  return "Fallback Sensor Signal Fault";
    case 94:  return "Limit Exceeded 1";
    case 95:  return "Limit Exceeded 2";
    case 96:  return "Reference Input Signal Fault";
    case 97:  return "Invalid Setpoint";
    case 105: return "Electronic Rectifier Protection";
    case 106: return "Electronic Inverter Protection";
    case 110: return "Electrical Asymmetry";
    case 114: return "Frost Protection";
    case 117: return "Door Opened";
    case 125: return "Outdoor Temperature Sensor";
    case 126: return "Zone Air Supply Temperature Sensor";
    case 127: return "Shunt Relative Pressure Sensor";
    case 130: return "Cable Theft";
    case 131: return "Unbalance";
    case 132: return "Invalid GSC File";
    case 133: return "Generic Limit Exceeded";
    case 134: return "Data Fault from Remote Sensors";
    case 142: return "Running on Battery Common";
    case 143: return "Multi Sensor Signal Fault";
    case 148: return "Motor DE Bearing Temp High";
    case 149: return "Motor NDE Bearing Temp High";
    case 152: return "Communication Fault Add-On Module";
    case 155: return "Inrush Fault";
    case 156: return "Internal Freq Converter Communication";
    case 157: return "Real Time Clock Out of Order";
    case 159: return "CIM Communication";
    case 161: return "Sensor Supply Fault 5V/12V";
    case 162: return "Sensor Supply Fault 24V";
    case 163: return "Motor Drive Unit Config Fault";
    case 164: return "LiqTec Sensor Signal Fault";
    case 165: return "AI Signal Fault";
    case 166: return "AI2 Signal Fault";
    case 167: return "AI3 Signal Fault";
    case 168: return "Pressure Sensor Signal Fault";
    case 169: return "Flow Sensor Signal Fault";
    case 175: return "Supply Flow Temperature Sensor";
    case 176: return "Return Flow Temperature Sensor";
    case 181: return "PTC Sensor Signal Fault";
    case 190: return "Sensor 1 Limit Exceeded";
    case 191: return "Level Control High Water";
    case 193: return "Sensor Limit 4 Exceeded";
    case 197: return "Operation with Reduced Pressure";
    case 200: return "Application Fault";
    case 203: return "Alarm on All Pumps";
    case 204: return "Inconsistency Between Sensors";
    case 205: return "Level Switch Inconsistency";
    case 206: return "Water Shortage Level 1";
    case 207: return "Water Leakage";
    case 208: return "Cavitation";
    case 209: return "Non-Return Valve Fault";
    case 210: return "Overpressure";
    case 211: return "Shunt Relative Pressure Surveillance";
    case 215: return "Pipe Filling Time Out";
    case 220: return "Motor Contactor Wear Out";
    case 225: return "PMCM Lost Node External";
    case 226: return "IO Module Communication";
    case 228: return "Flow Switch Monitoring";
    case 229: return "Water on Floor";
    case 230: return "Invalid MAC Address";
    case 236: return "Pump Fault";
    case 237: return "Pump 2 Fault";
    case 240: return "Motor Bearing Lubrication";
    case 241: return "Motor Phase Failure";
    case 242: return "Motor Model Auto Recognition Failure";
    case 245: return "Pump Max Running Time Protection";
    case 247: return "Power On Notice";
    case 248: return "Running on Battery";
    case 249: return "User Configurable Alert";
    case 250: return "User Configurable Alert 1";
    case 255: return "Electronics Short Circuit Fault";
    default:  return nullptr;
  }
}

std::string SensorPublisher::format_codes(const std::vector<uint16_t>& codes) {
  if (codes.empty()) {
    return "None";
  }
  
  std::string result;
  for (size_t i = 0; i < codes.size(); i++) {
    if (i > 0) {
      result += ", ";
    }
    const char *desc = get_alarm_description(codes[i]);
    char buf[64];
    if (desc) {
      snprintf(buf, sizeof(buf), "%s (%u)", desc, codes[i]);
    } else {
      snprintf(buf, sizeof(buf), "Unknown (%u)", codes[i]);
    }
    result += buf;
  }
  
  return result;
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
