// Host tests for dst_rule.h -- the pump's own daylight-saving rule, and whether
// the node's timezone agrees with it (issue #286).
//
// Why this matters here rather than in the sibling Python library, which
// documents the same object and does not need it: that library expresses naive
// local wall clock end to end and never applies a host UTC offset, so it has no
// host rule to disagree with the pump's. This component's public surface is
// Home Assistant, which speaks UTC, so it converts at the edge --
// utc_to_local_unix() takes the offset from the HOST's zone. That conversion
// preserves the user's wall clock across a transition (an 07:00 event stays at
// 07:00) only while the two rules agree, and nothing checked that they did.
//
// The failure is the same shape as issue #263: an event stored at an instant
// nobody asked for, confirming clean, because the confirm applies the same
// wrong offset in reverse. #263 fixed the case where the arithmetic wraps; this
// is the case where it is against the wrong rule, and an hour is comfortably
// inside every bound we have.

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/dst_rule.h"

using esphome::alpha_hwr::services::compare_dst_rules;
using esphome::alpha_hwr::services::resolve_transition_day;
using esphome::alpha_hwr::services::transitions_coincide;
using esphome::alpha_hwr::services::decode_dst_rule;
using esphome::alpha_hwr::services::DstAgreement;
using esphome::alpha_hwr::services::dst_rules_agree;
using esphome::alpha_hwr::services::DstRule;
using esphome::alpha_hwr::services::DstTransition;
using esphome::alpha_hwr::services::format_dst_agreement;
using esphome::alpha_hwr::services::probe_host_dst_rule;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  do {                                                                         \
    if (condition) {                                                           \
      tests_passed++;                                                          \
      std::cout << "[PASS] " << message << std::endl;                          \
    } else {                                                                   \
      tests_failed++;                                                          \
      std::cout << "[FAIL] " << message << std::endl;                          \
    }                                                                          \
  } while (0)

// The bench unit's own bytes, recorded in resources and independently described
// in the sibling library's pump_time.py. Two sources agreeing on a layout that
// would otherwise rest on one capture.
static const uint8_t BENCH_RULE[10] = {0x01, 0x03, 0x07, 0x02, 0x02,
                                       0x0b, 0x07, 0x01, 0x02, 0x3c};

