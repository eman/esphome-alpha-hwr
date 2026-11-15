/**
 * GENI Telemetry Decoder
 * 
 * This module decodes Class 10 DataObject telemetry from the pump.
 * It extracts and validates sensor data from raw payload bytes.
 * 
 * Telemetry Objects
 * -----------------
 * The Alpha HWR pump sends telemetry using Class 10 DataObjects:
 * 
 * 1. **Motor State** (OpSpec 0x30 responses)
 *    - AC voltage, current, power, speed, converter temperature
 * 
 * 2. **Flow/Pressure** (OpSpec 0x2B responses)
 *    - Flow rate, head, inlet pressure, outlet pressure
 * 
 * 3. **Temperature** (OpSpec 0x14 responses)
 *    - Media temperature, PCB temperature, control box temperature
 * 
 * 4. **Alarms/Warnings** (OpSpec 0x09 and 0x13 responses)
 *    - Active alarm codes and warning codes
 * 
 * 5. **Cumulative Energy** (Object 87, Sub 1; type 232, version 1)
 *    - Pump energy counter in kWh, converted from joules, and system power
 *
 * 6. **Passive Notifications** (OpSpec 0x0E)
 *    - Streaming telemetry in legacy format
 * 
 * Payload Format
 * --------------
 * All telemetry payloads use big-endian encoding:
 * - Floats: IEEE 754 single-precision (4 bytes)
 * - Energy counter: IEEE 754 double-precision (8 bytes)
 * - Integers: Big-endian uint16 (2 bytes)
 * 
 * Values are validated against reasonable ranges to detect corruption.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/protocol/telemetry_decoder.py
 */

#pragma once

#include "esphome/core/component.h"
#include "frame_parser.h"
#include <cstdint>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace protocol {

/**
 * Decoded motor state telemetry.
 * 
 * Contains electrical and mechanical motor data from register-read responses.
 */
struct MotorStateTelemetry {
  bool has_voltage_ac;
  float voltage_ac_v;
  
  bool has_voltage_dc;
  float voltage_dc_v;
  
  bool has_current;
  float current_a;
  
  bool has_power;
  float power_w;
  
  bool has_speed;
  float speed_rpm;
  
  bool has_converter_temp;
  float converter_temperature_c;

  MotorStateTelemetry() 
    : has_voltage_ac(false), voltage_ac_v(0),
      has_voltage_dc(false), voltage_dc_v(0),
      has_current(false), current_a(0),
      has_power(false), power_w(0),
      has_speed(false), speed_rpm(0),
      has_converter_temp(false), converter_temperature_c(0) {}
};

/**
 * Decoded cumulative energy and system power telemetry.
 *
 * Contains system totals from OverallEnergyConsumption (type 232, version 1).
 * Energy and power are validated independently. The power field models the
 * documented system totals but is not wired to a separate ESPHome entity.
 */
struct EnergyTelemetry {
  bool has_energy;
  float energy_kwh;

  bool has_power;
  float power_w;

  EnergyTelemetry()
    : has_energy(false), energy_kwh(0),
      has_power(false), power_w(0) {}
};

/**
 * Decoded flow and pressure telemetry.
 */
struct FlowPressureTelemetry {
  bool has_flow;
  float flow_m3h;
  
  bool has_head;
  float head_m;
  
  bool has_inlet_pressure;
  float inlet_pressure_bar;
  
  bool has_outlet_pressure;
  float outlet_pressure_bar;

  FlowPressureTelemetry()
    : has_flow(false), flow_m3h(0),
      has_head(false), head_m(0),
      has_inlet_pressure(false), inlet_pressure_bar(0),
      has_outlet_pressure(false), outlet_pressure_bar(0) {}
};

/**
 * Decoded temperature telemetry.
 */
struct TemperatureTelemetry {
  bool has_media_temp;
  float media_temperature_c;
  
  bool has_pcb_temp;
  float pcb_temperature_c;
  
  bool has_control_box_temp;
  float control_box_temperature_c;

  TemperatureTelemetry()
    : has_media_temp(false), media_temperature_c(0),
      has_pcb_temp(false), pcb_temperature_c(0),
      has_control_box_temp(false), control_box_temperature_c(0) {}
};

/**
 * Decoded alarm/warning codes.
 */
struct AlarmWarningTelemetry {
  std::vector<uint16_t> codes;
  
  AlarmWarningTelemetry() : codes() {}
};

