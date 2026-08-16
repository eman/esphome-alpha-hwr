#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include "transport.h"
#include <ctime>

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * @brief Time management service for Grundfos ALPHA HWR pumps.
 *
 * Handles reading and synchronizing the pump's real-time clock (RTC).
 * The RTC is used for schedule execution and event logging.
 *
 * Protocol Details, as captured from the pump (2026-08-15) rather than as
 * transcribed from the Python reference, which described both frames wrongly:
 *
 * - Read Time: Object 94, SubID 101 (DateTimeActual), type 322 version 1.
 *   Object body, which is what the command callback receives:
 *     [Size(3BE) = 12][Year(2BE)][Month][Day][Hour][Minute][Second][5 trailing]
 *   The first three bytes were long described as "[Status(2)][Length(1)]" and
 *   are the ordinary size header; the trailing five are not read.
 *
 * - Set Time: Object 94, SubID 100 (DateTimeConfig), type 321 version 2.
 *   22-byte APDU, 11 data bytes:
 *     [0A][94][5E][00 64][01 41][02][00 00 0B][01][Year(2BE)][M][D][h][m][s][0][0][0]
 *   The older description here -- 19 payload bytes, a frame starting
 *   [27][len][07][5E][64][70] -- matched neither the code below it nor the
 *   wire; see send_set_clock_command() for what the fields are and how they
 *   were identified.
 */
class TimeService {
 public:
  /**
   * @brief Construct a new TimeService.
   * @param transport Transport layer for BLE communication
   */
  explicit TimeService(core::Transport *transport) : transport_(transport) {}
  
#ifdef USE_TIME
  void set_time_id(time::RealTimeClock *time_id) { time_id_ = time_id; }
#endif

  /**
   * @brief Read the pump's internal real-time clock.
   *
   * Reads from Object 94, SubID 101 (DateTimeActual).
   *
   * @param callback Called with pump time as ESPTime, or empty ESPTime on failure
   *
   * Implementation Notes:
   * - Uses Class 10 GET on Object 94, SubID 101
   * - Response format: [Status(2)][Length(1)][Year(2)][Month(1)][Day(1)][Hour(1)][Minute(1)][Second(1)]
   * - Status 0x0000 = valid, 0xFFFF = unset
   * - Year is big-endian uint16
   * - Invalid dates (year < 1970) indicate unset clock
   */
  void get_clock_async(std::function<void(ESPTime)> callback);

  /**
   * @brief Wire primitive: write the pump's real-time clock.
   *
   * Object 94 SubID 100 (DateTimeConfig), type 321 version 2, 11 data bytes:
   * `[0x01][Year(2BE)][Month][Day][Hour][Minute][Second][0][0][0]`. The fields
   * are LOCAL time, matching what SubID 101 reads back.
   *
   * Deliberately fire-and-forget, and deliberately not the verdict. Nothing in
   * this repo records what -- if anything -- the pump answers a SubID 100 SET
   * with, so there is no ACK shape to match: the transport's short-ACK path is
   * an allowlist keyed on the queued OpSpec plus object bytes and this write is
   * not on it. Passing a callback anyway would buy a 3 s wildcard window that
   * can only ever close empty, and its `false` would be indistinguishable from
   * a genuine send failure. So the caller confirms the only honest way, by
   * reading SubID 101 back -- the same "the ACK is not the verdict" rule the
   * Class 3 commands follow for the opposite reason (issue #46).
   *
   * Bench-confirmed (2026-08-15): this exact APDU,
   *   0A 94 5E 00 64 01 41 02 00 00 0B 01 07 EA 08 0F 14 27 07 00 00 00
   * written at 20:39:07, read back as 20:39:08 one round trip later. The write
   * lands whether or not anything acknowledges it.
   *
   * Not a standalone write path (AGENTS §6): the only caller is
   * WriteOperationService::run_set_clock_(), which owns the confirm, the
   * watchdog and the settle event.
   *
   * @param local_now Node wall-clock time to write, as local fields.
   */
  void send_set_clock_command(const ESPTime &local_now);

  /**
   * @brief Resolve the node's own wall clock.
   *
   * @param out Receives local time when this returns true.
   * @return False when no `time_id` is configured, or when the component has
   *   one but SNTP has not synced yet -- in both cases there is nothing worth
   *   writing to the pump.
   */
  bool current_time(ESPTime &out) const;

 private:
  core::Transport *transport_;
#ifdef USE_TIME
  time::RealTimeClock *time_id_{nullptr};
#endif

  /**
   * @brief Parse clock response data.
   *
   * Extracts datetime from Class 10 response payload.
   *
   * @param data Raw response data
   * @param len Data length
   * @return ESPTime parsed from response, or empty ESPTime on failure
   */
  static ESPTime parse_clock_response(const uint8_t *data, size_t len);
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
