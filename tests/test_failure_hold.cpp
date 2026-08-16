// Host tests for failure_hold.h — which held fault reason wins, and what
// releases it.
//
// Issue #175 gave the subscribe step a hold so the watchdog firing 60 s later
// could not replace a named cause with its own generic symptom. It guarded that
// write with `failure_hold_ != FailureHold::SUBSCRIBE`, which is a list of
// exceptions rather than the rank the change described: an AUTH hold is neither
// NONE nor SUBSCRIBE, so "No data from pump" overwrote it — the same overwrite,
// on the one hold that records a bond-erasing failure. Worse than losing the
// string: the overwrite also re-labels the origin DATA, and a DATA hold is
// released by inbound data, which an unbonded pump keeps sending after a failed
// SMP. The pairing diagnostic would then be erased by the very next
// notification.
//
// So the rank is the enum order and every write site asks the same question.
// ble_connection_manager.cpp is compiled by no host test, so the question lives
// here, where it can be answered wrongly and noticed.

#include <cstdint>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/failure_hold.h"

using esphome::alpha_hwr::core::FailureHold;
using esphome::alpha_hwr::core::failure_hold_admits;
using esphome::alpha_hwr::core::failure_hold_released_by_auth;
using esphome::alpha_hwr::core::failure_hold_released_by_data;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  if (condition) {                                                             \
    tests_passed++;                                                            \
    std::cout << "[PASS] " << message << std::endl;                            \
  } else {                                                                     \
    tests_failed++;                                                            \
    std::cout << "[FAIL] " << message << std::endl;                            \
  }

// Every hold, weakest first. Hand-written, so the range scan at the bottom is
// what stops a fifth enumerator from skipping the tables below.
static const FailureHold ALL[] = {
    FailureHold::NONE,
    FailureHold::DATA,
    FailureHold::SUBSCRIBE,
    FailureHold::AUTH,
};

static const char *name(FailureHold h) {
  switch (h) {
    case FailureHold::NONE: return "NONE";
    case FailureHold::DATA: return "DATA";
    case FailureHold::SUBSCRIBE: return "SUBSCRIBE";
    case FailureHold::AUTH: return "AUTH";
  }
  return "?";
}

void test_the_defect_the_rank_replaces() {
  std::cout << "\n=== The watchdog cannot overwrite a pairing failure ==="
            << std::endl;

  // The regression this file exists for. The watchdog writes at rank DATA.
  TEST_ASSERT(!failure_hold_admits(FailureHold::AUTH, FailureHold::DATA),
              "\"No data from pump\" does not replace an auth failure — the "
              "silence is that failure's symptom, not a competing cause");
  TEST_ASSERT(!failure_hold_admits(FailureHold::SUBSCRIBE, FailureHold::DATA),
              "...nor a named subscribe failure (issue #175's original fix)");

  // ...and the reason it mattered more than a wrong string: admitting the write
  // also sets the origin to DATA, which inbound data releases. An unbonded pump
  // keeps delivering notifications after a failed SMP, so the pairing reason
  // would have been erased by the next notification rather than merely
  // relabelled.
  TEST_ASSERT(failure_hold_released_by_data(FailureHold::DATA) &&
                  !failure_hold_released_by_data(FailureHold::AUTH),
              "...which also protects the origin: DATA is released by inbound "
              "data and AUTH is not, so the relabel would have erased it");
}

void test_a_hold_admits_only_its_equals_and_betters() {
  std::cout << "\n=== The rank is the whole rule ===" << std::endl;

  for (auto held : ALL) {
    for (auto incoming : ALL) {
      const bool expected = static_cast<uint8_t>(held) <=
                            static_cast<uint8_t>(incoming);
      TEST_ASSERT(failure_hold_admits(held, incoming) == expected,
                  std::string("holding ") + name(held) + ", a " +
                      name(incoming) + " reason " +
                      (expected ? "writes" : "does not write"));
    }
  }
}