static void with_tz(const char *tz, void (*body)()) {
  setenv("TZ", tz, 1);
  tzset();
  body();
  setenv("TZ", "UTC", 1);
  tzset();
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

static void test_the_bench_rule_decodes_to_the_us_rule() {
  std::cout << "\n=== The captured rule decodes to the US rule ===" << std::endl;
  const DstRule r = decode_dst_rule(BENCH_RULE, sizeof(BENCH_RULE));
  TEST_ASSERT(r.valid, "the ten bytes decode");
  TEST_ASSERT(r.enabled, "DST is enabled on the bench unit");
  TEST_ASSERT(r.start.month == 3 && r.start.weekday == 7 &&
                  r.start.occurrence == 2 && r.start.hour == 2,
              "starts the second Sunday of March at 02:00");
  TEST_ASSERT(r.end.month == 11 && r.end.weekday == 7 && r.end.occurrence == 1 &&
                  r.end.hour == 2,
              "ends the first Sunday of November at 02:00");
  TEST_ASSERT(r.offset_minutes == 60, "shifts by 60 minutes");
}

static void test_a_short_frame_is_not_decoded() {
  std::cout << "\n=== A frame too short to hold the rule is refused ===" << std::endl;
  for (size_t len = 0; len < 10; len++) {
    const DstRule r = decode_dst_rule(BENCH_RULE, len);
    if (r.valid) {
      TEST_ASSERT(false, "a short frame was decoded anyway");
      return;
    }
  }
  TEST_ASSERT(true, "every length below ten is refused, rather than read past the end");
  TEST_ASSERT(!decode_dst_rule(nullptr, 10).valid, "and a null body is refused");
}

// A rule whose fields are nonsense is not something to compare against
// anything; saying "mismatch" for it would blame the timezone for a bad frame.
static void test_an_impossible_enabled_rule_is_not_valid() {
  std::cout << "\n=== An enabled rule with impossible fields is refused ===" << std::endl;
  uint8_t bad[10];
  memcpy(bad, BENCH_RULE, sizeof(bad));
  bad[1] = 13;  // month 13
  TEST_ASSERT(!decode_dst_rule(bad, sizeof(bad)).valid, "month 13 is not a month");
  memcpy(bad, BENCH_RULE, sizeof(bad));
  bad[6] = 0;  // weekday 0
  TEST_ASSERT(!decode_dst_rule(bad, sizeof(bad)).valid, "weekday 0 is not a weekday");
}

// The fields of a DISABLED rule are whatever the factory left there, and on the
// bench unit they are still populated. Refusing those would report "unknown"
// for a pump that is perfectly clear that it does not shift.
static void test_a_disabled_rule_is_valid_whatever_its_fields_say() {
  std::cout << "\n=== A disabled rule is valid even with junk fields ===" << std::endl;
  uint8_t off[10];
  memcpy(off, BENCH_RULE, sizeof(off));
  off[0] = 0x00;  // disabled
  off[1] = 0xFF;  // and a month nobody could mean
  const DstRule r = decode_dst_rule(off, sizeof(off));
  TEST_ASSERT(r.valid, "it decodes");
  TEST_ASSERT(!r.enabled, "and says the pump does not shift, which is the whole content");
}

// ---------------------------------------------------------------------------
// Probing the host zone
// ---------------------------------------------------------------------------

static void test_the_host_probe_finds_the_us_rule() {
  std::cout << "\n=== The host probe finds US Pacific's rule ===" << std::endl;
  with_tz("PST8PDT,M3.2.0/2,M11.1.0/2", []() {
    const DstRule h = probe_host_dst_rule(2026);
    TEST_ASSERT(h.valid && h.enabled, "Pacific observes DST in 2026");
    TEST_ASSERT(h.start.month == 3 && h.start.weekday == 7 &&
                    h.start.occurrence == 2 && h.start.hour == 2,
                "found the second Sunday of March at 02:00, by observation");
    TEST_ASSERT(h.end.month == 11 && h.end.weekday == 7 && h.end.occurrence == 1 &&
                    h.end.hour == 2,
                "and the first Sunday of November at 02:00");
    TEST_ASSERT(h.offset_minutes == 60, "and a 60-minute shift");
  });
}

static void test_a_zone_without_dst_is_reported_as_such() {
  std::cout << "\n=== A zone that never shifts is found not to ===" << std::endl;
  with_tz("MST7", []() {  // Arizona: mountain time, no DST
    const DstRule h = probe_host_dst_rule(2026);
    TEST_ASSERT(h.valid, "the probe succeeds");
    TEST_ASSERT(!h.enabled, "...and finds no transitions, rather than failing");
  });
  with_tz("UTC", []() {
    TEST_ASSERT(!probe_host_dst_rule(2026).enabled, "UTC does not shift either");
  });
}

// The southern hemisphere, where the year STARTS in daylight time, so the first
// transition of the calendar year is the one OFF it. The rule's start/end must
// still come out in the pump's order (start = the shift onto daylight time), or
// every southern installation reads as a mismatch.
static void test_a_southern_hemisphere_zone_is_not_reported_backwards() {
  std::cout << "\n=== A southern-hemisphere zone is not read backwards ===" << std::endl;
  with_tz("AEST-10AEDT,M10.1.0,M4.1.0/3", []() {  // Sydney
    const DstRule h = probe_host_dst_rule(2026);
    TEST_ASSERT(h.valid && h.enabled, "Sydney observes DST");
    TEST_ASSERT(h.start.month == 10,
                "the START is the October shift ONTO daylight time, even though "
                "the April shift comes first in the calendar year");
    TEST_ASSERT(h.end.month == 4, "and the END is the April shift off it");
  });
}

// ---------------------------------------------------------------------------
// Comparing
// ---------------------------------------------------------------------------

static void test_a_matching_zone_agrees() {
  std::cout << "\n=== The bench pump and US Pacific agree ===" << std::endl;
  with_tz("PST8PDT,M3.2.0/2,M11.1.0/2", []() {
    const DstRule pump = decode_dst_rule(BENCH_RULE, sizeof(BENCH_RULE));
    const DstRule host = probe_host_dst_rule(2026);
    const DstAgreement a = compare_dst_rules(pump, host, 2026);
    TEST_ASSERT(a == DstAgreement::AGREE, "the rules agree");
    TEST_ASSERT(dst_rules_agree(a), "and nothing needs the user's attention");
    TEST_ASSERT(format_dst_agreement(a, pump, host).find("OK") == 0,
                "the entity reads OK");
    TEST_ASSERT(format_dst_agreement(a, pump, host).find("Mar Sun#2") != std::string::npos,
                "...and still states the rule, so the user can check it");
  });
}

// The case @jfriend00's report makes concrete: a pump with DST disabled on a
// node whose zone observes it. The host shifts twice a year and the pump never
// does, so every stored event moves by an hour relative to the pump's clock.
static void test_a_pump_that_does_not_shift_in_a_zone_that_does() {
  std::cout << "\n=== Pump disabled, host zone observes DST -> mismatch ===" << std::endl;
  with_tz("PST8PDT,M3.2.0/2,M11.1.0/2", []() {
    uint8_t off[10];
    memcpy(off, BENCH_RULE, sizeof(off));
    off[0] = 0x00;
    const DstRule pump = decode_dst_rule(off, sizeof(off));
    const DstRule host = probe_host_dst_rule(2026);
    const DstAgreement a = compare_dst_rules(pump, host, 2026);
    TEST_ASSERT(a == DstAgreement::HOST_ONLY, "the disagreement is named, not just flagged");
    TEST_ASSERT(!dst_rules_agree(a), "and it is a mismatch");
    TEST_ASSERT(format_dst_agreement(a, pump, host).find("the pump does not") !=
                    std::string::npos,
                "the entity says which side does not shift");
  });
}

static void test_a_pump_that_shifts_in_a_zone_that_does_not() {
  std::cout << "\n=== Pump enabled, host zone does not shift -> mismatch ===" << std::endl;
  with_tz("MST7", []() {
    const DstRule pump = decode_dst_rule(BENCH_RULE, sizeof(BENCH_RULE));
    const DstRule host = probe_host_dst_rule(2026);
    TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::PUMP_ONLY,
                "the pump shifts and Arizona does not");
  });
}

