#include <iostream>
#include <vector>
#include <cstdint>
#include <cstdio>
#include "fixture_crc.h"
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/transport.h"
#include "../components/alpha_hwr/session.h"

uint32_t mock_millis = 0;
int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

void test_schedule_write_payload() {
  std::cout << "\n=== Testing ScheduleService Write Payload ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  esphome::alpha_hwr::core::Session session;
  esphome::alpha_hwr::services::ScheduleService service(transport, session);

  // Intercept the BLE write from Transport
  std::vector<std::vector<uint8_t>> sent_chunks;
  transport.set_write_callback([&sent_chunks](const uint8_t* data, size_t len) -> bool {
    sent_chunks.push_back(std::vector<uint8_t>(data, data + len));
    return true;
  });

  // Set session to ready state to allow writes
  session.on_authenticated();

  // Write a simple schedule (layer 1)
  std::vector<esphome::alpha_hwr::ScheduleEntry> entries;
  entries.push_back(esphome::alpha_hwr::ScheduleEntry("Monday", 6, 0, 8, 0)); // Mon 6am-8am
  
  bool success = service.write_entries(entries, 1);
  TEST_ASSERT(success, "write_entries() accepted the schedule");

  // Run the FSM to dispatch all chunks (59 bytes total -> 3 chunks)
  mock_millis += 50;
  transport.loop();
  mock_millis += 51;
  transport.loop();
  mock_millis += 51;
  transport.loop();

  TEST_ASSERT(sent_chunks.size() == 3, "Schedule packet was split into 3 chunks");

  // Reassemble the packet chunks to inspect the APDU
  std::vector<uint8_t> full_packet;
  for (const auto& chunk : sent_chunks) {
    full_packet.insert(full_packet.end(), chunk.begin(), chunk.end());
  }
  std::cout << "Packet size: " << full_packet.size() << " bytes" << std::endl;
  std::cout << "Bytes: ";
  for (uint8_t b : full_packet) {
    printf("%02X ", b);
  }
  std::cout << std::endl;

  TEST_ASSERT(full_packet.size() == 59, "Full GENI packet length is 59 bytes (53 APDU + 6 header/footer)");

  // Check the payload details
  // APDU: Class=10 (0x0A), OpSpec=5 (0xB3), Obj=84 (0x54), Sub=1001 (0x03E9)
  // GENI format: [0x27][LEN][DST][SRC] + APDU
  TEST_ASSERT(full_packet[4] == 0x0A, "Class 10 (Settings)");
  TEST_ASSERT(full_packet[5] == 0xB3, "OpSpec 5 (Write)");
  TEST_ASSERT(full_packet[6] == 0x54, "Object 84 (ClockProgram)");
  TEST_ASSERT(full_packet[7] == 0x03 && full_packet[8] == 0xE9, "SubID 1001 (Layer 1)");
}

// Single-event timestamps live in the pump's LOCAL-Unix clock domain on the
// wire, while our SingleEvent fields hold UTC. Verify the shift helpers.
void test_single_event_tz_shift() {
  using esphome::alpha_hwr::services::utc_to_local_unix;
  using esphome::alpha_hwr::services::local_unix_to_utc;

  const uint32_t utc = 1784908899;  // arbitrary real epoch
  const int32_t pdt = -25200;       // seconds east of UTC (PDT)
  const int32_t cet = 3600;         // a positive offset

  TEST_ASSERT(utc_to_local_unix(utc, pdt) == utc - 25200,
              "UTC->local shifts west by the offset (PDT)");
  TEST_ASSERT(utc_to_local_unix(utc, cet) == utc + 3600,
              "UTC->local shifts east by the offset (CET)");
  // Round trip is exact for any offset (same offset both ways).
  TEST_ASSERT(local_unix_to_utc(utc_to_local_unix(utc, pdt), pdt) == utc,
              "UTC->local->UTC round-trips exactly (negative offset)");
  TEST_ASSERT(local_unix_to_utc(utc_to_local_unix(utc, cet), cet) == utc,
              "UTC->local->UTC round-trips exactly (positive offset)");
  // 0 is the disabled/cleared sentinel and is never shifted (in either dir).
  TEST_ASSERT(utc_to_local_unix(0, pdt) == 0, "sentinel 0 not shifted (encode)");
  TEST_ASSERT(local_unix_to_utc(0, pdt) == 0, "sentinel 0 not shifted (decode)");
}

