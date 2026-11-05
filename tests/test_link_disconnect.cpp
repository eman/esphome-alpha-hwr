// Host tests for link_disconnect.h.
//
// The reason-name table used to be a switch inside the DISCONNECT handler of
// ble_connection_manager.cpp, written only when no fault hold was in force --
// which is why ten hours of drops under a held `0x52` left no record. The
// table now feeds two surfaces (the held fault string and the unheld Pump Link
// Last Disconnect), so it is extracted and pinned: every code the stack can
// report has a name, the two sentinels fall through to the generic one, and
// the formatted string carries the stage, which is what makes the same code
// readable as two different events.
//
// The codes are compared against the mock's esp_gatt_conn_reason_t, which is
// the enum the production code passes; tests/mocks/esp_gattc_api.h mirrors
// esp_gatt_defs.h value for value.
#include <cstring>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/link_disconnect.h"
#include "esp_gattc_api.h"

using esphome::alpha_hwr::core::disconnect_reason_name;
using esphome::alpha_hwr::core::format_link_disconnect;

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message)                                        \
  if (condition) {                                                             \
    tests_passed++;                                                            \
    std::cout << "  [PASS] " << message << std::endl;                          \
  } else {                                                                     \
    tests_failed++;                                                            \
    std::cout << "  [FAIL] " << message << std::endl;                          \
  }

static std::string name_of(esp_gatt_conn_reason_t r) {
  return disconnect_reason_name(static_cast<uint16_t>(r));
}

/// The name each enumerator must get. A switch over the enum with NO default,
/// so this file is built in the Makefile's -Werror=switch group: a reason
/// added to esp_gatt_conn_reason_t without a case here fails the build, rather
/// than silently reading "Disconnected" on both link surfaces.
static const char *expected_name(esp_gatt_conn_reason_t r) {
  switch (r) {
    case ESP_GATT_CONN_UNKNOWN: return "Disconnected";
    case ESP_GATT_CONN_L2C_FAILURE: return "L2CAP Failure";
    case ESP_GATT_CONN_TIMEOUT: return "Connection Timeout";
    case ESP_GATT_CONN_TERMINATE_PEER_USER: return "Remote Terminated";
    case ESP_GATT_CONN_TERMINATE_LOCAL_HOST: return "Local Host Terminated";
    case ESP_GATT_CONN_FAIL_ESTABLISH: return "Failed To Establish";
    case ESP_GATT_CONN_LMP_TIMEOUT: return "LL Response Timeout";
    case ESP_GATT_CONN_CONN_CANCEL: return "Connection Cancelled";
    case ESP_GATT_CONN_NONE: return "Disconnected";
  }
  return nullptr;
}

void test_every_enumerator_is_covered() {
  std::cout << "\n=== Every enumerator gets the name the table promises ===" << std::endl;
  const esp_gatt_conn_reason_t all[] = {
      ESP_GATT_CONN_UNKNOWN,          ESP_GATT_CONN_L2C_FAILURE,
      ESP_GATT_CONN_TIMEOUT,          ESP_GATT_CONN_TERMINATE_PEER_USER,
      ESP_GATT_CONN_TERMINATE_LOCAL_HOST, ESP_GATT_CONN_FAIL_ESTABLISH,
      ESP_GATT_CONN_LMP_TIMEOUT,      ESP_GATT_CONN_CONN_CANCEL,
      ESP_GATT_CONN_NONE,
  };
  bool all_match = true;
  for (auto r : all) {
    if (name_of(r) != expected_name(r)) {
      all_match = false;
      std::cout << "    mismatch for 0x" << std::hex << static_cast<unsigned>(r) << std::dec
                << ": got \"" << name_of(r) << "\"" << std::endl;
    }
  }
  TEST_ASSERT(all_match, "The table and the enum agree on every value");
}

