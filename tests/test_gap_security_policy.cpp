// Host tests for gap_security_policy.h — whose pairing request is this, and do
// we answer it.
//
// The defect these pin: BLE GAP events are broadcast, not routed. ESPHome's
// esp32_ble_tracker hands every GAP event to every client, and ble_client hands
// it on to every node, so this component's five GAP security branches saw the
// pairing traffic of every other BLE peer on the node — a bluetooth_proxy
// connection, a second ble_client, anything — and all five acted on it. Two of
// them replied "yes, let's pair" on a stranger's behalf. One of them, the
// AUTH_CMPL handler, took a stranger's SMP failure for the pump's: it latched
// that reason at AUTH rank (which failure_hold.h ranks above everything, so it
// masks the real cause) and disconnected the pump.
//
// ble_connection_manager.cpp is compiled by no host test, so — as with
// failure_hold.h and subscribe_outcome.h — the decision is extracted to a
// header and answered here, where getting it wrong is visible.

#include <cstdint>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/gap_security_policy.h"

using esphome::alpha_hwr::core::GapSecurityAction;
using esphome::alpha_hwr::core::gap_addr_matches;
using esphome::alpha_hwr::core::gap_security_action;

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

// The pump on the bench, and a stranger that differs only in the last octet —
// the hardest case for a comparison that stops early or compares too few bytes.
static const uint8_t PUMP[6] = {0xF8, 0xE6, 0x1A, 0x2B, 0x3C, 0x4D};
static const uint8_t STRANGER_LAST[6] = {0xF8, 0xE6, 0x1A, 0x2B, 0x3C, 0x4E};
static const uint8_t STRANGER_FIRST[6] = {0xF9, 0xE6, 0x1A, 0x2B, 0x3C, 0x4D};
static const uint8_t UNSET[6] = {0, 0, 0, 0, 0, 0};

// A perfectly ordinary address that happens to contain zero octets, including
// in the position the unset-scan looks at first. Without it, "is this peer
// unset?" can be reduced to `peer_addr[0] == 0` -- or its loop shortened to one
// octet -- and every test above still passes, because no other address here
// contains a zero anywhere. In production that reduction reads a pump at
// 00:1E:2A:... as having no address configured and ignores its AUTH_CMPL
// forever: encryption_pending_ never clears, the deferred CCCD write never
// fires, and the session parks in SUBSCRIBING on every single connection.
// 00:xx:xx is among the most common OUI prefixes there is, so this is a real
// address, not a contrived one.
static const uint8_t PUMP_WITH_ZEROS[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};

static void test_the_pump_matches_itself() {
  TEST_ASSERT(gap_addr_matches(PUMP, PUMP), "the configured pump address matches");
}

static void test_a_real_address_containing_zeroes_is_not_mistaken_for_unset() {
  TEST_ASSERT(gap_addr_matches(PUMP_WITH_ZEROS, PUMP_WITH_ZEROS),
              "an address with a zero first octet is a configured address, not an unset one");
  TEST_ASSERT(!gap_addr_matches(PUMP, PUMP_WITH_ZEROS),
              "...and it still rejects a different device");
  TEST_ASSERT(gap_security_action(gap_addr_matches(PUMP_WITH_ZEROS, PUMP_WITH_ZEROS), true) ==
                  GapSecurityAction::ACCEPT,
              "...and its own pairing request is still accepted");
}

// The comparison length. Shortening it is caught by the near-miss loop below,
// but lengthening it is not: comparing a seventh byte of a six-byte address
// compares one object against itself in every test here and reads out of
// bounds in production, where the two addresses are distinct objects.
static void test_the_address_length_is_the_address_length() {
  TEST_ASSERT(esphome::alpha_hwr::core::BD_ADDR_LEN == 6,
              "a BLE address is six octets -- neither truncated nor over-read");
}

// A comparison that stops at the first byte, or runs one byte short, still
// accepts every address in this codebase's own logs. Both ends are pinned.
static void test_every_octet_is_compared() {
  TEST_ASSERT(!gap_addr_matches(STRANGER_LAST, PUMP),
              "a device differing only in the last octet does not match");
  TEST_ASSERT(!gap_addr_matches(STRANGER_FIRST, PUMP),
              "a device differing only in the first octet does not match");

  for (size_t i = 0; i < 6; i++) {
    uint8_t near_miss[6];
    for (size_t j = 0; j < 6; j++) {
      near_miss[j] = PUMP[j];
    }
    near_miss[i] = static_cast<uint8_t>(near_miss[i] ^ 0xFF);
    TEST_ASSERT(!gap_addr_matches(near_miss, PUMP),
                "a device differing in octet " + std::to_string(i) + " does not match");
  }
}

