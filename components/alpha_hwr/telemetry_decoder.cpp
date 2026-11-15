/**
 * GENI Telemetry Decoder Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/protocol/telemetry_decoder.py
 */

#include "telemetry_decoder.h"
#include "codec.h"
#include <cmath>

namespace esphome {
namespace alpha_hwr {
namespace protocol {

MotorStateTelemetry decode_motor_state_response(const uint8_t* data, size_t len) {
  MotorStateTelemetry result;
  
  // Motor state response: floats start at offset 13
  // Float array layout:
  // [0] = Voltage AC (V) at offset 13
  // [1] = Voltage DC (V) at offset 17
  // [2] = Current (A) at offset 21
  // [3] = Power (W) at offset 25
  // [4] = Unknown/reserved at offset 29
  // [5] = Speed (RPM) at offset 33
  // [6] = Converter temp (°C) at offset 37
  
  // AC voltage (offset 13, float[0])
  if (len >= 17) {
    float voltage_ac = decode_float_be(data, 13);
    if (!std::isnan(voltage_ac) && voltage_ac >= 0 && voltage_ac <= 500) {
      result.has_voltage_ac = true;
      result.voltage_ac_v = voltage_ac;
    }
  }
  
  // DC voltage (offset 17, float[1])
  if (len >= 21) {
    float voltage_dc = decode_float_be(data, 17);
    if (!std::isnan(voltage_dc) && voltage_dc >= 0 && voltage_dc <= 500) {
      result.has_voltage_dc = true;
      result.voltage_dc_v = voltage_dc;
    }
  }
  
  // Current (offset 21, float[2])
  if (len >= 25) {
    float current = decode_float_be(data, 21);
    if (!std::isnan(current) && current >= 0 && current <= 50) {
      result.has_current = true;
      result.current_a = current;
    }
  }
  
  // Power (offset 25, float[3])
  if (len >= 29) {
    float power = decode_float_be(data, 25);
    if (!std::isnan(power) && power >= 0 && power <= 1000) {
      result.has_power = true;
      result.power_w = power;
    }
  }
  
  // Speed/RPM (offset 33, float[5])
  if (len >= 37) {
    float rpm = decode_float_be(data, 33);
    if (!std::isnan(rpm) && rpm >= 0 && rpm <= 10000) {
      result.has_speed = true;
      result.speed_rpm = rpm;
    }
  }
  
  // Converter temperature (offset 37, float[6])
  if (len >= 41) {
    float temp = decode_float_be(data, 37);
    // Allow NaN for temperature - it means sensor not available
    if (std::isnan(temp) || (temp >= -20 && temp <= 120)) {
      result.has_converter_temp = true;
      result.converter_temperature_c = temp;
    }
  }
  
  return result;
}

FlowPressureTelemetry decode_flow_pressure_response(const uint8_t* data, size_t len) {
  FlowPressureTelemetry result;
  
  // Flow/pressure response: floats start at offset 13
  // Float array layout:
  // [0-5] = Reserved/unknown
  // [6] = Flow rate (m³/h) at offset 37
  // [7] = Head pressure (m) at offset 41
  // [8] = Inlet pressure (bar) at offset 45 - often NaN
  // [9] = Outlet pressure (bar) at offset 49 - often NaN
  
  // Flow rate (offset 37, float[6])
  if (len >= 41) {
    float flow = decode_float_be(data, 37);
    if (!std::isnan(flow) && flow >= 0 && flow <= 100) {
      result.has_flow = true;
      result.flow_m3h = flow;
    }
  }
  
  // Head (offset 41, float[7])
  if (len >= 45) {
    float head = decode_float_be(data, 41);
    if (!std::isnan(head) && head >= 0 && head <= 50) {
      result.has_head = true;
      result.head_m = head;
    }
  }
  
  // Inlet pressure (offset 45, float[8]) - may be NaN
  if (len >= 49) {
    float inlet_pressure = decode_float_be(data, 45);
    // Allow NaN for pressure sensors
    if (std::isnan(inlet_pressure) || (inlet_pressure >= 0 && inlet_pressure <= 20)) {
      result.has_inlet_pressure = true;
      result.inlet_pressure_bar = inlet_pressure;
    }
  }
  
  // Outlet pressure (offset 49, float[9]) - may be NaN
  if (len >= 53) {
    float outlet_pressure = decode_float_be(data, 49);
    // Allow NaN for pressure sensors
    if (std::isnan(outlet_pressure) || (outlet_pressure >= 0 && outlet_pressure <= 20)) {
      result.has_outlet_pressure = true;
      result.outlet_pressure_bar = outlet_pressure;
    }
  }
  
  return result;
}

TemperatureTelemetry decode_temperature_response(const uint8_t* data, size_t len) {
  TemperatureTelemetry result;
  
  // Temperature response: floats start at offset 13
  // Float array layout:
  // [0] = Media temperature (°C) at offset 13
  // [1] = PCB temperature (°C) at offset 17
  // [2] = Control box temperature (°C) at offset 21
  
  // Media temperature (offset 13, float[0])
  if (len >= 17) {
    float temp = decode_float_be(data, 13);
    if (!std::isnan(temp) && temp >= -20 && temp <= 100) {
      result.has_media_temp = true;
      result.media_temperature_c = temp;
    }
  }
  
  // PCB temperature (offset 17, float[1])
  if (len >= 21) {
    float temp = decode_float_be(data, 17);
    if (!std::isnan(temp) && temp >= -20 && temp <= 150) {
      result.has_pcb_temp = true;
      result.pcb_temperature_c = temp;
    }
  }
  
  // Control box temperature (offset 21, float[2])
  if (len >= 25) {
    float temp = decode_float_be(data, 21);
    if (!std::isnan(temp) && temp >= -20 && temp <= 150) {
      result.has_control_box_temp = true;
      result.control_box_temperature_c = temp;
    }
  }
  
  return result;
}

bool decode_faults_payload(const uint8_t* payload, size_t len, AlarmWarningTelemetry& faults) {
  if (payload == nullptr || len < 5) {
    return false;
  }
  const uint32_t object_size = (uint32_t(payload[0]) << 16) |
                              (uint32_t(payload[1]) << 8) | payload[2];
  if (object_size != len - 3) {
    return false;
  }
  const uint16_t count = decode_uint16_be(payload, 3);
  if (object_size != 2 + size_t(count) * 4) {
    return false;
  }

  AlarmWarningTelemetry result;
  for (size_t i = 0; i < count; i++) {
    const uint16_t code = decode_uint16_be(payload, 5 + 4 * i);
    if (code != 0) {
      result.codes.push_back(code);
    }
  }
  faults = result;
  return true;
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
