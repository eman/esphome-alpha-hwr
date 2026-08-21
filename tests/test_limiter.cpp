// Host tests for limiter.h -- the pump's MaxFlow/MinFlow limiters (issue #274).
//
// Why this matters: the limiter constrains the pump *below* what it was asked
// for, and every signal the component publishes says the write worked, because
// it did. Measured by @jfriend00 on a real installation, constant speed with
// MaxFlow enabled at 1.6 gpm, 3000 RPM commanded and 1883 delivered -- settled
// `accepted`, with the pump reporting 3000 back from 86/7, its own answer
// rather than a local echo. Nothing said otherwise.
//
// Every fixture below is a real frame from a real pump: jfriend00's ALPHA HWR
// with MaxFlow enabled, and the bench unit with both disabled. Bytes rather
// than hand-built structs, because the decode is the thing under test.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/limiter.h"

using namespace esphome::alpha_hwr::services;

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

// 86/600 on jfriend00's pump, MaxFlow ENABLED at 3.5 gpm. Type 895 v1, 18-byte
// payload: [limiter_name][enable][limit f32][kp][ti][td].
static const uint8_t MAXFLOW_ON_3_5_GPM[18] = {
    0x01, 0x01, 0x39, 0x67, 0x8A, 0xC2,
    0x3F, 0x19, 0x99, 0x9A, 0x3F, 0xCC, 0xCC, 0xCD, 0x3E, 0xCC, 0xCC, 0xCD};

// The same object after they changed the setting to 1.6 gpm.
static const uint8_t MAXFLOW_ON_1_6_GPM[18] = {
    0x01, 0x01, 0x38, 0xD3, 0xB2, 0x11,
    0x3F, 0x19, 0x99, 0x9A, 0x3F, 0xCC, 0xCC, 0xCD, 0x3E, 0xCC, 0xCC, 0xCD};

// 86/601 on the same pump: MinFlow DISABLED at 2.50 gpm.
static const uint8_t MINFLOW_OFF_2_5_GPM[18] = {
    0x02, 0x00, 0x39, 0x25, 0x63, 0x1D,
    0x3F, 0x19, 0x99, 0x9A, 0x3F, 0xCC, 0xCC, 0xCD, 0x3E, 0xCC, 0xCC, 0xCD};

// 86/640, all three observed pump states. Type 896 v1, 6-byte payload.
static const uint8_t STATUS_STOPPED[6]        = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t STATUS_UNDER_THE_CAP[6]  = {0x01, 0x00, 0x45, 0x65, 0x70, 0x00};
static const uint8_t STATUS_LIMITING[6]       = {0x01, 0x01, 0x44, 0xEB, 0x63, 0x4A};
// 86/660, the manager, in the same three states.
static const uint8_t MANAGER_IDLE[6]          = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t MANAGER_RUNNING[6]       = {0x00, 0x00, 0x44, 0xCE, 0x40, 0x00};
static const uint8_t MANAGER_LIMITING[6]      = {0x01, 0x01, 0x44, 0xEB, 0x27, 0xD6};
// 86/641 throughout: MinFlow disabled, never limiting.
static const uint8_t MINFLOW_STATUS_IDLE[6]   = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// The gpm conversion is EXACT on every limit value seen, on two pumps. Both
// figures below were entered in the app in gallons and land on the wire in m³/s
// to every digit the float carries, which is what makes gpm the unit to report.
static void test_the_config_record_decodes_to_the_entered_value() {
  std::cout << "\n=== A limiter config decodes to the value entered in the app ===" << std::endl;
  const LimiterConfig a = decode_limiter_config(MAXFLOW_ON_3_5_GPM, 18);
  TEST_ASSERT(a.valid, "the 18 bytes decode");
  TEST_ASSERT(a.name == LimiterName::MAX_FLOW, "limiter_name 0x01 is MaxFlow");
  TEST_ASSERT(a.enabled, "and it is enabled");
  TEST_ASSERT(std::fabs(a.limit_m3s - 2.2081568e-04f) < 1e-10f,
              "the native value is 2.2081568e-04 m³/s");
  TEST_ASSERT(std::fabs(a.limit_gpm() - 3.5f) < 0.001f,
              "which is 3.50 gpm -- the number typed into the app");

  const LimiterConfig b = decode_limiter_config(MAXFLOW_ON_1_6_GPM, 18);
  TEST_ASSERT(std::fabs(b.limit_gpm() - 1.6f) < 0.001f,
              "and the same object after the setting changed reads 1.60 gpm");
}

