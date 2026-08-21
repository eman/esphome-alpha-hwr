/**
 * BLE Transport Layer for GENI Protocol Communication
 * 
 * This module handles low-level BLE packet transport including:
 * - Packet fragmentation and reassembly (BLE 20-byte MTU limit)
 * - BLE write operations with automatic packet splitting
 * - Notification handling with callback mechanism
 * 
 * The transport layer sits between the BLE client (ESP-IDF BLE) and the
 * protocol layer, providing a clean abstraction for sending/receiving
 * GENI protocol packets.
 * 
 * Architecture:
 * ┌─────────────────────────────────┐
 * │   Protocol Layer / Services     │
 * │  (sends/receives GENI packets)  │
 * └────────────┬────────────────────┘
 *              │
 *              ▼
 * ┌─────────────────────────────────┐
 * │       Transport Layer           │
 * │  - Packet reassembly            │
 * │  - BLE write with splitting     │
 * │  - Notification callbacks       │
 * └────────────┬────────────────────┘
 *              │
 *              ▼
 * ┌─────────────────────────────────┐
 * │      ESP-IDF BLE GATT           │
 * │  - BLE GATT operations          │
 * │  - Connection management        │
 * └─────────────────────────────────┘
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/core/transport.py
 */

#pragma once

#include "esphome/core/component.h"
#include "frame_builder.h"
#include <vector>
#include <deque>
#include <functional>

namespace esphome {
namespace alpha_hwr {
namespace core {

/**
 * BLE transport for GENI protocol packets.
 * 
 * Manages low-level BLE communication including notification handling
 * and packet reassembly. Provides a clean interface for higher-level
 * protocol operations.
 * 
 * Key Features:
 * - Automatic packet reassembly for fragmented BLE notifications
 * - Automatic packet splitting for writes exceeding 20-byte MTU
 * - Frame start detection (0x24/0x27)
 * - Length-based completion checking
 * - Buffer overflow protection
 * - Callback mechanism for complete packets
 * 
 * Packet Fragmentation:
 * ---------------------
 * BLE limits notifications to ~20 bytes (MTU - headers). GENI packets
 * can be larger, so they arrive in fragments:
 * 
 * Fragment 1: [0x24][LEN][...]           (frame start byte + data)
 * Fragment 2: [...]                       (continuation)
 * Fragment N: [...][CRC_H][CRC_L]         (end of packet)
 * 
 * The transport accumulates fragments until:
 *   received_bytes >= (length_field + 4)
 * 
 * Then delivers the complete packet via callback.
 * 
 * Frame Start Bytes:
 * ------------------
 * 0x24 = Response frame (pump -> client)
 * 0x27 = Request frame (client -> pump, also echoed back)
 * 
 * These bytes indicate a new packet is starting, not a continuation.
 * 
 * Example Usage:
 * --------------
 * ```cpp
 * Transport transport;
 * 
 * // Set callback for complete packets
 * transport.set_packet_callback([this](const uint8_t* data, size_t len) {
 *   decode_packet(data, len);
 * });
 * 
 * // Handle BLE notification (called from ESP-IDF callback)
 * void on_ble_notification(const uint8_t* data, size_t len) {
 *   transport.on_notification(data, len);
 * }
 * 
 * // Write packet (automatically splits if >20 bytes)
 * uint8_t packet[11];
 * build_class10_read(0x570045, packet);
 * transport.write_packet(packet, 11, ble_write_func);
 * ```
 * 
 * Notes for Reimplementation:
 * ---------------------------
 * This C++ implementation differs from Python reference in key ways:
 * 
 * 1. **No Async/Await**: ESP-IDF uses callback-based BLE, not async
 * 2. **No Transaction Lock**: ESPHome runs in single loop context
 * 3. **No Response Queue**: Direct callback delivery instead
 * 4. **No Keep-Alive**: Managed at higher layer (poll_telemetry)
 * 
 * Python's asyncio primitives (Lock, Queue) aren't needed because
 * ESPHome components run sequentially in the main loop.
 */
class Transport {
 public:

