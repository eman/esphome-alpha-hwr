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
 * Protocol Details:
 * - Read Time: Object 94, SubID 101 (DateTimeActual)
 *   Response: [Status(2)][Length(1)][Year(2BE)][Month][Day][Hour][Minute][Second]
 *   Status 0x0000 = valid, 0xFFFF = unset
 *
 * - Set Time: Object 94, SubID 100 (DateTimeConfig)
 *   Payload: [Year(2BE)][Month][Day][Hour][Minute][Second] + 13 padding bytes (19 total)
 *   Frame: [0x27][Length][0x07][0x5E][0x64][0x70][DateTime...][CRC]
 *
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/time.py
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
   * @brief Synchronize the pump's internal real-time clock with system time.
   *
   * Writes to Object 94, SubID 100 (DateTimeConfig) using the standard protocol format.
   *
   * @param callback Called with success status (true if clock was set successfully)
   *
   * Implementation Notes:
   * - Uses Object 94, SubID 100 (DateTimeConfig) with SET operation
   * - Format: [UnknownByte][Object][SubID][OpSpec][DateTime...]
   * - DateTime format: [Year(2BE)][Month][Day][Hour][Minute][Second][padding(13 bytes)]
   * - Total payload: 19 bytes
   * - Uses ESPHome's sntp_time to get current local time
   */
  void set_clock_async(std::function<void(bool)> callback);

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
