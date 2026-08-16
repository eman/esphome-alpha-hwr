#pragma once

#include "auth_gate.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include <cstddef>
#include <functional>

namespace esphome {
namespace alpha_hwr {
namespace core {

class Transport;

/**
 * @brief Authentication handler for Grundfos ALPHA HWR pumps
 * 
 * Implements the 3-stage GENI protocol authentication handshake:
 * 
 * Stage 1: Legacy Magic Burst (3x repeats, 50ms intervals)
 *   - Purpose: Backward compatibility with older firmware
 *   - Packet: Class 2 register-based SET operation
 *   - Register: 0x9495, Value: 0x96
 * 
 * Stage 2: Class 10 Unlock Burst (5x repeats, 50ms intervals)
 *   - Purpose: Primary authentication command
 *   - Packet: Class 10 DataObject SET operation
 *   - SubID: 0x5600, ObjID: 0x0006
 * 
 * Stage 3: Extension Packets (2 packets, sequential)
 *   - Purpose: Session establishment and capability negotiation
 *   - EXT_1 (Class 0x05) first, then EXT_2 (Class 0x0B)
 *   - Extends authentication timeout and seals the handshake
 * 
 * Timing Requirements:
 *   - Inter-packet delay: 50ms (allows pump processing)
 *   - Stage 1->2 delay: 100ms (allows stage completion)
 *   - Stage 2->3 delay: 200ms (allows authentication to settle)
 *   - Final stabilization: 500ms (ensures pump is ready)
 *
 * Those four delays are FLOORS, not the whole schedule. Each stage boundary is
 * gated on the pump having answered the stage's packets, waiting past the floor
 * if it has not and proceeding anyway at a ceiling. The pump replies to every
 * packet, in every stage, and until issue #174 none of it reached this class --
 * auth_gate.h carries the evidence, the reason the replies are observed rather
 * than matched as command responses, and the reason the ceiling fails open.
 *
 * Reference:
 *   Python implementation: reference/alpha-hwr/src/alpha_hwr/core/authentication.py
 *   Protocol docs: https://eman.github.io/alpha-hwr/reimplementation/
 */
class Authentication {
 public:
  /**
   * @brief Callback function type for completion notification
   * 
   * Called when authentication handshake completes successfully.
   * The session should transition to READY state after this callback.
   */
  using CompletionCallback = std::function<void()>;
  
  /**
   * @brief Callback function type for scheduling delayed tasks
   * 
   * @param delay_ms Delay in milliseconds
   * @param callback Function to call after delay
   */
  using SchedulerCallback = std::function<void(uint32_t delay_ms, std::function<void()> callback)>;
  
  /**
   * @brief Construct an Authentication handler
   */
  explicit Authentication(Transport &transport);
  
  /**
   * @brief Set the scheduler callback
   * 
   * This callback will be used to schedule delayed tasks.
   * 
   * @param callback Function to schedule delayed tasks
   */
  void set_scheduler_callback(SchedulerCallback callback);
  
  /**
   * @brief Set the completion callback
   * 
   * This callback will be invoked when the authentication handshake
   * completes successfully.
   * 
   * @param callback Function to call on completion
   */
  void set_completion_callback(CompletionCallback callback);
  
  /**
   * @brief Start the authentication handshake
   * 
   * Initiates the 3-stage authentication sequence. This is a non-blocking
   * operation that uses ESPHome's scheduler to manage timing.
   * 
   * The handshake will complete in approximately 1 second, after which
   * the completion callback will be invoked.
   */
  void start();
  
  /**
   * @brief Cancel any in-progress authentication
   * 
   * Stops the authentication sequence if it's currently running.
   * This does not reset the pump's authentication state.
   */
  void cancel();
  
  /**
   * @brief Check if authentication is currently in progress
   *
   * @return true if handshake is running
   */
  bool is_running() const { return running_; }

  /**
   * @brief Offer an inbound frame to the handshake.
   *
   * Wired to Transport::set_frame_observer(), so this sees every CRC-valid
   * frame on its way past and consumes none of them -- dispatch to command
   * callbacks and to the telemetry parser is unaffected. See auth_gate.h for
   * why the replies cannot instead be matched as command responses.
   *
   * Frames arriving while no handshake is running are ignored.
   */
  void on_frame(const uint8_t *data, size_t len);

  /**
   * @brief Replies counted during the handshake that just ran.
   *
   * Zero means the pump answered nothing at all -- the deaf link, known by the
   * end of the handshake rather than 60 s later via the inbound-data watchdog.
   * See Authentication::complete() for when that is (2700 ms of scheduled
   * delay, since every gate runs to its ceiling first) and for the false
   * negative the class-based counting leaves in it. Valid after the completion
   * callback fires; reset by start().
   */
  uint16_t replies_seen() const { return replies_total_; }