static void test_a_disabled_limiter_reads_as_disabled() {
  std::cout << "\n=== A disabled limiter is not merely a zero limit ===" << std::endl;
  const LimiterConfig c = decode_limiter_config(MINFLOW_OFF_2_5_GPM, 18);
  TEST_ASSERT(c.valid && c.name == LimiterName::MIN_FLOW, "limiter_name 0x02 is MinFlow");
  TEST_ASSERT(!c.enabled, "the enable byte reads 0, matching what the app shows");
  TEST_ASSERT(std::fabs(c.limit_gpm() - 2.5f) < 0.001f,
              "and it still carries a value -- 2.50 gpm -- which is why the "
              "enable byte has to be read rather than inferred from the limit");
}

static void test_short_records_are_refused() {
  std::cout << "\n=== Records too short to decode are refused ===" << std::endl;
  bool all_refused = true;
  for (size_t len = 0; len < 18; len++)
    if (decode_limiter_config(MAXFLOW_ON_3_5_GPM, len).valid) all_refused = false;
  TEST_ASSERT(all_refused, "every config length below 18 is refused");
  all_refused = true;
  for (size_t len = 0; len < 6; len++)
    if (decode_limiter_status(STATUS_LIMITING, len).valid) all_refused = false;
  TEST_ASSERT(all_refused, "every status length below 6 is refused");
  TEST_ASSERT(!decode_limiter_config(nullptr, 18).valid, "and a null body is refused");
  TEST_ASSERT(!decode_limiter_status(nullptr, 6).valid, "...for both records");
}

// The magnitudes are what settle that limiter_reference is a SPEED. A flow in
// the pump's native m³/s would be around 1e-04; these read 1650, 1883 and 3671.
static void test_the_reference_is_a_speed_not_a_flow() {
  std::cout << "\n=== limiter_reference is an RPM speed, not a flow ===" << std::endl;
  const LimiterStatus under = decode_limiter_status(STATUS_UNDER_THE_CAP, 6);
  TEST_ASSERT(!under.limiting, "running under the cap: not limiting");
  TEST_ASSERT(std::fabs(under.reference - 3671.0f) < 0.5f,
              "and it publishes 3671 -- the top of the constant-speed range, "
              "i.e. no restriction");

  const LimiterStatus lim = decode_limiter_status(STATUS_LIMITING, 6);
  TEST_ASSERT(lim.limiting, "commanded 3000 and clamped: limiting");
  TEST_ASSERT(std::fabs(lim.reference - 1883.1f) < 0.2f,
              "and it publishes the speed it is imposing, 1883.1 -- against a "
              "measured motor speed of 1895 RPM at that moment");
  TEST_ASSERT(lim.reference > 100.0f,
              "both are orders of magnitude away from a flow in m³/s (~1e-04), "
              "which is what settles the unit");

  const LimiterStatus stopped = decode_limiter_status(STATUS_STOPPED, 6);
  TEST_ASSERT(!stopped.limiting && stopped.reference == 0.0f,
              "and a stopped pump publishes 0.0, not a stale reference");
}

// ---------------------------------------------------------------------------
// The question a user actually asks
// ---------------------------------------------------------------------------

static void test_nothing_read_is_not_no_limiter() {
  std::cout << "\n=== 'Not read' is distinguishable from 'no limiter' ===" << std::endl;
  LimiterState s{};
  TEST_ASSERT(!s.known(), "an empty state is not known");
  TEST_ASSERT(!s.limiting(), "and does not claim a limiter is active");
  TEST_ASSERT(format_limiter_state(s) == "unknown",
              "the entity says unknown rather than reporting an all-clear it "
              "has no basis for");
}

static void test_the_bench_pump_reports_no_limiter() {
  std::cout << "\n=== Both limiters disabled reads as no limiter ===" << std::endl;
  LimiterState s{};
  uint8_t off[18];
  memcpy(off, MAXFLOW_ON_3_5_GPM, sizeof(off));
  off[1] = 0x00;  // disable it, as on the bench unit
  s.max_flow = decode_limiter_config(off, 18);
  s.min_flow = decode_limiter_config(MINFLOW_OFF_2_5_GPM, 18);
  s.max_flow_status = decode_limiter_status(MINFLOW_STATUS_IDLE, 6);
  s.manager = decode_limiter_status(MANAGER_IDLE, 6);

  TEST_ASSERT(s.known(), "the state is known");
  TEST_ASSERT(!s.any_enabled(), "nothing is enabled");
  TEST_ASSERT(!s.limiting(), "and nothing is limiting");
  TEST_ASSERT(format_limiter_state(s) == "No limiter enabled", "reported plainly");
}

