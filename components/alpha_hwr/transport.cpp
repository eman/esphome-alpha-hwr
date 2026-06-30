/**
 * BLE Transport Layer Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#include "transport.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <algorithm>

namespace esphome {
namespace alpha_hwr {
namespace core {

static const char *const TAG = "alpha_hwr.transport";

Transport::Transport() 
    : reassembling_(false),
      expected_packet_length_(0),
      packet_callback_(nullptr) {
  reassembly_buffer_.reserve(MAX_PACKET_SIZE);
  ESP_LOGD(TAG, "Transport initialized");
}

void Transport::set_packet_callback(PacketCallback callback) {
  this->packet_callback_ = callback;
}

void Transport::loop() {
  if (this->command_queue_.empty()) {
    this->state_ = State::IDLE;
    return;
  }

  uint32_t now = millis();
  auto &cmd = this->command_queue_.front();

  switch (this->state_) {
    case State::IDLE:
      // Check pacing between commands
      if (now - this->last_send_time_ < this->send_pacing_ms_) {
        return;
      }
      this->state_ = State::SENDING_CHUNKS;
      cmd.bytes_sent = 0;
      // Fall through to SENDING_CHUNKS
      [[fallthrough]];

    case State::SENDING_CHUNKS: {
      // Check pacing between chunks
      if (now - this->last_send_time_ < this->send_pacing_ms_) {
        return;
      }

      size_t remaining = cmd.packet.size() - cmd.bytes_sent;
      size_t to_send = std::min(remaining, BLE_MTU_LIMIT);

      ESP_LOGV(TAG, "Sending chunk: %zu bytes (%zu/%zu sent)", 
               to_send, cmd.bytes_sent + to_send, cmd.packet.size());

      if (!this->write_callback_) {
        ESP_LOGW(TAG, "Write callback not set, dropping command");
        if (cmd.callback) {
          cmd.callback(false, nullptr, 0);
        }
        this->command_queue_.pop_front();
        this->state_ = State::IDLE;
        break;
      }
      if (this->write_callback_(cmd.packet.data() + cmd.bytes_sent, to_send)) {
        cmd.bytes_sent += to_send;
        this->last_send_time_ = now;

        if (cmd.bytes_sent >= cmd.packet.size()) {
          // Finished sending all chunks
          // If a callback is registered, we expect a response (including wildcard matches where obj_id=0)
          if (cmd.callback) {
            this->state_ = State::AWAITING_RESPONSE;
            cmd.timestamp_ms = now;
            cmd.waiting_for_response = true;
            ESP_LOGV(TAG, "Command sent, waiting for response (Obj %d Sub %d)", 
                     cmd.expect_obj_id, cmd.expect_sub_id);
          } else {
            ESP_LOGV(TAG, "Command sent (no response expected)");
            this->command_queue_.pop_front();
            this->state_ = State::IDLE;
          }
        }
      } else {
        ESP_LOGE(TAG, "Failed to send chunk, dropping command");
        if (cmd.callback) {
          cmd.callback(false, nullptr, 0);
        }
        this->command_queue_.pop_front();
        this->state_ = State::IDLE;
      }
      break;
    }

    case State::AWAITING_RESPONSE:
      if (now - cmd.timestamp_ms > cmd.timeout_ms) {
        // Wildcard commands (obj=0, sub=0) are used when the response cannot
        // be matched by Object/Sub-ID — this covers both optional feature reads
        // (trend data, device info) and protocol operations where the pump's
        // response uses an OpSpec that doesn't carry standard Obj/Sub fields
        // (e.g., control-mode writes that reply with OpSpec 0x15). A timeout
        // here means either the feature is absent or the window closed; either
        // way it is expected behaviour — log at DEBUG to avoid noise.
        if (cmd.expect_obj_id == 0 && cmd.expect_sub_id == 0) {
          ESP_LOGD(TAG, "Command timeout (wildcard match) — pump did not respond");
        } else {
          ESP_LOGW(TAG, "Command timeout waiting for Obj %d Sub %d",
                   cmd.expect_obj_id, cmd.expect_sub_id);
        }
        if (cmd.callback) {
          cmd.callback(false, nullptr, 0);
        }
        this->command_queue_.pop_front();
        this->state_ = State::IDLE;
      }
      break;

    default:
      break;
  }
}

void Transport::send_command(const std::vector<uint8_t>& packet, uint16_t expect_obj_id, 
                            uint16_t expect_sub_id, CommandCallback callback, 
                            uint32_t timeout_ms, bool allow_register_read) {
  Command cmd;
  cmd.packet = packet;
  cmd.expect_obj_id = expect_obj_id;
  cmd.expect_sub_id = expect_sub_id;
  cmd.callback = callback;
  cmd.timeout_ms = timeout_ms;
  cmd.allow_register_read = allow_register_read;
  
  this->command_queue_.push_back(cmd);
  ESP_LOGV(TAG, "Command queued (queue size: %zu)", this->command_queue_.size());
}

bool Transport::is_frame_start(uint8_t byte) {
  return (byte == FRAME_START_RESPONSE || byte == FRAME_START_REQUEST);
}

uint16_t Transport::calculate_expected_length() const {
  if (reassembly_buffer_.size() < 2) {
    return 0;
  }
  // GENI packet: [Start(1)][Length(1)][Dst][Src][...APDU...][CRC_H][CRC_L]
  // Length field counts bytes after itself: Dst + Src + APDU (excludes Start, Length, CRC)
  // Total = 1(Start) + 1(Length) + Length_field + 2(CRC) = Length_field + 4
  return reassembly_buffer_[1] + 4;
}

void Transport::on_notification(const uint8_t* data, size_t len) {
  if (len == 0) {
    return;
  }

  ESP_LOGV(TAG, "BLE notification: %d bytes", len);

  // Check if this is the start of a new packet
  // Frame start bytes: 0x24 (response) or 0x27 (request/echo)
  if (is_frame_start(data[0])) {
    // New packet starting
    ESP_LOGV(TAG, "New packet detected (frame start: 0x%02X)", data[0]);
    
    reassembly_buffer_.clear();
    reassembly_buffer_.insert(reassembly_buffer_.end(), data, data + len);
    reassembling_ = true;

    // Calculate expected length
    if (reassembly_buffer_.size() >= 2) {
      expected_packet_length_ = calculate_expected_length();
      ESP_LOGV(TAG, "Expected packet length: %d bytes", expected_packet_length_);
    }
  } else if (reassembling_) {
    // Continuation of existing packet
    reassembly_buffer_.insert(reassembly_buffer_.end(), data, data + len);
    ESP_LOGV(TAG, "Packet reassembly: %d/%d bytes", 
             reassembly_buffer_.size(), expected_packet_length_);
  } else {
    // Unexpected data (not a frame start, not reassembling)
    ESP_LOGW(TAG, "Unexpected notification data (not frame start, not reassembling)");
    return;
  }

  // Safety: Check buffer overflow
  if (reassembly_buffer_.size() > MAX_PACKET_SIZE) {
    ESP_LOGW(TAG, "Reassembly buffer overflow (%d bytes), clearing", 
             reassembly_buffer_.size());
    reset();
    return;
  }

   // Check if packet is complete
   if (reassembling_ && expected_packet_length_ > 0 && 
       reassembly_buffer_.size() >= expected_packet_length_) {
     ESP_LOGV(TAG, "Packet complete: %d bytes", reassembly_buffer_.size());

     // Log first 12 bytes for debugging packet structure
     if (reassembly_buffer_.size() >= 12) {
       ESP_LOGV(TAG, "Packet bytes [0-11]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                reassembly_buffer_[0], reassembly_buffer_[1], reassembly_buffer_[2], reassembly_buffer_[3],
                reassembly_buffer_[4], reassembly_buffer_[5], reassembly_buffer_[6], reassembly_buffer_[7],
                reassembly_buffer_[8], reassembly_buffer_[9], reassembly_buffer_[10], reassembly_buffer_[11]);
     }

     // Try to dispatch to registered response handler first
     bool dispatched = try_dispatch_response(reassembly_buffer_.data(), reassembly_buffer_.size());

     // If not dispatched to a handler, invoke general packet callback
     if (!dispatched && packet_callback_) {
       packet_callback_(reassembly_buffer_.data(), reassembly_buffer_.size());
     } else if (!dispatched) {
       ESP_LOGW(TAG, "Complete packet received but no handler or callback registered");
     }

     // Clear state for next packet
     reassembling_ = false;
     reassembly_buffer_.clear();
     expected_packet_length_ = 0;
   }
}

void Transport::reset() {
  ESP_LOGD(TAG, "Resetting transport state");
  reassembling_ = false;
  reassembly_buffer_.clear();
  expected_packet_length_ = 0;
  // Discard any queued commands and pending response handlers so stale BLE
  // writes from the previous connection do not execute on the next connect.
  command_queue_.clear();
  pending_handlers_.clear();
  state_ = State::IDLE;
}

void Transport::register_response_handler(uint16_t object_id, uint16_t sub_id, ResponseCallback callback) {
  // Safety: Limit number of pending handlers to prevent memory issues
  if (pending_handlers_.size() >= MAX_PENDING_HANDLERS) {
    ESP_LOGW(TAG, "Too many pending handlers (%d), rejecting new handler for Object %d SubID %d",
             pending_handlers_.size(), object_id, sub_id);
    return;
  }

  // Get current timestamp (millis() equivalent)
  uint32_t now = millis();

  PendingHandler handler;
  handler.object_id = object_id;
  handler.sub_id = sub_id;
  handler.callback = callback;
  handler.timestamp_ms = now;

  pending_handlers_.push_back(handler);

  ESP_LOGV(TAG, "Registered response handler for Object %d SubID %d (total pending: %d)",
           object_id, sub_id, pending_handlers_.size());
}

void Transport::check_timeouts(uint32_t timeout_ms) {
  if (pending_handlers_.empty()) {
    return;
  }

  uint32_t now = millis();
  size_t initial_count = pending_handlers_.size();

  // Remove timed-out handlers
  // Use erase-remove idiom to remove elements while iterating
  pending_handlers_.erase(
    std::remove_if(pending_handlers_.begin(), pending_handlers_.end(),
      [now, timeout_ms](const PendingHandler& handler) {
        uint32_t age = now - handler.timestamp_ms;
        if (age > timeout_ms) {
          ESP_LOGW(TAG, "Response handler timeout for Object %d SubID %d (%d ms)",
                   handler.object_id, handler.sub_id, age);
          return true;  // Remove this handler
        }
        return false;  // Keep this handler
      }),
    pending_handlers_.end()
  );

  size_t removed = initial_count - pending_handlers_.size();
  if (removed > 0) {
    ESP_LOGD(TAG, "Removed %d timed-out handlers (%d remaining)", removed, pending_handlers_.size());
  }
}

bool Transport::try_dispatch_response(const uint8_t* data, size_t len) {
  // Early safety check: packet must contain at least header (10 bytes) + CRC (2 bytes)
  if (len < 12) {
    ESP_LOGV(TAG, "Packet too short (%zu bytes, need >= 12) to be a valid response", len);
    return false;
  }

  // Extract payload: skip header (10 bytes) and CRC (2 bytes)
  const uint8_t* payload = data + 10;
  size_t payload_len = len - 12;

  // Extract identifiers from the response packet
  uint8_t opspec = (len > 5) ? data[5] : 0x00;
  uint16_t packet_sub_id = (len > 7) ? (data[6] << 8) | data[7] : 0;
  uint16_t packet_obj_id = (len > 9) ? (data[8] << 8) | data[9] : 0;

  // Log incoming packets at verbose level when waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    if (len > 5) {
      ESP_LOGV(TAG, "[AWAITING] Packet received: len=%d, Class=%02X, OpSpec=%02X, Sub=%d, Obj=%d (waiting for Obj %d Sub %d)",
               len, (len > 4 ? data[4] : 0xFF), opspec, packet_sub_id, packet_obj_id, cmd.expect_obj_id, cmd.expect_sub_id);
    }
  }

  // 1. Check if we are waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    
    // First, validate basic packet structure
    if (len < 12) {
      ESP_LOGV(TAG, "Packet too short, discarding");
      return false;  // Too short, discard it
    }
    
    // Check if this is a Class 10 packet (0x0A at byte 4)
    bool is_class10 = (data[4] == 0x0A);
    
    // Check if this is a Class 7 packet (0x07 at byte 4)
    bool is_class7 = (data[4] == 0x07);
    
    // Handle Class 7 responses (device info strings, etc.)
    // Class 7 uses a different packet structure: [STX][LEN][DST][SRC][Class][Cmd][ID][...STRING...][CRC]
    // When expect_obj_id == 0 && expect_sub_id == 0, we match by Class byte only
    if (is_class7 && cmd.expect_obj_id == 0x0000 && cmd.expect_sub_id == 0x0000) {
      ESP_LOGV(TAG, "Class 7 response matched (wildcard match by class byte)");
      if (cmd.callback) {
        cmd.callback(true, data, len);
      }
      this->command_queue_.pop_front();
      this->state_ = State::IDLE;
      return true;
    }
    
    if (!is_class10) {
      ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), discarding for command response matching", data[4]);
      return false;  // Not Class 10 and not handled above, let it go to packet callback or discard
    }
    
    // This IS a Class 10 response. Now check if it matches our expected Object/Sub ID
    // Extract OpSpec
    opspec = (len > 5) ? data[5] : 0x00;
    
    // Determine packet structure based on OpSpec
    bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 || 
                             opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
    
     if (is_register_read && !cmd.allow_register_read) {
       // This is telemetry register-read response, not a DataObject response
       // Discard it for command matching purposes (unless command explicitly allows it)
       ESP_LOGV(TAG, "Class 10 register-read (OpSpec=0x%02X), skipping for command response (waiting for Obj %d Sub %d)", 
                opspec, cmd.expect_obj_id, cmd.expect_sub_id);
       return false;
     }
    
    // This is a Class 10 DataObject response. Extract Object/Sub IDs
    // Frame structure: [STX][LEN][DST][SRC][Class][OpSpec][ObjH][ObjL][SubH][SubL]...[CRC]
    // So bytes 6-7 are Object ID (2 bytes, big-endian), bytes 8-9 are Sub-ID (2 bytes, big-endian)
    if (len > 9) {
      packet_obj_id = (data[6] << 8) | data[7];  // Object ID is at bytes 6-7
      packet_sub_id = (data[8] << 8) | data[9];  // Sub-ID is at bytes 8-9
    } else {
      ESP_LOGV(TAG, "DataObject packet too short to extract IDs");
      return false;
    }
    
    // Now check if this matches our expected Object/Sub ID
    bool matched = false;
    
    // WILDCARD MATCH: If expect_obj_id == 0, accept ANY Class 10 packet
    // This is used for Object 86 Sub 6 reads, which receive passive notifications (OpSpec 0x0E)
    // Reference: Python base.py::match_class10_response only checks p[4] == 0x0A
    if (cmd.expect_obj_id == 0x0000 && cmd.expect_sub_id == 0x0000) {
      matched = true;
      ESP_LOGV(TAG, "Wildcard match: accepting any Class 10 packet (OpSpec=0x%02X, Obj=%d, Sub=%d)",
               opspec, packet_obj_id, packet_sub_id);
    } else {
      // Exact match: check Object ID and Sub-ID
      matched = (packet_obj_id == cmd.expect_obj_id && (packet_sub_id == cmd.expect_sub_id || packet_sub_id == 0));
      
      // BACKUP MATCH: If ObjID doesn't match but SubID matches our expected ObjID (swapped)
      if (!matched && packet_sub_id == cmd.expect_obj_id) {
        matched = true;
      }
    }

    if (matched) {
      ESP_LOGV(TAG, "Command response matched for Obj %d (Sub %d -> %d)", 
               packet_obj_id, cmd.expect_sub_id, packet_sub_id);
      if (cmd.callback) {
        cmd.callback(true, payload, payload_len);
      }
      this->command_queue_.pop_front();
      this->state_ = State::IDLE;
      return true;
     } else {
       // This is a Class 10 DataObject response but doesn't match what we're waiting for
       ESP_LOGV(TAG, "Class 10 DataObject MISMATCH: got Obj=0x%04X Sub=0x%04X, want Obj=0x%04X Sub=0x%04X, OpSpec=0x%02X",
                packet_obj_id, packet_sub_id, cmd.expect_obj_id, cmd.expect_sub_id, opspec);
       return false;
     }
  }

  // 2. Check registered response handlers
  if (pending_handlers_.empty()) {
    return false;  // No handlers registered
  }

  // Validate packet structure
  if (len < 12) {
    ESP_LOGV(TAG, "Packet too short for response matching (%d bytes)", len);
    return false;
  }

  // Check if this is a Class 10 response (most common for read operations)
  if (data[4] != 0x0A) {
    ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), skipping response matching", data[4]);
    return false;
  }

  // Extract OpSpec to see what kind of response this is
  opspec = (len > 5) ? data[5] : 0x00;

  packet_obj_id = 0;
  packet_sub_id = 0;
  
  // Only parse as DataObject format for non-register-read OpSpecs
  bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 || 
                           opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
  
  if (is_register_read) {
    // This is a telemetry register read - don't try to match it
    ESP_LOGV(TAG, "Skipping register-read response (OpSpec=0x%02X) for response matching", opspec);
    return false;
  }
  
  // Parse as DataObject format
  if (len > 9) {
    packet_obj_id = (data[6] << 8) | data[7];  // Object ID is 16-bit big-endian at bytes 6-7
    packet_sub_id = (data[8] << 8) | data[9];  // Sub-ID is 16-bit big-endian at bytes 8-9
  } else {
    ESP_LOGV(TAG, "Packet too short to extract Object/SubID");
    return false;
  }

  ESP_LOGV(TAG, "DataObject response: OpSpec=0x%02X, Object %d SubID %d (checking %d handlers)",
           opspec, packet_obj_id, packet_sub_id, pending_handlers_.size());

  // Search for matching handler
  for (auto it = pending_handlers_.begin(); it != pending_handlers_.end(); ++it) {
    if (it->object_id == packet_obj_id && it->sub_id == packet_sub_id) {
      ESP_LOGV(TAG, "Response handler matched for Object %d SubID %d", packet_obj_id, packet_sub_id);

      // Invoke callback with payload (protocol header has already been stripped earlier in try_dispatch_response())
      if (it->callback) {
        it->callback(payload, payload_len);
        ESP_LOGV(TAG, "Response handler invoked with %d bytes payload", payload_len);
      }

      // Remove handler (one-shot)
      pending_handlers_.erase(it);
      ESP_LOGV(TAG, "Response handler removed (%d remaining)", pending_handlers_.size());

      return true;  // Handler was found and invoked
    }
  }

  ESP_LOGV(TAG, "No matching response handler for Object %d SubID %d", packet_obj_id, packet_sub_id);
  return false;  // No matching handler found
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