// Both observe DST, on different dates: a US pump installed in Europe. This is
// the case a bare enabled/disabled check cannot see, and the one that produces
// a wrong hour for a few weeks each spring and autumn rather than all year.
static void test_two_zones_that_both_shift_on_different_dates() {
  std::cout << "\n=== Both shift, on different dates -> mismatch ===" << std::endl;
  with_tz("CET-1CEST,M3.5.0/2,M10.5.0/3", []() {  // EU rule: LAST Sunday both ends
    const DstRule pump = decode_dst_rule(BENCH_RULE, sizeof(BENCH_RULE));
    const DstRule host = probe_host_dst_rule(2026);
    TEST_ASSERT(host.valid && host.enabled, "the EU zone observes DST");
    const DstAgreement a = compare_dst_rules(pump, host, 2026);
    TEST_ASSERT(a == DstAgreement::RULES_DIFFER,
                "a US pump in an EU zone is a mismatch, though both observe DST");
    const std::string s = format_dst_agreement(a, pump, host);
    TEST_ASSERT(s.find("pump ") != std::string::npos && s.find("node ") != std::string::npos,
                "the entity states BOTH rules, so the user can see which is wrong");
    TEST_ASSERT(s.size() < 255, "and fits a text sensor");
  });
}

// "The last Sunday of October" and "the fourth Sunday of October" are DIFFERENT
// rules that coincide in some years and not others, so neither field equality
// nor a loose `>= 4` comparison answers the question. Both sides are resolved to
// a concrete date in the year being asked about instead.
//
// My first version collapsed every occurrence `>= 4` into "the same", on the
// reasoning that "last" is sometimes the fourth. That is wrong in the direction
// that matters and CI's reviewer caught it: it reports AGREE for a pump that
// changes a week before the node, which is exactly the case this feature exists
// to catch.
static void test_last_and_fourth_are_the_same_rule_only_when_they_coincide() {
  std::cout << "\n=== 'Last' and 'fourth' agree only in years where they coincide ===" << std::endl;
  const DstTransition last{10, 7, 5, 3};    // last Sunday of October -- the EU rule
  const DstTransition fourth{10, 7, 4, 3};  // the fourth Sunday

  // 2027: October has five Sundays, so the two are a week apart.
  TEST_ASSERT(resolve_transition_day(last, 2027) == 31,
              "October 2027's last Sunday is the 31st");
  TEST_ASSERT(resolve_transition_day(fourth, 2027) == 24,
              "and its fourth Sunday is the 24th");
  TEST_ASSERT(!transitions_coincide(last, fourth, 2027),
              "so in 2027 they are not the same rule -- a pump on one and a node "
              "on the other change a week apart, which is the whole point");

  // 2026: October has four Sundays, so "last" IS the fourth.
  TEST_ASSERT(resolve_transition_day(last, 2026) == resolve_transition_day(fourth, 2026),
              "October 2026's last Sunday IS its fourth");
  TEST_ASSERT(transitions_coincide(last, fourth, 2026),
              "so in 2026 they agree, and a correct EU pump in an EU zone is not "
              "reported as a mismatch");

  const DstTransition second{10, 7, 2, 3};
  TEST_ASSERT(!transitions_coincide(last, second, 2026),
              "the second Sunday is a different rule in any year");
}

