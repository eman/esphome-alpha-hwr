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
    uint16_t expect_obj_id{0};
    uint16_t expect_sub_id{0};
    CommandCallback callback{nullptr};
    uint32_t timeout_ms{3000};
    uint32_t timestamp_ms{0};
    bool waiting_for_response{false};
    bool allow_register_read{false};  // When true, don't filter register-read OpSpecs
    bool expect_short_ack{false};     // When true, disables the Class 10 wildcard matching path
    bool quiet_timeout{false};        // When true, a response timeout is expected (fire-and-forget
                                      // write); log it at DEBUG instead of WARNING
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
   * @param packet The complete GENI packet to send
   * @param expect_obj_id If non-zero, wait for response with this Object ID
   * @param expect_sub_id If non-zero, wait for response with this Sub-ID
   * @param callback Called when command completes or times out
   * @param timeout_ms How long to wait for response
   */
  void send_command(const std::vector<uint8_t>& packet, uint16_t expect_obj_id = 0,
                    uint16_t expect_sub_id = 0, CommandCallback callback = nullptr,
                    uint32_t timeout_ms = 3000, bool allow_register_read = false,
                    bool expect_short_ack = false, bool quiet_timeout = false);

  /**
   * Helper to build and queue a command directly from an APDU.
   * This abstracts away the GENI addressing (Service ID, Source ID)
   * from the service layer.
   */
  void send_apdu_command(const uint8_t* apdu, size_t apdu_len,
                         uint16_t expect_obj_id = 0, uint16_t expect_sub_id = 0,
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
   * Reset transport state.
   * 
   * Clears reassembly buffer and flags. Should be called on:
   * - BLE disconnection
   * - Fatal errors
   * - Manual reset request
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

  // Callback for complete packets
  PacketCallback packet_callback_;            ///< Called when packet is complete
  WriteCallback write_callback_{nullptr};    ///< Callback for BLE writes

  // Response handler management
  std::vector<PendingHandler> pending_handlers_;  ///< Registered response handlers

  // Constants
  static constexpr size_t MAX_PACKET_SIZE = 256;  ///< Maximum GENI packet size (safety limit)
  static constexpr size_t BLE_MTU_LIMIT = 20;     ///< BLE write size limit (MTU - headers)
  static constexpr uint8_t FRAME_START_RESPONSE = 0x24;  ///< Response frame start byte
  static constexpr uint8_t FRAME_START_REQUEST = 0x27;   ///< Request frame start byte (echo)
  static constexpr size_t MAX_PENDING_HANDLERS = 10;     ///< Maximum pending response handlers
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
