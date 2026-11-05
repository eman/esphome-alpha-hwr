#pragma once
//
// The raw record of a link drop, as against the diagnosis of one.
//
// Pump Link Fault is a *diagnosis*: failure_hold.h ranks the reasons so that
// the cause of an outage (an auth failure, a deaf link, a pump refusing to
// pair) is not overwritten by the routine disconnects of the reconnect loop
// that follows it. That is the right surface for an operator, and it has a
// cost that showed on one installation in November 2026: a bonded reconnect
// failed with `Pairing Not Supported (0x52)` at 03:25, the hold kept that
// string on the fault surface for the next ten hours, and every disconnect
// underneath it -- the reasons, the cadence, the stage each link died at --
// went unrecorded. The only record was the serial log, with nobody attached.
// When the node was finally examined the question "what did the pump do
// between 03:25 and 13:40" had no answer at all.
//
// So two more surfaces, both unheld and both event-driven:
//
//   * Pump Link Last Disconnect -- the reason and the session stage of the
//     most recent drop, overwritten on every drop. `Remote Terminated (0x13)
//     in SERVICE_DISCOVERY` is the pump refusing an unbonded link (#244);
//     `Connection Timeout (0x08) in READY` is a working link going quiet. The
//     same reason code means different things at different stages, which is
//     why the stage is in the string.
//   * Pump Link Disconnects -- how many drops since boot. A text sensor gated
//     on change cannot show that the *same* reason is repeating every five
//     seconds; a counter can. Pump Link Recycles does not serve: it counts
//     watchdog recycles only, and read 0 through the whole incident.
//
// The naming lives here rather than in ble_connection_manager.cpp so the
// table is host-tested (issue #175 did the same for subscribe_outcome.h): a
// code added to the table without a name would otherwise read as
// "Disconnected (0xNN)" on both surfaces and nothing would notice.

#include <cstdint>
#include <cstdio>

namespace esphome {
namespace alpha_hwr {
namespace core {

/// Human-readable name of an ESP-IDF GATT disconnect reason
/// (esp_gatt_conn_reason_t), for the two link surfaces that show one.
///
/// The codes are the HCI error codes the BLE stack reports a disconnect
/// with, in esp_gattc_api.h; the values are repeated here as literals so this
/// header compiles on the host without ESP-IDF, and tests/test_link_disconnect.cpp
/// pins each one against the enum the production code actually passes.
inline const char *disconnect_reason_name(uint16_t reason) {
  switch (reason) {
    case 0x01: return "L2CAP Failure";              // ESP_GATT_CONN_L2C_FAILURE
    case 0x08: return "Connection Timeout";         // ESP_GATT_CONN_TIMEOUT
    case 0x13: return "Remote Terminated";          // ESP_GATT_CONN_TERMINATE_PEER_USER
    case 0x16: return "Local Host Terminated";      // ESP_GATT_CONN_TERMINATE_LOCAL_HOST
    case 0x22: return "LL Response Timeout";        // ESP_GATT_CONN_LMP_TIMEOUT
    case 0x3e: return "Failed To Establish";        // ESP_GATT_CONN_FAIL_ESTABLISH
    case 0x0100: return "Connection Cancelled";     // ESP_GATT_CONN_CONN_CANCEL
    // ESP_GATT_CONN_UNKNOWN (0) and ESP_GATT_CONN_NONE (0x0101) are "no known
    // reason" sentinels; they fall to the default with everything else.
    default: return "Disconnected";
  }
}

/// The Pump Link Last Disconnect string: `<name> (0xNN) in <stage>`, where
/// stage is the session state the link was in when it dropped
/// (Session::get_state_name()).
///
/// The reason-and-code half is byte-identical to what the fault surface
/// shows for an unheld disconnect, so the two can be read against each other.
inline void format_link_disconnect(char *buf, size_t len, uint16_t reason,
                                   const char *stage) {
  snprintf(buf, len, "%s (0x%02x) in %s", disconnect_reason_name(reason),
           static_cast<unsigned>(reason), stage);
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
