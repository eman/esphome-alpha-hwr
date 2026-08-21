#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "esphome/core/time.h"

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

  /// Exact field equality. Note this is NOT how two rules are compared --
  /// see resolve_transition_day() and compare_dst_rules(), which resolve both
  /// sides to a concrete date first.
  ///
  /// An earlier version of this operator treated any two occurrences `>= 4` as
  /// equal, reasoning that "last" is sometimes the fourth. That is wrong in the
  /// direction that matters: a genuine "fourth Sunday" rule and a genuine "last
  /// Sunday" rule are *different rules*, and in October 2027 they fall a week
  /// apart (the 24th against the 31st). Collapsing them reported AGREE for
  /// exactly the case this feature exists to catch.
  bool operator==(const DstTransition &o) const {
    return month == o.month && weekday == o.weekday && hour == o.hour &&
           occurrence == o.occurrence;
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

/// True when every field could describe a real recurring date.
///
/// `occurrence` runs 1-5, where 5 is "the last one in the month"; anything
/// above 5 is not a slot the pump has.
inline bool dst_transition_fields_in_range(const DstTransition &d) {
  const bool month_ok = d.month >= 1 && d.month <= 12;
  const bool weekday_ok = d.weekday >= 1 && d.weekday <= 7;
  const bool occurrence_ok = d.occurrence >= 1 && d.occurrence <= 5;
  const bool hour_ok = d.hour <= 23;
  return month_ok && weekday_ok && occurrence_ok && hour_ok;
}

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
  // Every field of an enabled rule is checked, not just the month and weekday.
  // A payload carrying occurrence 0 or hour 24 would otherwise decode into a
  // genuine-looking rule that resolves to no date at all -- reported to the user
  // as a mismatch against their timezone, which blames the wrong thing.
  //
  // Only when enabled: a disabled rule's fields are whatever the factory left
  // there, and on the bench unit they are still populated. Refusing those would
  // report "unknown" for a pump that is perfectly clear it does not shift.
  if (r.enabled) {
    const bool start_ok = dst_transition_fields_in_range(r.start);
    const bool end_ok = dst_transition_fields_in_range(r.end);
    if (!start_ok || !end_ok)
      r.valid = false;
  }
  return r;
}

inline bool dst_year_is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

inline uint8_t dst_days_in_month(uint8_t month, int year) {
  static const uint8_t DIM[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12)
    return 0;
  if (month == 2 && dst_year_is_leap(year))
    return 29;
  return DIM[month - 1];
}

/// Days from 1970-01-01 to @p year-@p month-@p day.
///
/// Arithmetic rather than libc: ESP-IDF's newlib has no timegm(), and its
/// mktime() does not apply the TZ to a UTC-field tm (the trap
/// local_utc_offset_seconds() in schedule_service.h documents). Both would be
/// the obvious way to do this and neither works on the target.
inline int64_t dst_days_from_epoch(int year, uint8_t month, uint8_t day) {
  int64_t days = 0;
  for (int y = 1970; y < year; y++)
    days += dst_year_is_leap(y) ? 366 : 365;
  for (uint8_t m = 1; m < month; m++)
    days += dst_days_in_month(m, year);
  return days + (day - 1);
}

/// Day of the week for a civil date, in the pump's encoding (1 = Monday).
inline uint8_t dst_weekday_of(int year, uint8_t month, uint8_t day) {
  // 1970-01-01 was a Thursday, which is 4 here.
  const int64_t d = dst_days_from_epoch(year, month, day);
  int w = static_cast<int>((d + 3) % 7);
  if (w < 0)
    w += 7;
  return static_cast<uint8_t>(w + 1);
}

/**
 * The day of the month a transition rule lands on in @p year, or 0 when the
 * fields cannot describe one.
 *
 * This is what makes two rules comparable. "The last Sunday of October" and
 * "the fourth Sunday of October" are different rules that happen to coincide in
 * some years, so comparing the *fields* is wrong in one direction and comparing
 * them loosely is wrong in the other. Resolving both sides to a concrete date
 * in the year being asked about is right in both: it answers "will these two
 * clocks change on the same day", which is the only question anybody has.
 *
 * `occurrence >= 5` means "the last one in the month", which is how the EU rule
 * is normally encoded.
 */