  /// How long a reply owed by an abandoned command may still turn up, during
  /// which the short-ACK branch declines to match on shape alone (issue #248).
  ///
  /// 500 ms, against a measured tail of 295. Across resources/traffic_capture --
  /// reassembled from ATT fragments and de-duplicated, n = 19768 pairs -- replies
  /// arrive at p50 55 ms, p90 85, p99 144, max 295, and NOTHING exceeds 400.
  /// (`tools/geni_capture_scan.py latency`; the p99 was previously given as 121
  /// and the pair count as ~12k, both from a scan that lost part of three
  /// PacketLogger captures to an endianness misread. An earlier
  /// revision cited a 994 ms maximum and sized two constants from it; that figure
  /// was an artifact of scanning un-reassembled packets. See
  /// resources/traffic_capture/README.md.)
  ///
  /// It does not have to cover the worst case alone, which is why it is not
  /// larger: the mode write is awaited, so its acknowledgement is normally
  /// consumed by the command that earned it, and this is the backstop for one
  /// that arrives after that wait gave up.
  static constexpr uint32_t STALE_REPLY_WINDOW_MS = 500;

  /// How long an awaited Class 10 SET waits for its acknowledgement (issue #253).
  ///
  /// A SET reply is head-only by specification -- "the SET operation never
  /// returns anything but the APDU Head" (App. Prog. Manual fig 3.5 note 1) --
  /// so there is nothing to wait for beyond the acknowledgement itself, and
  /// nothing a longer wait could collect.
  ///
  /// 400 ms against a measured worst case of 193. Across
  /// resources/traffic_capture, reassembled and de-duplicated, the pump answers
  /// every SET in 36-193 ms; the widest tail anywhere in that corpus, over all
  /// 19768 request/reply pairs, is 295 ms. The value is deliberately the same as
  /// ControlService::MODE_ACK_TIMEOUT_MS, which was sized to equal the delay its
  /// caller already waits -- there is no reason for two numbers here, and the
  /// pump does not distinguish these writes anyway.
  ///
  /// It is short on purpose. A send that is not answered must not hold the queue
  /// for longer than the next scheduled step, and no caller of these writes
  /// treats the acknowledgement as its verdict: every one of them confirms by
  /// reading the value back.
  static constexpr uint32_t SET_ACK_TIMEOUT_MS = 400;

  /**
   * Callback type for complete packets.
   */
  using PacketCallback = std::function<void(const uint8_t* data, size_t len)>;

  /**
   * Callback type for BLE write operations.
   */
  using WriteCallback = std::function<bool(const uint8_t* data, size_t len)>;

  /**
   * Callback type for response handlers.
   */
  using ResponseCallback = std::function<void(const uint8_t* data, size_t len)>;

  /**
   * Callback for command completion.
   */
  using CommandCallback = std::function<void(bool success, const uint8_t* data, size_t len)>;

  struct Command {
    std::vector<uint8_t> packet;
    size_t bytes_sent{0};
    // Expected reply identity. NOT an Object/Sub ID -- see send_command().
    // expect_type_low_ver = (TypeL << 8) | Version (reply bytes 8-9)
    // expect_type_high    = TypeH                  (reply bytes 6-7)
    uint16_t expect_type_low_ver{0};
    uint16_t expect_type_high{0};
    CommandCallback callback{nullptr};
    uint32_t timeout_ms{3000};
    uint32_t timestamp_ms{0};
    bool waiting_for_response{false};
    bool allow_register_read{false};  // When true, don't filter register-read OpSpecs
    bool expect_short_ack{false};     // When true, disables the Class 10 wildcard matching path
    // When true, silence is not worth a warning: the caller's verdict comes from
    // a readback, not from this acknowledgement. It does NOT mean silence is
    // expected -- every Class 10 SET in the captures is answered -- and it has
    // no effect beyond the log level. In particular the reply debt is recorded
    // either way (issue #248).
    bool quiet_timeout{false};
    // This command had an ambiguous frame withheld from it because an earlier
    // command was still owed one (issue #248). It must therefore NOT record a
    // fresh debt when it times out: the reply it was waiting for is the frame
    // that was taken from it, and counting that twice is what makes the
    // suppression self-sustaining.
    bool suppressed_a_frame{false};
  };

  Transport();

  void set_write_callback(WriteCallback callback) { write_callback_ = callback; }

  /**
   * Process transport state machine and command queue.
   * Should be called from the main component loop().
   */
  void loop();