// The state that is neither "all clear" nor "limiting", and the reason it gets
// its own wording: an enabled MaxFlow that is not biting right now starts
// biting the moment the setpoint goes up. A user who sees "no limiter" here
// will not understand the clamp when it arrives.
static void test_enabled_but_not_limiting_is_its_own_state() {
  std::cout << "\n=== Enabled but not limiting is reported as such ===" << std::endl;
  LimiterState s{};
  s.max_flow = decode_limiter_config(MAXFLOW_ON_1_6_GPM, 18);
  s.min_flow = decode_limiter_config(MINFLOW_OFF_2_5_GPM, 18);
  s.max_flow_status = decode_limiter_status(STATUS_UNDER_THE_CAP, 6);
  s.min_flow_status = decode_limiter_status(MINFLOW_STATUS_IDLE, 6);
  s.manager = decode_limiter_status(MANAGER_RUNNING, 6);

  TEST_ASSERT(s.any_enabled(), "MaxFlow is on");
  TEST_ASSERT(!s.limiting(), "but it is not constraining the pump right now");
  const std::string text = format_limiter_state(s);
  TEST_ASSERT(text.find("MaxFlow enabled") != std::string::npos, "the entity names it");
  TEST_ASSERT(text.find("1.60") != std::string::npos, "quotes the cap in gpm");
  TEST_ASSERT(text.find("not limiting") != std::string::npos,
              "and says it is not biting yet, rather than reading as an alarm");
  TEST_ASSERT(text.find("MinFlow") == std::string::npos,
              "and does not name the disabled one");
}

// jfriend00's measured state: 3000 RPM commanded, 1883 delivered, every write
// settled `accepted`. This is the state the whole issue exists to make visible.
static void test_actively_limiting_is_named() {
  std::cout << "\n=== A limiter actively limiting is named ===" << std::endl;
  LimiterState s{};
  s.max_flow = decode_limiter_config(MAXFLOW_ON_1_6_GPM, 18);
  s.min_flow = decode_limiter_config(MINFLOW_OFF_2_5_GPM, 18);
  s.max_flow_status = decode_limiter_status(STATUS_LIMITING, 6);
  s.min_flow_status = decode_limiter_status(MINFLOW_STATUS_IDLE, 6);
  s.manager = decode_limiter_status(MANAGER_LIMITING, 6);

  TEST_ASSERT(s.limiting(), "the pump is being held below what it was asked for");
  TEST_ASSERT(s.limiting_name() == LimiterName::MAX_FLOW,
              "and the manager names MaxFlow as the binding constraint");
  const std::string text = format_limiter_state(s);
  TEST_ASSERT(text.find("MaxFlow limiting") != std::string::npos, "the entity says so");
  TEST_ASSERT(text.find("1.60") != std::string::npos, "and at what cap");
}

// The manager changes its limiter_name from 0x00 to 0x01 when MaxFlow becomes
// binding, so it answers "which one" directly. The per-limiter records are the
// fallback for a firmware that does not answer 660.
static void test_the_per_limiter_records_are_a_fallback_for_the_manager() {
  std::cout << "\n=== Without the manager, the per-limiter records still answer ===" << std::endl;
  LimiterState s{};
  s.max_flow = decode_limiter_config(MAXFLOW_ON_1_6_GPM, 18);
  s.max_flow_status = decode_limiter_status(STATUS_LIMITING, 6);
  // manager deliberately left unread
  TEST_ASSERT(!s.manager.valid, "660 was not answered");
  TEST_ASSERT(s.limiting(), "and the pump is still known to be limiting");
  TEST_ASSERT(s.limiting_name() == LimiterName::MAX_FLOW, "and by which limiter");
}

// A manager that says something is limiting but does not say what. Reporting
// "none limiting" there would be worse than saying less.
static void test_a_manager_with_no_name_still_reports_limiting() {
  std::cout << "\n=== A limiting manager with no name still reports limiting ===" << std::endl;
  LimiterState s{};
  uint8_t anon[6];
  memcpy(anon, MANAGER_LIMITING, sizeof(anon));
  anon[0] = 0x00;  // limiting, but naming nobody
  s.manager = decode_limiter_status(anon, 6);
  TEST_ASSERT(s.limiting(), "the pump is limiting");
  TEST_ASSERT(s.limiting_name() == LimiterName::NONE, "and we cannot say which");
  TEST_ASSERT(format_limiter_state(s).find("limiting") != std::string::npos,
              "the entity reports the limiting rather than the missing name");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Pump flow limiters (issue #274)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_config_record_decodes_to_the_entered_value();
  test_a_disabled_limiter_reads_as_disabled();
  test_short_records_are_refused();
  test_the_reference_is_a_speed_not_a_flow();

  test_nothing_read_is_not_no_limiter();
  test_the_bench_pump_reports_no_limiter();
  test_enabled_but_not_limiting_is_its_own_state();
  test_actively_limiting_is_named();
  test_the_per_limiter_records_are_a_fallback_for_the_manager();
  test_a_manager_with_no_name_still_reports_limiting();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
