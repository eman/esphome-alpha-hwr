#pragma once
#include "esphome/core/component.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

// Mock of ESPHome's ESPTime (esphome/core/time.h).
//
// Field layout, is_valid()'s year>=2019 rule and fields_in_range()'s bounds are
// copied from the real header so a struct that passes here passes there.
//
// Fidelity boundary, stated because a lying mock is itself a defect. Real
// ESPHome resolves local time through its own POSIX-TZ engine when
// USE_TIME_TIMEZONE is set, and falls back to plain UTC when it is not. This
// mock always goes through libc (mktime/localtime/gmtime), so it tracks the
// process TZ environment variable. Tests that care about the local<->UTC
// mapping must therefore pin TZ themselves; under the zero-offset, no-DST pin
// test_write_operations.cpp's main() sets (TZ=UTC) all three implementations --
// real-with-timezone, real-without, and this one -- agree exactly.
//
// One clock test deliberately un-pins it to drive the DST fall-back fold, where
// they must still agree on which of two instants an ambiguous local time means:
// real ESPHome documents "prefer standard time" and this mock gets there by
// handing mktime tm_isdst = -1.

namespace esphome {

inline uint8_t days_in_month(uint8_t month, uint16_t year) {
  static const uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
  return DAYS[month - 1];
}

struct ESPTime {
  uint8_t second{0};
  uint8_t minute{0};
  uint8_t hour{0};
  uint8_t day_of_week{0};
  uint8_t day_of_month{0};
  uint16_t day_of_year{0};
  uint8_t month{0};
  uint16_t year{0};
  bool is_dst{false};
  time_t timestamp{0};

  bool fields_in_range(bool check_day_of_week = true, bool check_day_of_year = true) const {
    bool valid = this->second < 61 && this->minute < 60 && this->hour < 24 && this->month > 0 &&
                 this->month < 13 && this->day_of_month > 0 &&
                 this->day_of_month <= days_in_month(this->month, this->year);
    if (check_day_of_week) valid = valid && this->day_of_week > 0 && this->day_of_week < 8;
    if (check_day_of_year) valid = valid && this->day_of_year > 0 && this->day_of_year < 367;
    return valid;
  }

  bool is_valid() const { return this->year >= 2019 && this->fields_in_range(); }

  static ESPTime from_c_tm(struct tm *c_tm, time_t c_time) {
    ESPTime res{};
    res.second = static_cast<uint8_t>(c_tm->tm_sec);
    res.minute = static_cast<uint8_t>(c_tm->tm_min);
    res.hour = static_cast<uint8_t>(c_tm->tm_hour);
    res.day_of_week = static_cast<uint8_t>(c_tm->tm_wday + 1);
    res.day_of_month = static_cast<uint8_t>(c_tm->tm_mday);
    res.day_of_year = static_cast<uint16_t>(c_tm->tm_yday + 1);
    res.month = static_cast<uint8_t>(c_tm->tm_mon + 1);
    res.year = static_cast<uint16_t>(c_tm->tm_year + 1900);
    res.is_dst = c_tm->tm_isdst > 0;
    res.timestamp = c_time;
    return res;
  }

  static ESPTime from_epoch_utc(time_t epoch) {
    struct tm buf;
    struct tm *c_tm = ::gmtime_r(&epoch, &buf);
    if (c_tm == nullptr) return ESPTime{};
    return ESPTime::from_c_tm(c_tm, epoch);
  }

  static ESPTime from_epoch_local(time_t epoch) {
    struct tm buf;
    struct tm *c_tm = ::localtime_r(&epoch, &buf);
    if (c_tm == nullptr) return ESPTime{};
    return ESPTime::from_c_tm(c_tm, epoch);
  }

  struct tm to_c_tm() {
    struct tm c_tm {};
    c_tm.tm_sec = this->second;
    c_tm.tm_min = this->minute;
    c_tm.tm_hour = this->hour;
    c_tm.tm_mday = this->day_of_month;
    c_tm.tm_mon = this->month - 1;
    c_tm.tm_year = this->year - 1900;
    c_tm.tm_wday = this->day_of_week - 1;
    c_tm.tm_yday = this->day_of_year - 1;
    c_tm.tm_isdst = this->is_dst;
    return c_tm;
  }

  void recalc_timestamp_local() {
    struct tm c_tm = this->to_c_tm();
    c_tm.tm_isdst = -1;  // let libc resolve the DST flag from the local fields
    this->timestamp = ::mktime(&c_tm);
  }

  void recalc_timestamp_utc(bool /*use_day_of_year*/ = true) {
    struct tm c_tm = this->to_c_tm();
    this->timestamp = ::timegm(&c_tm);
  }

  size_t strftime(char *buffer, size_t buffer_len, const char *format) {
    struct tm c_tm = this->to_c_tm();
    return ::strftime(buffer, buffer_len, format, &c_tm);
  }
};

namespace time {
class RealTimeClock : public Component {
 public:
  ESPTime now() { return ESPTime::from_epoch_local(this->epoch_); }
  void set_epoch_for_test(time_t epoch) { this->epoch_ = epoch; }

 protected:
  time_t epoch_{0};
};
}  // namespace time
}  // namespace esphome
