/**
 * BLE Transport Layer Implementation
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#include "transport.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "frame_builder.h"
#include "frame_parser.h"
#include "response_match.h"
#include <algorithm>
#include <cinttypes>

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
          ESP_LOGD(TAG, "Command timeout (wildcard match) — pump did not respond (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   now, cmd.timestamp_ms, cmd.timeout_ms);
        } else if (cmd.quiet_timeout) {
          // Fire-and-forget write (e.g. schedule layer commit): the pump commits
          // on timeout and its ACK arrives outside the response window, so this
          // timeout is the expected settling path, not an error. See the write
          // callsites in schedule_service.cpp, which treat it as success.
          ESP_LOGD(TAG, "Command timeout (expected, fire-and-forget) for Obj %d Sub %d (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   cmd.expect_obj_id, cmd.expect_sub_id, now, cmd.timestamp_ms, cmd.timeout_ms);
        } else {
          ESP_LOGW(TAG, "Command timeout waiting for Obj %d Sub %d (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   cmd.expect_obj_id, cmd.expect_sub_id, now, cmd.timestamp_ms, cmd.timeout_ms);
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
                             uint16_t expect_sub_id, CommandCallback callback, uint32_t timeout_ms,
                             bool allow_register_read, bool expect_short_ack, bool quiet_timeout) {
  Command cmd;
  cmd.packet = packet;
  cmd.expect_obj_id = expect_obj_id;
  cmd.expect_sub_id = expect_sub_id;
  cmd.callback = callback;
  cmd.timeout_ms = timeout_ms;
  cmd.allow_register_read = allow_register_read;
  cmd.expect_short_ack = expect_short_ack;
  cmd.quiet_timeout = quiet_timeout;

  this->command_queue_.push_back(cmd);
  ESP_LOGV(TAG, "Command queued (queue size: %zu)", this->command_queue_.size());
}

void Transport::send_apdu_command(const uint8_t* apdu, size_t apdu_len,
                                  uint16_t expect_obj_id, uint16_t expect_sub_id,
                                  CommandCallback callback, uint32_t timeout_ms,
                                  bool allow_register_read, bool expect_short_ack,
                                  bool quiet_timeout) {
  uint8_t packet_raw[256];
  // build_geni_packet uses SERVICE_ID_HIGH (0xE7) and SOURCE_ADDRESS (0xF8) automatically
  size_t packet_len = protocol::build_geni_packet(
      protocol::SERVICE_ID_HIGH, protocol::SOURCE_ADDRESS,
      apdu, apdu_len, packet_raw);

  if (packet_len == 0) {
    // APDU too large to fit in a single GENI frame (build_geni_packet returns 0).
    // Fail fast instead of queuing an empty command that would never be sent
    // or matched, leaving the caller waiting until timeout.
    ESP_LOGE(TAG, "send_apdu_command: failed to build GENI frame (apdu_len=%zu)", apdu_len);
    if (callback) {
      callback(false, nullptr, 0);
    }
    return;
  }

  std::vector<uint8_t> packet(packet_raw, packet_raw + packet_len);
  
  this->send_command(packet, expect_obj_id, expect_sub_id, callback, timeout_ms, allow_register_read, expect_short_ack, quiet_timeout);
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
  // A continuation fragment may legitimately begin with 0x24/0x27 -- those are
  // ordinary payload bytes mid-frame. Treating such a fragment as a new packet
  // discards the frame being reassembled and dispatches the fragment as a runt
  // (observed 8 times in the reference captures: ~0.02% frame loss, masked only
  // because the runt then fails CRC). So a frame start only begins a new packet
  // when we are not mid-frame -- with a staleness guard so a truncated frame
  // cannot wedge reassembly forever.
  bool stale = reassembling_ &&
               (millis() - reassembly_started_ms_) > REASSEMBLY_TIMEOUT_MS;
  if (stale) {
    ESP_LOGW(TAG, "Abandoning stale partial frame (%d/%d bytes after %" PRIu32 " ms)",
             (int) reassembly_buffer_.size(), expected_packet_length_,
             millis() - reassembly_started_ms_);
    reassembling_ = false;
    reassembly_buffer_.clear();
    expected_packet_length_ = 0;
  }

  if (is_frame_start(data[0]) && !reassembling_) {
    // New packet starting
    ESP_LOGV(TAG, "New packet detected (frame start: 0x%02X)", data[0]);

    reassembly_buffer_.clear();
    reassembly_buffer_.insert(reassembly_buffer_.end(), data, data + len);
    reassembling_ = true;
    reassembly_started_ms_ = millis();

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

     // Trim to the declared frame length before anything looks at the bytes.
     // The completion test above is `>=`, so a notification carrying trailing
     // bytes leaves them in the buffer; they are outside what the CRC covers,
     // and parse_frame() clamps the same way for the same reason.
     size_t frame_len = reassembly_buffer_.size();
     if (expected_packet_length_ >= 4 && frame_len > expected_packet_length_) {
       frame_len = expected_packet_length_;
     }

     // Reject a corrupt frame before it can answer a command.
     //
     // Only telemetry checked the CRC (via parse_frame); the command-response
     // path did not, so every control, schedule, single-event, event-log and
     // device-info payload -- including the readbacks that decide write
     // verdicts -- was parsed from unverified bytes. A runt produced by a
     // mid-frame fragment, or any radio corruption, could satisfy the
     // class/object match and be taken for the answer to a queued command.
     if (!protocol::frame_crc_valid(reassembly_buffer_.data(), frame_len)) {
       ESP_LOGW(TAG, "Dropping %u-byte frame with a bad CRC", (unsigned) frame_len);
       reassembling_ = false;
       reassembly_buffer_.clear();
       expected_packet_length_ = 0;
       return;
     }

     // Try to dispatch to registered response handler first
     bool dispatched = try_dispatch_response(reassembly_buffer_.data(), frame_len);

     // If not dispatched to a handler, invoke general packet callback
     if (!dispatched && packet_callback_) {
       packet_callback_(reassembly_buffer_.data(), frame_len);
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
          ESP_LOGW(TAG, "Response handler timeout for Object %d SubID %d (%" PRIu32 " ms)",
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
  // Class 3 and Class 7 responses can be as short as 8 bytes (e.g. a Class 3
  // command ACK: [Start][Len][Dest][SvcH][Class][Ack][CRC-H][CRC-L]) -- well
  // under the 12-byte minimum the Class 10 DataObject path below requires.
  // Handle their wildcard-by-class-byte matching first, before that length
  // gate, so short responses aren't discarded before we even look at them.
  if (len >= 5 && this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    // What class was the *queued* command itself sent as? (byte 4 of the
    // outgoing GENI frame is the class byte, same offset as in responses.)
    // Only match a Class 3/7 response against a command that was actually
    // sent as that same class -- otherwise an unrelated Class 10 telemetry
    // notification could be mistaken for the response to a queued Class 3
    // command (or vice versa).
    uint8_t queued_class = (cmd.packet.size() > 4) ? cmd.packet[4] : 0x00;

    // Class 3: command ACK ([03 00] = success/clean, [03 01 xx] = rejected/
    // descriptor-only -- see ControlService::send_remote_mode_command()).
    // Class 7: device info strings, etc.
    // Both use a different packet structure than Class 10 DataObjects, so
    // when expect_obj_id == 0 && expect_sub_id == 0 we match by class byte
    // alone -- but only when the queued command was sent as that class.
    if (protocol::class3_or_7_wildcard_matches(
            queued_class, data[4],
            cmd.expect_obj_id == 0x0000 && cmd.expect_sub_id == 0x0000)) {
      ESP_LOGV(TAG, "Class %d response matched (wildcard match by class byte)",
               data[4] == protocol::CLASS_3_COMMAND_ACK ? 3 : 7);
      if (cmd.callback) {
        cmd.callback(true, data, len);
      }
      this->command_queue_.pop_front();
      this->state_ = State::IDLE;
      return true;
    }

    // If the queued command was itself Class 3 or Class 7 but this response
    // is neither (e.g. a Class 10 telemetry notification arrived first),
    // it's definitely not our answer -- let it fall through to the general
    // packet callback instead of risking the Class 10 wildcard path below
    // matching it by accident.
    if (protocol::ignore_unrelated_while_awaiting_class3_or_7(queued_class, data[4])) {
      ESP_LOGV(TAG, "Awaiting Class %d response, ignoring unrelated Class 0x%02X packet",
               queued_class, data[4]);
      return false;
    }

    // Some ALPHA HWR Class 10 SET commands (notably Object 91/Sub 430
    // temperature-range writes using OpSpec 0x97, 0x96, 0x95, or 0x91, and
    // Object 0601/Sub 5600 mode writes using OpSpec 0x90) are ACKed with a short
    // Class 10 OpSpec 0x01 frame that does not carry Obj/Sub fields.
    // Handle that before the generic len>=12 DataObject parser below.
    if (queued_class == 0x0A && len >= 6 && data[4] == 0x0A && (data[5] == 0x01 || data[5] == 0x81) &&
        cmd.expect_obj_id == 0x0000 && cmd.expect_sub_id == 0x0000 &&
        cmd.packet.size() > 9 &&
        (cmd.packet[5] == 0x97 || cmd.packet[5] == 0x96 || cmd.packet[5] == 0xB3 ||
         cmd.packet[5] == 0x95 || cmd.packet[5] == 0x91 || cmd.packet[5] == 0x90 ||
         cmd.packet[5] == 0x8F) &&  // queued OpSpec (0x8F: DHW config write, #106)
        ((cmd.packet[6] == 91 && cmd.packet[7] == 0x01 && cmd.packet[8] == 0xAE) || // old format
         (cmd.packet[6] == 91 && cmd.packet[7] == 0x01 && cmd.packet[8] == 0xA5) || // Obj 91 Sub 421 DHW config (#106)
         (cmd.packet[6] == 0x01 && cmd.packet[7] == 0xAE && cmd.packet[8] == 0x00 && cmd.packet[9] == 91) || // new format SubID 430 Obj 91
         (cmd.packet[6] == 0x56 && cmd.packet[7] == 0x00 && cmd.packet[8] == 0x06 && cmd.packet[9] == 0x01))) {  // Sub 5600 Obj 0601 (mode write)
      uint8_t err_code = (len >= 7) ? data[6] : 0xFF;
      ESP_LOGI(TAG, "Matched short Class 10 ACK (OpSpec 0x%02X) for Class 10 SET write. ErrCode=0x%02X", data[5], err_code);
      if (cmd.callback) {
        bool success = (data[5] == 0x01) || (data[5] == 0x81 && err_code == 0x00);
        cmd.callback(success, data, len);
      }
      this->command_queue_.pop_front();
      this->state_ = State::IDLE;
      return true;
    }
  }

  // Early safety check: packet must contain at least minimum header (9 bytes) + CRC (2 bytes)
  if (len < 11) {
    ESP_LOGV(TAG, "Packet too short (%zu bytes, need >= 11) to be a valid response", len);
    return false;
  }
  
  // Extract payload: skip header (10 bytes) and CRC (2 bytes)
  const uint8_t* payload = (len >= 12) ? (data + 10) : nullptr;
  size_t payload_len = (len >= 12) ? (len - 12) : 0;

  // Extract identifiers from the response packet
  uint8_t opspec = data[5];
  uint16_t packet_sub_id = (data[6] << 8) | data[7];
  uint16_t packet_obj_id = (data[8] << 8) | data[9];

  // Log incoming packets at verbose level when waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    ESP_LOGV(TAG, "[AWAITING] Packet received: len=%d, Class=%02X, OpSpec=%02X, Sub=%d, Obj=%d (waiting for Obj %d Sub %d)",
             len, data[4], opspec, packet_sub_id, packet_obj_id, cmd.expect_obj_id, cmd.expect_sub_id);
  }

  // 1. Check if we are waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    
    // Check if this is a Class 10 packet (0x0A at byte 4)
    bool is_class10 = (data[4] == 0x0A);
    
    if (!is_class10) {
      ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), discarding for command response matching", data[4]);
      return false;  // Not Class 10 and not handled above, let it go to packet callback or discard
    }
    
    // This IS a Class 10 response. Now check if it matches our expected Object/Sub ID
    // Extract OpSpec
    opspec = data[5];
    
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
    // Frame structure depends on OpSpec!
    // OpSpec 0x0E (Passive Notif): [SubH][SubL][ObjH][ObjL][Payload...]
    // OpSpec 0x02 (Positive ACK):  [Obj(1 byte)][SubH][SubL][Payload...]
    if (opspec == 0x0E || opspec == 0x01) {
      if (len >= 10) {
        // Here GENI defines bytes 6-7 as SubID, 8-9 as ObjID.
        // We will store them correctly to avoid confusion, even though older code might have them swapped.
        packet_sub_id = (data[6] << 8) | data[7];
        packet_obj_id = (data[8] << 8) | data[9];
        // The payload starts at data + 10 for these!
        // But our global payload extraction above did data + 10. We will keep it.
      } else {
        ESP_LOGV(TAG, "DataObject 0x0E packet too short to extract IDs");
        return false;
      }
    } else if (opspec == 0x02) {
      if (len >= 9) {
        packet_obj_id = data[6];                     // 1 byte ObjID
        packet_sub_id = (data[7] << 8) | data[8];    // 2 bytes SubID
        // Note: Payload starts at data + 9!
        // So we MUST override the payload extraction for this opspec!
        payload = data + 9;
        payload_len = len - 11; // len - header(9) - CRC(2)
      } else {
        ESP_LOGV(TAG, "DataObject 0x02 packet too short to extract IDs");
        return false;
      }
    } else {
      // Fallback for other opspecs (e.g. 0x90, 0x97 short ACKs already handled above, etc)
      // Assume 2-byte Sub, 2-byte Obj for safety if >= 10
      if (len >= 10) {
        packet_sub_id = (data[6] << 8) | data[7];
        packet_obj_id = (data[8] << 8) | data[9];
      } else {
        return false;
      }
    }
    
    // Now check if this matches our expected Object/Sub ID
    bool matched = false;
    
    // WILDCARD MATCH: If expect_obj_id == 0, accept ANY Class 10 packet
    // This is used for Object 86 Sub 6 reads, which receive passive notifications (OpSpec 0x0E)
    // Reference: Python base.py::match_class10_response only checks p[4] == 0x0A
    if (cmd.expect_obj_id == 0x0000 && cmd.expect_sub_id == 0x0000 && !cmd.expect_short_ack) {
      matched = true;
      ESP_LOGV(TAG, "Wildcard match: accepting any Class 10 packet (OpSpec=0x%02X, Obj=%d, Sub=%d)",
               opspec, packet_obj_id, packet_sub_id);
    } else {
      // Exact match: check Object ID and Sub-ID
      matched = (packet_obj_id == cmd.expect_obj_id && (packet_sub_id == cmd.expect_sub_id || packet_sub_id == 0));
      
      // BACKUP MATCH: If ObjID doesn't match but SubID matches our expected ObjID (swapped)
      if (!matched && cmd.expect_obj_id != 0 && packet_sub_id == cmd.expect_obj_id) {
        matched = true;
      }
    }
    
    // SPECIAL CASE WORKAROUND: Object 91 config reads (Cache Sync / DHW config)
    // The pump responds to these read requests with a size-specific OpSpec
    // (0x15 for the 14-byte Sub 430 temperature-range settings, 0x0D for the
    // 6-byte Sub 421 DHW on/off configuration -- bench/capture-verified, see
    // issue #106) which does NOT carry the standard Obj/Sub IDs in the payload
    // header. Python reference simply accepted these via a wildcard match and
    // sliced at byte 10. We explicitly handle the known cases here to avoid
    // wildcard matching other packets.
    if (!matched && cmd.expect_obj_id == 91 && len >= 12 &&
        ((cmd.expect_sub_id == 430 && opspec == 0x15) ||
         (cmd.expect_sub_id == 421 && opspec == 0x0D))) {
      matched = true;
      payload = data + 10;
      payload_len = len - 12; // len - 10(header) - 2(CRC)
      ESP_LOGV(TAG, "Matched Object 91 Sub %d using OpSpec 0x%02X workaround",
               cmd.expect_sub_id, opspec);
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
  opspec = data[5];

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
  packet_obj_id = (data[6] << 8) | data[7];  // Object ID is 16-bit big-endian at bytes 6-7
  packet_sub_id = (data[8] << 8) | data[9];  // Sub-ID is 16-bit big-endian at bytes 8-9

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
