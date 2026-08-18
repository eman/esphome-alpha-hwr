#pragma once

#include <cstdint>

// A pump that will not pair, told apart from a pump that has not been asked to
// yet.
//
// BLEConnectionManager::handle_connection_opened() stays silent on an unbonded
// connection, because this pump initiates bonding itself with
// ESP_GAP_BLE_SEC_REQ_EVT and a central-initiated pairing request on an
// unbonded pump comes back 0x52 ("Pairing Not Supported"), losing the pump's
// own request in the process. That is correct, and the comment there says so.
//
// What it does not say is that "unbonded" covers two states that behave
// nothing alike (issue #230):
//
//   A. Neither side is bonded and the pump is in pairing mode. It sends
//      SEC_REQ on connect, the silent wait is answered, and the node bonds --
//      1.4 s after the pump was put into pairing mode, on the bench.
//   B. The pump is bonded to us and we are not bonded to it. Reached by
//      clearing only the client's bond: `ble_client.remove_bond`, an NVS
//      erase, or a re-flash that loses NVS. The pump sees an unencrypted peer
//      it holds a bond for, sends NO SEC_REQ, and terminates the link. The
//      node reconnects, is dropped again, and loops roughly every 5 s
//      indefinitely, logging "waiting for pump to initiate pairing" each time
//      -- which reads as though patience is the answer. It is not: state B is
//      not remotely recoverable, and the only fix is to put the pump into
//      Bluetooth pairing mode by hand, at the pump -- a procedure that needs
//      the Grundfos GO app and needs this node stopped first, because the pump
//      holds one BLE connection at a time. docs/configuration.md carries it.
//
// This detector exists to turn that silent forever-loop into a diagnosis. It
// does NOT change what the connect path does -- nothing here initiates
// pairing, because initiating is the thing that returns 0x52; it decides when
// the loop has gone on long enough to be worth naming.
//
// What it can and cannot tell apart. The observable is an absence -- no
// SEC_REQ -- so state B is indistinguishable from a never-bonded pump that
// simply has not been put into pairing mode, and this deliberately does not
// claim to know which. It does not need to: the remedy is the same physical
// action in both, which is what the report says. Claiming "bonded to another
// client" as fact would be an inference the evidence does not carry.
//
// The predicate is four terms, and the last two are the ones that keep this
// quiet on links whose trouble is something else:
//
//   - the connection opened with no stored bond, and
//   - no security exchange happened on it (no SEC_REQ from the pump, no
//     successful pairing), and
//   - no notification arrived on it either, and
//   - the pump ended it, not us.
//
// The data term matters because unbonded operation is a supported mode:
// `enable_pairing` defaults to false and passive telemetry needs no bond, so a
// perfectly healthy installation can run unbonded forever. Such a link
// subscribes and carries data; a stalled one is dropped by the pump within
// about 2 s, before anything flows. Without the data term, a healthy unbonded
// node would accumulate a "stall" cycle on every ordinary reconnect and
// eventually report a fault that is not there.
//
// The fourth term is about a different wrong answer, and it has two halves
// because there are two ways a link can end without the pump having decided
// anything.
//
// The first is our own teardown. An unbonded link that subscribes and then
// hears nothing is torn down by the inbound-data watchdog every 60 s, and those
// teardowns look identical from here to a pump refusing the link -- unbonded,
// no security, no data. Counting them would report a pairing refusal for a pump
// that is merely deaf, about three minutes in.
//
// The second is the radio. A link lost to range, interference or a supervision
// timeout also ends with no bond, no security and no data, and the window in
// which that is indistinguishable from a refusal is wide: a stalled pump drops
// at about 2.1 s, and a healthy link does not carry data until roughly 2.5-3.5 s
// (post-connect delay, discovery, the CCCD write, the stabilize window, then the
// first read and its reply). The three-cycle threshold does not separate them,
// because every cause of early drops repeats. The first version of this header
// stated this term and did not implement it, and a skeptic pass drove three
// unbonded opens dropped with ESP_GATT_CONN_TIMEOUT straight to "Pump not
// accepting pairing" -- replacing a correct radio diagnostic with a pairing
// misdiagnosis, which is the exact inversion of the point.
//
// So the disconnect reason is consulted, and the reasons that mean the link was
// lost or that we ended it do not count. Deliberately as an exclusion list
// rather than an allow-list of "the pump terminated": the reference log for
// issue #230 shows 0x13 (Remote User Terminated), but that is one specimen's
// firmware, and admitting only 0x13 would silently stop detecting the state on
// a pump that ends the link some other way. Failing to detect it is the failure
// this whole header exists to prevent, so unknown reasons keep counting.
//
// A connection that never opened is not a cycle. ESP_GATTC_OPEN_EVT fires for
// failed opens too, and the caller already declines to run its
// connection-opened handler on those -- a pump that is powered down or out of
// range produces a stream of them, and counting those as refusals to pair
// would report a pairing problem for a pump that is not there.
//
// In its own header so the rule can be exercised directly and exhaustively --
// every ordering, the saturation behaviour, hundreds of cycles -- which is not
// practical through the component. That is the honest reason, and it is worth
// being precise about because the neighbouring headers give a different one:
// failure_hold.h, subscribe_outcome.h and gap_security_policy.h all say
// ble_connection_manager.cpp "is compiled by no host test". That stopped being
// true -- tests/Makefile puts it in COMPONENT_SRCS, and test_component_wiring
// and test_api_bridge both link it. Repeating the claim here would have been an
// excuse for leaving the wiring untested, so the wiring is tested too, in
// tests/test_component_wiring.cpp: the extraction is for depth, not for reach.