inline uint8_t resolve_transition_day(const DstTransition &d, int year) {
  const uint8_t dim = dst_days_in_month(d.month, year);
  if (dim == 0)
    return 0;
  if (d.weekday < 1 || d.weekday > 7)
    return 0;
  if (d.occurrence < 1)
    return 0;

  uint8_t first = 0;
  for (uint8_t day = 1; day <= 7; day++) {
    if (dst_weekday_of(year, d.month, day) == d.weekday) {
      first = day;
      break;
    }
  }
  if (first == 0)
    return 0;  // unreachable: any weekday occurs in the first seven days

  if (d.occurrence >= 5) {
    uint8_t last = first;
    while (static_cast<uint8_t>(last + 7) <= dim)
      last = static_cast<uint8_t>(last + 7);
    return last;
  }
  const int day = first + 7 * (d.occurrence - 1);
  if (day > dim)
    return 0;  // e.g. "the fifth Sunday" of a month with four
  return static_cast<uint8_t>(day);
}

/**
 * Turn what the sampler found into a rule.
 *
 * Separated from probe_host_dst_rule() so the counts it rejects can be tested
 * directly. They cannot be reached reliably through a timezone: a POSIX TZ
 * string always describes exactly two transitions a year, and the real zones
 * that have one (a country adopting or abolishing DST) or three (Morocco's
 * Ramadan rule) depend on which tzdata the machine happens to ship. A test
 * written against those passes or fails by accident -- CI's mutation sweep
 * caught exactly that, with this guard surviving because the fixture zone had
 * two transitions on the runner.
 *
 * @param count Transitions the sampler saw across the year.
 * @param found The first two of them.
 * @param delta Each one's offset change, in seconds.
 */
inline DstRule assemble_probed_rule(int count, const DstTransition *found,
                                    const int32_t *delta) {
  DstRule r{};

  // A zone that never shifts. Honest and comparable: the pump may say the same.
  if (count == 0) {
    r.valid = true;
    r.enabled = false;
    return r;
  }

  // Anything other than a clean pair is a rule this comparison cannot express,
  // and saying so beats emitting half of one. A year in which a country adopts
  // or abolishes DST has ONE transition, and the old code left the other half
  // {0,0,0,0} while claiming `enabled` -- printed to the user as "? ?#0 00:00"
  // (Brazil 2019, Fiji 2021, Apia 2021, Montevideo 2015). Morocco has THREE,
  // for Ramadan, and the third was silently dropped. `valid` stays false, so
  // compare_dst_rules() answers UNKNOWN and the entity says "unknown" rather
  // than accusing the user's timezone.
  if (count != 2)
    return r;

  r.valid = true;
  r.enabled = true;

  // Order them the way the pump does: `start` is the shift ONTO daylight time.
  // Keyed on the offset DIRECTION, not on an is_dst flag -- a zone redefinition
  // can flip is_dst without moving the clock (America/Grand_Turk 2018), and
  // reading that as a transition reported "+0" with start and end reversed.
  const bool first_is_the_spring_shift = delta[0] > 0;
  r.start = first_is_the_spring_shift ? found[0] : found[1];
  r.end = first_is_the_spring_shift ? found[1] : found[0];

  const int32_t magnitude = delta[0] < 0 ? -delta[0] : delta[0];
  r.offset_minutes = static_cast<uint16_t>(magnitude / 60);
  return r;
}

/**
 * Derive the node timezone's own DST rule, by observation rather than by
 * parsing a TZ string.
 *
 * There is no way to ask for a zone's rule directly -- ESPHome parses the TZ
 * string into its own structure and does not expose it -- so this finds the
 * transitions the way anyone would by hand: sample the year, notice where the
 * UTC OFFSET changes, and bisect for the second it changes on.
 *
 * Offset rather than an is_dst flag, and ESPTime rather than libc. Both are
 * corrections; see the notes in the body.
 *
 * @param year Calendar year to probe. The rule is read per-year deliberately --
 *   zones change their rules, and the answer that matters is the one for the
 *   year the pump is about to run a schedule in.
 */
