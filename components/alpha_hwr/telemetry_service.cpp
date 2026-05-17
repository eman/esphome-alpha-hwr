/**
 * Telemetry Service Implementation
 * 
 * Manages all telemetry operations for the ALPHA HWR pump.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#include "telemetry_service.h"
#include "transport.h"
#include "codec.h"
#include "frame_builder.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"
#include "sensor_publisher.h"
#include "control_service.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

TelemetryService::TelemetryService(core::Transport &transport) : transport_(transport) {
  ESP_LOGI(TAG, "TelemetryService created");
}

void TelemetryService::set_sensor_publisher(SensorPublisher* publisher) {
  sensor_publisher_ = publisher;
}

void TelemetryService::set_control_service(ControlService* control_service) {
  control_service_ = control_service;
}

void TelemetryService::start() {
  if (running_) {
    ESP_LOGW(TAG, "Service already running");
    return;
  }
  
  ESP_LOGI(TAG, "Starting telemetry service");
  running_ = true;
}

void TelemetryService::stop() {
  if (!running_) {
    return;
  }
  
  ESP_LOGI(TAG, "Stopping telemetry service");
  running_ = false;
}

bool TelemetryService::is_running() const {
  return running_;
}

void TelemetryService::send_read_request(uint32_t register_addr) {
  // Build the read packet using frame_builder
  uint8_t packet_raw[11];  // Class 10 read packets are always 11 bytes
  protocol::build_class10_read(register_addr, packet_raw);
  
  std::vector<uint8_t> packet(packet_raw, packet_raw + 11);
  
  ESP_LOGV(TAG, "Queueing READ request for register 0x%06X", register_addr);
  
  // Send via transport queue (no response matching here as telemetry uses various OpSpecs)
  this->transport_.send_command(packet);
}

void TelemetryService::poll() {
  if (!running_) {
    ESP_LOGD(TAG, "Service not running, skipping poll");
    return;
  }
  
  ESP_LOGD(TAG, "Polling telemetry registers...");
  
  // Reset alarm/warning response toggle (alarms queued first, then warnings)
  next_09_is_warnings_ = false;
  
  // Queue all read requests. The transport layer will pace them.
  send_read_request(0x570045); // Motor state
  send_read_request(0x5D0122); // Flow/pressure
  send_read_request(0x5D012C); // Temperature
  send_read_request(0x580000); // Alarms
  send_read_request(0x58000B); // Warnings
}

void TelemetryService::on_packet(const uint8_t* data, size_t len) {
  // Parse frame using protocol layer
  protocol::ParsedFrame frame = protocol::parse_frame(data, len);
  
  // Validate frame structure and CRC before decoding.
  if (!protocol::validate_frame_integrity(frame)) {
    ESP_LOGD(TAG, "Invalid frame integrity, ignoring");
    return;
  }
  
  // Check for response frame (we only process responses)
  if (frame.frame_type != protocol::FrameType::RESPONSE) {
    ESP_LOGD(TAG, "Not a response frame, ignoring");
    return;
  }
  
  // Check for Class 10 (we only handle Class 10 for now)
  if (frame.class_byte != protocol::CLASS_10) {
    ESP_LOGD(TAG, "Not Class 10 (got 0x%02X), ignoring", frame.class_byte);
    return;
  }
  
  uint8_t opspec = frame.opspec;
  ESP_LOGV(TAG, "Class 10 packet, OpSpec: 0x%02X, Sub=0x%04X, Obj=0x%04X", 
           opspec, frame.sub_id, frame.obj_id);
  
  // Route to appropriate handler based on OpSpec
  switch (opspec) {
    case 0x30:  // Motor state response
      handle_motor_state_response(data, len);
      break;
    
    case 0x2B:  // Flow/pressure response
      handle_flow_pressure_response(data, len);
      break;
    
    case 0x14:  // Temperature response
      handle_temperature_response(data, len);
      break;
    
    case 0x09:  // Alarms/warnings register-read response (offset 13)
    case 0x13:  // Alarms/warnings read response (offset 10)
      if (len >= 10) {
        uint16_t sub_id = protocol::decode_uint16_be(data, 6);
        uint16_t obj_id = protocol::decode_uint16_be(data, 8);
        
        // For 0x13: sub_id/obj_id are actual Sub/Obj IDs
        // For 0x09: sub_id is sequence number, obj_id is register ID
        // Route alarms vs warnings by matching known patterns
        if (opspec == 0x13 && obj_id == 0x0058) {
          if (sub_id == 0x0000) {
            handle_alarms_response(data, len, opspec);
          } else if (sub_id == 0x000B) {
            handle_warnings_response(data, len, opspec);
          }
        } else if (opspec == 0x09) {
          // Register-read format: use the 3-byte register address from the
          // request echo in the response (bytes 10-12 for opspec 0x09 responses
          // with len >= 15). Fall back to poll-order toggle only if we can't
          // distinguish by register address.
          bool routed = false;
          if (len >= 15) {
            // Extract 3-byte register address echoed in the response
            uint32_t reg_addr = (static_cast<uint32_t>(data[10]) << 16) |
                                (static_cast<uint32_t>(data[11]) << 8) |
                                static_cast<uint32_t>(data[12]);
            if (reg_addr == 0x580000) {
              handle_alarms_response(data, len, opspec);
              routed = true;
            } else if (reg_addr == 0x58000B) {
              handle_warnings_response(data, len, opspec);
              routed = true;
            }
          }
          // Fallback: use polling order if register address wasn't available
          if (!routed) {
            if (!next_09_is_warnings_) {
              handle_alarms_response(data, len, opspec);
              next_09_is_warnings_ = true;
            } else {
              handle_warnings_response(data, len, opspec);
            }
          }
        }
      }
      break;
    
    case 0x0E:  // Passive notification
      handle_passive_notification(data, len);
      break;
    
    default:
      ESP_LOGD(TAG, "Unhandled OpSpec: 0x%02X", opspec);
      break;
  }
}

void TelemetryService::handle_motor_state_response(const uint8_t* data, size_t len) {
  
  // Decode motor state using telemetry decoder
  protocol::MotorStateTelemetry motor = protocol::decode_motor_state_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_motor_state(motor);
  }
}

void TelemetryService::handle_flow_pressure_response(const uint8_t* data, size_t len) {
  
  // Decode flow/pressure using telemetry decoder
  protocol::FlowPressureTelemetry flow = protocol::decode_flow_pressure_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_flow_pressure(flow);
  }
}

void TelemetryService::handle_temperature_response(const uint8_t* data, size_t len) {
  
  // Decode temperature using telemetry decoder
  protocol::TemperatureTelemetry temp = protocol::decode_temperature_response(data, len);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_temperature(temp);
  }
}

void TelemetryService::handle_alarms_response(const uint8_t* data, size_t len, uint8_t opspec) {
  ESP_LOGD(TAG, "Alarms response (OpSpec 0x%02X)", opspec);
  
  // Decode alarms using telemetry decoder (opspec determines data offset)
  protocol::AlarmWarningTelemetry alarms = protocol::decode_alarms_warnings_response(data, len, opspec);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_alarms(alarms.codes);
  }
}

void TelemetryService::handle_warnings_response(const uint8_t* data, size_t len, uint8_t opspec) {
  ESP_LOGD(TAG, "Warnings response (OpSpec 0x%02X)", opspec);
  
  // Decode warnings using telemetry decoder (opspec determines data offset)
  protocol::AlarmWarningTelemetry warnings = protocol::decode_alarms_warnings_response(data, len, opspec);
  
  // Publish to sensors
  if (sensor_publisher_) {
    sensor_publisher_->publish_warnings(warnings.codes);
  }
}

void TelemetryService::handle_passive_notification(const uint8_t* data, size_t len) {
  if (len < 10) {
    return;
  }
  
  uint16_t sub_id = protocol::decode_uint16_be(data, 6);
  uint16_t obj_id = protocol::decode_uint16_be(data, 8);
  
  ESP_LOGD(TAG, "Passive telemetry notification: Sub=0x%04X, Obj=0x%04X", sub_id, obj_id);
  
  // Standard Motor State (Sub 0x0045, Obj 0x0057 = Decimal Sub 69, Obj 87)
  if (sub_id == 0x0045 && obj_id == 0x0057) {
    protocol::MotorStateTelemetry motor = protocol::decode_passive_motor_state(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_motor_state(motor);
    }
  }
  // Standard Flow/Head (Sub 0x0122, Obj 0x005D = Decimal Sub 290, Obj 93)
  else if (sub_id == 0x0122 && obj_id == 0x005D) {
    protocol::FlowPressureTelemetry flow = protocol::decode_passive_flow_pressure(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_flow_pressure(flow);
    }
  }
  // Standard Temperature (Sub 0x012C, Obj 0x005D = Decimal Sub 300, Obj 93)
  else if (sub_id == 0x012C && obj_id == 0x005D) {
    protocol::TemperatureTelemetry temp = protocol::decode_passive_temperature(data, len);
    if (sensor_publisher_) {
      sensor_publisher_->publish_temperature(temp);
    }
  }
  // Control Mode Status (OpSpec 0x0E, Sub 0x0001, Obj 0x2F01 = Decimal Sub 1, Obj 12033)
  // This is the passive notification that contains control mode, operation mode, and setpoint
  // Reference: Python control.py lines 470-496 (checks full packet length, not just payload)
  else if (sub_id == 0x0001 && obj_id == 0x2F01) {
    ESP_LOGI(TAG, "Control mode notification received (Obj 0x2F01, Sub 1)");
    
    if (len >= 12) {  // Need header + CRC before payload parsing
      const uint8_t* payload = data + 10;
      size_t payload_len = len - 12;  // Full packet minus header (10) and CRC (2)
      
      // Payload structure: [00 00 XX][control_source][operation_mode][control_mode][setpoint(4 bytes float)]
      size_t offset =
          (payload_len >= 3 && payload[0] == 0x00 && payload[1] == 0x00) ? 3 : 0;
      
      if (payload_len >= offset + 7) {
        uint8_t control_source = payload[offset];
        uint8_t operation_mode = payload[offset + 1];
        uint8_t control_mode = payload[offset + 2];
        
        // Extract setpoint as big-endian float (4 bytes at offset+3)
        float setpoint = protocol::decode_float_be(payload, offset + 3);
        
        ESP_LOGI(TAG, "Control Mode Status: mode=%d, op_mode=%d, setpoint=%.2f, source=%d",
                 control_mode, operation_mode, setpoint, control_source);
        
        // Update ControlService with the mode from passive notification
        if (control_service_) {
          control_service_->update_mode_from_notification(control_mode, operation_mode, setpoint);
        } else {
          ESP_LOGW(TAG, "Control service not set, cannot update mode");
        }
      } else {
        ESP_LOGW(TAG,
                 "Control mode payload too short for parsing: %zu bytes (need >= %zu)",
                 payload_len, offset + 7);
      }
    } else {
      ESP_LOGW(TAG, "Control mode packet too short: %zu bytes (need at least 12)",
               len);
    }
  }
  else {
    ESP_LOGV(TAG, "Unknown passive notification (Sub=0x%04X, Obj=0x%04X)", sub_id, obj_id);
  }
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
