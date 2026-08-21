#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * The pump's own daylight-saving rule, and whether the node agrees with it.
 *
 * ## Why this exists (issue #286)
 *
 * The pump keeps a DST rule of its own and applies it to its own clock, twice a
 * year, independently of anything the node believes. `DaylightSavingTime`
 * (Object 94, SubID 102, type 323 v1), read from the bench unit:
 *
 *     01 03 07 02 02 0b 07 01 02 3c
 *       enabled        1
 *       start          Mar, Sunday, occurrence 2, hour 2
 *       end            Nov, Sunday, occurrence 1, hour 2
 *       time_offset    60 minutes
 *
 * Single-event windows are stored in the pump's LOCAL Unix time, and
 * `utc_to_local_unix()` takes that offset from the **host's** zone at the
 * instant in question. That is correct only while the host's rule and the
 * pump's stored rule agree. They can differ three ways, and all three are
 * reachable without anybody doing anything strange:
 *
 *   - a pump shipped for one market and installed in another (the rule is
 *     stored per unit and is writable);
 *   - a node whose `time_id` zone is not the pump's locale;
 *   - `enabled = 0` on a pump whose Home Assistant instance is in a DST zone,
 *     which moves every stored event by an hour relative to the pump's clock,
 *     twice a year, silently.
 *
 * The failure is the same shape as issue #263 -- an event stored at an instant
 * nobody asked for, confirming clean, because the confirm applies the same
 * wrong offset in reverse. #263 fixed the case where the arithmetic *wraps*;
 * this is the case where the arithmetic is against the wrong rule, and no range
 * check can see it. An hour is comfortably inside every bound we have.
 *
 * ## What this does NOT do
 *
 * It does not write 94/102. Three clients write this pump's clock -- the GO
 * app, this component and the Python library -- and the pump cannot say which
 * base a value arrived in, so a component that silently corrected the pump's
 * rule would be the third party in a fight the user cannot see. Reporting the
 * disagreement is the whole job here; deciding it is the user's.
 */

/// One end of a DST rule: "the Nth <weekday> of <month>, at <hour> local".
struct DstTransition {
  uint8_t month{0};       ///< 1-12
  uint8_t weekday{0};     ///< 1 = Monday ... 7 = Sunday, as the pump encodes it
  uint8_t occurrence{0};  ///< 1-4, or 5 meaning "the last one in the month"
  uint8_t hour{0};        ///< 0-23, local

  bool operator==(const DstTransition &o) const {
    const bool same_day = month == o.month && weekday == o.weekday && hour == o.hour;
    if (!same_day)
      return false;
    // 5 is "the last one", which in a month where that weekday occurs only four
    // times IS the fourth. Treating them as different is how a correct EU rule
    // ("last Sunday in October") reads as a mismatch in some years and not
    // others -- the noisiest possible false positive, once every few years.
    const bool both_last = occurrence >= 4 && o.occurrence >= 4;
    return occurrence == o.occurrence || both_last;
  }
  bool operator!=(const DstTransition &o) const { return !(*this == o); }
};

/// A whole DST rule, from either side.
struct DstRule {
  bool valid{false};    ///< False when it could not be decoded / probed.
  bool enabled{false};  ///< False = this clock never shifts.
  DstTransition start{};
  DstTransition end{};
  uint16_t offset_minutes{0};  ///< How far the clock shifts. 60 everywhere seen.
};

/// How the pump's rule and the node's zone relate.
enum class DstAgreement {
  UNKNOWN,       ///< Not read yet, or the frame could not be decoded.
  AGREE,         ///< Both observe DST, on the same dates, by the same amount.
  NEITHER,       ///< Neither observes DST. Agreement, and worth saying so.
  PUMP_ONLY,     ///< The pump shifts; this node's zone does not.
  HOST_ONLY,     ///< This node's zone shifts; the pump does not.
  RULES_DIFFER,  ///< Both shift, on different dates or by a different amount.
};

/// True when nothing needs the user's attention.
inline bool dst_rules_agree(DstAgreement a) {
  return a == DstAgreement::AGREE || a == DstAgreement::NEITHER;
}

/// Bytes in the type 323 v1 object body, after the 3-byte size header.
static constexpr size_t DST_RULE_BODY_LEN = 10;

/**
 * Decode `DaylightSavingTime` (94/102, type 323 v1).
 *
 * @param body The object body: the 10 bytes AFTER the 3-byte size header, the
 *   same slice `TimeService::parse_clock_response()` skips to.
 */
