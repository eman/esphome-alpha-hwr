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
#include <utility>

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
      // A previous write died part-way through a packet, so the peer is holding
      // a partial frame that would absorb whatever we send next. Wait for its
      // staleness guard rather than feeding it. Gated here, in IDLE only, so a
      // command already in flight still gets its response and its timeout.
      if (this->peer_resync_pending_) {
        if (now - this->peer_resync_started_ms_ < PEER_RESYNC_HOLD_MS) {
          return;
        }
        ESP_LOGD(TAG, "Peer resync hold elapsed; resuming sends");
        this->peer_resync_pending_ = false;
      }
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
        this->fail_front_command_();
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
                     cmd.expect_type_low_ver, cmd.expect_type_high);
          } else {
            ESP_LOGV(TAG, "Command sent (no response expected)");
            this->command_queue_.pop_front();
            this->state_ = State::IDLE;
          }
        }
      } else {
        ESP_LOGE(TAG, "Failed to send chunk, dropping command");
        // Only when part of the frame is already on the wire. A first-chunk
        // failure put nothing there, so the peer has nothing to resynchronise
        // from and must not be made to wait a second for it.
        if (cmd.bytes_sent > 0) {
          ESP_LOGW(TAG, "Partial frame left at the peer (%zu/%zu bytes); holding sends for %" PRIu32 " ms",
                   cmd.bytes_sent, cmd.packet.size(), PEER_RESYNC_HOLD_MS);
          this->peer_resync_pending_ = true;
          this->peer_resync_started_ms_ = now;
        }
        this->fail_front_command_();
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
        if (cmd.expect_type_low_ver == 0 && cmd.expect_type_high == 0) {
          ESP_LOGD(TAG, "Command timeout (wildcard match) — pump did not respond (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   now, cmd.timestamp_ms, cmd.timeout_ms);
        } else if (cmd.quiet_timeout) {
          // A write whose caller does not treat the acknowledgement as its
          // verdict -- it confirms by reading the value back -- so silence here
          // is not an error to report, only a fact to record. It is NOT
          // "expected": every Class 10 SET in resources/traffic_capture is
          // answered, 420 of 420. The flag used to be explained as "the pump
          // commits on timeout and its ACK arrives outside the response
          // window", which was a story told about a schedule layer write that
          // was waiting for a reply the protocol forbids (issue #253).
          // quiet_timeout means "do not log this at warning" and nothing more.
          ESP_LOGD(TAG, "Command timeout (unanswered; the readback decides) for Obj %d Sub %d (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   cmd.expect_type_low_ver, cmd.expect_type_high, now, cmd.timestamp_ms, cmd.timeout_ms);
        } else {
          ESP_LOGW(TAG, "Command timeout waiting for Obj %d Sub %d (now=%" PRIu32 ", timestamp=%" PRIu32 ", timeout=%" PRIu32 ")",
                   cmd.expect_type_low_ver, cmd.expect_type_high, now, cmd.timestamp_ms, cmd.timeout_ms);
        }
        // A command that gave up is not a command whose reply cannot arrive
        // (issue #248). Nothing cancels a request in GENIbus, so the pump still
        // owes an answer -- and that answer arrives with no owner, in exactly
        // the shape the short-ACK branch accepts.
        //
        // Every timeout counts, quiet ones included. An earlier cut exempted
        // them, reasoning that a quiet timeout means silence is expected. It is
        // not: the flag is set by the Class 10 writes whose verdict comes from a
        // readback, and every one of those is acknowledged in every captured
        // instance -- 420 SETs, no exceptions. They are the likeliest sources of
        // a late reply, not the least, and exempting them left this issue's hole
        // open from 1.1 s to 4 s. `quiet_timeout` means "do not log this at
        // warning" and nothing else.
        this->note_reply_owed_(cmd.suppressed_a_frame);
        this->fail_front_command_();
        this->state_ = State::IDLE;
      }
      break;

    default:
      break;
  }
}

void Transport::send_command(const std::vector<uint8_t>& packet, uint16_t expect_type_low_ver,
                             uint16_t expect_type_high, CommandCallback callback, uint32_t timeout_ms,
                             bool allow_register_read, bool expect_short_ack, bool quiet_timeout) {
  Command cmd;
  cmd.packet = packet;
  cmd.expect_type_low_ver = expect_type_low_ver;
  cmd.expect_type_high = expect_type_high;
  cmd.callback = callback;
  cmd.timeout_ms = timeout_ms;
  cmd.allow_register_read = allow_register_read;
  cmd.expect_short_ack = expect_short_ack;
  cmd.quiet_timeout = quiet_timeout;

  this->command_queue_.push_back(cmd);
  ESP_LOGV(TAG, "Command queued (queue size: %zu)", this->command_queue_.size());
}

void Transport::send_apdu_command(const uint8_t* apdu, size_t apdu_len,
                                  uint16_t expect_type_low_ver, uint16_t expect_type_high,
                                  CommandCallback callback, uint32_t timeout_ms,
                                  bool allow_register_read, bool expect_short_ack,
                                  bool quiet_timeout) {
  // Sized to the protocol's maximum telegram, not to a round number. A GENIbus
  // telegram is at most 259 bytes (App. Prog. Manual, "Short form technical
  // specification"): start delimiter, length, DA, SA, up to MAX_PDU_LEN of PDU,
  // and two CRC bytes. This was 256, three short of a legal frame, and
  // build_geni_packet's guard was on what the length byte could hold rather
  // than on what the protocol allows -- so between them a large APDU would have
  // been built past the end of this array.
  uint8_t packet_raw[protocol::MAX_TELEGRAM_LEN];
  // build_geni_packet fills in the destination and source addresses (0xE7/0xF8)
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
  
  this->send_command(packet, expect_type_low_ver, expect_type_high, callback, timeout_ms, allow_register_read, expect_short_ack, quiet_timeout);
}

