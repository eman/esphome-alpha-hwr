#pragma once

#include <cstddef>
#include <cstdint>

// Who a BLE GAP security event belongs to, and whether we should answer it.
//
// GAP events are not delivered to the connection that caused them. They are
// broadcast: esp32_ble_tracker::gap_event_handler() forwards every event to
// every registered client, and ble_client::BLEClient::gap_event_handler()
// forwards it again to every node hanging off that client. A node therefore
// sees the security traffic of every other BLE peer on the device -- a
// bluetooth_proxy connection, a second ble_client, anything.
//
// ESPHome's own BLEClientBase knows this and calls check_addr() before it acts
// on SEC_REQ or AUTH_CMPL. This component did not, on any of its five GAP
// security branches, which had two consequences:
//
//   1. It answered pairing requests aimed at other components' peers. The
//      auto-accept was written for a pump that demands Just Works bonding, and
//      it was silently extended to every device the node talks to.
//
//   2. It mistook other peers' AUTH_CMPL for its own. That one is not merely
//      hardening: an unrelated device failing SMP while our pump's encryption
//      is in flight would latch that stranger's failure reason into the Pump
//      Link Status companion at AUTH rank -- which outranks every other hold,
//      so it masks the real cause -- and disconnect the pump. A stranger's
//      *success* is just as wrong: it publishes pairing-OK for the pump, clears
//      a genuine AUTH hold, and can fire the deferred CCCD write on a link that
//      never authenticated.
//
// The address comparison and the accept/decline decision live here, rather than
// inline in the .cpp, for the reason failure_hold.h and subscribe_outcome.h do:
// ble_connection_manager.cpp is compiled by no host test, so a rule expressed
// there is unverifiable. The rule is the part worth pinning.

namespace esphome {
namespace alpha_hwr {
namespace core {

/// Length of a BLE device address, in bytes. Mirrors esp_bd_addr_t without
/// dragging the ESP-IDF headers into the host tests.
constexpr size_t BD_ADDR_LEN = 6;

/// True when `event_addr` names the peer this component is bound to.
///
/// `peer_addr` is the ble_client's configured remote address, which ESPHome
/// fills in at config time (BLEClientBase::set_address), so it is valid before
/// the link opens as well as during it. An all-zero peer address means no
/// address is configured yet and matches nothing -- answering security traffic
/// while we have no idea who our own peer is would be the exact failure this
/// header exists to prevent.
/// True when @p addr is a real address rather than "none configured".
///
/// ESPHome zeroes the remote address when a client is torn down, and it is zero
/// before codegen assigns one, so all-zero is how "we do not know our own peer"
/// is spelled. Note this is a property of the *whole* address: a pump on a
/// 00:xx:xx OUI is perfectly ordinary and must not be mistaken for unset.
inline bool gap_addr_is_set(const uint8_t *addr) {
  if (addr == nullptr) {
    return false;
  }
  for (size_t i = 0; i < BD_ADDR_LEN; i++) {
    if (addr[i] != 0) {
      return true;
    }
  }
  return false;
}

inline bool gap_addr_matches(const uint8_t *event_addr, const uint8_t *peer_addr) {
  if (event_addr == nullptr || !gap_addr_is_set(peer_addr)) {
    return false;
  }
  for (size_t i = 0; i < BD_ADDR_LEN; i++) {
    if (event_addr[i] != peer_addr[i]) {
      return false;
    }
  }
  return true;
}

/// What to do with a pairing request that has reached one of our handlers.
enum class GapSecurityAction : uint8_t {
  ACCEPT,   ///< Our pump, and pairing is configured: reply yes.
  DECLINE,  ///< Our pump, but enable_pairing is false: do not consent.
  IGNORE,   ///< Some other component's peer: say nothing at all.
};

/// Decide how to answer a pairing request from `addr_is_ours`, given config.
///
/// IGNORE rather than DECLINE for a stranger is deliberate. The peer belongs to
/// some other component, which may well want to pair with it; replying "no" on
/// its behalf would be the same overreach as replying "yes", just in the other
/// direction. Silence leaves the decision where it belongs.
///
/// DECLINE is what `enable_pairing: false` means -- it defaults to false and
/// documents itself as passive telemetry only. init_security() already honours
/// it by never configuring the security parameters; the reply paths did not.
inline GapSecurityAction gap_security_action(bool addr_is_ours, bool pairing_enabled) {
  if (!addr_is_ours) {
    return GapSecurityAction::IGNORE;
  }
  return pairing_enabled ? GapSecurityAction::ACCEPT : GapSecurityAction::DECLINE;
}

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