  /**
   * Queue a command for transmission.
   *
   * The two `expect_*` values identify the RESPONSE, and they are not an
   * Object ID and a Sub-ID however much they look like one. A GENI reply
   * carries neither; bytes 6-9 are `[00][TypeH][TypeL][Version]`, the object
   * type and version. This pair splits that at the wrong boundary, for
   * historical reasons preserved in try_dispatch_response():
   *
   *   expect_type_high    = TypeH                   (response bytes 6-7)
   *   expect_type_low_ver = (TypeL << 8) | Version  (response bytes 8-9)
   *
   * so e.g. Type 303 v1 (the mode/control object) is passed as
   * `(0x2F01, 0x0001)`, NOT `(0x0001, 0x2F01)`. Getting that backwards used to
   * be survivable because a fallback branch absorbed it; it no longer is, and
   * a swap now makes the read time out. Both zero means "match any Class 10
   * response" (see the wildcard path in try_dispatch_response).
   *
   * Two call sites use these fields differently, to carry the REQUEST's object
   * and sub-id (91/430, 91/421) for a workaround branch that matches on what
   * was asked for. Those are the exception, and they are commented at the site.
   *
   * @param packet The complete GENI packet to send
   * @param expect_type_low_ver (TypeL << 8) | Version of the expected reply; 0 for any
   * @param expect_type_high TypeH of the expected reply; 0 for any
   * @param callback Called when command completes or times out
   * @param timeout_ms How long to wait for response
   */
  void send_command(const std::vector<uint8_t>& packet, uint16_t expect_type_low_ver = 0,
                    uint16_t expect_type_high = 0, CommandCallback callback = nullptr,
                    uint32_t timeout_ms = 3000, bool allow_register_read = false,
                    bool expect_short_ack = false, bool quiet_timeout = false);

  /**
   * Helper to build and queue a command directly from an APDU.
   * This abstracts away the GENI addressing (Service ID, Source ID)
   * from the service layer.
   *
   * `expect_type_low_ver` / `expect_type_high` mean exactly what they mean in
   * send_command() above -- read that note before passing them, including why
   * their order is not interchangeable.
   */
  void send_apdu_command(const uint8_t* apdu, size_t apdu_len,
                         uint16_t expect_type_low_ver = 0, uint16_t expect_type_high = 0,
                         CommandCallback callback = nullptr,
                         uint32_t timeout_ms = 3000, bool allow_register_read = false,
                         bool expect_short_ack = false, bool quiet_timeout = false);

  /**
   * Set callback for complete packets.
   * 
   * The callback will be invoked whenever a complete GENI packet
   * has been reassembled from BLE notification fragments.
   * 
   * @param callback Function to call with complete packets
   */
  void set_packet_callback(PacketCallback callback);

  /**
   * Handle incoming BLE notification data.
   * 
   * This should be called from the ESP-IDF BLE notification event handler.
   * It accumulates packet fragments and invokes the packet callback when
   * a complete packet is ready.
   * 
   * Packet Reassembly Logic:
   * 1. If data[0] is 0x24 or 0x27 (frame start), start new packet
   * 2. Otherwise, append to current buffer
   * 3. Check if complete: buffer_size >= (length_field + 4)
   * 4. If complete, invoke callback and clear buffer
   * 
   * @param data Pointer to notification data
   * @param len Length of notification data
   */
  void on_notification(const uint8_t* data, size_t len);

  /**
   * Reset transport state: the link is gone.
   *
   * Clears the reassembly buffer, the pending response handlers and the FSM,
   * and FAILS every queued command -- each callback is invoked with
   * `(false, nullptr, 0)`, the same shape a timeout delivers (issue #259).
   *
   * That last part is the contract, and it used to be the opposite. The queue
   * was cleared silently, so a service with a read in flight never heard
   * anything again: no success, no failure, no timeout, because the timeout
   * went with the queue entry. Every caller of an async read had to be told
   * about the drop by some other route, and each one that was is a separate
   * hand-written hook -- WriteOperationService::on_disconnect(), the read-chain
   * generation counter, ControlService::invalidate_cache(). A caller that
   * nobody remembered to wire up simply waited forever.
   *
   * Failing the queue instead lets a multi-command read unwind through the
   * failure branch it already has. A chain that continues after a failed step
   * queues its next command from inside the callback; those are taken too, so
   * nothing survives into the next connection -- which is what clearing the
   * queue was for in the first place.
   *
   * Callers: BLE disconnection. NOT the inbound overflow path, which is a loss
   * of frame sync on a LIVE link and has no business cancelling commands the
   * pump may still answer -- see on_notification().
   */
  void reset();