inline DstRule decode_dst_rule(const uint8_t *body, size_t len) {
  DstRule r{};
  if (body == nullptr || len < DST_RULE_BODY_LEN)
    return r;  // valid stays false
  r.valid = true;
  r.enabled = body[0] != 0;
  r.start.month = body[1];
  r.start.weekday = body[2];
  r.start.occurrence = body[3];
  r.start.hour = body[4];
  r.end.month = body[5];
  r.end.weekday = body[6];
  r.end.occurrence = body[7];
  r.end.hour = body[8];
  r.offset_minutes = body[9];
  // A rule whose months are out of range is not a rule we can compare against
  // anything. Only checked when enabled: a disabled rule's fields are whatever
  // the factory left there, and on the bench unit they are still populated.
  if (r.enabled) {
    const bool months_in_range = r.start.month >= 1 && r.start.month <= 12 &&
                                 r.end.month >= 1 && r.end.month <= 12;
    const bool weekdays_in_range = r.start.weekday >= 1 && r.start.weekday <= 7 &&
                                   r.end.weekday >= 1 && r.end.weekday <= 7;
    if (!months_in_range || !weekdays_in_range)
      r.valid = false;
  }
  return r;
}

/// The pump's weekday encoding (1 = Monday ... 7 = Sunday) from `tm_wday`
/// (0 = Sunday ... 6 = Saturday).
inline uint8_t dst_weekday_from_tm(int tm_wday) {
  return static_cast<uint8_t>(tm_wday == 0 ? 7 : tm_wday);
}

/**
 * Derive the node timezone's own DST rule, by observation rather than by
 * parsing a TZ string.
 *
 * There is no portable way to ask libc "what is your rule"; ESP-IDF newlib and
 * glibc agree on `localtime_r` and on nothing else useful here. So this finds
 * the transitions the way anyone would by hand: sample the year, notice where
 * `tm_isdst` changes, and bisect for the second it changes on.
 *
 * @param year Calendar year to probe. The rule is read per-year deliberately --
 *   zones change their rules, and the answer that matters is the one for the
 *   year the pump is about to run a schedule in.
 */
