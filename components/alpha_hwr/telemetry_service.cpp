/**
 * Telemetry Service Implementation
 * 
 * Manages all telemetry operations for the ALPHA HWR pump.
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/telemetry.py
 */

#include "telemetry_service.h"

#include <cinttypes>

#include "transport.h"
#include "codec.h"
#include "frame_builder.h"
#include "frame_parser.h"
#include "response_match.h"
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
  fault_generation_++;
  fault_poll_pending_ = false;
  fault_poll_failed_ = false;
  energy_generation_++;
  energy_read_pending_ = false;
  energy_attempted_ = false;
  energy_read_state_ = EnergyReadState::UNKNOWN;
}

bool TelemetryService::is_running() const {
  return running_;
}

void TelemetryService::send_read_request(uint32_t register_addr) {
  // Build the read packet using frame_builder
  uint8_t packet_raw[11];  // Class 10 read packets are always 11 bytes
  protocol::build_class10_read(register_addr, packet_raw);
  
  std::vector<uint8_t> packet(packet_raw, packet_raw + 11);
  
  ESP_LOGV(TAG, "Queueing READ request for register 0x%06" PRIX32, register_addr);
  
  // Telemetry replies are routed by object type in on_packet().
  this->transport_.send_command(packet);
}

void TelemetryService::poll() {
  if (!running_) {
    ESP_LOGD(TAG, "Service not running, skipping poll");
    return;
  }
  
  ESP_LOGD(TAG, "Polling telemetry registers...");
  
  // Queue energy first when due, so this poll's unawaited reads cannot leave
  // unidentified refusals ahead of its awaited request.
  poll_energy_();

  // Queue all read requests. The transport layer will pace them.
  send_read_request(0x570045); // Motor state
  send_read_request(0x5D0122); // Flow/pressure
  send_read_request(0x5D012C); // Temperature
  if (!fault_poll_pending_ && !fault_poll_failed_) {
    fault_poll_pending_ = true;
    send_fault_read_request(false);
  }
}

void TelemetryService::on_packet(const uint8_t* data, size_t len) {
  protocol::Class10ObjectView object;
  if (!protocol::parse_class10_object(data, len, object)) {
    return;
  }

  // Profile 52_7 defines these layouts. Motor v3's 41-byte body is also
  // confirmed by HWR captures (00 01 00 03). Versions share the decoded prefix.
  // Bound the decoder window to the declared body, excluding CRC/trailing data.
  const size_t object_end = 13 + object.body_len;
  switch (object.type) {
    case 256:  // ProtectedMotorStateDetails
      if ((object.version == 1 && object.body_len >= 37) ||
          ((object.version == 2 || object.version == 3) && object.body_len >= 41)) {
        handle_motor_state_response(data, object_end);
      }
      break;

    case 565:  // PumpedMediaRelatedProcessValuesExtended
      if (object.version == 2 && object.body_len >= 36) {
        handle_flow_pressure_response(data, object_end);
      }
      break;

    case 534:  // MediaTemperatureInfo; v2 appends steady_state to v1's floats
      if ((object.version == 1 && object.body_len >= 12) ||
          (object.version == 2 && object.body_len >= 13)) {
        handle_temperature_response(data, object_end);
      }
      break;

    case 303:  // OperationStatusRequest
      if (object.version == 1 && object.body_len >= 7) {
        handle_control_status(object.body, object.body_len);
      }
      break;

    default:
      ESP_LOGV(TAG, "Unhandled telemetry type %u v%u", object.type, object.version);
      break;
  }
}