namespace esphome {
namespace alpha_hwr {
namespace core {

/// Consecutive connect-and-drop cycles with no bond, no security exchange and
/// no data before the stall is reported.
///
/// Three, against a loop that repeats about every 5 s, so roughly 15 s. One
/// cycle is not evidence of anything -- a single dropped link has a dozen
/// ordinary causes -- and the loop is unbounded, so nothing is lost by waiting
/// for a pattern. There is no upper limit to be short of: whatever the
/// threshold, a stalled pump reaches it.
inline constexpr uint8_t PAIRING_STALL_CYCLES = 3;

/// How many further cycles pass between repeats of the report.
///
/// The report is a WARN line and a latched fault string. The fault string
/// persists on its own, but the log line is what someone attaching a log
/// viewer ten minutes into the loop will look for, and logging it on every
/// cycle would put twelve identical warnings a minute in front of them
/// forever. Twelve cycles is about a minute at the observed cadence: often
/// enough to be found by anyone watching, rare enough not to bury the rest of
/// the log.
inline constexpr uint8_t PAIRING_STALL_REMINDER_CYCLES = 12;

/// Does this disconnect reason mean the link was lost, or ended by us, rather
/// than refused by the pump?
///
/// Taken as raw values rather than esp_gatt_conn_reason_t so this header stays
/// free of ESP-IDF and can be compiled and tested on its own, the way
/// response_match.h is. ble_connection_manager.cpp static_asserts each constant
/// against the real enumerator, so the numbers cannot drift unnoticed.
///
/// ESP_GATT_CONN_TERMINATE_PEER_USER (0x13) is deliberately absent: it is the
/// pump deciding, which is the thing being counted. So is
/// ESP_GATT_CONN_UNKNOWN (0x00) and anything else unlisted -- see the note
/// above on why an exclusion list rather than an allow-list.
inline bool disconnect_reason_is_link_loss(uint16_t reason) {
  switch (reason) {
    case 0x0001:  // ESP_GATT_CONN_L2C_FAILURE
    case 0x0008:  // ESP_GATT_CONN_TIMEOUT -- link supervision timeout
    case 0x0016:  // ESP_GATT_CONN_TERMINATE_LOCAL_HOST -- definitionally ours,
                  // and this also covers a teardown that did not come through
                  // force_disconnect(), such as a stock ble_client.disconnect
                  // action wired up in a user's YAML
    case 0x0022:  // ESP_GATT_CONN_LMP_TIMEOUT
    case 0x003E:  // ESP_GATT_CONN_FAIL_ESTABLISH
    case 0x0100:  // ESP_GATT_CONN_CONN_CANCEL
      return true;
    default:
      return false;
  }
}

/// Counts connections that opened unbonded and were dropped without the pump
/// ever offering to pair.
class PairingStallDetector {
 public:
  /// A GATT connection opened. @p bonded_at_open is the stored-bond state
  /// captured at that moment; a bonded open is not a candidate cycle at all.
  void on_connection_opened(bool bonded_at_open) {
    saw_open_ = true;
    // Reseats progress_ rather than OR-ing into it, which is safe only because
    // one link produces one open: ESPHome's ble_client will not issue a second
    // connect while the first is CONNECTING or CONNECTED, so there is no live
    // link whose recorded SEC_REQ this could erase.
    progress_ = false;
    if (bonded_at_open) note_progress_();
  }

  /// The pump asked us to secure the link (ESP_GAP_BLE_SEC_REQ_EVT).
  ///
  /// This is the signal being waited for, and it counts however the request is
  /// answered: what it proves is that the pump is willing to pair, which is
  /// exactly what a stalled pump never says. Whether this node consents is a
  /// separate question that `enable_pairing` decides.
  void note_security_request() { note_progress_(); }

  /// Pairing/bonding completed successfully (ESP_GAP_BLE_AUTH_CMPL_EVT).
  void note_bond_established() { note_progress_(); }

  /// A notification arrived, so this link is carrying data.
  void note_data() { note_progress_(); }

  /// This node tore the link down itself.
  ///
  /// An exclusion rather than progress, and it shares the flag because the
  /// effect wanted is the same one: do not count this cycle. The stall's
  /// signature is the *pump* dropping an unbonded link within a couple of
  /// seconds; what this covers is the inbound-data watchdog recycling a link
  /// that subscribed and went quiet, which is a different fault with its own
  /// diagnosis and must not be overwritten by this one.
  void note_local_teardown() { progress_ = true; }

