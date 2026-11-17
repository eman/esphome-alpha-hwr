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
 * 1. **Motor State** (type 256, versions 1-3)
 *    - AC/DC voltage, current, AC/DC power, speed, converter temperature
 * 
 * 2. **Flow/Pressure** (type 565, version 2)
 *    - Flow rate, head, inlet pressure, outlet pressure
 * 
 * 3. **Temperature** (type 534, versions 1-2)
 *    - Media temperature, PCB temperature, control box temperature
 * 
 * 4. **Alarms/Warnings** (type 570, version 1)
 *    - Active alarm codes and warning codes
 * 
 * 5. **Cumulative Energy** (Object 87, Sub 1; type 232, version 1)
 *    - Pump energy counter in kWh, converted from joules, and system power
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
  
  bool has_dc_power;
  float dc_power_w;

  bool has_speed;
  float speed_rpm;
  
  bool has_converter_temp;
  float converter_temperature_c;

  MotorStateTelemetry() 
    : has_voltage_ac(false), voltage_ac_v(0),
      has_voltage_dc(false), voltage_dc_v(0),
      has_current(false), current_a(0),
      has_power(false), power_w(0),
      has_dc_power(false), dc_power_w(0),
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
 * Decode energy and power from OverallEnergyConsumption (type 232, version 1).
 *
 * Class 10 Object 87, Sub 1 payload format, after removing the frame header and CRC:
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
 * Decode FaultsByArrayExtended (type 570, version 1).
 *
 * Payload format after the transport strips the frame/type header and CRC:
 * - Bytes 0-2: Object size (big-endian uint24)
 * - Bytes 3-4: Number of ExtendedErrorCode records (big-endian uint16)
 * - Bytes 5+: Records containing error code and subcode (two uint16 each)
 *
 * Only primary error codes are published; malformed arrays leave faults unchanged.
 *
 * @param payload Size-prefixed object payload
 * @param len Length of payload
 * @param faults Output decoded primary fault codes
 * @return True if the complete fault array is valid, including an empty array
 *
 * Reference: GENI profile 52_7, FaultsByArrayExtended and ExtendedErrorCode
 */
bool decode_faults_payload(const uint8_t* payload, size_t len, AlarmWarningTelemetry& faults);

/**
 * Decode motor state from a validated type-256 object reply.
 * 
 * Class 10 object replies have this format:
 * - Bytes 0-12: Packet header
 * - Bytes 10-12: Object size (big-endian uint24)
 * - Bytes 13+: Array of IEEE 754 floats (big-endian)
 * 
 * Float array layout for motor state:
 * [0] = Voltage AC (V) at offset 13
 * [1] = Voltage DC (V) at offset 17
 * [2] = Current (A) at offset 21
 * [3] = Power (W) at offset 25
 * [4] = DC power (W) at offset 29
 * [5] = Speed (RPM) at offset 33
 * [6] = Converter temp (°C) at offset 37
 * 
 * @param data Full packet bytes (not just payload)
 * @param len End of the validated object body (excluding CRC and trailing bytes)
 * @return Decoded motor state with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x30
 */
MotorStateTelemetry decode_motor_state_response(const uint8_t* data, size_t len);

/**
 * Decode flow/pressure from a validated type-565 object reply.
 * 
 * Float array layout for flow/pressure:
 * [0-5] = Reserved/unknown
 * [6] = Flow rate (m³/h) at offset 37
 * [7] = Head pressure (m) at offset 41
 * [8] = Inlet pressure (bar) at offset 45 - often NaN
 * [9] = Outlet pressure (bar) at offset 49 - often NaN
 * 
 * @param data Full packet bytes
 * @param len End of the validated object body (excluding CRC and trailing bytes)
 * @return Decoded flow/pressure with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x2B
 */
FlowPressureTelemetry decode_flow_pressure_response(const uint8_t* data, size_t len);

/**
 * Decode temperature from a validated type-534 object reply.
 * 
 * Float array layout for temperature:
 * [0] = Media temperature (°C) at offset 13
 * [1] = PCB temperature (°C) at offset 17
 * [2] = Control box temperature (°C) at offset 21
 * 
 * @param data Full packet bytes
 * @param len End of the validated object body (excluding CRC and trailing bytes)
 * @return Decoded temperature with validation flags
 * 
 * Reference: TelemetryDecoder.decode_register_read_response() with opspec=0x14
 */
TemperatureTelemetry decode_temperature_response(const uint8_t* data, size_t len);

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