// An all-zero peer address means the ble_client has no address configured. The
// tempting reading is "no address configured, so nothing to protect" — but a
// stranger sending security traffic while we do not know our own peer is
// precisely the case that must not be answered, and a plain memcmp would match
// an all-zero event address against it.
static void test_an_unconfigured_peer_matches_nothing() {
  TEST_ASSERT(!gap_addr_matches(PUMP, UNSET),
              "no peer address configured: a real device does not match");
  TEST_ASSERT(!gap_addr_matches(UNSET, UNSET),
              "no peer address configured: an all-zero event address does not match either");
}

static void test_a_null_address_is_not_a_match() {
  TEST_ASSERT(!gap_addr_matches(nullptr, PUMP), "a null event address does not match");
  TEST_ASSERT(!gap_addr_matches(PUMP, nullptr), "a null peer address does not match");
}

// The whole point: a stranger's request is ignored whatever our config says.
// Gating on enable_pairing alone would have left the auto-accept live for every
// other peer on any node that has pairing switched on — which is every node
// that actually controls the pump.
static void test_a_stranger_is_ignored_however_we_are_configured() {
  TEST_ASSERT(gap_security_action(false, true) == GapSecurityAction::IGNORE,
              "pairing enabled: a stranger's request is ignored");
  TEST_ASSERT(gap_security_action(false, false) == GapSecurityAction::IGNORE,
              "pairing disabled: a stranger's request is ignored");
}

// enable_pairing defaults to false and documents itself as passive telemetry
// only. init_security() honoured it; the reply paths did not.
static void test_enable_pairing_governs_our_own_pump() {
  TEST_ASSERT(gap_security_action(true, true) == GapSecurityAction::ACCEPT,
              "our pump, pairing enabled: accept");
  TEST_ASSERT(gap_security_action(true, false) == GapSecurityAction::DECLINE,
              "our pump, pairing disabled: decline");
}

// The three inputs that produce a non-ACCEPT are each individually sufficient.
// Stated as a table so a policy that collapses to "accept unless X" is visible.
static void test_the_decision_table() {
  struct Row {
    bool ours;
    bool pairing;
    GapSecurityAction expected;
    const char *name;
  };
  const Row rows[] = {
      {true, true, GapSecurityAction::ACCEPT, "ours + pairing on"},
      {true, false, GapSecurityAction::DECLINE, "ours + pairing off"},
      {false, true, GapSecurityAction::IGNORE, "stranger + pairing on"},
      {false, false, GapSecurityAction::IGNORE, "stranger + pairing off"},
  };
  for (const auto &r : rows) {
    TEST_ASSERT(gap_security_action(r.ours, r.pairing) == r.expected,
                std::string("decision table: ") + r.name);
  }

  // Exactly one of the four inputs accepts. A policy that accepts more than one
  // row has stopped being a filter.
  int accepts = 0;
  for (const auto &r : rows) {
    if (gap_security_action(r.ours, r.pairing) == GapSecurityAction::ACCEPT) {
      accepts++;
    }
  }
  TEST_ASSERT(accepts == 1, "exactly one of the four input combinations accepts");
}

// Compiled with -Werror=switch, so this is a build-time assertion as much as a
// test: both handlers in ble_connection_manager.cpp switch over this enum, and
// a new action added without deciding what each of them does with it would
// otherwise fall through to whatever the default happened to be. The three
// actions are the three answers a pairing request can get — reply yes, reply
// no, or say nothing — so a fourth is a claim that deserves to break the build.
static const char *action_name(GapSecurityAction a) {
  switch (a) {
    case GapSecurityAction::ACCEPT:
      return "ACCEPT";
    case GapSecurityAction::DECLINE:
      return "DECLINE";
    case GapSecurityAction::IGNORE:
      return "IGNORE";
  }
  return "UNREACHABLE";
}

static void test_every_action_is_accounted_for() {
  TEST_ASSERT(std::string(action_name(gap_security_action(true, true))) == "ACCEPT",
              "ours + pairing on names ACCEPT");
  TEST_ASSERT(std::string(action_name(gap_security_action(true, false))) == "DECLINE",
              "ours + pairing off names DECLINE");
  TEST_ASSERT(std::string(action_name(gap_security_action(false, true))) == "IGNORE",
              "a stranger names IGNORE");
}

int main() {
  std::cout << "==========================================" << std::endl;
  std::cout << "GAP Security Policy Tests" << std::endl;
  std::cout << "==========================================" << std::endl;

  test_the_pump_matches_itself();
  test_a_real_address_containing_zeroes_is_not_mistaken_for_unset();
  test_the_address_length_is_the_address_length();
  test_every_octet_is_compared();
  test_an_unconfigured_peer_matches_nothing();
  test_a_null_address_is_not_a_match();
  test_a_stranger_is_ignored_however_we_are_configured();
  test_enable_pairing_governs_our_own_pump();
  test_the_decision_table();
  test_every_action_is_accounted_for();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