void test_nothing_is_held_against_a_first_reason() {
  std::cout << "\n=== An unheld reason is always writable ===" << std::endl;

  // Every site must still be able to name a cause when none is held, including
  // the two that write at rank NONE: the plain disconnect handler, and a CCCD
  // write that failed because the link was already gone.
  for (auto incoming : ALL)
    TEST_ASSERT(failure_hold_admits(FailureHold::NONE, incoming),
                std::string("with no hold, a ") + name(incoming) +
                    " reason is written");
}

void test_a_fault_refreshes_its_own_text() {
  std::cout << "\n=== Equal ranks overwrite ===" << std::endl;

  // Not incidental. The watchdog backs off, and each recycle's reason carries
  // the window it actually waited — "No data from pump (60s)", then "(120s)".
  // A strict `> held` rule, or the tempting `held == NONE` one-liner, would
  // freeze the fault surface at the first fire and hide the escalation.
  TEST_ASSERT(failure_hold_admits(FailureHold::DATA, FailureHold::DATA),
              "a second watchdog fire updates its own reason with the new "
              "backed-off window");
  TEST_ASSERT(failure_hold_admits(FailureHold::AUTH, FailureHold::AUTH),
              "a later pairing failure replaces an earlier one");
  TEST_ASSERT(failure_hold_admits(FailureHold::NONE, FailureHold::NONE),
              "a plain disconnect reason replaces another plain one");
}

void test_every_hold_has_exactly_one_release() {
  std::cout << "\n=== Each hold is released by the evidence that refutes it ==="
            << std::endl;

  // A hold nothing releases is permanent, and a permanent hold silences every
  // later fault. A hold everything releases is not a hold.
  for (auto h : ALL) {
    if (h == FailureHold::NONE) continue;
    const bool by_data = failure_hold_released_by_data(h);
    const bool by_auth = failure_hold_released_by_auth(h);
    TEST_ASSERT(by_data != by_auth,
                std::string(name(h)) + " has exactly one release path");
  }

  TEST_ASSERT(!failure_hold_released_by_data(FailureHold::NONE) &&
                  !failure_hold_released_by_auth(FailureHold::NONE),
              "NONE is not a hold, so nothing releases it");
}

void test_the_release_asymmetry_is_the_reason_this_is_an_enum() {
  std::cout << "\n=== DATA and AUTH are not interchangeable ===" << std::endl;

  // An SMP failure on an unbonded pump latches its reason WITHOUT tearing the
  // link down, and that link subscribes and delivers notifications normally
  // (passive telemetry needs no bond). Releasing AUTH on data would wipe the
  // pairing diagnostic the hold exists to preserve.
  TEST_ASSERT(!failure_hold_released_by_data(FailureHold::AUTH),
              "inbound data does not release a pairing failure — an unbonded "
              "pump keeps sending after one");

  // The mirror. With pairing disabled (the default) AUTH_CMPL never fires at
  // all, so releasing the watchdog's hold there would leave it held forever;
  // and reaching a successful AUTH_CMPL says nothing about whether the pump is
  // answering, which is the defect the watchdog exists for.
  TEST_ASSERT(!failure_hold_released_by_auth(FailureHold::DATA),
              "a successful auth does not release a deaf-link reason on a link "
              "that is still deaf");
  TEST_ASSERT(failure_hold_released_by_data(FailureHold::DATA),
              "...inbound data does, which is what actually refutes it");

  // SUBSCRIBE rides with DATA: both say the pump is not talking, and both are
  // refuted the moment it does.
  TEST_ASSERT(failure_hold_released_by_data(FailureHold::SUBSCRIBE) &&
                  !failure_hold_released_by_auth(FailureHold::SUBSCRIBE),
              "a subscribe fault is released by data, like the watchdog's");
}