  /**
   * Check if currently reassembling a packet.
   * 
   * @return true if reassembly in progress
   */
  bool is_reassembling() const { return reassembling_; }

  /**
   * Get current reassembly buffer size.
   * 
   * Useful for debugging and monitoring.
   * 
   * @return Number of bytes in reassembly buffer
   */
  size_t get_buffer_size() const { return reassembly_buffer_.size(); }

  /**
   * Get expected packet length.
   * 
   * @return Expected total packet length in bytes (0 if not reassembling)
   */
  uint16_t get_expected_length() const { return expected_packet_length_; }

  /**
   * Register a response handler for a specific request.
   * 
   * When a packet matching the specified Object ID and Sub-ID is received,
   * the callback will be invoked with the payload data.
   * 
   * This enables async read operations where:
   * 1. Service sends request packet
   * 2. Service registers response handler with expected Object/Sub IDs
   * 3. When response arrives, callback is invoked with payload
   * 4. Service processes payload and updates state
   * 
   * Implementation Notes:
   * - Handlers timeout after 2 seconds (configurable)
   * - Only one handler per Object+Sub ID combination
   * - Handlers are one-shot (automatically removed after invocation)
   * - GENI frame structure: [STX][LEN][DST][SRC][Class][OpSpec][ObjH][ObjL][SubH][SubL][...DATA...][CRC]
   * - Object ID is at bytes 6-7 (big-endian)
   * - Sub-ID is at bytes 8-9 (big-endian)
   * 
   * Example Usage:
   * ```cpp
   * // Register handler for schedule state response (Object 84, SubID 1)
   * transport.register_response_handler(84, 1, 
   *   [this](const uint8_t* data, size_t len) {
   *     if (len >= 8) {
   *       bool enabled = data[7] != 0;  // Byte 7 is enabled flag
   *       this->schedule_enabled_ = enabled;
   *     }
   *   }
   * );
   * 
   * // Send read request
   * uint8_t request[11];
   * build_class10_read_request(84, 1, request);
   * transport.write_packet(request, 11, ble_write_func);
   * 
   * // Callback will be invoked when response arrives
   * ```
   * 
   * @param object_id Object ID to match (0-65535)
   * @param sub_id Sub-ID to match (0-65535)
   * @param callback Function to call with payload data
   */
  void register_response_handler(uint16_t object_id, uint16_t sub_id, ResponseCallback callback);

  /**
   * Check for timed-out response handlers.
   * 
   * Should be called periodically from component loop() to cleanup
   * stale handlers that never received a response.
   * 
   * Handlers older than timeout_ms (default 2000ms) are removed and
   * logged as warnings.
   * 
   * @param timeout_ms Handler timeout in milliseconds (default 2000)
   */
  void check_timeouts(uint32_t timeout_ms = 2000);

 private:
  /**
   * Check if buffer contains a frame start byte.
   * 
   * Frame start bytes:
   *   0x24 = Response frame
   *   0x27 = Request frame (echo)
   * 
   * @param data First byte of notification
   * @return true if this is a frame start byte
   */
  static bool is_frame_start(uint8_t byte);


  /// How many replies the pump still owes us for commands that gave up.
  ///
  /// A COUNT, not a deadline, and the distinction is the design. The first cut
  /// kept only "suppress until time T" and it cascaded: a suppressed frame
  /// leaves its own command to time out, that timeout re-arms the window, and
  /// the next command's acknowledgement lands inside the new one. With the pump
  /// answering in ~54 ms and pacing at 50, the loop closes on itself and every
  /// following write fails against a healthy pump -- four for four in the
  /// harness that found it.
  ///
  /// A debt cannot do that. A timeout adds one owed reply; consuming an
  /// ambiguous frame pays one off; and a command that already had a frame
  /// suppressed adds nothing on its own timeout, because the reply it was owed
  /// is the frame we took. Damage is bounded at exactly one lost acknowledgement
  /// per genuinely late reply, and the sequence always converges.
  uint8_t owed_replies_{0};