inline DstRule probe_host_dst_rule(int year) {
  DstRule r{};

  auto local_at = [](time_t t) {
    struct tm lt {};
    localtime_r(&t, &lt);
    return lt;
  };
  auto utc_offset_at = [](time_t t) {
    struct tm lt {}, gt {};
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    int32_t off = (lt.tm_hour - gt.tm_hour) * 3600 + (lt.tm_min - gt.tm_min) * 60 +
                  (lt.tm_sec - gt.tm_sec);
    int day_delta = lt.tm_yday - gt.tm_yday;
    if (lt.tm_year != gt.tm_year)
      day_delta = (lt.tm_year > gt.tm_year) ? 1 : -1;
    return off + day_delta * 86400;
  };

  // Midnight UTC on 1 January of `year`, computed arithmetically.
  //
  // NOT timegm(): ESP-IDF's newlib does not provide it, and the host build
  // hides that completely -- glibc and Apple libc both have it, so this
  // compiled and passed every host test while failing the firmware build. And
  // NOT mktime() either: on ESP-IDF that does not apply the TZ to a UTC-field
  // tm, which is the same trap local_utc_offset_seconds() in
  // schedule_service.h documents. Days-since-epoch is portable arithmetic and
  // has neither problem.
  if (year < 1970 || year > 2100)
    return r;  // valid stays false
  int64_t days = 0;
  for (int y = 1970; y < year; y++) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    days += leap ? 366 : 365;
  }
  const bool this_year_is_leap =
      (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  const time_t year_start = static_cast<time_t>(days * 86400);
  const time_t year_end =
      year_start + static_cast<time_t>((this_year_is_leap ? 366 : 365) * 86400);

  r.valid = true;

  // Daily samples. A transition lasts months, so a day is far finer than it
  // needs to be, and 365 localtime_r calls is nothing next to a BLE round trip.
  const int32_t STEP = 86400;
  int transitions_found = 0;
  DstTransition found[2]{};
  int32_t offset_before_first = 0;
  int32_t offset_after_first = 0;

  int prev_isdst = local_at(year_start).tm_isdst;
  bool saw_dst = prev_isdst > 0;

  for (time_t t = year_start + STEP; t < year_end; t += STEP) {
    const int isdst = local_at(t).tm_isdst;
    if (isdst > 0)
      saw_dst = true;
    if (isdst == prev_isdst)
      continue;

    // Bisect the day for the second the offset changes.
    time_t lo = t - STEP, hi = t;
    while (hi - lo > 1) {
      const time_t mid = lo + (hi - lo) / 2;
      if (local_at(mid).tm_isdst == prev_isdst)
        lo = mid;
      else
        hi = mid;
    }

    // `hi` is the first second under the new offset. The rule's hour is the
    // local hour the clock was ABOUT to reach: spring 01:59:59 -> 03:00:00 is
    // "hour 2", and autumn 01:59:59 DST -> 01:00:00 standard is also "hour 2".
    // Both come out as (local hour just before the change) + 1.
    const struct tm just_before = local_at(hi - 1);
    if (transitions_found < 2) {
      DstTransition &d = found[transitions_found];
      d.month = static_cast<uint8_t>(just_before.tm_mon + 1);
      d.weekday = dst_weekday_from_tm(just_before.tm_wday);
      d.occurrence = static_cast<uint8_t>(((just_before.tm_mday - 1) / 7) + 1);
      d.hour = static_cast<uint8_t>((just_before.tm_hour + 1) % 24);
      // "Last in the month" is what a rule usually means when the date is in
      // the final week; record it the way the pump would encode it so the two
      // are comparable without special-casing at the comparison.
      const int days_in_month_after = just_before.tm_mday + 7;
      static const int DIM[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      if (days_in_month_after > DIM[just_before.tm_mon])
        d.occurrence = 5;
    }
    if (transitions_found == 0) {
      offset_before_first = utc_offset_at(hi - 1);
      offset_after_first = utc_offset_at(hi);
    }
    transitions_found++;
    prev_isdst = local_at(t).tm_isdst;
  }

  if (!saw_dst || transitions_found == 0) {
    r.enabled = false;
    return r;
  }
  r.enabled = true;

  // Order them the way the pump does: start = the shift ONTO daylight time.
  const bool first_is_the_spring_shift = offset_after_first > offset_before_first;
  r.start = first_is_the_spring_shift ? found[0] : found[1];
  r.end = first_is_the_spring_shift ? found[1] : found[0];

  const int32_t delta = offset_after_first - offset_before_first;
  const int32_t magnitude = delta < 0 ? -delta : delta;
  r.offset_minutes = static_cast<uint16_t>(magnitude / 60);
  return r;
}

/// Compare the pump's rule against the node's zone.
inline DstAgreement compare_dst_rules(const DstRule &pump, const DstRule &host) {
  if (!pump.valid || !host.valid)
    return DstAgreement::UNKNOWN;
  if (!pump.enabled && !host.enabled)
    return DstAgreement::NEITHER;
  if (pump.enabled && !host.enabled)
    return DstAgreement::PUMP_ONLY;
  if (!pump.enabled && host.enabled)
    return DstAgreement::HOST_ONLY;
  const bool same_dates = pump.start == host.start && pump.end == host.end;
  const bool same_amount = pump.offset_minutes == host.offset_minutes;
  if (same_dates && same_amount)
    return DstAgreement::AGREE;
  return DstAgreement::RULES_DIFFER;
}

/// "Mar Sun#2 02:00", for the user-facing string.
inline std::string format_dst_transition(const DstTransition &d) {
  static const char *MONTHS[13] = {"?",   "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const char *DAYS[8] = {"?", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  const char *m = (d.month >= 1 && d.month <= 12) ? MONTHS[d.month] : "?";
  const char *w = (d.weekday >= 1 && d.weekday <= 7) ? DAYS[d.weekday] : "?";
  char buf[32];
  if (d.occurrence >= 5)
    snprintf(buf, sizeof(buf), "%s %s(last) %02u:00", m, w, d.hour);
  else
    snprintf(buf, sizeof(buf), "%s %s#%u %02u:00", m, w, d.occurrence, d.hour);
  return std::string(buf);
}

/**
 * The `Pump Clock DST` entity's text.
 *
 * Says what disagrees rather than only that something does: "my schedule moved
 * an hour in November" is the symptom this exists to pre-empt, and a bare
 * "mismatch" would leave the user exactly as far from the cause.
 *
 * Bounded well under the 255-byte text-sensor limit by construction -- the
 * longest form is two formatted transitions per side plus fixed text.
 */
inline std::string format_dst_agreement(DstAgreement a, const DstRule &pump,
                                        const DstRule &host) {
  switch (a) {
    case DstAgreement::UNKNOWN:
      return "unknown";
    case DstAgreement::NEITHER:
      return "OK (neither observes DST)";
    case DstAgreement::AGREE:
      return "OK (" + format_dst_transition(pump.start) + " - " +
             format_dst_transition(pump.end) + ", +" +
             std::to_string(pump.offset_minutes) + " min)";
    case DstAgreement::PUMP_ONLY:
      return "Mismatch: the pump shifts for DST, this node's timezone does not";
    case DstAgreement::HOST_ONLY:
      return "Mismatch: this node's timezone shifts for DST, the pump does not";
    case DstAgreement::RULES_DIFFER:
      return "Mismatch: pump " + format_dst_transition(pump.start) + "/" +
             format_dst_transition(pump.end) + " +" +
             std::to_string(pump.offset_minutes) + ", node " +
             format_dst_transition(host.start) + "/" +
             format_dst_transition(host.end) + " +" +
             std::to_string(host.offset_minutes);
  }
  return "unknown";
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