// The date arithmetic the comparison rests on, checked against dates anyone can
// verify. Written without libc (no timegm on ESP-IDF), so it is worth pinning
// rather than trusting.
static void test_the_date_arithmetic() {
  std::cout << "\n=== Resolving a rule to a date ===" << std::endl;
  // The US rule, 2026: second Sunday of March is the 8th, first Sunday of
  // November is the 1st.
  TEST_ASSERT(resolve_transition_day(DstTransition{3, 7, 2, 2}, 2026) == 8,
              "second Sunday of March 2026 is the 8th");
  TEST_ASSERT(resolve_transition_day(DstTransition{11, 7, 1, 2}, 2026) == 1,
              "first Sunday of November 2026 is the 1st");
  // A leap February, where the last occurrence depends on the extra day.
  TEST_ASSERT(resolve_transition_day(DstTransition{2, 7, 5, 2}, 2026) == 22,
              "last Sunday of February 2026 (28 days) is the 22nd");
  TEST_ASSERT(resolve_transition_day(DstTransition{2, 7, 5, 2}, 2028) == 27,
              "last Sunday of February 2028 (29 days) is the 27th");
  // A rule that describes no date in this month.
  TEST_ASSERT(resolve_transition_day(DstTransition{2, 7, 5, 2}, 2026) != 29,
              "and never a day the month does not have");
  TEST_ASSERT(resolve_transition_day(DstTransition{11, 7, 4, 2}, 2026) == 22,
              "fourth Sunday of November 2026 is the 22nd");
}