// The round trip the component actually performs, across a DST boundary.
//
// The test above passes the *same* offset both ways, so it verifies the algebra
// and nothing else -- which is why it never saw this. In production the two
// directions resolve their offsets independently: the write path has a real UTC
// instant to resolve from, and the read path has only the local value the pump
// returned. Feeding a local value to local_utc_offset_seconds(), which expects
// UTC, evaluates the offset |offset| seconds away from the true instant -- seven
// hours early in PDT. Any event inside that window resolved to the wrong side of
// the transition and came back an hour out, so the confirm comparator settled
// the write REJECTED while the pump held exactly the right value.
void test_single_event_tz_shift_across_dst() {
  using esphome::alpha_hwr::services::utc_to_local_unix;
  using esphome::alpha_hwr::services::local_utc_offset_seconds;
  using esphome::alpha_hwr::services::local_unix_to_utc_resolved;

  // US Pacific, with the transition rules given explicitly so the test does not
  // depend on a tz database being present on the build host.
  setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();

  // Sanity: the fixture must actually straddle a transition, or the scan below
  // proves nothing.
  const uint32_t spring_forward = 1772964000;  // 2026-03-08 10:00 UTC, PST->PDT
  const uint32_t fall_back = 1793523600;       // 2026-11-01 09:00 UTC, PDT->PST
  TEST_ASSERT(local_utc_offset_seconds((time_t)(spring_forward - 3600)) == -28800,
              "An hour before spring-forward the offset is PST (-8)");
  TEST_ASSERT(local_utc_offset_seconds((time_t) spring_forward) == -25200,
              "At spring-forward it becomes PDT (-7)");
  TEST_ASSERT(local_utc_offset_seconds((time_t)(fall_back - 3600)) == -25200,
              "An hour before fall-back the offset is PDT (-7)");
  TEST_ASSERT(local_utc_offset_seconds((time_t) fall_back) == -28800,
              "At fall-back it becomes PST (-8)");

  // Walk both transitions at 15-minute steps from 12 h before to 12 h after,
  // doing exactly what the component does: encode with the offset at the
  // event's UTC instant, decode with only the local value in hand.
  //
  // The two transitions are NOT symmetric, and the difference is the whole
  // result. Spring-forward is fully recoverable. Fall-back is not: the hour
  // from the transition onward is *repeated* in local time, so two distinct UTC
  // instants encode to the same wire value and no decode can tell them apart.
  // That is a property of storing local time on the wire -- the pump's own
  // clock program has it -- not something this conversion can fix. What the fix
  // does is shrink the damage from "the whole offset, seven or eight hours" to
  // "the one hour that is genuinely ambiguous".
  struct R { int bad; int32_t first, last; };
  auto scan = [&](uint32_t base) {
    R r{0, 0, 0};
    for (int32_t d = -12 * 3600; d <= 12 * 3600; d += 900) {
      const uint32_t utc = static_cast<uint32_t>(static_cast<int64_t>(base) + d);
      const uint32_t wire =
          utc_to_local_unix(utc, local_utc_offset_seconds((time_t) utc));
      if (local_unix_to_utc_resolved(wire) != utc) {
        if (r.bad == 0) r.first = d;
        r.last = d;
        r.bad++;
      }
    }
    return r;
  };

  const R spring = scan(spring_forward);
  TEST_ASSERT(spring.bad == 0,
              "Spring-forward round-trips exactly at every instant in a 24 h "
              "window — the skipped hour produces no ambiguous local values");

  const R fall = scan(fall_back);
  TEST_ASSERT(fall.bad > 0,
              "Fall-back keeps a residual: the repeated hour maps two UTC "
              "instants to one local value, which no decode can undo");
  TEST_ASSERT(fall.first == 0 && fall.last < 3600,
              "...and it is confined to exactly that repeated hour, "
              "[transition, transition+1h)");

  // The bound that matters. Before the fix the read path resolved its offset
  // from the local value, so the error covered the whole offset — seven hours
  // at spring-forward, eight at fall-back, at both transitions. 4 of 97
  // quarter-hour samples is the one ambiguous hour; anything larger means the
  // offset resolution has regressed rather than the ambiguity showing through.
  TEST_ASSERT(fall.bad == 4,
              "The residual is 4 of 97 samples — the ambiguous hour alone, not "
              "the 32 an unresolved offset produced");

  // The sentinel still survives the resolving path. (Decorative, honestly: the
  // early return in local_unix_to_utc_resolved only saves two localtime_r
  // calls, since local_unix_to_utc already answers 0 for 0 at any offset.
  // Removing the guard leaves this assertion green. Kept because the guard is
  // the cheap path, not because this pins it.)
  TEST_ASSERT(local_unix_to_utc_resolved(0) == 0,
              "sentinel 0 not shifted by the resolving decode");

  // A zone whose offset is NOT a whole number of hours. Until this case the
  // only TZ-driven test was US Pacific, so the minutes and seconds terms of
  // local_utc_offset_seconds() were never exercised: neutering either of them
  // left the whole suite green. That matters more now that the function has
  // moved from a file-static into a header whose comment implies generality.
  setenv("TZ", "ACST-9:30ACDT,M10.1.0,M4.1.0", 1);  // +9:30 / +10:30
  tzset();
  const uint32_t acst_ref = 1772964000;  // any instant outside a transition
  TEST_ASSERT(local_utc_offset_seconds((time_t) acst_ref) % 3600 != 0,
              "The fixture zone really does have a sub-hour offset");
  {
    const uint32_t wire = utc_to_local_unix(
        acst_ref, local_utc_offset_seconds((time_t) acst_ref));
    TEST_ASSERT(local_unix_to_utc_resolved(wire) == acst_ref,
                "A half-hour offset round-trips exactly — the minutes term of "
                "the offset calculation is load-bearing");
  }

  // Local and UTC in different calendar years, which is the only time the
  // day_delta year branch fires. Breaking it costs a full 86400 s, and nothing
  // exercised it before: mutating it to 0 left the suite green.
  setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  {
    // 2026-01-01 03:00 UTC is 2025-12-31 19:00 local — different years.
    const uint32_t newyear = 1767236400;
    struct tm lt {}, gt {};
    time_t ref = (time_t) newyear;
    localtime_r(&ref, &lt);
    gmtime_r(&ref, &gt);
    TEST_ASSERT(lt.tm_year != gt.tm_year,
                "The fixture really does straddle a year boundary");
    TEST_ASSERT(local_utc_offset_seconds(ref) == -28800,
                "The offset across a year boundary is still -8 h, not off by a "
                "day");
    const uint32_t wire =
        utc_to_local_unix(newyear, local_utc_offset_seconds(ref));
    TEST_ASSERT(local_unix_to_utc_resolved(wire) == newyear,
                "...and the round trip is exact across it");
  }

  unsetenv("TZ");
  tzset();
}

