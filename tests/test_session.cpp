// Host tests for the connection session FSM (session.cpp).
//
// Why this file exists: session.h documents a state machine in prose and in an
// ASCII diagram, and until now nothing checked the code against either. The
// diagram was wrong -- it promised "any state can transition to ERROR on
// failure", and the ERROR state had no way in: `on_error()` and `reset()` had
// no caller anywhere in the component (issue #174 audit). Removing them is
// safe only if the transitions that *do* exist are pinned, which is what this
// file is for.
//
// The FSM is small enough that the whole thing can be asserted rather than
// sampled, so it is: every documented transition, and the behaviour on the
// out-of-order calls the code explicitly warns about.

#include <iostream>

#include "../components/alpha_hwr/session.h"

uint32_t mock_millis = 0;

using esphome::alpha_hwr::core::Session;
using esphome::alpha_hwr::core::SessionState;

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

// ── The happy path, one transition at a time ────────────────────────────────
void test_the_documented_connection_sequence() {
  std::cout << "\n=== The documented connection sequence ===" << std::endl;
  Session s;

  TEST_ASSERT(s.get_state() == SessionState::IDLE, "A fresh session is IDLE");
  TEST_ASSERT(!s.is_connected(), "...and not connected");
  TEST_ASSERT(!s.is_ready(), "...and not ready");

  s.on_connected();
  TEST_ASSERT(s.get_state() == SessionState::SERVICE_DISCOVERY,
              "on_connected() -> SERVICE_DISCOVERY");
  TEST_ASSERT(s.is_connected(), "...and connected becomes true immediately");
  TEST_ASSERT(!s.is_ready(), "...but not ready");

  s.on_service_found();
  TEST_ASSERT(s.get_state() == SessionState::SUBSCRIBING,
              "on_service_found() -> SUBSCRIBING");

  s.on_subscribed();
  TEST_ASSERT(s.get_state() == SessionState::AUTHENTICATING,
              "on_subscribed() -> AUTHENTICATING");

  s.on_authenticated();
  TEST_ASSERT(s.get_state() == SessionState::READY,
              "on_authenticated() -> READY");
  TEST_ASSERT(s.is_ready(), "...and is_ready() agrees");
  TEST_ASSERT(s.is_connected(), "...and so does is_connected()");
}

// ── Disconnect, from every state ────────────────────────────────────────────
// "* -> IDLE : Disconnect" is the one transition the diagram claims from
// anywhere, so assert it from everywhere rather than from READY alone.
void test_disconnect_returns_to_idle_from_every_state() {
  std::cout << "\n=== Disconnect returns to IDLE from every state ===" << std::endl;

  {
    Session s;
    s.on_connected();
    s.on_disconnected();
    TEST_ASSERT(s.get_state() == SessionState::IDLE, "SERVICE_DISCOVERY -> IDLE");
  }
  {
    Session s;
    s.on_connected();
    s.on_service_found();
    s.on_disconnected();
    TEST_ASSERT(s.get_state() == SessionState::IDLE, "SUBSCRIBING -> IDLE");
  }
  {
    Session s;
    s.on_connected();
    s.on_service_found();
    s.on_subscribed();
    s.on_disconnected();
    TEST_ASSERT(s.get_state() == SessionState::IDLE, "AUTHENTICATING -> IDLE");
  }
  {
    Session s;
    s.on_connected();
    s.on_service_found();
    s.on_subscribed();
    s.on_authenticated();
    s.on_disconnected();
    TEST_ASSERT(s.get_state() == SessionState::IDLE, "READY -> IDLE");
    TEST_ASSERT(!s.is_connected(), "...and is_connected() goes false");
    TEST_ASSERT(!s.is_ready(), "...and is_ready() goes false");
  }
  {
    Session s;
    s.on_disconnected();
    TEST_ASSERT(s.get_state() == SessionState::IDLE,
                "IDLE -> IDLE is a no-op, not an error");
  }
}