// A payload whose fields cannot describe a date is not a rule to compare
// against a timezone. Reporting a mismatch for one blames the user's zone for a
// bad frame.
static void test_impossible_transition_fields_are_refused() {
  std::cout << "\n=== Fields that cannot describe a date are refused ===" << std::endl;
  uint8_t bad[10];
  memcpy(bad, BENCH_RULE, sizeof(bad));
  bad[3] = 0;  // occurrence 0
  TEST_ASSERT(!decode_dst_rule(bad, sizeof(bad)).valid, "occurrence 0 is not an occurrence");
  memcpy(bad, BENCH_RULE, sizeof(bad));
  bad[3] = 9;  // occurrence 9
  TEST_ASSERT(!decode_dst_rule(bad, sizeof(bad)).valid, "nor is occurrence 9");
  memcpy(bad, BENCH_RULE, sizeof(bad));
  bad[8] = 24;  // hour 24
  TEST_ASSERT(!decode_dst_rule(bad, sizeof(bad)).valid, "hour 24 is not an hour");
}

// Same dates, different AMOUNT. Every zone tested above shifts by 60 minutes,
// so the offset comparison was never exercised by any of them -- CI's mutation
// sweep caught that the comparison could be deleted with the suite still green.
//
// Not a hypothetical: Lord Howe Island shifts by 30 minutes, and the pump's
// `time_offset` field is a whole byte of minutes, so it can carry any of this.
// A pump shifting an hour where the node shifts half an hour is a half-hour
// error in every stored window for part of the year -- smaller than the
// wrong-dates case and just as silent.
//
// Built from structs rather than from a TZ fixture on purpose: the point is the
// comparison, and routing it through a zone would make it depend on which
// half-hour zones the CI image happens to ship.
static void test_the_same_dates_with_a_different_shift_is_a_mismatch() {
  std::cout << "\n=== Same dates, different shift -> mismatch ===" << std::endl;
  DstRule pump{};
  pump.valid = true;
  pump.enabled = true;
  pump.start = DstTransition{10, 7, 1, 2};
  pump.end = DstTransition{4, 7, 1, 3};
  pump.offset_minutes = 60;

  DstRule host = pump;      // identical in every respect...
  host.offset_minutes = 30; // ...except how far it moves

  TEST_ASSERT(pump.start == host.start && pump.end == host.end,
              "the two rules transition on exactly the same instants");
  const DstAgreement a = compare_dst_rules(pump, host, 2026);
  TEST_ASSERT(a == DstAgreement::RULES_DIFFER,
              "and they still disagree, because the amount is part of the rule");
  const std::string s = format_dst_agreement(a, pump, host);
  TEST_ASSERT(s.find("+60") != std::string::npos && s.find("+30") != std::string::npos,
              "the entity quotes both shifts, since the dates alone look identical");

  // The control: make the amounts agree and they agree.
  host.offset_minutes = 60;
  TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::AGREE,
              "with the same shift they agree, so the assertion above is about "
              "the offset and not about something else");
}

// One end at a time. The EU-versus-US case below moves BOTH transitions, so
// either half of the date comparison could be deleted with it still reporting
// RULES_DIFFER -- which is what CI's mutation sweep found. A pump and a node
// can genuinely disagree at one end only: the US moved its start date in 2007
// and left its end in the same month, and a pump carrying a pre-2007 rule
// against a current zone is exactly that shape.
static void test_a_start_date_that_differs_alone_is_a_mismatch() {
  std::cout << "\n=== A start date differing alone -> mismatch ===" << std::endl;
  DstRule pump{};
  pump.valid = true;
  pump.enabled = true;
  pump.start = DstTransition{3, 7, 2, 2};   // second Sunday of March, the current US rule
  pump.end = DstTransition{11, 7, 1, 2};    // first Sunday of November
  pump.offset_minutes = 60;

  DstRule host = pump;
  host.start = DstTransition{4, 7, 1, 2};   // first Sunday of APRIL, the pre-2007 rule

  TEST_ASSERT(transitions_coincide(pump.end, host.end, 2026),
              "the two agree on when DST ENDS");
  TEST_ASSERT(!transitions_coincide(pump.start, host.start, 2026),
              "and disagree only on when it starts");
  TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::RULES_DIFFER,
              "which is a mismatch -- for three weeks every spring every stored "
              "window is an hour out");

  host.start = pump.start;
  TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::AGREE,
              "and restoring the start date is enough to make them agree, so "
              "the assertion above is about the start and nothing else");
}