  /**
   * @brief Packets the handshake sent, for comparison with replies_seen().
   *
   * Ten on a completed handshake, fewer on one cut short.
   */
  uint16_t packets_sent() const { return packets_total_; }

 private:
  Transport &transport_;  ///< Transport layer
  SchedulerCallback scheduler_callback_;  ///< Callback to schedule delayed tasks
  CompletionCallback completion_callback_;  ///< Callback for completion
  bool running_ = false;  ///< True if authentication is in progress
  uint32_t auth_sequence_ = 0;  ///< Sequence counter to invalidate stale lambdas

  /// Stage whose packets are currently in flight (1-3, 0 before start).
  /// auth_frame_answers_stage() uses it to refuse crediting a reply to a stage
  /// that has not sent anything yet.
  uint8_t current_stage_ = 0;
  /// Replies counted per stage, indexed by stage - 1.
  uint8_t stage_replies_[3] = {0, 0, 0};
  /// Ticks the gate now waiting has already spent past its floor.
  uint8_t gate_waits_ = 0;
  uint16_t replies_total_ = 0;
  uint16_t packets_total_ = 0;

  // Authentication stage functions
  void stage1_legacy_burst(int repeat_count);
  void stage2_class10_burst(int repeat_count);
  void stage3_extensions();
  void complete();

  /// Packets stage @p stage sends, and therefore the replies its gate waits
  /// for.
  static uint8_t packets_in_stage(uint8_t stage);

  /// Re-evaluate the gate at the end of stage @p stage and either wait another
  /// tick or run @p advance. Scheduled at the stage's floor delay by the stage
  /// itself, and re-scheduled at AUTH_GATE_POLL_MS by itself while waiting.
  void gate_stage_(uint8_t stage, std::function<void()> advance);

  /// Schedule @p fn after @p delay_ms with the stale-callback guard every
  /// scheduled step in this class needs.
  void schedule_(uint32_t delay_ms, std::function<void()> fn);

  // Helper function to send a packet
  bool send_packet(const uint8_t* data, size_t len);
};

// ============================================================================
// AUTHENTICATION PACKETS
// ============================================================================
// 
// These packets are copied EXACTLY from the Python reference implementation
// at reference/alpha-hwr/src/alpha_hwr/core/authentication.py
// 
// DO NOT modify these packets unless the Python reference is also updated!
// ============================================================================

/**
 * @brief Legacy Magic Packet (Class 2, Register-based SET)
 * 
 * Frame: 27 07 E7 F8 02 03 94 95 96 EB 47
 * 
 * Breakdown:
 *   27       - Frame start
 *   07       - Length (7 bytes)
 *   E7 F8    - Service ID (GENI)
 *   02       - Class 2 (Register-based operations)
 *   03       - OpSpec: SET operation, 3 data bytes
 *   94 95    - Register address: 0x9495 (unlock register)
 *   96       - Data value: 0x96 (unlock code)
 *   EB 47    - CRC-16-CCITT
 */
static const uint8_t AUTH_LEGACY[] = {0x27, 0x07, 0xE7, 0xF8, 0x02, 0x03, 0x94, 0x95, 0x96, 0xEB, 0x47};

/**
 * @brief Class 10 Unlock Packet (DataObject SET)
 * 
 * Frame: 27 07 E7 F8 0A 03 56 00 06 C5 5A
 * 
 * Breakdown:
 *   27       - Frame start
 *   07       - Length (7 bytes)
 *   E7 F8    - Service ID (GENI)
 *   0A       - Class 10 (DataObject operations)
 *   03       - OpSpec: SET operation
 *   56 00    - Sub-ID: 0x5600 (control/unlock subsystem)
 *   06       - Object ID: 0x0006 (unlock object)
 *   C5 5A    - CRC-16-CCITT
 */
static const uint8_t AUTH_CLASS10[] = {0x27, 0x07, 0xE7, 0xF8, 0x0A, 0x03, 0x56, 0x00, 0x06, 0xC5, 0x5A};

/**
 * @brief Extension Packet 1 (Class 0x05)
 * 
 * Frame: 27 05 E7 F8 05 C1 4B C3 82
 * 
 * Purpose: Extend authentication session (Part 1). Must be sent first.
 * Order documented in protocol/connection.md Step C.
 */
static const uint8_t AUTH_EXT_1[] = {0x27, 0x05, 0xE7, 0xF8, 0x05, 0xC1, 0x4B, 0xC3, 0x82};

/**
 * @brief Extension Packet 2 (Class 0x0B)
 * 
 * Frame: 27 05 E7 F8 0B C1 0F D0 C3
 * 
 * Purpose: Extend authentication session (Part 2). Sent after EXT_1.
 */
static const uint8_t AUTH_EXT_2[] = {0x27, 0x05, 0xE7, 0xF8, 0x0B, 0xC1, 0x0F, 0xD0, 0xC3};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