inline DstRule probe_host_dst_rule(int year) {
  DstRule r{};
  if (year < 1970 || year > 2100)
    return r;  // valid stays false

  // The node's own zone, sampled through ESPTime -- **not** libc (issue #289).
  // ESPHome calls tzset() only under USE_HOST, so on the ESP32 localtime_r
  // answers UTC and this whole function used to find no transitions at all,
  // reporting every user in a DST zone as disagreeing with their pump.
  auto offset_at = [](time_t t) -> int32_t {
    ESPTime fields = ESPTime::from_epoch_local(t);
    fields.recalc_timestamp_utc(false);
    return static_cast<int32_t>(fields.timestamp - t);
  };

  const time_t year_start = static_cast<time_t>(dst_days_from_epoch(year, 1, 1) * 86400);
  const time_t year_end =
      static_cast<time_t>(dst_days_from_epoch(year + 1, 1, 1) * 86400);

  // Daily samples. A DST season lasts months, so a day is far finer than it
  // needs to be, and 365 conversions is nothing next to a BLE round trip.
  const int32_t STEP = 86400;

  DstTransition found[2]{};
  int32_t delta[2] = {0, 0};
  int transitions_found = 0;

  int32_t prev_offset = offset_at(year_start);
  const int32_t first_offset = prev_offset;

  for (time_t t = year_start + STEP; t < year_end; t += STEP) {
    const int32_t offset = offset_at(t);
    if (offset == prev_offset)
      continue;

    // Bisect the day for the second the offset changes.
    time_t lo = t - STEP, hi = t;
    while (hi - lo > 1) {
      const time_t mid = lo + (hi - lo) / 2;
      if (offset_at(mid) == prev_offset)
        lo = mid;
      else
        hi = mid;
    }

    if (transitions_found < 2) {
      // `hi` is the first second under the new offset, so `hi - 1` is the last
      // reading before it. The rule names the wall clock the clock was ABOUT to
      // reach -- spring 01:59:59 -> 03:00:00 is "hour 2", and autumn
      // 01:59:59 DST -> 01:00:00 standard is also "hour 2".
      //
      // Both the DATE and the HOUR must come from that one reading and be
      // advanced together. Taking the date from `hi - 1` while wrapping only
      // the hour named a day 24 h early for every zone whose transition is at
      // local midnight -- Havana, Cairo, Beirut, the Azores, Santiago, Nuuk;
      // 212 start-side wrong answers across the tz database. That is the bug
      // that got this feature withdrawn from #287.
      const ESPTime before = ESPTime::from_epoch_local(hi - 1);
      int y = static_cast<int>(before.year);
      uint8_t mo = before.month;
      uint8_t d = before.day_of_month;
      uint8_t h = static_cast<uint8_t>(before.hour + 1);
      if (h == 24) {  // the wall clock rolls into the next civil day
        h = 0;
        if (++d > dst_days_in_month(mo, y)) {
          d = 1;
          if (++mo > 12) {
            mo = 1;
            y++;
          }
        }
      }
      DstTransition &out = found[transitions_found];
      out.month = mo;
      out.weekday = dst_weekday_of(y, mo, d);
      out.occurrence = static_cast<uint8_t>(((d - 1) / 7) + 1);
      out.hour = h;
      delta[transitions_found] = offset - prev_offset;
    }
    transitions_found++;
    prev_offset = offset;
  }

  (void) first_offset;
  return assemble_probed_rule(transitions_found, found, delta);
}

/// True when two transition rules land on the same instant in @p year.
///
/// Resolved to a concrete date rather than compared field by field, so "the
/// last Sunday of October" and "the fourth Sunday of October" agree in a year
/// where they coincide and disagree in a year where they do not -- which is
/// what they actually do, and what a user needs to know.
inline bool transitions_coincide(const DstTransition &a, const DstTransition &b,
                                 int year) {
  if (a.month != b.month)
    return false;
  if (a.hour != b.hour)
    return false;
  const uint8_t day_a = resolve_transition_day(a, year);
  const uint8_t day_b = resolve_transition_day(b, year);
  if (day_a == 0 || day_b == 0)
    return false;  // one of them describes no date; not something to call equal
  return day_a == day_b;
}

/**
 * Compare the pump's rule against the node's zone, for @p year.
 *
 * The year is a parameter rather than a constant because the comparison is
 * about dates: a rule is a recurrence, and two recurrences can coincide in one
 * year and not the next. The year that matters is the one the pump is about to
 * run a schedule in.
 */
inline DstAgreement compare_dst_rules(const DstRule &pump, const DstRule &host,
                                      int year) {
  if (!pump.valid || !host.valid)
    return DstAgreement::UNKNOWN;
  if (!pump.enabled && !host.enabled)
    return DstAgreement::NEITHER;
  if (pump.enabled && !host.enabled)
    return DstAgreement::PUMP_ONLY;
  if (!pump.enabled && host.enabled)
    return DstAgreement::HOST_ONLY;
  const bool same_start = transitions_coincide(pump.start, host.start, year);
  const bool same_end = transitions_coincide(pump.end, host.end, year);
  const bool same_amount = pump.offset_minutes == host.offset_minutes;
  if (same_start && same_end && same_amount)
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