// The schedule state poll runs on the telemetry cadence (~10s) and is almost
// always a no-op confirmation. Its callback republishes the schedule text
// sensors, so firing it on every poll cost an API state frame per subscriber
// per poll for identical content (issue #127): announce transitions only.
void test_state_change_callback_fires_only_on_change() {
  std::cout << "\n=== Testing schedule state change callback ===" << std::endl;
  esphome::alpha_hwr::core::Transport transport;
  esphome::alpha_hwr::core::Session session;
  esphome::alpha_hwr::services::ScheduleService service(transport, session);

  transport.set_write_callback([](const uint8_t *, size_t) -> bool { return true; });
  session.on_authenticated();

  int callbacks = 0;
  bool last_reported = false;
  service.set_state_change_callback([&](bool enabled) {
    callbacks++;
    last_reported = enabled;
  });

  // One poll round trip: issue the request, run the FSM out, answer it with an
  // Object 84 overview frame (OpSpec 0x13, [00 00 0A] header + 10-byte body,
  // byte 4 of the body = schedule enabled).
  auto poll_with_enabled = [&](bool enabled) {
    service.poll_state();
    for (int i = 0; i < 4; i++) {
      mock_millis += 51;
      transport.loop();
    }
    std::vector<uint8_t> frame = {0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x13, 0x00, 0x00, 0xDA, 0x01,
                                  0x00, 0x00, 0x0A,
                                  0x8C, 0x23, 0x05, 0x05, static_cast<uint8_t>(enabled ? 1 : 0),
                                  0x01, 0x00, 0x00, 0x00, 0x00,
                                  0xAA, 0xBB};
    frame[1] = static_cast<uint8_t>(frame.size() - 4);
    frame = with_crc(std::move(frame));
    transport.on_notification(frame.data(), frame.size());
    mock_millis += 51;
    transport.loop();
  };

  poll_with_enabled(true);
  TEST_ASSERT(callbacks == 1 && last_reported, "First cached state fires the callback");

  for (int i = 0; i < 5; i++)
    poll_with_enabled(true);
  TEST_ASSERT(callbacks == 1, "Repeat polls of unchanged state fire nothing");

  poll_with_enabled(false);
  TEST_ASSERT(callbacks == 2 && !last_reported, "An actual transition fires the callback");

  poll_with_enabled(false);
  TEST_ASSERT(callbacks == 2, "...and then goes quiet again");

  bool cached = true;
  TEST_ASSERT(service.get_state(&cached) && !cached,
              "Cached state still tracks every poll, callback or not");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Schedule Service Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_schedule_write_payload();
  test_single_event_tz_shift();
  test_single_event_tz_shift_across_dst();
  test_state_change_callback_fires_only_on_change();
  
  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}