  /// The link dropped. Closes the cycle.
  ///
  /// @return true when the caller should report the stall now -- once as it is
  ///         first established, then once every PAIRING_STALL_REMINDER_CYCLES.
  bool on_disconnected(uint16_t reason) {
    if (!saw_open_) return false;  // a failed open is not a cycle
    saw_open_ = false;
    if (progress_ || disconnect_reason_is_link_loss(reason)) {
      // progress_ is not cleared here, deliberately: on_connection_opened()
      // reseats it for every connection, so clearing it a second time would be
      // code no test could distinguish from its absence.
      //
      // The one reset of since_report_ outside the reminder branch -- see
      // note_progress_(). It is what gives a relapse a full window rather than
      // resuming the previous episode's, and every ending reaches it.
      //
      // A link-loss reason resets the count as well as declining to add to it.
      // The claim being made is about the pump's behaviour, and a link the
      // radio dropped is not evidence for it in either direction -- so the
      // three cycles the report rests on have to be three the pump actually
      // ended, not three assorted failures that happened to line up.
      consecutive_ = 0;
      since_report_ = 0;
      return false;
    }
    if (consecutive_ < UINT8_MAX) consecutive_++;
    if (consecutive_ < PAIRING_STALL_CYCLES) return false;
    if (consecutive_ == PAIRING_STALL_CYCLES) {
      // No since_report_ reset here, and the sweep is how that was settled: an
      // entry pinning one SURVIVED, which for a line whose removal changes
      // nothing is the correct answer rather than a coverage hole. The
      // invariant is that since_report_ is zero whenever consecutive_ is below
      // the threshold -- every site that zeroes one zeroes the other, and the
      // increment path below the threshold touches neither -- so on arrival
      // here it is already zero. The reset in on_disconnected() is what
      // actually guarantees a relapse gets a full window -- it is the one site
      // every ending passes through -- and that is where the entry points.
      return true;
    }
    // Counting cycles since the last report rather than testing consecutive_
    // modulo the reminder: consecutive_ saturates, and a modulo on a saturated
    // counter either fires on every cycle forever or on none of them, decided
    // by whether the ceiling happens to land on a multiple.
    //
    // No saturation guard on this one, unlike consecutive_ above, because it
    // cannot need one: it is reset every PAIRING_STALL_REMINDER_CYCLES and so
    // never approaches the ceiling. A guard here would be an equivalent mutant
    // -- code no test could distinguish from its absence -- and the pair reading
    // as symmetric when it is not was worth removing rather than explaining.
    since_report_++;
    if (since_report_ >= PAIRING_STALL_REMINDER_CYCLES) {
      since_report_ = 0;
      return true;
    }
    return false;
  }

  /// True once the stall is established, and for as long as it lasts.
  ///
  /// This gates the latched fault string, so it stays true between the
  /// throttled log reports rather than following them -- and, just as
  /// importantly, it stops being true the moment any clearing signal arrives,
  /// so the caller can drop that string instead of leaving it on the surface
  /// until something else overwrites it. For a held reason, "something else"
  /// may be never: a pump put into pairing mode sends SEC_REQ, which is the
  /// event that refutes the whole diagnosis, and before the clearing signals
  /// zeroed the count immediately the fault sensor went on reading "Pump not
  /// accepting pairing" about a pump that had just offered to pair.
  bool stalled() const { return consecutive_ >= PAIRING_STALL_CYCLES; }

  /// Consecutive cycles counted so far, for the report.
  uint8_t consecutive_cycles() const { return consecutive_; }

 private:
  /// Something happened on this connection that takes it out of the count, and
  /// ends any stall already established. Immediate rather than deferred to the
  /// disconnect: the caller releases its latched fault on stalled() going
  /// false, and a pump that has just offered to pair must stop being described
  /// as one that will not.
  ///
  /// Does not touch since_report_, deliberately. There is exactly one reset of
  /// that counter outside the reminder branch, in on_disconnected(), and it is
  /// there because that is the only site every ending passes through: this one
  /// misses a link the radio took away, while the disconnect sees both -- and
  /// setting progress_ here guarantees the disconnect takes that branch.
  /// Three redundant resets is what this had at first, one per site, and the
  /// sweep found them the only way redundancy can be found: each was mutated
  /// away in turn and the suite stayed green each time, because the other two
  /// covered it. Only the survivor of that process is load-bearing, and it is
  /// the only one a mutation entry can pin.
  void note_progress_() {
    progress_ = true;
    consecutive_ = 0;
  }

  bool saw_open_{false};
  bool progress_{false};
  uint8_t consecutive_{0};
  uint8_t since_report_{0};
};

}  // namespace core
}  // namespace alpha_hwr
}  // namespace esphome