  /// Whether that debt is still live, and since when. The flag is what makes the
  /// timestamp safe to read: a bare millis() deadline compared with a signed
  /// difference goes positive again 24.9 days after it was set and would then
  /// suppress matching for another 24.9 days. Same shape as peer_resync_pending_
  /// above, for the same reason.
  bool owed_pending_{false};
  uint32_t owed_since_ms_{0};

  /// Is a reply to an abandoned command still plausibly in flight? Clears an
  /// expired debt, so it is not const.
  bool stale_reply_possible_();

  /// Record that a command gave up without its reply. `already_suppressed` is
  /// that command's own flag: when set, its debt was collected by the
  /// suppression itself and must not be counted again.
  void note_reply_owed_(bool already_suppressed);

  /// Settle the command at the head of the queue, having taken it OFF the queue
  /// first. Every completion goes through here, success and failure alike. See
  /// the note at the definition: the callback is service code, it can queue,
  /// clear and reset the transport, and it must not be able to reach the entry
  /// it belongs to while it runs.
  void complete_front_command_(bool ok, const uint8_t *data, size_t len);

  /// complete_front_command_(false, nullptr, 0) -- the shape a timeout reports.
  void fail_front_command_();

  /// Fail every queued command, and everything their callbacks queue in turn.
  ///
  /// Iterative, not recursive, and that is not a style preference. A read chain
  /// unwinds one command per failure, and the longest of them is as long as the
  /// pump says it is -- EventLogService reads `min(available_entries,
  /// max_entries)` entries, both uint16 fields straight off the wire. Letting
  /// send_command() invoke the callback inline would put that whole chain on the
  /// stack at once, on a part with about 8 KB of it. So the queue is drained in
  /// a loop and re-queued commands are pulled into the same drain.
  ///
  /// Bounded, for the same reason: a chain that answers every failure with
  /// another send would otherwise spin here until the task watchdog fires. Past
  /// the cap the remainder is dropped the old way -- silently, which strands
  /// that caller, but a stranded read is recoverable and a panic is not. The
  /// cap is reported, never silent.
  void abandon_queue_();

  /// True while abandon_queue_() owns the queue, so a reset() reached from one
  /// of the callbacks it is invoking does not start a second drain over the
  /// same commands.
  bool abandoning_{false};

  /// Unwind steps allowed in one abandon_queue_() call. Far above any real
  /// chain: the longest in the tree is one command per event-log entry, and a
  /// pump that reports more entries than this is not one we can serve anyway.
  static constexpr size_t MAX_ABANDON_STEPS = 512;

  /**
   * Extract expected packet length from buffer.
   * 
   * GENI packet structure:
   *   [Frame Start][Length][Payload...][CRC_H][CRC_L]
   * 
   * Total length = Length field + 4 bytes (start + length + 2-byte CRC)
   * 
   * @return Expected total packet length
   */
  uint16_t calculate_expected_length() const;

  /**
   * Try to match and dispatch packet to a registered response handler.
   * 
   * Extracts Object ID and Sub-ID from packet, looks for matching handler,
   * and invokes it with the payload data if found.
   * 
   * GENI Frame Structure:
   *   [STX][LEN][DST][SRC][Class][OpSpec][ObjH][ObjL][SubH][SubL][...DATA...][CRC_H][CRC_L]
   *   Byte 0: STX (0x24 for response)
   *   Byte 1: Length
   *   Byte 2: Destination
   *   Byte 3: Source
   *   Byte 4: Class (0x0A for Class 10)
   *   Byte 5: OpSpec
   *   Bytes 6-7: Object ID (big-endian)
   *   Bytes 8-9: Sub-ID (big-endian)
   *   Bytes 10 to -2: Payload data
   *   Last 2 bytes: CRC
   * 
   * @param data Complete GENI packet
   * @param len Packet length
   * @return true if handler was found and invoked, false otherwise
   */
  bool try_dispatch_response(const uint8_t* data, size_t len);

  /**
   * Pending response handler entry.
   */
  struct PendingHandler {
    uint16_t object_id;       ///< Object ID to match
    uint16_t sub_id;          ///< Sub-ID to match
    ResponseCallback callback; ///< Callback to invoke
    uint32_t timestamp_ms;    ///< Registration timestamp (for timeout)
  };