void test_the_write_sites_in_sequence() {
  std::cout << "\n=== The four write sites, replayed as episodes ==="
            << std::endl;

  // Each site writes at a fixed rank: disconnect handler NONE, watchdog DATA,
  // subscribe SUBSCRIBE (or NONE for a CCCD failure), auth failure AUTH. An
  // episode is a sequence of those, and what the operator ends up seeing is
  // whatever survived.
  struct Step { FailureHold rank; bool expect_written; };

  // Issue #175's episode: a subscribe fault, then the watchdog 60 s later,
  // then the disconnect it provokes.
  {
    FailureHold held = FailureHold::NONE;
    const Step steps[] = {{FailureHold::SUBSCRIBE, true},
                          {FailureHold::DATA, false},
                          {FailureHold::NONE, false}};
    bool ok = true;
    for (const auto &s : steps) {
      const bool wrote = failure_hold_admits(held, s.rank);
      ok = ok && wrote == s.expect_written;
      if (wrote) held = s.rank;
    }
    TEST_ASSERT(ok && held == FailureHold::SUBSCRIBE,
                "subscribe fault → watchdog → disconnect: the subscribe cause "
                "is what the operator still sees");
  }

  // This PR's episode: a pairing failure on an unbonded pump, the link then
  // going quiet, the watchdog recycling it, and a subscribe that fails on the
  // unauthenticated link that follows.
  {
    FailureHold held = FailureHold::NONE;
    const Step steps[] = {{FailureHold::AUTH, true},
                          {FailureHold::DATA, false},
                          {FailureHold::NONE, false},
                          {FailureHold::SUBSCRIBE, false}};
    bool ok = true;
    for (const auto &s : steps) {
      const bool wrote = failure_hold_admits(held, s.rank);
      ok = ok && wrote == s.expect_written;
      if (wrote) held = s.rank;
    }
    TEST_ASSERT(ok && held == FailureHold::AUTH,
                "pairing failure → watchdog → disconnect → subscribe fault: "
                "the pairing cause survives all three");
  }

  // Recovery: the auth hold clears on a successful re-auth, and the surface is
  // writable again — a fault must not outlive the fault.
  {
    FailureHold held = FailureHold::AUTH;
    if (failure_hold_released_by_auth(held)) held = FailureHold::NONE;
    TEST_ASSERT(held == FailureHold::NONE &&
                    failure_hold_admits(held, FailureHold::NONE),
                "a successful re-auth clears the hold and the next reason "
                "writes again");
  }
}

void test_the_rank_is_pinned_and_a_new_hold_cannot_skip_a_release() {
  std::cout << "\n=== The order is the rank, and it is pinned ===" << std::endl;

  // ALL[] is hand-written and cannot notice a fifth enumerator by itself. What
  // does is -Werror=switch on this target (see tests/Makefile): both release
  // switches in failure_hold.h, and name() above, are switches over this enum,
  // so an enumerator added without a release rule fails the build. That is the
  // failure worth failing a build over — a hold nothing releases is permanent,
  // and a permanent hold silences every later fault.
  //
  // The rank is the declaration order, so an enumerator inserted rather than
  // appended silently demotes everything after it. Pin the order itself.
  TEST_ASSERT(static_cast<uint8_t>(FailureHold::NONE) == 0 &&
                  static_cast<uint8_t>(FailureHold::DATA) == 1 &&
                  static_cast<uint8_t>(FailureHold::SUBSCRIBE) == 2 &&
                  static_cast<uint8_t>(FailureHold::AUTH) == 3,
              "NONE < DATA < SUBSCRIBE < AUTH — the order IS the rank, so a "
              "reorder is a behaviour change and fails here");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Failure Hold Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_the_defect_the_rank_replaces();
  test_a_hold_admits_only_its_equals_and_betters();
  test_nothing_is_held_against_a_first_reason();
  test_a_fault_refreshes_its_own_text();
  test_every_hold_has_exactly_one_release();
  test_the_release_asymmetry_is_the_reason_this_is_an_enum();
  test_the_write_sites_in_sequence();
  test_the_rank_is_pinned_and_a_new_hold_cannot_skip_a_release();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
