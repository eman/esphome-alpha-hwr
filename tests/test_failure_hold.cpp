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
using esphome::alpha_hwr::core::failure_hold_released_by_pump_ready;
using esphome::alpha_hwr::core::failure_hold_released_by_session_ready;

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
    FailureHold::READY,
    FailureHold::DATA,
    FailureHold::PAIRING_STALL,
    FailureHold::SUBSCRIBE,
    FailureHold::AUTH,
};

static const char *name(FailureHold h) {
  switch (h) {
    case FailureHold::NONE: return "NONE";
    case FailureHold::READY: return "READY";
    case FailureHold::DATA: return "DATA";
    case FailureHold::PAIRING_STALL: return "PAIRING_STALL";
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

  // Spelled out rather than computed. An earlier version derived `expected`
  // from `held <= incoming` on the underlying values, which is the production
  // expression restated -- it would have agreed with any consistent ordering,
  // including one that ranks a subscribe fault above a bond-erasing pairing
  // failure. Sixteen hand-written outcomes say what the policy IS.
  struct Row { FailureHold held; FailureHold incoming; bool writes; };
  static const Row TABLE[] = {
      {FailureHold::NONE,          FailureHold::NONE,          true},
      {FailureHold::NONE,          FailureHold::READY,         true},
      {FailureHold::NONE,          FailureHold::DATA,          true},
      {FailureHold::NONE,          FailureHold::PAIRING_STALL, true},
      {FailureHold::NONE,          FailureHold::SUBSCRIBE,     true},
      {FailureHold::NONE,          FailureHold::AUTH,          true},

      {FailureHold::READY,         FailureHold::NONE,          false},
      {FailureHold::READY,         FailureHold::READY,         true},
      {FailureHold::READY,         FailureHold::DATA,          true},
      {FailureHold::READY,         FailureHold::PAIRING_STALL, true},
      {FailureHold::READY,         FailureHold::SUBSCRIBE,     true},
      {FailureHold::READY,         FailureHold::AUTH,          true},

      {FailureHold::DATA,          FailureHold::NONE,          false},
      {FailureHold::DATA,          FailureHold::READY,         false},
      {FailureHold::DATA,          FailureHold::DATA,          true},
      {FailureHold::DATA,          FailureHold::PAIRING_STALL, true},
      {FailureHold::DATA,          FailureHold::SUBSCRIBE,     true},
      {FailureHold::DATA,          FailureHold::AUTH,          true},

      {FailureHold::PAIRING_STALL, FailureHold::NONE,          false},
      {FailureHold::PAIRING_STALL, FailureHold::READY,         false},
      {FailureHold::PAIRING_STALL, FailureHold::DATA,          false},
      {FailureHold::PAIRING_STALL, FailureHold::PAIRING_STALL, true},
      {FailureHold::PAIRING_STALL, FailureHold::SUBSCRIBE,     true},
      {FailureHold::PAIRING_STALL, FailureHold::AUTH,          true},

      {FailureHold::SUBSCRIBE,     FailureHold::NONE,          false},
      {FailureHold::SUBSCRIBE,     FailureHold::READY,         false},
      {FailureHold::SUBSCRIBE,     FailureHold::DATA,          false},
      {FailureHold::SUBSCRIBE,     FailureHold::PAIRING_STALL, false},
      {FailureHold::SUBSCRIBE,     FailureHold::SUBSCRIBE,     true},
      {FailureHold::SUBSCRIBE,     FailureHold::AUTH,          true},

      {FailureHold::AUTH,          FailureHold::NONE,          false},
      {FailureHold::AUTH,          FailureHold::READY,         false},
      {FailureHold::AUTH,          FailureHold::DATA,          false},
      {FailureHold::AUTH,          FailureHold::PAIRING_STALL, false},
      {FailureHold::AUTH,          FailureHold::SUBSCRIBE,     false},
      {FailureHold::AUTH,          FailureHold::AUTH,          true},
  };

  const int rows = static_cast<int>(sizeof(TABLE) / sizeof(TABLE[0]));
  const int holds = static_cast<int>(sizeof(ALL) / sizeof(ALL[0]));
  TEST_ASSERT(rows == holds * holds,
              "the table covers every (held, incoming) pair");

  for (const auto &r : TABLE)
    TEST_ASSERT(failure_hold_admits(r.held, r.incoming) == r.writes,
                std::string("holding ") + name(r.held) + ", a " +
                    name(r.incoming) + " reason " +
                    (r.writes ? "writes" : "does not write"));
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

void test_the_release_matrix() {
  std::cout << "\n=== Each hold is released by the evidence that refutes it ==="
            << std::endl;

  // The whole matrix, hand-written. An earlier version asserted instead that
  // each hold has EXACTLY ONE release path -- which passed, but pinned an
  // invariant nothing needed: AUTH now has two, and the exclusivity claim
  // would have rejected the fix for an unbounded hold rather than the bug.
  struct Row {
    FailureHold hold;
    bool by_data;
    bool by_auth;
    bool by_ready;
    bool by_pump_ready;
  };
  static const Row TABLE[] = {
      {FailureHold::NONE,           false, false, false, false},
      {FailureHold::READY,          false, false, false, true},
      {FailureHold::DATA,           true, false, false, false},
      {FailureHold::PAIRING_STALL,  true, true, true, false},
      {FailureHold::SUBSCRIBE,      true, false, false, false},
      {FailureHold::AUTH,           false, true, true, false},
  };

  TEST_ASSERT(sizeof(TABLE) / sizeof(TABLE[0]) == sizeof(ALL) / sizeof(ALL[0]),
              "the table covers every hold");

  for (const auto &r : TABLE) {
    TEST_ASSERT(failure_hold_released_by_data(r.hold) == r.by_data,
                std::string(name(r.hold)) + ": inbound data " +
                    (r.by_data ? "releases" : "does not release") + " it");
    TEST_ASSERT(failure_hold_released_by_auth(r.hold) == r.by_auth,
                std::string(name(r.hold)) + ": a successful auth " +
                    (r.by_auth ? "releases" : "does not release") + " it");
    TEST_ASSERT(failure_hold_released_by_session_ready(r.hold) == r.by_ready,
                std::string(name(r.hold)) + ": reaching READY " +
                    (r.by_ready ? "releases" : "does not release") + " it");
    TEST_ASSERT(failure_hold_released_by_pump_ready(r.hold) == r.by_pump_ready,
                std::string(name(r.hold)) + ": the pump becoming ready " +
                    (r.by_pump_ready ? "releases" : "does not release") + " it");
  }

  // The property that actually matters, kept as a property: no hold may be
  // permanent. One that nothing releases silences every later fault for the
  // rest of the boot, which is the defect the READY release exists to close.
  for (auto h : ALL) {
    if (h == FailureHold::NONE) continue;
    TEST_ASSERT(failure_hold_released_by_data(h) ||
                    failure_hold_released_by_auth(h) ||
                    failure_hold_released_by_session_ready(h) ||
                    failure_hold_released_by_pump_ready(h),
                std::string(name(h)) + " has at least one release path");
  }

  TEST_ASSERT(!failure_hold_released_by_data(FailureHold::NONE) &&
                  !failure_hold_released_by_auth(FailureHold::NONE) &&
                  !failure_hold_released_by_session_ready(FailureHold::NONE),
              "NONE is not a hold, so nothing releases it");
}

void test_an_auth_hold_cannot_outlive_its_own_usefulness() {
  std::cout << "\n=== An auth hold is bounded by READY ===" << std::endl;

  // The defect the READY release closes. Nothing outranks AUTH, so before it
  // the only exit was an AUTH_CMPL success -- which a pump that never pairs
  // never produces. The hold then masked every later fault for the rest of the
  // boot, and it could not even report the pairing failure any more: the fault
  // string is displayed only while the session is NOT ready.
  FailureHold held = FailureHold::AUTH;

  // An unbonded pump keeps delivering telemetry after a failed SMP, so the
  // session goes on to READY with the hold still set...
  TEST_ASSERT(!failure_hold_released_by_data(held),
              "inbound data still does not release it — while the link is "
              "coming up, the pairing failure is the operative fault");
  TEST_ASSERT(failure_hold_released_by_session_ready(held),
              "...but READY does, because past that point the string is not "
              "shown at all");
  held = FailureHold::NONE;

  // ...so a later, unrelated outage reports its own cause instead of a stale
  // pairing string.
  TEST_ASSERT(failure_hold_admits(held, FailureHold::DATA),
              "a later deaf link then reports \"No data from pump\", not the "
              "pairing failure from hours earlier");

  // And the watchdog's own hold is NOT released by READY: a deaf link reaches
  // READY on every cycle (auth completing proves only that a chain of timers
  // ran), so releasing there would clear the reason on exactly the links it
  // exists to describe.
  TEST_ASSERT(!failure_hold_released_by_session_ready(FailureHold::DATA),
              "READY does not release the watchdog's hold — it is reached on "
              "deaf links too");
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
                  static_cast<uint8_t>(FailureHold::READY) == 1 &&
                  static_cast<uint8_t>(FailureHold::DATA) == 2 &&
                  static_cast<uint8_t>(FailureHold::PAIRING_STALL) == 3 &&
                  static_cast<uint8_t>(FailureHold::SUBSCRIBE) == 4 &&
                  static_cast<uint8_t>(FailureHold::AUTH) == 5,
              "NONE < READY < DATA < PAIRING_STALL < SUBSCRIBE < AUTH — the "
              "order IS the rank, so a reorder is a behaviour change and fails "
              "here");
}

// ── The two orderings a skeptic pass corrected ─────────────────────────────
// The first version of the pairing-stall change (issue #230) latched at AUTH
// rank. Both of these episodes were driven against it and both came out wrong,
// so they are pinned as episodes rather than as two more table rows: a rank is
// only meaningful as the answer to "which of these does the operator see".
void test_an_inference_from_absence_loses_to_an_observed_event() {
  std::cout << "\n=== An inference from an absence loses to an observed event ==="
            << std::endl;

  // Issue #14: an encryption request on a bonded reconnect fails 0x61 and the
  // bond is erased. Every connection after it is unbonded and unanswered --
  // that failure MANUFACTURES the stall's precondition. At equal rank the
  // stall replaced the root cause about fifteen seconds later, and 0x61 is the
  // only thing pointing at reconnect_settle_time, which is the mitigation that
  // stops it recurring. The stall is the treatment; 0x61 is the prevention.
  TEST_ASSERT(!failure_hold_admits(FailureHold::AUTH, FailureHold::PAIRING_STALL),
              "A bond-erasing encryption failure is not replaced by the stall "
              "its own bond erasure created");
  TEST_ASSERT(failure_hold_admits(FailureHold::PAIRING_STALL, FailureHold::AUTH),
              "...while a real SMP failure still replaces a stall");

  // A link that reached the subscribe step got far past where a refusing pump
  // drops it -- about 2 s, before discovery completes. The stall cannot see how
  // far the link got, so at a higher rank it masked a live subscribe fault with
  // a diagnosis that had stopped applying.
  TEST_ASSERT(!failure_hold_admits(FailureHold::SUBSCRIBE, FailureHold::PAIRING_STALL),
              "A subscribe fault is not masked by a stall: reaching subscribe "
              "is itself evidence the pump did not refuse the link");
  TEST_ASSERT(failure_hold_admits(FailureHold::PAIRING_STALL, FailureHold::SUBSCRIBE),
              "...and the subscribe fault replaces the stall when both happen");

  // Against the watchdog it wins, on the same logic that puts SUBSCRIBE above
  // DATA: silence is the symptom every cause here shares.
  TEST_ASSERT(!failure_hold_admits(FailureHold::PAIRING_STALL, FailureHold::DATA),
              "\"No data from pump\" does not replace the reason there is no "
              "data");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "Failure Hold Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_the_defect_the_rank_replaces();
  test_a_hold_admits_only_its_equals_and_betters();
  test_nothing_is_held_against_a_first_reason();
  test_a_fault_refreshes_its_own_text();
  test_the_release_matrix();
  test_an_auth_hold_cannot_outlive_its_own_usefulness();
  test_the_release_asymmetry_is_the_reason_this_is_an_enum();
  test_the_write_sites_in_sequence();
  test_an_inference_from_absence_loses_to_an_observed_event();
  test_the_rank_is_pinned_and_a_new_hold_cannot_skip_a_release();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