  enum class State {
    IDLE,
    SENDING_CHUNKS,
    AWAITING_RESPONSE
  };

  State state_{State::IDLE};
  std::deque<Command> command_queue_;
  uint32_t last_send_time_{0};
  uint32_t send_pacing_ms_{50}; // Delay between fragments or commands

  // Reassembly state
  bool reassembling_{false};                  ///< True if currently accumulating packet fragments
  std::vector<uint8_t> reassembly_buffer_;    ///< Buffer for accumulating packet fragments
  uint16_t expected_packet_length_{0};        ///< Expected total packet length
  uint32_t reassembly_started_ms_{0};         ///< When the current reassembly began (staleness guard)

  /// A partial frame older than this is abandoned rather than absorbing the
  /// next frame's bytes. The pump paces fragments ~50 ms apart, so any real
  /// frame completes far inside this window.
  static constexpr uint32_t REASSEMBLY_TIMEOUT_MS = 1000;

  // Hold-off after a write that failed PART-WAY through a packet.
  //
  // The peer is then holding the head of a frame whose length byte promises
  // more, and a receiver built like this one appends whatever arrives next
  // rather than treating a frame start as a fresh packet -- see
  // on_notification(). Sending the next command straight away therefore feeds
  // it into the wreckage: it is swallowed, and its caller waits out a full
  // command timeout with nothing to show. The ordinary 50 ms pacing is not
  // remotely enough, and it is measured from the last SUCCESSFUL chunk, so it
  // is already satisfied when the failure happens.
  //
  // Derived from REASSEMBLY_TIMEOUT_MS rather than written out, so the two
  // cannot drift: we hold for as long as our own receiver would need to
  // abandon a partial, plus a margin, and that is the best available estimate
  // of the pump's behaviour -- its reassembler cannot be observed from here.
  // The cost is paid only on a partial write, which is a rare fault.
  static constexpr uint32_t PEER_RESYNC_HOLD_MS = REASSEMBLY_TIMEOUT_MS + 100;

  /// Set when a write failed with bytes already on the wire; blocks the next
  /// send until the peer's partial frame must have gone stale.
  bool peer_resync_pending_{false};
  uint32_t peer_resync_started_ms_{0};

  // Callback for complete packets
  PacketCallback packet_callback_;            ///< Called when packet is complete
  WriteCallback write_callback_{nullptr};    ///< Callback for BLE writes

  // Response handler management
  std::vector<PendingHandler> pending_handlers_;  ///< Registered response handlers

  // Constants
  /// The reassembly ceiling: the largest telegram the protocol permits, which is
  /// protocol::MAX_LEGAL_TELEGRAM_LEN (257 = MAX_PDU_LEN + 4). It was 256, a
  /// round number sitting one byte under the only legal size above it, so a
  /// maximum-length frame was discarded as an overflow (issue #278). Latent --
  /// the largest frame in the corpus is 61 bytes -- but wrong, and wrong in the
  /// direction that drops good data.
  static constexpr size_t MAX_PACKET_SIZE = protocol::MAX_LEGAL_TELEGRAM_LEN;
  /// How much of a frame goes into one GATT write. NOT a ceiling imposed by the
  /// negotiated MTU, whatever the name suggests, and the difference is the point:
  /// **this pump ignores a frame that is not split into 20-byte writes**,
  /// however large the MTU has been negotiated to be. Reported from the Python
  /// client on a link with a 65-byte MTU, where a 27-byte frame fits inside one
  /// ATT write and the pump does nothing with it at all -- no reply, no refusal.
  ///
  /// So raising this after an MTU negotiation, which is exactly the shape of a
  /// plausible optimisation, silently stops every write from working while
  /// leaving reads intact. It is a protocol requirement of the peer, not a
  /// transport limit of ours.
  static constexpr size_t BLE_MTU_LIMIT = 20;
  static constexpr uint8_t FRAME_START_RESPONSE = 0x24;  ///< Response frame start byte
  static constexpr uint8_t FRAME_START_REQUEST = 0x27;   ///< Request frame start byte (echo)
  static constexpr size_t MAX_PENDING_HANDLERS = 10;     ///< Maximum pending response handlers
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