// ── is_connected() means "anything but IDLE" ────────────────────────────────
// It used to read `!= IDLE && != ERROR`. Removing ERROR must not change which
// states count as connected, so state it directly.
void test_is_connected_is_exactly_not_idle() {
  std::cout << "\n=== is_connected() is exactly 'not IDLE' ===" << std::endl;

  Session s;
  TEST_ASSERT(!s.is_connected(), "IDLE is not connected");
  s.on_connected();
  TEST_ASSERT(s.is_connected(), "SERVICE_DISCOVERY is connected");
  s.on_service_found();
  TEST_ASSERT(s.is_connected(), "SUBSCRIBING is connected");
  s.on_subscribed();
  TEST_ASSERT(s.is_connected(), "AUTHENTICATING is connected");
  s.on_authenticated();
  TEST_ASSERT(s.is_connected(), "READY is connected");
}

// ── Re-authentication ───────────────────────────────────────────────────────
// on_authenticating() is documented as reachable from SUBSCRIBING (first auth)
// and from READY (re-auth), and as idempotent while already AUTHENTICATING.
void test_reauthentication_from_ready() {
  std::cout << "\n=== Re-authentication ===" << std::endl;

  Session s;
  s.on_connected();
  s.on_service_found();
  s.on_subscribed();
  s.on_authenticated();
  TEST_ASSERT(s.get_state() == SessionState::READY, "Starts READY");

  s.on_authenticating();
  TEST_ASSERT(s.get_state() == SessionState::AUTHENTICATING,
              "on_authenticating() from READY re-enters AUTHENTICATING");
  TEST_ASSERT(!s.is_ready(), "...and READY is given up while it runs");

  s.on_authenticating();
  TEST_ASSERT(s.get_state() == SessionState::AUTHENTICATING,
              "...and calling it again is idempotent");

  s.on_authenticated();
  TEST_ASSERT(s.get_state() == SessionState::READY, "Re-auth completes back to READY");
}

// ── Out-of-order calls warn but still transition ────────────────────────────
// This is the actual behaviour, and it is worth pinning precisely because it is
// permissive: every on_*() guard logs a warning and then transitions anyway. A
// reader of session.cpp could easily assume the guards reject. They do not.
//
// It matters for stale callbacks: anything that can call on_authenticated()
// after a disconnect drives the session to READY from IDLE without the link
// having been re-established. Nothing does today -- Authentication's sequence
// number and running_ flag stop it (issue #174) -- but that safety lives in the
// caller, not here, and this test is what says so.
void test_out_of_order_calls_transition_anyway() {
  std::cout << "\n=== Out-of-order calls warn, but still transition ===" << std::endl;

  {
    Session s;
    s.on_authenticated();  // straight from IDLE
    TEST_ASSERT(s.get_state() == SessionState::READY,
                "on_authenticated() from IDLE reaches READY -- the guard warns, "
                "it does not refuse");
  }
  {
    Session s;
    s.on_subscribed();  // from IDLE, skipping discovery
    TEST_ASSERT(s.get_state() == SessionState::AUTHENTICATING,
                "on_subscribed() from IDLE reaches AUTHENTICATING for the same reason");
  }
  {
    Session s;
    s.on_service_found();
    TEST_ASSERT(s.get_state() == SessionState::SUBSCRIBING,
                "on_service_found() from IDLE reaches SUBSCRIBING likewise");
  }
}

// ── State names ─────────────────────────────────────────────────────────────
// get_state_name() feeds a log line in alpha_hwr.cpp, and it carried an "ERROR"
// case for a state nothing could enter.
void test_every_state_has_a_name() {
  std::cout << "\n=== Every state has a name ===" << std::endl;

  Session s;
  TEST_ASSERT(std::string(s.get_state_name()) == "IDLE", "IDLE");
  s.on_connected();
  TEST_ASSERT(std::string(s.get_state_name()) == "SERVICE_DISCOVERY", "SERVICE_DISCOVERY");
  s.on_service_found();
  TEST_ASSERT(std::string(s.get_state_name()) == "SUBSCRIBING", "SUBSCRIBING");
  s.on_subscribed();
  TEST_ASSERT(std::string(s.get_state_name()) == "AUTHENTICATING", "AUTHENTICATING");
  s.on_authenticated();
  TEST_ASSERT(std::string(s.get_state_name()) == "READY", "READY");
}

int main() {
  std::cout << "===========================================================" << std::endl;
  std::cout << "  Session FSM Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_documented_connection_sequence();
  test_disconnect_returns_to_idle_from_every_state();
  test_is_connected_is_exactly_not_idle();
  test_reauthentication_from_ready();
  test_out_of_order_calls_transition_anyway();
  test_every_state_has_a_name();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