static void test_an_end_date_that_differs_alone_is_a_mismatch() {
  std::cout << "\n=== An end date differing alone -> mismatch ===" << std::endl;
  DstRule pump{};
  pump.valid = true;
  pump.enabled = true;
  pump.start = DstTransition{3, 7, 2, 2};
  pump.end = DstTransition{11, 7, 1, 2};    // first Sunday of November
  pump.offset_minutes = 60;

  DstRule host = pump;
  host.end = DstTransition{10, 7, 5, 2};    // last Sunday of OCTOBER, the EU end

  TEST_ASSERT(transitions_coincide(pump.start, host.start, 2026),
              "the two agree on when DST STARTS");
  TEST_ASSERT(!transitions_coincide(pump.end, host.end, 2026),
              "and disagree only on when it ends");
  TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::RULES_DIFFER,
              "which is a mismatch");

  host.end = pump.end;
  TEST_ASSERT(compare_dst_rules(pump, host, 2026) == DstAgreement::AGREE,
              "and restoring the end date is enough to make them agree");
}

static void test_an_unread_rule_is_unknown_not_a_mismatch() {
  std::cout << "\n=== An unread rule is unknown, not an accusation ===" << std::endl;
  DstRule unread{};  // valid == false, as before the read lands
  with_tz("PST8PDT,M3.2.0/2,M11.1.0/2", []() {});
  const DstRule host = probe_host_dst_rule(2026);
  const DstAgreement a = compare_dst_rules(unread, host, 2026);
  TEST_ASSERT(a == DstAgreement::UNKNOWN, "an unread rule is UNKNOWN");
  TEST_ASSERT(format_dst_agreement(a, unread, host) == "unknown",
              "and the entity says so rather than accusing the timezone");
}

static void test_neither_shifting_is_agreement() {
  std::cout << "\n=== Neither shifting is agreement, and says so ===" << std::endl;
  with_tz("UTC", []() {
    uint8_t off[10];
    memcpy(off, BENCH_RULE, sizeof(off));
    off[0] = 0x00;
    const DstRule pump = decode_dst_rule(off, sizeof(off));
    const DstRule host = probe_host_dst_rule(2026);
    const DstAgreement a = compare_dst_rules(pump, host, 2026);
    TEST_ASSERT(a == DstAgreement::NEITHER, "neither observes DST");
    TEST_ASSERT(dst_rules_agree(a), "which is agreement");
    TEST_ASSERT(format_dst_agreement(a, pump, host).find("neither") != std::string::npos,
                "and the entity says why, rather than reading like a rule was found");
  });
}

int main() {
  setenv("TZ", "UTC", 1);
  tzset();

  std::cout << "===========================================================" << std::endl;
  std::cout << "  Pump DST rule vs the node's timezone (issue #286)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_bench_rule_decodes_to_the_us_rule();
  test_a_short_frame_is_not_decoded();
  test_an_impossible_enabled_rule_is_not_valid();
  test_a_disabled_rule_is_valid_whatever_its_fields_say();

  test_the_host_probe_finds_the_us_rule();
  test_a_zone_without_dst_is_reported_as_such();
  test_a_southern_hemisphere_zone_is_not_reported_backwards();

  test_a_matching_zone_agrees();
  test_a_pump_that_does_not_shift_in_a_zone_that_does();
  test_a_pump_that_shifts_in_a_zone_that_does_not();
  test_two_zones_that_both_shift_on_different_dates();
  test_last_and_fourth_are_the_same_rule_only_when_they_coincide();
  test_the_date_arithmetic();
  test_impossible_transition_fields_are_refused();
  test_the_same_dates_with_a_different_shift_is_a_mismatch();
  test_a_start_date_that_differs_alone_is_a_mismatch();
  test_an_end_date_that_differs_alone_is_a_mismatch();
  test_an_unread_rule_is_unknown_not_a_mismatch();
  test_neither_shifting_is_agreement();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