void TelemetryService::send_fault_read_request(bool warnings) {
  const uint32_t generation = fault_generation_;
  // Active alarms and warnings both reply as FaultsByArrayExtended (570 v1).
  // Only the serialized request identifies the instance; no reply echoes 88/0
  // or 88/11. Transport validates the complete object before the callback.
  transport_.send_class10_object_read(88, warnings ? 11 : 0, 570, 1,
      [this, warnings, generation](bool success, const uint8_t* payload, size_t len) {
        if (generation != fault_generation_) {
          return;
        }
        protocol::AlarmWarningTelemetry faults;
        if (!running_ || !success || !protocol::decode_faults_payload(payload, len, faults)) {
          // A late alarm reply is indistinguishable from a warning reply. Do not
          // send the next same-type request after a failed read; reconnect must
          // establish a fresh session before fault polling resumes.
          fault_poll_pending_ = false;
          fault_poll_failed_ = true;
          ESP_LOGW(TAG, "Fault read failed; waiting for reconnect before polling faults again");
          return;
        }
        if (sensor_publisher_) {
          if (warnings) {
            sensor_publisher_->publish_warnings(faults.codes);
          } else {
            sensor_publisher_->publish_alarms(faults.codes);
          }
        }
        if (warnings) {
          fault_poll_pending_ = false;
        } else {
          send_fault_read_request(true);
        }
      });
}

void TelemetryService::set_energy_read_state_(EnergyReadState state, const char* reason) {
  if (energy_read_state_ == state) {
    return;
  }
  energy_read_state_ = state;
  if (state == EnergyReadState::UNSUPPORTED) {
    ESP_LOGW(TAG, "Energy counter unsupported (%s); disabling energy reads until reconnect",
             reason);
  } else if (state == EnergyReadState::FAILED) {
    ESP_LOGW(TAG, "Energy read failed (%s); preserving the last value and retrying "
                  "on its normal cadence", reason);
  } else if (state == EnergyReadState::AVAILABLE) {
    ESP_LOGI(TAG, "Energy counter available");
  }
}

void TelemetryService::poll_energy_() {
  if (sensor_publisher_ == nullptr || !sensor_publisher_->has_energy_sensor() ||
      energy_read_pending_ || energy_read_state_ == EnergyReadState::UNSUPPORTED) {
    return;
  }
  const uint32_t now = millis();
  if (energy_attempted_ && now - last_energy_attempt_ms_ < energy_update_interval_ms_) {
    return;
  }
  energy_attempted_ = true;
  last_energy_attempt_ms_ = now;
  energy_read_pending_ = true;
  const uint32_t generation = energy_generation_;
  // OverallEnergyConsumption (type 232 v1); successful payloads are validated
  // by Transport before reaching the decoder. No passive energy publication.
  transport_.send_class10_object_read(87, 1, 232, 1,
      [this, generation](bool success, const uint8_t* payload, size_t len) {
        if (generation != energy_generation_ || !running_) {
          return;
        }
        energy_read_pending_ = false;
        if (!success) {
          protocol::ShortClass10Reply reply;
          if (protocol::parse_short_class10_reply(payload, len, reply)) {
            if (reply.ack == protocol::ApduAck::UNKNOWN_CLASS ||
                reply.ack == protocol::ApduAck::UNKNOWN_DATA_ITEM) {
              set_energy_read_state_(EnergyReadState::UNSUPPORTED,
                                     protocol::short_class10_reply_name(reply));
              return;
            }
            set_energy_read_state_(EnergyReadState::FAILED, protocol::short_class10_reply_name(reply));
            return;
          }
          set_energy_read_state_(EnergyReadState::FAILED);
          return;
        }
        protocol::EnergyTelemetry energy = protocol::decode_energy_payload(payload, len);
        if (energy.has_energy) {
          set_energy_read_state_(EnergyReadState::AVAILABLE);
          sensor_publisher_->publish_energy(energy);
        } else {
          set_energy_read_state_(EnergyReadState::FAILED);
        }
      });
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

void TelemetryService::handle_control_status(const uint8_t* body, size_t len) {
  if (len < 7 || !control_service_) {
    return;
  }

  // OperationStatusRequest v1: source, operation mode, control mode, setpoint.
  const uint8_t control_source = body[0];
  const uint8_t operation_mode = body[1];
  const uint8_t control_mode = body[2];
  const float setpoint = protocol::decode_float_be(body, 3);
  control_service_->update_mode_from_notification(control_mode, operation_mode, setpoint, control_source);
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