/**
 * Decode energy and power from an OverallEnergyConsumption payload (type 232, version 1).
 *
 * Class 10 Object 87, Sub 1 payload format, after the transport strips the header and CRC:
 * - Bytes 0-2: Object size (big-endian uint24)
 * - Bytes 3-10: System energy (big-endian IEEE 754 double, joules)
 * - Bytes 11-14: System power (big-endian IEEE 754 float, watts)
 *
 * Converts the pump's cumulative counter from joules to kWh (divide by 3600000).
 * Invalid or truncated objects return no valid fields. Energy and power are
 * validated independently; unavailable energy does not discard valid power.
 *
 * @param payload Size-prefixed object payload (not the full packet)
 * @param len Length of payload
 * @return Decoded energy and power with validation flags
 *
 * Reference: power_and_energy_info_obj.system_energy, GENI unitIndex 87 (Ws)
 */
EnergyTelemetry decode_energy_payload(const uint8_t* payload, size_t len);

/**
 * Decode motor state from register-read response (OpSpec 0x30).
 * 
 * Register-read responses have this format:
 * - Bytes 0-12: Packet header
 * - Byte 12: DataLen
 * - Bytes 13+: Array of IEEE 754 floats (big-endian)
 * 
 * Float array layout for motor state:
 * [0] = Voltage AC (V) at offset 13
 * [1] = Voltage DC (V) at offset 17
 * [2] = Current (A) at offset 21
 * [3] = Power (W) at offset 25
 * [4] = Unknown/reserved at offset 29
 * [5] = Speed (RPM) at offset 33
 * [6] = Converter temp (°C) at offset 37
 * 
 * @param data Full packet bytes (not just payload)
 * @param len Length of packet
 * @return Decoded motor state with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x30
 */
MotorStateTelemetry decode_motor_state_response(const uint8_t* data, size_t len);

/**
 * Decode flow/pressure from register-read response (OpSpec 0x2B).
 * 
 * Float array layout for flow/pressure:
 * [0-5] = Reserved/unknown
 * [6] = Flow rate (m³/h) at offset 37
 * [7] = Head pressure (m) at offset 41
 * [8] = Inlet pressure (bar) at offset 45 - often NaN
 * [9] = Outlet pressure (bar) at offset 49 - often NaN
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @return Decoded flow/pressure with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x2B
 */
FlowPressureTelemetry decode_flow_pressure_response(const uint8_t* data, size_t len);

/**
 * Decode temperature from register-read response (OpSpec 0x14).
 * 
 * Float array layout for temperature:
 * [0] = Media temperature (°C) at offset 13
 * [1] = PCB temperature (°C) at offset 17
 * [2] = Control box temperature (°C) at offset 21
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @return Decoded temperature with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x14
 */
TemperatureTelemetry decode_temperature_response(const uint8_t* data, size_t len);

/**
 * Decode alarms/warnings from response (OpSpec 0x09 or 0x13).
 * 
 * OpSpec 0x09 format (Active Query Response):
 * - Bytes 13+: Array of uint16 codes (big-endian)
 * 
 * OpSpec 0x13 format (Read Response):
 * - Bytes 10+: Array of uint16 codes (big-endian)
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @param opspec OpSpec byte (0x09 or 0x13)
 * @return List of active alarm/warning codes (non-zero only)
 * 
 * Reference: TelemetryDecoder.decode_alarms_warnings()
 */
AlarmWarningTelemetry decode_alarms_warnings_response(const uint8_t* data, size_t len, uint8_t opspec);

/**
 * Decode passive notification motor state (OpSpec 0x0E, Obj 87, Sub 69).
 * 
 * Legacy streaming format:
 * - Offset 10: Voltage (float)
 * - Offset 18: Current (float)
 * - Offset 26: Power (float)
 * - Offset 30: RPM (float)
 * - Offset 34: Converter temp (float)
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @return Decoded motor state
 * 
 * Reference: Python decode_legacy_packet() for passive notifications
 */
MotorStateTelemetry decode_passive_motor_state(const uint8_t* data, size_t len);

/**
 * Decode passive notification flow/pressure (OpSpec 0x0E, Obj 93, Sub 290).
 * 
 * Legacy streaming format:
 * - Offset 10: Flow (float)
 * - Offset 14: Head (float)
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @return Decoded flow/pressure
 */
FlowPressureTelemetry decode_passive_flow_pressure(const uint8_t* data, size_t len);

/**
 * Decode passive notification temperature (OpSpec 0x0E, Obj 93, Sub 300).
 * 
 * Legacy streaming format:
 * - Offset 10: Media temp (float)
 * - Offset 14: PCB temp (float)
 * - Offset 18: Control box temp (float)
 * 
 * @param data Full packet bytes
 * @param len Length of packet
 * @return Decoded temperature
 */
TemperatureTelemetry decode_passive_temperature(const uint8_t* data, size_t len);

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