void test_every_stack_reason_has_a_name() {
  std::cout << "\n=== Every reason the stack reports has a name ===" << std::endl;
  TEST_ASSERT(name_of(ESP_GATT_CONN_L2C_FAILURE) == "L2CAP Failure", "0x01");
  TEST_ASSERT(name_of(ESP_GATT_CONN_TIMEOUT) == "Connection Timeout",
              "0x08 -- the supervision timeout, a link going quiet");
  TEST_ASSERT(name_of(ESP_GATT_CONN_TERMINATE_PEER_USER) == "Remote Terminated",
              "0x13 -- the pump closing the link");
  TEST_ASSERT(name_of(ESP_GATT_CONN_TERMINATE_LOCAL_HOST) == "Local Host Terminated",
              "0x16 -- this component closing it");
  TEST_ASSERT(name_of(ESP_GATT_CONN_LMP_TIMEOUT) == "LL Response Timeout", "0x22");
  TEST_ASSERT(name_of(ESP_GATT_CONN_FAIL_ESTABLISH) == "Failed To Establish", "0x3e");
  TEST_ASSERT(name_of(ESP_GATT_CONN_CONN_CANCEL) == "Connection Cancelled", "0x0100");
}

void test_the_sentinels_and_the_unknown_fall_through() {
  std::cout << "\n=== Sentinels and unknown codes read as plain Disconnected ===" << std::endl;
  TEST_ASSERT(name_of(ESP_GATT_CONN_UNKNOWN) == "Disconnected", "ESP_GATT_CONN_UNKNOWN (0)");
  TEST_ASSERT(name_of(ESP_GATT_CONN_NONE) == "Disconnected", "ESP_GATT_CONN_NONE (0x0101)");
  TEST_ASSERT(disconnect_reason_name(0x3d) == std::string("Disconnected"),
              "A code the table does not know still gets a name, not a null");
}

void test_the_string_carries_reason_code_and_stage() {
  std::cout << "\n=== The formatted string carries reason, code and stage ===" << std::endl;
  char buf[64];
  format_link_disconnect(buf, sizeof(buf), 0x13, "SERVICE_DISCOVERY");
  TEST_ASSERT(std::string(buf) == "Remote Terminated (0x13) in SERVICE_DISCOVERY",
              "The pump refusing an unbonded link reads as exactly that");
  format_link_disconnect(buf, sizeof(buf), 0x08, "READY");
  TEST_ASSERT(std::string(buf) == "Connection Timeout (0x08) in READY",
              "...and a working link going quiet reads differently from the same "
              "code in discovery");
  format_link_disconnect(buf, sizeof(buf), 0x0100, "IDLE");
  TEST_ASSERT(std::string(buf) == "Connection Cancelled (0x100) in IDLE",
              "A code above a byte prints all its digits, in the same %02x form "
              "the fault surface has always used");
}

void test_the_reason_half_matches_the_fault_surface() {
  std::cout << "\n=== The reason half is what the fault surface shows ===" << std::endl;
  // ble_connection_manager.cpp formats an unheld fault as "%s (0x%02x)" from
  // the same table; the two surfaces have to agree byte for byte up to the
  // stage so a reader can line them up.
  char fault[48];
  snprintf(fault, sizeof(fault), "%s (0x%02x)", disconnect_reason_name(0x3e), 0x3eu);
  char drop[64];
  format_link_disconnect(drop, sizeof(drop), 0x3e, "IDLE");
  TEST_ASSERT(std::strncmp(fault, drop, std::strlen(fault)) == 0,
              "Last Disconnect begins with the fault surface's own string");
  TEST_ASSERT(std::string(fault) == "Failed To Establish (0x3e)",
              "...which is the string the docs quote for a radio drop");
}

void test_a_short_buffer_is_truncated_not_overrun() {
  std::cout << "\n=== A short buffer is truncated, not overrun ===" << std::endl;
  char buf[16];
  std::memset(buf, 'x', sizeof(buf));
  format_link_disconnect(buf, sizeof(buf), 0x13, "SERVICE_DISCOVERY");
  TEST_ASSERT(buf[sizeof(buf) - 1] == '\0', "snprintf terminates inside the buffer");
  TEST_ASSERT(std::string(buf) == "Remote Terminat", "...and keeps the leading text");
}

int main() {
  std::cout << "=== link_disconnect.h tests ===" << std::endl;
  test_every_enumerator_is_covered();
  test_every_stack_reason_has_a_name();
  test_the_sentinels_and_the_unknown_fall_through();
  test_the_string_carries_reason_code_and_stage();
  test_the_reason_half_matches_the_fault_surface();
  test_a_short_buffer_is_truncated_not_overrun();
  std::cout << "\n=== Results: " << tests_passed << " passed, " << tests_failed
            << " failed ===" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