void Transport::note_reply_owed_(bool already_suppressed) {
  // A command that had a frame withheld has already been paid what it was owed
  // -- by the suppression itself. Counting it again is what turns one late
  // reply into an unbounded chain of them.
  if (already_suppressed) return;
  if (this->owed_replies_ < 0xFF) this->owed_replies_++;
  this->owed_pending_ = true;
  this->owed_since_ms_ = millis();
}

bool Transport::stale_reply_possible_() {
  if (!this->owed_pending_) return false;
  // Elapsed-since, never a stored deadline: the flag bounds the read, so the
  // subtraction is never evaluated outside the window it belongs to.
  if (millis() - this->owed_since_ms_ >= STALE_REPLY_WINDOW_MS) {
    ESP_LOGV(TAG, "Stale-reply window elapsed with %u still owed; clearing",
             (unsigned) this->owed_replies_);
    this->owed_pending_ = false;
    this->owed_replies_ = 0;
    return false;
  }
  return this->owed_replies_ > 0;
}

bool Transport::is_frame_start(uint8_t byte) {
  // 0x24 only. This used to accept 0x27 as well, on the strength of a comment
  // calling it "Request frame (client -> pump, also echoed back)" -- and the
  // captures do not support the echo. Across the 44,200 CRC-valid frames of
  // resources/traffic_capture, reassembled from ATT fragments and
  // de-duplicated, all 22,138 phone->pump frames begin 0x27 and all 22,062
  // pump->phone frames begin 0x24. Not one inbound frame begins 0x27.
  //
  // on_notification() is fed GATT notifications and nothing else, so it never
  // sees our own writes; accepting 0x27 admitted a byte that cannot legitimately
  // start a frame here. It is also the byte that begins the corrupt fragment in
  // issue #259's report, where it was taken for a frame start and cost that read
  // its answer. The claim was inherited rather than derived -- the Python client
  // carried the same test with the same justification (issue #278).
  return byte == FRAME_START_RESPONSE;
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
  // Frame start byte: 0x24. 0x27 is what WE send; see is_frame_start().
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

  // A frame start is only a frame start if what follows it could be a telegram.
  // The length field counts DA + SA + PDU and an APDU header is two bytes, so
  // the smallest value that describes a telegram carrying anything is 4 -- an
  // 8-byte Unknown Class refusal. (5 is the minimum OBSERVED in the corpus, in
  // both directions, and taking the floor from that was this change's first
  // attempt: no frame in the corpus is refused at the APDU head, so its minimum
  // excludes the one shape that only appears in such a refusal.)
  //
  // Without this, a fragment declaring 0 gave an expected length of 4, which the
  // completion test below satisfies immediately from any notification at all:
  // the frame is trimmed to four bytes, fails CRC, and is discarded -- having
  // consumed the frame-start slot, so the real frame's continuations arrive with
  // nowhere to go. That is exactly what issue #259's report shows happening.
  // Judged only when the byte is actually present; a one-byte notification says
  // nothing either way.
  // Written as two statements rather than `len < 2 || data[1] >= ...` because a
  // `|` in a line cannot be anchored by tools/mutation_check.sh: its entries are
  // split on that character, so the search field is truncated mid-expression.
  // The entry for this line was written as a one-liner first and came back
  // "malformed", which is the script telling the truth loudly -- restructuring
  // the code for it is the right trade, since an entry that cannot be written is
  // a hole that reads as covered.
  bool declares_a_possible_frame = true;
  if (len >= 2) declares_a_possible_frame = data[1] >= protocol::MIN_LENGTH_FIELD;

  if (is_frame_start(data[0]) && declares_a_possible_frame && !reassembling_) {
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
    // The length byte may only have arrived now. A frame start delivered as a
    // ONE-byte notification arms reassembly before the length is knowable, and
    // the branch above computes it only under `size() >= 2` -- so without this
    // recomputation expected_packet_length_ stays 0, the completion test can
    // never fire (it requires > 0), and every subsequent notification is
    // swallowed as a continuation until the staleness guard expires a second
    // later. At the corpus's 55 ms median reply latency that is up to ~18
    // replies lost to a single stray byte.
    //
    // It is also the case the length floor cannot catch: the floor is judged
    // against data[1], and at len == 1 there is no data[1] to judge.
    if (expected_packet_length_ == 0 && reassembly_buffer_.size() >= 2) {
      expected_packet_length_ = calculate_expected_length();
      ESP_LOGV(TAG, "Expected packet length (from a later fragment): %d bytes",
               expected_packet_length_);
    }
    ESP_LOGV(TAG, "Packet reassembly: %d/%d bytes", 
             reassembly_buffer_.size(), expected_packet_length_);
  } else if (!reassembling_ && is_frame_start(data[0]) && !declares_a_possible_frame) {
    // Distinguished from the generic line below because it is a different fact:
    // this DID look like a frame start and was refused on its length byte. A log
    // that says only "not frame start" about it sends the next person looking at
    // the wrong byte.
    ESP_LOGW(TAG, "Ignoring a frame start declaring %u bytes; the shortest telegram is %u",
             (unsigned) data[1], (unsigned) protocol::MIN_LENGTH_FIELD);
    return;
  } else {
    // Unexpected data (not a frame start, not reassembling)
    ESP_LOGW(TAG,
             "Unexpected notification data (leading byte 0x%02X is not a frame "
             "start, and nothing is being reassembled)",
             data[0]);
    return;
  }

  // Safety: Check buffer overflow.
  //
  // Only while the frame is still INCOMPLETE. This guard runs ahead of the
  // completion test below, and the completion test is `>=` precisely so that a
  // notification carrying trailing bytes -- two frames sharing one delivery --
  // is trimmed rather than lost. Without the second term the guard fires first
  // and throws away a complete, CRC-valid frame whose own declared length is
  // well inside the limit, for the sake of bytes that were never part of it.
  // Reachable only for frames from about 241 bytes up, since the negotiated MTU
  // leaves 20 bytes of ATT payload, so this is a latent fix rather than an
  // observed one (issue #278).
  const bool still_incomplete =
      expected_packet_length_ == 0 || reassembly_buffer_.size() < expected_packet_length_;
  if (reassembly_buffer_.size() > MAX_PACKET_SIZE && still_incomplete) {
    ESP_LOGW(TAG, "Reassembly buffer overflow (%d bytes); dropping the partial frame",
             (int) reassembly_buffer_.size());
    // Inbound frame sync, and nothing else. This used to call reset(), which
    // cancels the command queue -- so one corrupt fragment on a LIVE link could
    // strand every read in flight, which is issue #259's whole subject. The two
    // paths are not the same event:
    //
    //   - reset() is the disconnect. The commands cannot be answered because
    //     there is no link, and the component terminal-events every consumer in
    //     the same breath (invalidate_cache, write_op_service.on_disconnect,
    //     the read-chain generation bump).
    //   - This is a live link that has lost track of where a frame begins. The
    //     commands are still outstanding and the pump may well still answer
    //     them. Nothing here tells any consumer that anything happened, and
    //     nothing needs to: if the frame we lost was a reply someone was
    //     waiting for, that command's own timeout reports the failure, which is
    //     the path every caller already handles.
    //
    // Leaving the queue alone also means the peer-resync hold and the reply
    // debt survive on their own, instead of being saved and restored around a
    // call that should never have been made here.
    reassembling_ = false;
    reassembly_buffer_.clear();
    expected_packet_length_ = 0;
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
  pending_handlers_.clear();
  // The peer's partial frame died with the link, so a hold armed before the
  // drop would stall the first second of the next connection for nothing. This
  // is the disconnect path and only the disconnect path; the inbound overflow
  // in on_notification() no longer comes through here, so it no longer has to
  // save and restore this around the call.
  peer_resync_pending_ = false;
  // Same reasoning for the reply debt (issue #248): a reply owed by a command on
  // the connection that just died is not coming, and carrying the debt across
  // would spend the next connection's first acknowledgement paying it off.
  owed_pending_ = false;
  owed_replies_ = 0;
  state_ = State::IDLE;
  // Last, because the callbacks it invokes run service code, and that code must
  // see a transport that is already down rather than one still holding the
  // wreckage of the connection it is being told about.
  abandon_queue_();
}

void Transport::complete_front_command_(bool ok, const uint8_t *data, size_t len) {
  if (this->command_queue_.empty()) return;
  // Off the queue BEFORE the callback runs, which is the opposite of the order
  // this used to be in, and the order matters for three reasons.
  //
  // `cmd` at every call site is a REFERENCE into the deque. A callback is
  // service code and service code touches the transport: it queues the next
  // read of a chain, and it may reach reset(). Either way the reference is
  // dangling by the time the `pop_front()` that used to follow it ran -- and if
  // the queue had been emptied, that pop ran on an empty deque.
  //
  // Worse, moving the command out here steals the std::function's heap target
  // from the deque element. If the element were still reachable, the closure
  // could be freed while its own operator() was executing -- which the read
  // chains make concrete, since the queued callback holds the only strong
  // reference to the closure that owns it.
  //
  // The third reason arrived with issue #259. reset() now FAILS the queue
  // rather than clearing it, so a reset() reached from this callback would find
  // this very command still sitting at the front and invoke its callback a
  // second time, re-entrantly, from inside itself. Taking it off the queue
  // first removes it from anything the callback can reach.
  //
  // This applies to EVERY completion, not only the failures. The first cut of
  // the issue-#259 fix converted the three failure paths and left the three
  // success paths in try_dispatch_response() with the old shape -- an asymmetry
  // with no defence, since a callback does not become safe by having succeeded.
  // A skeptic reproduced all three symptoms on the success path under ASan:
  // double invocation, heap-use-after-free of the executing closure, and a
  // pop_front() on an empty deque.
  Command cmd = std::move(this->command_queue_.front());
  this->command_queue_.pop_front();
  if (cmd.callback) {
    cmd.callback(ok, data, len);
  }
}

void Transport::fail_front_command_() { this->complete_front_command_(false, nullptr, 0); }

void Transport::abandon_queue_() {
  if (this->command_queue_.empty()) return;
  // Reached from a callback this drain is already running. The outer loop still
  // owns the queue and will collect whatever that callback left in it.
  if (this->abandoning_) return;

  this->abandoning_ = true;
  std::deque<Command> abandoned;
  abandoned.swap(this->command_queue_);

  size_t steps = 0;
  while (!abandoned.empty()) {
    if (steps >= MAX_ABANDON_STEPS) {
      // Never silent. Reaching this means a chain is answering every failure
      // with another send, and the caller it belongs to is about to be stranded
      // the way every caller used to be -- worth a line in the log saying so.
      ESP_LOGE(TAG,
               "Abandon drain hit its %zu-step cap with %zu command(s) still queued; "
               "dropping them without telling their callers",
               (size_t) MAX_ABANDON_STEPS, abandoned.size());
      break;
    }
    steps++;

    Command cmd = std::move(abandoned.front());
    abandoned.pop_front();
    if (cmd.callback) {
      cmd.callback(false, nullptr, 0);
    }

    // A chain that continues past a failed step sends its next read from inside
    // that callback. Take those too: the point of clearing the queue was that a
    // write from the dead connection must not run on the next one, and a
    // half-unwound chain left sitting in the queue is exactly that write.
    while (!this->command_queue_.empty()) {
      abandoned.push_back(std::move(this->command_queue_.front()));
      this->command_queue_.pop_front();
    }
  }

  this->abandoning_ = false;
  // No clear() here. The inner drain above runs at the end of every iteration
  // and the cap breaks at the top of one, so the queue is already empty at both
  // exits -- a clear() would be dead code, and a skeptic confirmed deleting it
  // changes nothing observable.
  this->state_ = State::IDLE;
  ESP_LOGD(TAG, "Abandoned %zu queued command(s)", steps);
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
  // Replies on the wildcard-matched classes can be as short as 8 bytes (e.g. a
  // Class 3 command ACK: [Start][Len][Dest][SvcH][Class][Ack][CRC-H][CRC-L], or
  // the 9-byte Class 5 and Class 11 INFO replies) -- well under the 12-byte
  // minimum the Class 10 DataObject path below requires. Handle their
  // wildcard-by-class-byte matching first, before that length gate, so short
  // responses aren't discarded before we even look at them.
  if (len >= 5 && this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    // What class was the *queued* command itself sent as? (byte 4 of the
    // outgoing GENI frame is the class byte, same offset as in responses.)
    // Only match such a response against a command that was actually sent as
    // that same class -- otherwise an unrelated Class 10 telemetry
    // notification could be mistaken for the response to a queued Class 3
    // command (or vice versa).
    uint8_t queued_class = (cmd.packet.size() > 4) ? cmd.packet[4] : 0x00;

    // Class 2: measured-data reads (the identity read, issue #174).
    // Class 3: command ACK ([03 00] = success/clean, [03 01 xx] = rejected/
    // descriptor-only -- see ControlService::send_remote_mode_command()).
    // Class 5 / 11: INFO replies, one payload byte (issue #174).
    // Class 7: device info strings, etc.
    // All use a different packet structure than Class 10 DataObjects, so
    // when expect_type_low_ver == 0 && expect_type_high == 0 we match by class byte
    // alone -- but only when the queued command was sent as that class.
    // is_wildcard_matched_class() carries why that term is what makes this safe.
    if (protocol::class_wildcard_matches(
            queued_class, data[4],
            cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000)) {
      // Print the class that actually matched. This used to read
      // `data[4] == CLASS_3 ? 3 : 7`, a two-way pick that was exhaustive when
      // only two classes could reach it and silently reports 7 for every other.
      ESP_LOGV(TAG, "Class %u response matched (wildcard match by class byte)",
               static_cast<unsigned>(data[4]));
      this->complete_front_command_(true, data, len);
      this->state_ = State::IDLE;
      return true;
    }

    // If the queued command was itself on a wildcard-matched class but this
    // response is not (e.g. a Class 10 telemetry notification arrived first),
    // it's definitely not our answer -- let it fall through to the general
    // packet callback instead of risking the Class 10 wildcard path below
    // matching it by accident.
    if (protocol::ignore_unrelated_while_awaiting_wildcard_class(queued_class, data[4])) {
      ESP_LOGV(TAG, "Awaiting Class %d response, ignoring unrelated Class 0x%02X packet",
               queued_class, data[4]);
      return false;
    }

    // Some ALPHA HWR Class 10 SET commands are ACKed with a short Class 10 frame
    // that carries no Obj/Sub fields. Handle that before the generic len>=12
    // DataObject parser below.
    //
    // Every term below inspects the QUEUED COMMAND. None inspects the reply,
    // because there is nothing in the reply to inspect. That is not a shortcut:
    // every SET reply this pump has ever been captured sending is the SAME NINE
    // BYTES, `24 05 F8 E7 0A 01 00 AE A2`, across 420 writes in 20 distinct
    // address shapes -- the clock, the schedule layers, the overview commit, the
    // control request, the mode write, the temperature range, the DHW config.
    // Not one bit distinguishes which write is being acknowledged. The
    // specification says why: "the SET operation never returns anything but the
    // APDU Head" (App. Prog. Manual fig 3.5 note 1), and "the Data Reply is not
    // self contained, meaning that the Data Request is necessary to process it"
    // (note 3). Correlation is positional -- which request is outstanding -- and
    // there is nothing else on offer.
    //
    // The queued-command test is "is this a SET": the operation is the top two
    // bits of the APDU head, 10 = SET (fig C.2), and the low six are its payload
    // length. The seven values this used to list -- 0x97 0x96 0xB3 0x95 0x91 0x90
    // 0x8F -- are all SET with different lengths, so naming the operation says
    // what was meant and does not admit a GET the way a longer list eventually
    // would.
    //
    // `expect_short_ack` is the caller's declaration that it is awaiting exactly
    // this frame, and it replaces the list of five address shapes that used to
    // stand here (issue #253). The list could not be right: it inspected the
    // queued command, so it never narrowed WHICH write a reply answered -- only
    // which writes were allowed to be answered at all -- and every send closed
    // since has had to remember to add a row, with one row already gone dead
    // (`01 AE 00 5B`, a "new format" nothing builds) and one added speculatively
    // for a send that had no callback to use it. Requiring the declaration is
    // strictly narrower for anything that does not opt in, and it puts the
    // decision at the call site that knows the answer.
    const bool short_ack_shape =
        queued_class == 0x0A && len >= 6 && data[4] == 0x0A && protocol::apdu_payload_len(data[5]) <= 1 &&
        cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000 &&
        cmd.expect_short_ack &&
        cmd.packet.size() > 5 && protocol::apdu_is_set(cmd.packet[5]);

    // The frame has the right shape -- but shape is all we can test, so before
    // treating it as THIS command's answer, settle any debt owed to a command
    // that already gave up (issue #248).
    //
    // Paying the debt here rather than declining inside the condition above is
    // what makes the guard terminate. A frame that merely falls through leaves
    // the debt standing, this command times out, and the timeout records a
    // second debt -- one late reply then costs every acknowledgement that
    // follows it. Consuming the frame closes the account: at most one match is
    // lost per late reply, and `suppressed_a_frame` stops this command's own
    // timeout from opening a new one.
    if (short_ack_shape && this->stale_reply_possible_()) {
      ESP_LOGD(TAG, "Short Class 10 frame consumed as the reply owed to an "
                    "abandoned command (%u still owed)",
               (unsigned) this->owed_replies_);
      if (this->owed_replies_ > 0) this->owed_replies_--;
      if (this->owed_replies_ == 0) this->owed_pending_ = false;
      cmd.suppressed_a_frame = true;
      return false;
    }

    if (short_ack_shape) {
      // The acknowledge is the top two bits of the APDU head, not the byte
      // after it (issue #208). What follows an error reply is the ID of the
      // offending Data Item; this used to be read as an error code, so an
      // Unknown Data Item whose ID happened to be 0x00 was reported as a
      // successful write, and every other ID reported failure for the wrong
      // reason. See response_match.h for the encoding and the two captured
      // frames behind it.
      const protocol::ApduAck ack = protocol::apdu_ack(data[5]);
      // BOTH acknowledges, not just the head's. A Class 10 reply carries a
      // second status byte in its payload -- OK / BUSY / OPERATION_FAILED, named
      // by the Grundfos GO app's own decoder (GeniAPDU.CLASS10_ACK_*, read from
      // the byte after the head) and present with exactly those three values in
      // 459 captured replies. Reading only the head reported success for 39 of
      // them: every "busy" and every "operation failed" the pump has ever sent
      // us. Issue #208's defect, one layer further down.
      // len >= 9, not 7. A real short ACK is `24 05 F8 E7 0A 01 PL CRC CRC` --
      // nine bytes -- so the payload byte exists only at that length. At `>= 7`
      // an eight-byte CRC-valid frame whose head declares one payload byte makes
      // data[6] the CRC HIGH BYTE, and that byte now decides the write's verdict
      // rather than just the wording of a log line.
      const bool has_payload = protocol::apdu_payload_len(data[5]) == 1 && len >= 9;
      const uint8_t class10_ack = has_payload ? data[6] : 0;
      const bool success = protocol::class10_reply_is_ok(data[5], has_payload, class10_ack);
      if (success) {
        ESP_LOGI(TAG, "Matched short Class 10 ACK (head 0x%02X, ok) for Class 10 SET write", data[5]);
      } else if (protocol::apdu_ack_is_ok(data[5])) {
        // The head said ok and Class 10 did not. Reported at warning for the
        // same reason a refusal is: nothing else surfaces it, and before this it
        // was reported as a successful write.
        ESP_LOGW(TAG, "Class 10 SET not accepted: head 0x%02X, %s (code 0x%02X)",
                 data[5], protocol::class10_ack_name(class10_ack), class10_ack);
      } else {
        // Reported at warning level because it is the pump refusing the write,
        // which nothing else in this path would surface: before #208 a refusal
        // did not match here at all and the command failed by 3 s timeout, so
        // the log said "no response" about a pump that had answered promptly.
        //
        // Only Unknown Data Item and Illegal Operation carry the offending
        // item's ID. Unknown Class declares a zero-length payload, so there is
        // no byte to report and `len` is 8 rather than 9 -- which is why the
        // match above is `<= 1` and not `== 1`. Getting that wrong is how the
        // first cut of this shipped: it admitted 0x41, a length-1 Unknown
        // Class that the documented format says cannot occur, while rejecting
        // the 0x40 that does.
        if (protocol::apdu_payload_len(data[5]) == 1 && len >= 7) {
          ESP_LOGW(TAG, "Class 10 SET refused: head 0x%02X (%s), offending item ID 0x%02X",
                   data[5], protocol::apdu_ack_name(ack), data[6]);
        } else {
          ESP_LOGW(TAG, "Class 10 SET refused: head 0x%02X (%s)", data[5],
                   protocol::apdu_ack_name(ack));
        }
      }
      this->complete_front_command_(success, data, len);
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

  // ---------------------------------------------------------------------------
  // What a GENI response frame actually carries.
  //
  // These fields were called `obj_id` and `sub_id` for a long time, and that is
  // not what they are. A pump response contains NO Object ID and NO Sub-ID: the
  // request names an object, and the reply identifies only the object's TYPE.
  // Measured over the 21,236 captured Class 10 responses long enough to carry a
  // type header, byte 6 is 0x00 in 100% of
  // them, and bytes 7-8 identify the type of the object the request asked for.
  // Note the reply is not echoing anything: a read request is
  // `0A 03 [obj] [subH] [subL]` and carries no type at all.
  //
  //   byte:   5        6     7       8       9        10..12
  //         [length] [00] [TypeH] [TypeL] [Version] [size(3)]  payload...
  //
  // So the two values below split the type at the wrong boundary, for
  // historical reasons this code preserves rather than fixes:
  //
  //   packet_type_high    = TypeH                  (the type's high byte)
  //   packet_type_low_ver = (TypeL << 8) | Version
  //
  // which is why the match constants elsewhere look like Object IDs but are
  // not. Each is (TypeL << 8) | Version, and the type's HIGH byte is the
  // separate expect_type_high value:
  //
  //   0xF301  Type  243 v1  event log info      wire 00 00 F3 01
  //   0xF402  Type  244 v2  event log entry     wire 00 00 F4 02
  //   0xDE01  Type  222 v1  schedule entries    wire 00 00 DE 01
  //   0xDC01  Type  220 v1  single event        wire 00 00 DC 01
  //   0x2F01  Type  303 v1  mode / control      wire 00 01 2F 01
  //
  // Note that (TypeL << 8) | Version does NOT identify a type on its own:
  // Type 1012 (the temperature-range user settings object) also ends in
  // F4 02, but its wire header is 00 03 F4 02 -- it differs from Type 244
  // only in expect_type_high. An earlier version of this comment recorded
  // 0xF402 as "Type 1012", which is precisely the confusion this block
  // exists to prevent.
  //
  // The consequence that matters: matching here discriminates object TYPES, not
  // instances of a type. Sibling reads that share a type -- the five schedule
  // layers (Sub 1000..1004), the single-event slots, the event-log entries, the
  // Object 53 trends -- are byte-identical through this header and cannot be
  // told apart by it. Verified against the captures: obj 84 sub 1000-1004 all
  // answer 00 00 DE 01, obj 84 sub 900-904 all answer 00 00 DC 01, and obj 88
  // sub 10200-10219 all answer 00 00 F4 02.
  //
  // Byte 5 is likewise not an operation code in the response direction: it is
  // the APDU body length. `byte5 == total_len - 8` held for all 24,233 CRC-valid
  // captured inbound frames without exception -- but read that as "every frame
  // in that corpus carries exactly one APDU", which is what it actually says. A
  // telegram may carry several, and then byte 5 is the FIRST APDU's length and
  // the equality does not hold (issue #226). It is still called `opspec` below
  // because the matching logic keys off specific values of it.
  // ---------------------------------------------------------------------------
  uint8_t opspec = data[5];
  uint16_t packet_type_high = (data[6] << 8) | data[7];
  uint16_t packet_type_low_ver = (data[8] << 8) | data[9];

  // Log incoming packets at verbose level when waiting for a command response
  if (this->state_ == State::AWAITING_RESPONSE && !this->command_queue_.empty()) {
    auto &cmd = this->command_queue_.front();
    ESP_LOGV(TAG, "[AWAITING] Packet received: len=%zu, Class=%02X, OpSpec=%02X, Sub=%d, Obj=%d (waiting for Obj %d Sub %d)",
             len, data[4], opspec, packet_type_high, packet_type_low_ver, cmd.expect_type_low_ver, cmd.expect_type_high);
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
    
    // The telemetry-response filter, and why it only guards wildcard commands.
    //
    // Byte 5 is the APDU body length, not an operation code (`byte5 ==
    // total_len - 8` holds for all 24,233 CRC-valid inbound frames -- which is a
    // statement that the corpus is single-APDU throughout, not that telegrams
    // always are; see issue #226). Strictly it is a two-bit acknowledge over a
    // six-bit length --
    // the short-ACK branch above decodes both (issue #208) -- but no frame in
    // that corpus has either acknowledge bit set, so across it byte 5 is a
    // plain length. That is a statement about the corpus and not about the
    // pump: the corpus is the phone app's traffic, in which nothing was
    // refused.
    // Either way this test does not ask "is this a register read". It asks
    // "is this response's body 48, 43, 20, 46, 45 or 9 bytes". Four of the six
    // really are telemetry reply sizes -- pairing the captured reads of the same
    // five registers TelemetryService polls gives motor 0x30, flow/pressure
    // 0x2B, temperature 0x14, alarms and warnings 0x09 -- but the correspondence
    // is loose in both directions: 0x2E and 0x2D never appear at byte 5 in any
    // captured frame of any class, and 0x13, which that switch does handle, is
    // not on this list. So the list neither covers telemetry nor is limited
    // to it.
    //
    // That makes it a usable heuristic and nothing more. It cannot be deleted:
    // telemetry reads are queued as Class 10 wildcard commands (expect 0/0), so
    // without it a telemetry reply satisfies whatever wildcard command happens
    // to be at the head of the queue. And no discriminator can be recovered from
    // the captures, because the phone app read the very same telemetry registers
    // the same way we do -- Class 10 requests answered by Class 10 replies at
    // exactly these lengths. Response class equals request class in 24,223 of
    // 24,224 paired exchanges, so data[4] separates nothing here.
    //
    // But it must not outrank an exact type match. It used to run ahead of all
    // matching, so a reply carrying exactly the type the command asked for was
    // still discarded when its length collided -- the command then timed out
    // with its answer already in hand. Event-log entries (20 bytes) hit this and
    // were worked around at the call site with allow_register_read=true; cycle
    // timestamps reply at 47 bytes with 45 and 46 both on the list.
    //
    // So: when the command names a type, that type is the discriminator and
    // length gets no vote. Only wildcard commands, which have nothing else to
    // match on, still need the guard.
    bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 ||
                             opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
    //
    // Two notes on the condition below. It deliberately omits the
    // `!cmd.expect_short_ack` term that the wildcard MATCH further down
    // carries, so a 0/0 command with expect_short_ack set would be guarded as a
    // wildcard without matching as one. Every awaited Class 10 SET is exactly
    // that caller -- 0/0 with expect_short_ack (issues #248 and #253) -- and the
    // behaviour is what they want: a Class 10 telemetry frame arriving while one
    // waits is guarded here and falls through to the packet callback rather than
    // being taken for its acknowledgement. Adding the term would change that, so
    // it stays recorded rather than "fixed". And with the event-log workaround
    // gone, nothing passes allow_register_read=true any more -- the parameter
    // and its Command field are vestigial, kept because removing them is a
    // separate change.
    bool wildcard_command = (cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000);

     if (is_register_read && wildcard_command && !cmd.allow_register_read) {
       // Telemetry register-read response answering nothing we can match by
       // type. Let it fall through to the packet callback, which decodes it.
       ESP_LOGV(TAG, "Class 10 register-read (body=%u bytes), skipping for wildcard command response",
                (unsigned) opspec);
       return false;
     }
    
    // This is a Class 10 DataObject response. Extract Object/Sub IDs
    // Frame structure depends on OpSpec!
    // OpSpec 0x0E (Passive Notif): [SubH][SubL][ObjH][ObjL][Payload...]
    // OpSpec 0x02 (Positive ACK):  [Obj(1 byte)][SubH][SubL][Payload...]
    // Both the OpSpec 0x0E/0x01 case and every other case read the same two
    // fields from the same offsets, so there is one body rather than two
    // identical arms. There used to be an `opspec == 0x02` branch between them
    // for a "Positive ACK" layout ([Obj(1)][SubH][SubL][payload at +9]); it is
    // deleted because its premise is false, not because of any arithmetic
    // problem:
    //
    //   - Byte 5 is the APDU body length in the response direction, not an
    //     operation code, so `== 0x02` selects "a 10-byte frame" rather than a
    //     packet format. Verified: `byte5 == total_len - 8` holds for all
    //     24,233 CRC-valid inbound frames -- every one of which carries a single
    //     APDU. On a multi-APDU telegram byte 5 is the first APDU's length and
    //     the equality fails (issue #226).
    //   - Zero of 21,720 captured Class 10 responses had byte 5 == 0x02, and a
    //     10-byte frame cannot reach here anyway: the `len < 11` guard above
    //     returns first. (An earlier version of this comment claimed the
    //     branch underflowed at len == 10. It could not -- that guard makes
    //     len >= 11 here, so its `len - 11` was never reachable, let alone
    //     wrong. The branch was simply dead.)
    if (len >= 10) {
      packet_type_high = (data[6] << 8) | data[7];
      packet_type_low_ver = (data[8] << 8) | data[9];
    } else {
      ESP_LOGV(TAG, "DataObject packet too short to extract type fields");
      return false;
    }
    
    // Now check if this matches our expected Object/Sub ID
    bool matched = false;
    
    // WILDCARD MATCH: If expect_type_low_ver == 0, accept ANY Class 10 packet
    // This is used for Object 86 Sub 6 reads, which receive passive notifications (OpSpec 0x0E)
    // Reference: Python base.py::match_class10_response only checks p[4] == 0x0A
    if (cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000 && !cmd.expect_short_ack) {
      matched = true;
      ESP_LOGV(TAG, "Wildcard match: accepting any Class 10 packet (OpSpec=0x%02X, Obj=%d, Sub=%d)",
               opspec, packet_type_low_ver, packet_type_high);
    } else {
      // Exact match: check Object ID and Sub-ID
      matched = (packet_type_low_ver == cmd.expect_type_low_ver && (packet_type_high == cmd.expect_type_high || packet_type_high == 0));
      
      // A "BACKUP MATCH" for a supposedly swapped Obj/Sub used to sit here:
      //   if (!matched && expect != 0 && packet_type_high == expect) matched = true;
      //
      // Deleted. The fields are not Obj/Sub and the real pump does not swap
      // them; the branch was speculative from the commit that introduced it,
      // never a fix for an observed failure. What it actually absorbed was the
      // Object 86 Sub 7 mode read passing its two arguments the wrong way
      // round, which is fixed at that call site.
      //
      // It was not the mode read's alone, though. Instrumenting it shows a
      // second caller in the host suite: test_write_operations builds the
      // Obj 91 Sub 430 reply echoing 91/430 at bytes 6-9, which puts 0x005B in
      // packet_type_high and matches an expectation of 91. That fixture does
      // not describe the real pump -- captured Sub 430 replies carry the
      // ordinary type header 00 03 F4 02 (442 samples) -- and the Obj-91
      // workaround branch below catches it identically, so removing this
      // changes no behaviour. Recorded because the deletion was first
      // justified by a claim ("no call site can reach it") that instrumenting
      // disproved.
    }
    
    // SPECIAL CASE WORKAROUND: Object 91 config reads (Cache Sync / DHW config)
    // The pump responds to these read requests with a size-specific OpSpec
    // (0x15 for the 14-byte Sub 430 temperature-range settings, 0x0D for the
    // 6-byte Sub 421 DHW on/off configuration -- bench/capture-verified, see
    // issue #106) which does NOT carry the standard Obj/Sub IDs in the payload
    // header. Python reference simply accepted these via a wildcard match and
    // sliced at byte 10. We explicitly handle the known cases here to avoid
    // wildcard matching other packets.
    // Obj 91 config reads. NOTE these two fields are used here as the REQUEST's
    // object and sub-id (91 / 430 / 421), not as expected response type fields,
    // so this branch matches on what was asked for plus the reply's length byte.
    //
    // Not because the reply lacks a type header -- it has an ordinary one
    // (Sub 430 answers 00 03 F4 02, Sub 421 answers 00 03 D9 01, 442 and 563
    // captured samples). The reason is only that these call sites pass the
    // object and sub-id rather than the expected type, so the normal path has
    // nothing to compare. Giving them their real type expectations would let
    // this workaround go.
    if (!matched && cmd.expect_type_low_ver == 91 && len >= 12 &&
        ((cmd.expect_type_high == 430 && opspec == 0x15) ||
         (cmd.expect_type_high == 421 && opspec == 0x0D))) {
      matched = true;
      payload = data + 10;
      payload_len = len - 12; // len - 10(header) - 2(CRC)
      ESP_LOGV(TAG, "Matched Object 91 Sub %d using OpSpec 0x%02X workaround",
               cmd.expect_type_high, opspec);
    }

    if (matched) {
      ESP_LOGV(TAG, "Command response matched for Obj %d (Sub %d -> %d)", 
               packet_type_low_ver, cmd.expect_type_high, packet_type_high);
      this->complete_front_command_(true, payload, payload_len);
      this->state_ = State::IDLE;
      return true;
     } else {
       // This is a Class 10 DataObject response but doesn't match what we're waiting for
       ESP_LOGV(TAG, "Class 10 DataObject MISMATCH: got Obj=0x%04X Sub=0x%04X, want Obj=0x%04X Sub=0x%04X, OpSpec=0x%02X",
                packet_type_low_ver, packet_type_high, cmd.expect_type_low_ver, cmd.expect_type_high, opspec);
       return false;
     }
  }

  // 2. Check registered response handlers
  if (pending_handlers_.empty()) {
    return false;  // No handlers registered
  }

  // Validate packet structure
  if (len < 12) {
    ESP_LOGV(TAG, "Packet too short for response matching (%zu bytes)", len);
    return false;
  }

  // Check if this is a Class 10 response (most common for read operations)
  if (data[4] != 0x0A) {
    ESP_LOGV(TAG, "Not a Class 10 packet (class=0x%02X), skipping response matching", data[4]);
    return false;
  }

  // Extract OpSpec to see what kind of response this is
  opspec = data[5];

  packet_type_low_ver = 0;
  packet_type_high = 0;
  
  // Only parse as DataObject format for non-register-read OpSpecs
  bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 || 
                           opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
  
  if (is_register_read) {
    // This is a telemetry register read - don't try to match it
    ESP_LOGV(TAG, "Skipping register-read response (OpSpec=0x%02X) for response matching", opspec);
    return false;
  }
  
  // Parse as DataObject format.
  //
  // NOTE: this path assigns the OTHER WAY ROUND from try_dispatch_response
  // above -- bytes 6-7 land in the "low_ver" slot and 8-9 in "high", which is
  // backwards relative to the wire layout documented there.
  //
  // Left exactly as it was, because the whole path is unreachable:
  // register_response_handler() has no callers anywhere in the repo, so
  // pending_handlers_ is always empty. Swapping the assignment to agree with
  // the dispatch path leaves the suite green -- that is deadness, not
  // coverage. Deleting the path is a reasonable follow-up; it is out of scope
  // for a change about naming.
  packet_type_low_ver = (data[6] << 8) | data[7];
  packet_type_high = (data[8] << 8) | data[9];

  ESP_LOGV(TAG, "DataObject response: OpSpec=0x%02X, Object %d SubID %d (checking %d handlers)",
           opspec, packet_type_low_ver, packet_type_high, pending_handlers_.size());

  // Search for matching handler
  for (auto it = pending_handlers_.begin(); it != pending_handlers_.end(); ++it) {
    if (it->object_id == packet_type_low_ver && it->sub_id == packet_type_high) {
      ESP_LOGV(TAG, "Response handler matched for Object %d SubID %d", packet_type_low_ver, packet_type_high);

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

  ESP_LOGV(TAG, "No matching response handler for Object %d SubID %d", packet_type_low_ver, packet_type_high);
  return false;  // No matching handler found
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
