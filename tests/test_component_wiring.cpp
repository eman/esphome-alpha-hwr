// Host tests for the component's BLE lifecycle wiring (alpha_hwr.cpp +
// ble_connection_manager.cpp).
//
// Why this file exists: `tests/test_session.cpp` pins the session FSM, but the
// FSM is only as good as what drives it, and what drives it lives in the two
// largest files in the repo -- 1239 and 851 lines that `esphome compile` was
// the only thing to ever build. Between them they own every GATT and GAP event
// the pump can produce. Nothing host-tested any of it (issue #174 audit tail).
//
// The tests drive the real public entry points -- gattc_event_handler(),
// gap_event_handler(), parse_device(), setup(), loop() -- against the ESP-IDF
// and ESPHome mocks in tests/mocks/. No behaviour is simulated on the
// component's behalf: a GATT event goes in at the same door the BLE stack uses,
// and what comes out is observed through `Pump Ready`, which is what a user
// sees, rather than through private state.
//
// The mocks are deliberately thin. Where the real stack would do something
// asynchronous, the mock records the call and does nothing, because a mock that
// invents the asynchronous half is a mock that can hide the bug it was written
// to catch.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "../components/alpha_hwr/alpha_hwr.h"
#include "fixture_crc.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/application.h"

uint32_t mock_millis = 0;

namespace esphome {
Application App;
}  // namespace esphome

using esphome::alpha_hwr::AlphaHwrComponent;
using esphome::ble_client::BLECharacteristic;
using esphome::ble_client::BLEClient;
using esphome::ble_client::BLEService;
using esphome::esp32_ble_tracker::ESPBTDevice;
using esphome::esp32_ble_tracker::ESPBTUUID;

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

// The pump this component is built for, as the scan filter identifies it.
static constexpr uint16_t GRUNDFOS_COMPANY_ID = 0xFEFF;
static constexpr uint8_t FAMILY_ALPHA = 0x34;
static constexpr uint8_t TYPE_HWR = 0x07;

/// A component wired to a mock BLE client, with the one sensor these tests
/// observe attached.
struct Rig {
  BLEClient client;
  esphome::binary_sensor::BinarySensor ready;
  esphome::text_sensor::TextSensor link_status;
  // The Pump Link Fault companion. Attached by default, because the fault
  // surface is where the fault-hold rank is actually observable: failure_hold.h
  // decides which cause wins, and until this was wired the whole decision was
  // exercised only through a pure-header test that could not see whether the
  // component asks it the right question (issue #230).
  esphome::text_sensor::TextSensor link_fault;
  // The gap histogram (issue #176 part 1). Attached by default so every test
  // here exercises the publish path, and because the frame-budget assertions
  // below are only meaningful against a rig that has them on.
  esphome::sensor::Sensor gaps_over[6];
  esphome::sensor::Sensor recycles;
  esphome::sensor::Sensor gaps_truncated;
  esphome::sensor::Sensor watch_time;
  // Pump Clock Drift. Attached by default because the only way to see that a
  // disconnect publishes to it is to have it wired (issue #259).
  esphome::sensor::Sensor clock_drift;
  // The node's own wall clock. NOT attached in the constructor: an unattached
  // time_id is the sentinel case for every clock caller (issue #270), and it is
  // the state every test in this file ran in before the clock had a fixture at
  // all. A test that wants one calls attach_node_clock().
  esphome::time::RealTimeClock node_clock;
  // The limiter entities (issue #274). NOT attached by default: the component
  // only reads the limiter family when one of them is configured, and that
  // restraint is itself pinned below.
  esphome::text_sensor::TextSensor flow_limiter;
  esphome::binary_sensor::BinarySensor flow_limited;
  AlphaHwrComponent component{&client};

  BLEService service;
  BLECharacteristic characteristic;

  Rig() {
    mock_millis = 1000;
    esp_gattc_mock().reset();
    esp_gap_mock().reset();
    component.set_ready_binary_sensor(&ready);
    component.set_pump_link_status_text_sensor(&link_status);
    component.set_pump_last_link_failure_text_sensor(&link_fault);
    component.set_link_gaps_over_15s_sensor(&gaps_over[0]);
    component.set_link_gaps_over_20s_sensor(&gaps_over[1]);
    component.set_link_gaps_over_30s_sensor(&gaps_over[2]);
    component.set_link_gaps_over_45s_sensor(&gaps_over[3]);
    component.set_link_gaps_over_60s_sensor(&gaps_over[4]);
    component.set_link_gaps_over_90s_sensor(&gaps_over[5]);
    // The shipped defaults: naming on at 300s, recycling off. A test that
    // wants the link torn down asks for it, exactly as a user must.
    component.set_ready_timeout(300000);
    component.set_ready_recycle(false);
    component.set_link_recycles_sensor(&recycles);
    component.set_link_gaps_truncated_sensor(&gaps_truncated);
    component.set_link_watch_time_sensor(&watch_time);
    component.set_clock_diff_sensor(&clock_drift);
  }

  void setup() { component.setup(); }

  /// Run until `Pump Ready` comes on, or give up. Returns whether it did.
  ///
  /// A fixed window was wrong twice over: it passed by a single 10 s poll tick
  /// (ready lands at 50 s, the window was 60 s), it failed outright at 40 s or
  /// at coarser step granularity, and it silently depended on ~30 unanswered
  /// requests burning 3 s transport timeouts -- so raising that timeout broke
  /// it. Driving to the condition states the intent and survives the chain
  /// getting slower, which is the only thing a caller cares about.
  bool run_until_ready(uint32_t limit_ms = 300000) {
    const uint32_t deadline = mock_millis + limit_ms;
    while (mock_millis < deadline) {
      advance(1000, 20);
      if (ready_is_on()) return true;
    }
    return false;
  }

  /// Advance time and let the component run, the way ESPHome would: loop(),
  /// then any timer that has come due, then deliver whatever the pump owes.
  uint32_t last_update_ms{0};

  void advance(uint32_t ms, int steps = 20) {
    for (int i = 0; i < steps; i++) {
      mock_millis += ms / steps;
      component.loop();
      component.mock_run_due_timeouts();
      // PollingComponent's interval is not driven by the mock -- ESPHome calls
      // update() itself on a schedule, and Pump Ready is published from there,
      // so the rig has to supply the same cadence or the chain completes and
      // nothing ever reports it.
      if (mock_millis - last_update_ms >= component.get_update_interval()) {
        last_update_ms = mock_millis;
        component.update();
      }
      answer_outstanding_writes();
    }
  }

  /// Bring the GATT link up as far as the pump being subscribed: open, service
  /// discovered, notifications registered. Each step is a real event through
  /// the real handler.
  void connect_and_subscribe() {
    prepare_gatt_db();
    open(ESP_GATT_OK);
    finish_discovery_and_subscribe();
  }

  void prepare_gatt_db() {
    service.uuid = esphome::alpha_hwr::GRUNDFOS_SERVICE_UUID;
    characteristic.uuid = esphome::alpha_hwr::GENI_CHAR_UUID;
    characteristic.handle = 42;
    service.characteristics.push_back(&characteristic);
    client.mock_set_service(&service);
    client.mock_set_characteristic(&characteristic);
  }

  void finish_discovery_and_subscribe() {
    esp_ble_gattc_cb_param_t cmpl{};
    cmpl.search_cmpl.status = ESP_GATT_OK;
    cmpl.search_cmpl.conn_id = 1;
    component.gattc_event_handler(ESP_GATTC_SEARCH_CMPL_EVT, 1, &cmpl);

    esp_ble_gattc_cb_param_t reg{};
    reg.reg_for_notify.status = ESP_GATT_OK;
    reg.reg_for_notify.handle = 42;
    component.gattc_event_handler(ESP_GATTC_REG_FOR_NOTIFY_EVT, 1, &reg);
  }

  /// Feed a frame back as a GATT notification, the way the pump's replies
  /// arrive. Goes in through the real handler, so reassembly and CRC checking
  /// are production code.
  void notify(std::vector<uint8_t> frame) {
    esp_ble_gattc_cb_param_t p{};
    p.notify.conn_id = 1;
    p.notify.handle = 42;
    p.notify.is_notify = true;
    p.notify.value = frame.data();
    p.notify.value_len = static_cast<uint16_t>(frame.size());
    component.gattc_event_handler(ESP_GATTC_NOTIFY_EVT, 1, &p);
  }

  size_t answered_writes{0};

  /// Withhold the schedule-overview reply, leaving one of the two caches that
  /// Pump Ready waits on unpopulated.
  bool answer_overview{true};

  /// Answer the pump-clock read (Obj 94 Sub 101). Off by default because the
  /// drift leg's behaviour when the read goes UNANSWERED is itself pinned below
  /// (issue #259), and because every test written before this fixture existed
  /// ran against a pump that never answered it.
  bool answer_clock{false};

  /// Give the node a synced wall clock reading @p epoch.
  void attach_node_clock(time_t epoch) {
    node_clock.set_epoch_for_test(epoch);
    component.set_time_id(&node_clock);
  }

  /// The limiter family (issue #274). Answered by default: the reads are only
  /// issued when a limiter entity is attached, and no test that does not attach
  /// one will ever ask for them.
  bool answer_limiters{true};
  bool limiter_max_flow_enabled{true};
  bool limiter_limiting{false};
  int limiter_reads{0};

  /// Answer every limiter sub-id except this one (0 = answer all). Models the
  /// pump that goes quiet part-way through a chain, which is what the
  /// stop-at-first-failure rule exists for.
  uint16_t fail_limiter_sub{0};

  /// Which limiter sub-ids were actually requested, in order. The chain's
  /// stopping is not visible in the entity -- a half-read family reads
  /// "unknown" either way -- so what it asks for next is the observable.
  std::vector<uint16_t> limiter_subs_seen;

  bool limiter_sub_was_requested(uint16_t sub) const {
    for (uint16_t s : limiter_subs_seen)
      if (s == sub) return true;
    return false;
  }

  /// Attach both limiter entities, which is what makes the reads happen at all.
  void attach_limiter_entities() {
    component.set_flow_limiter_text_sensor(&flow_limiter);
    component.set_flow_limiter_active_binary_sensor(&flow_limited);
  }

  /// Go completely deaf: answer nothing at all, without consuming the backlog,
  /// so that setting it back to true delivers everything the pump owed. This is
  /// how a quiet interval is produced against the real notification path rather
  /// than by poking the sampler directly.
  bool answer_writes{true};

  /// Answer anything the component has written that we know a reply for.
  /// Deliberately answers only the frames a test has taught it about: an
  /// unrecognised request goes unanswered, which is what a real pump that does
  /// not implement something would do.
  void answer_outstanding_writes();

  void open(esp_gatt_status_t status) {
    esp_ble_gattc_cb_param_t p{};
    p.open.status = status;
    p.open.conn_id = 1;
    component.gattc_event_handler(ESP_GATTC_OPEN_EVT, 1, &p);
  }

  void disconnect(esp_gatt_conn_reason_t reason) {
    esp_ble_gattc_cb_param_t p{};
    p.disconnect.reason = reason;
    p.disconnect.conn_id = 1;
    component.gattc_event_handler(ESP_GATTC_DISCONNECT_EVT, 1, &p);
  }

  bool ready_is_on() const { return ready.state; }
};

/// The pump's answer to each request the initial read chain sends. Frames carry
/// real CRCs, stamped with the production routine via with_crc(), so the
/// transport's own checking is exercised rather than bypassed.
///
/// Shapes are the ones captured from hardware and recorded in transport.cpp;
/// only the CRC is recomputed here.
///
/// There are no Class 2, Class 5 or Class 11 branches, and their absence is
/// load-bearing rather than an oversight: those three classes were unique to
/// the opening sequence removed in issue #174, so a rig that still answered
/// them would look like it expected requests nothing sends any more.
/// Wrap a DataObject body in the response envelope the transport expects:
/// type identifiers at bytes 8-9, a 3-byte size header, then the body.
/// @param type_high Bytes 6-7 of the reply, the type's high half. Zero for the
///   objects that carry a type below 256; the limiter family is type 895/896,
///   so its replies really do read `00 03 7F 01` and a frame hardcoding `00 00`
///   here would never match the read that asked for it.
static std::vector<uint8_t> data_object_frame(uint8_t type_hi, uint8_t type_lo,
                                              const uint8_t *body, size_t body_len,
                                              uint16_t type_high = 0x0000,
                                              int opspec = -1) {
  std::vector<uint8_t> f = {0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x13,
                            static_cast<uint8_t>(type_high >> 8),
                            static_cast<uint8_t>(type_high & 0xFF),
                            type_hi, type_lo, 0x00, 0x00,
                            static_cast<uint8_t>(body_len)};
  f.insert(f.end(), body, body + body_len);
  f.push_back(0x00);
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
  // Byte 5 is the APDU body length in the response direction, and on every one
  // of the 24,233 CRC-valid captured inbound frames it equals total_len - 8.
  // Left at the historical 0x13 unless a caller asks, so the fixtures that
  // predate this keep the bytes they were verified with.
  if (opspec >= 0) f[5] = static_cast<uint8_t>(opspec);
  return with_crc(std::move(f));
}

void Rig::answer_outstanding_writes() {
  if (!answer_writes) return;
  auto &writes = esp_gattc_mock().writes;
  while (answered_writes < writes.size()) {
    const std::vector<uint8_t> &req = writes[answered_writes++];
    if (req.size() < 6) continue;
    const uint8_t cls = req[4];
    const uint8_t opspec = req[5];

    if (cls == 0x0A && opspec == 0x03 && req.size() >= 9) {
      const uint8_t obj = req[6];
      const uint16_t sub = static_cast<uint16_t>((req[7] << 8) | req[8]);
      if (obj == 0x56 && sub >= 600 && sub <= 660) {
        // The limiter family (issue #274). Every byte below is from a real
        // pump: @jfriend00's unit with MaxFlow enabled, driven into the
        // limiting state. Type 895 v1 (config, 18 bytes) and 896 v1 (status,
        // 6 bytes), whose replies carry the type high half at bytes 6-7.
        limiter_reads++;
        limiter_subs_seen.push_back(sub);
        if (sub == fail_limiter_sub) {
          // deliberately unanswered: this is the read that times out
        } else if (!answer_limiters) {
          // fall through: nothing answers, which is a pump without the family
        } else if (sub == 600) {
          const uint8_t enable_byte = limiter_max_flow_enabled ? 0x01 : 0x00;
          const uint8_t cfg[18] = {0x01, enable_byte,
                                   0x38, 0xD3, 0xB2, 0x11,   // 1.6 gpm
                                   0x3F, 0x19, 0x99, 0x9A, 0x3F, 0xCC, 0xCC, 0xCD,
                                   0x3E, 0xCC, 0xCC, 0xCD};
          notify(data_object_frame(0x7F, 0x01, cfg, sizeof(cfg), 0x0003, 0x19));
        } else if (sub == 601) {
          const uint8_t cfg[18] = {0x02, 0x00, 0x39, 0x25, 0x63, 0x1D,  // 2.5 gpm, off
                                   0x3F, 0x19, 0x99, 0x9A, 0x3F, 0xCC, 0xCC, 0xCD,
                                   0x3E, 0xCC, 0xCC, 0xCD};
          notify(data_object_frame(0x7F, 0x01, cfg, sizeof(cfg), 0x0003, 0x19));
        } else if (sub == 640) {
          // Either "running under the cap, ref 3671" or "limiting at 1883.1".
          const uint8_t idle[6] = {0x01, 0x00, 0x45, 0x65, 0x70, 0x00};
          const uint8_t busy[6] = {0x01, 0x01, 0x44, 0xEB, 0x63, 0x4A};
          notify(data_object_frame(0x80, 0x01, limiter_limiting ? busy : idle, 6, 0x0003, 0x0D));
        } else if (sub == 641) {
          const uint8_t idle[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
          notify(data_object_frame(0x80, 0x01, idle, 6, 0x0003, 0x0D));
        } else if (sub == 660) {
          const uint8_t idle[6] = {0x00, 0x00, 0x44, 0xCE, 0x40, 0x00};
          const uint8_t busy[6] = {0x01, 0x01, 0x44, 0xEB, 0x27, 0xD6};
          notify(data_object_frame(0x80, 0x01, limiter_limiting ? busy : idle, 6, 0x0003, 0x0D));
        }
      } else if (obj == 0x56) {
        // Operation status (Obj 86) -- the frame captured on hardware, and what
        // populates the control-mode cache.
        notify(with_crc({0x24, 0x12, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01,
                         0x00, 0x00, 0x07, 0x00, 0x01, 0x02, 0x44, 0xCE, 0x40, 0x00,
                         0x00, 0x00}));
      } else if (obj == 91 && sub == 430) {
        // Temperature-range config. Answered with OpSpec 0x15, which is the
        // size-specific reply this read is matched on (issue #106). Supplies
        // autoadapt and the two setpoints, the last three fields
        // ControlService::is_cache_valid() waits for.
        notify(with_crc({0x24, 0x16, 0xF8, 0xE7, 0x0A, 0x15, 0x00, 0x03, 0xF4, 0x02,
                         0x00, 0x00, 0x0E, 0x01,
                         0x42, 0x0C, 0x00, 0x00,   // 35.0
                         0x42, 0x1B, 0x99, 0x9A,   // 38.9
                         0x00, 0x00, 0x00, 0x00}));
      } else if (obj == 94 && sub == 101 && answer_clock) {
        // DateTimeActual (Obj 94 Sub 101), type 322 v1. The body is the bench
        // capture recorded in time_service.cpp -- 2026-08-15 20:38:55, with the
        // five trailing bytes nothing reads -- so the parser under test sees
        // the bytes the pump really sends rather than a tidied version.
        const uint8_t clock[12] = {0x07, 0xEA, 0x08, 0x0F, 0x14, 0x26,
                                   0x37, 0x48, 0x00, 0x06, 0x00, 0x01};
        notify(data_object_frame(0x01, 0x42, clock, sizeof(clock)));
      } else if (obj == 84 && sub == 1 && answer_overview) {
        // ClockProgramOverview (Obj 84 Sub 1) -- the other cache Pump Ready
        // waits on. Built with the same envelope as the write-operation
        // suite's fixture rather than hand-rolled: my first attempt was a byte
        // short, which fails the payload_len >= 13 guard silently.
        const uint8_t overview[10] = {0x8C, 5, 0x05, 0x05, 0x01,
                                      0x01, 0x00, 0x00, 0x00, 0x00};
        notify(data_object_frame(0xDA, 0x01, overview, sizeof(overview)));
      }
    }
  }
}

/// An advertisement shaped like a real ALPHA HWR.
///
/// The layout matters and is easy to get wrong: three header bytes, then
/// family at index 3, type at 4, version at 5. My first version of this
/// fixture put family at index 2, which made the *rejection* assertions below
/// pass for the wrong reason -- every one of them was comparing the wrong
/// bytes and mismatching by accident. The acceptance case is what caught it.
static ESPBTDevice make_advertisement(uint8_t family, uint8_t type) {
  ESPBTDevice d;
  d.set_address(0x001E2A003C4Dull);
  d.add_service_data(ESPBTUUID::from_uint16(GRUNDFOS_COMPANY_ID),
                     {0x00, 0x00, 0x00, family, type, 0x02});
  return d;
}

// ── The scan filter ─────────────────────────────────────────────────────────
// is_alpha_hwr_device() is what decides which advertisement is worth
// connecting to, and it is the first place a wrong product is rejected. It is
// static and public, so it can be asked directly.
void test_the_scan_filter_identifies_the_pump() {
  std::cout << "\n=== The scan filter identifies an ALPHA HWR ===" << std::endl;

  using Manager = esphome::alpha_hwr::core::BLEConnectionManager;
  const ESPBTUUID service = ESPBTUUID::from_uint16(GRUNDFOS_COMPANY_ID);

  ESPBTDevice ours = make_advertisement(FAMILY_ALPHA, TYPE_HWR);
  TEST_ASSERT(Manager::is_alpha_hwr_device(ours, GRUNDFOS_COMPANY_ID, FAMILY_ALPHA, TYPE_HWR, service),
              "An ALPHA HWR advertisement is accepted");

  ESPBTDevice wrong_family = make_advertisement(0x35, TYPE_HWR);
  TEST_ASSERT(!Manager::is_alpha_hwr_device(wrong_family, GRUNDFOS_COMPANY_ID, FAMILY_ALPHA, TYPE_HWR, service),
              "A different product family is rejected");

  ESPBTDevice wrong_type = make_advertisement(FAMILY_ALPHA, 0x08);
  TEST_ASSERT(!Manager::is_alpha_hwr_device(wrong_type, GRUNDFOS_COMPANY_ID, FAMILY_ALPHA, TYPE_HWR, service),
              "The right family with the wrong type is rejected");

  ESPBTDevice bare;
  TEST_ASSERT(!Manager::is_alpha_hwr_device(bare, GRUNDFOS_COMPANY_ID, FAMILY_ALPHA, TYPE_HWR, service),
              "An advertisement with no service data at all is rejected");
}

// ── A failed open must not start the connection sequence ────────────────────
// ESP_GATTC_OPEN_EVT arrives for failures too, carrying a non-OK status. The
// handler filters on it; if it did not, a failed open would drive the session
// forward on a link that was never established.
void test_a_failed_open_does_not_by_itself_stop_the_sequence() {
  std::cout << "\n=== What a failed GATT open does, and does not, prevent ==="
            << std::endl;

  // Asserted by outcome rather than by counting calls to
  // esp_ble_gattc_search_service(). That counter looked like the right
  // observable and was not: the component never calls it on the happy path --
  // ESPHome's base layer drives discovery -- so it reads 0 after a *successful*
  // connect too, and the assertion held in every scenario including one with
  // the status filter deleted.
  Rig r;
  r.setup();
  r.prepare_gatt_db();

  r.open(static_cast<esp_gatt_status_t>(0x85));  // any non-OK status
  r.finish_discovery_and_subscribe();

  // And it DOES become ready. That is the finding, not a bug in the test.
  //
  // The status filter gates only the connection callback, i.e. whether the
  // session is told the link opened. Everything after it -- discovery,
  // subscribe, authenticate -- is driven by later events, and the session's
  // guards warn about an unexpected order and then transition anyway (pinned
  // in tests/test_session.cpp). So a failed open followed by the rest of the
  // sequence still reaches READY.
  //
  // Nothing on real hardware produces that order: the stack does not emit
  // SEARCH_CMPL for a link that never opened. This asserts the permissiveness
  // rather than a defence, so that making the guards strict -- which would be a
  // real behaviour change -- fails here and gets noticed.
  TEST_ASSERT(r.run_until_ready(60000),
              "A failed open does not by itself stop the rest of the sequence: "
              "the session's guards warn on the unexpected order and proceed");
}

// ── Pump Ready reflects the link, not the wish ──────────────────────────────
void test_pump_ready_is_off_until_the_chain_completes() {
  std::cout << "\n=== Pump Ready is off until the chain completes ===" << std::endl;

  Rig r;
  r.setup();
  TEST_ASSERT(!r.ready_is_on(), "Off after setup(), before any connection");

  r.open(ESP_GATT_OK);
  TEST_ASSERT(!r.ready_is_on(), "Still off on a bare GATT open -- the pump has "
                                "not been discovered, subscribed or handshaken yet");

  r.advance(3000);
  TEST_ASSERT(!r.ready_is_on(),
              "And still off after time passes with no service discovered -- "
              "readiness is not a timer");
}

// ── GAP events are filtered by address ──────────────────────────────────────
// GAP events are broadcast to every registered client, not routed to the one
// they concern (issue #201). A stranger's security request must not be
// answered on the pump's behalf. This drives the real handler rather than the
// pure predicate that tests/test_gap_security_policy.cpp already covers, so it
// is the wiring being asserted, not the decision.
void test_gap_events_from_a_stranger_are_ignored() {
  std::cout << "\n=== GAP events from another device are ignored ===" << std::endl;

  Rig r;
  // Pairing is off by default, and with it off the component declines even the
  // pump's own request (deliberately -- see the DECLINE branch). Turn it on, so
  // that what this test measures is the address filter and not the enable flag.
  r.component.set_pairing_enabled(true);
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  const uint8_t stranger[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  r.client.mock_set_remote_bda(pump);
  r.open(ESP_GATT_OK);

  esp_gap_mock().reset();
  esp_ble_gap_cb_param_t p{};
  std::memcpy(p.ble_security.ble_req.bd_addr, stranger, 6);
  r.component.gap_event_handler(ESP_GAP_BLE_SEC_REQ_EVT, &p);

  TEST_ASSERT(esp_gap_mock().security_rsp == 0,
              "A security request from another device is not answered at all");

  esp_ble_gap_cb_param_t ours{};
  std::memcpy(ours.ble_security.ble_req.bd_addr, pump, 6);
  r.component.gap_event_handler(ESP_GAP_BLE_SEC_REQ_EVT, &ours);

  TEST_ASSERT(esp_gap_mock().security_rsp == 1,
              "...while one from the connected pump is");
}

// ── With pairing disabled, even the pump is declined ────────────────────────
// This is the default, and it surprised me while writing the test above: with
// enable_pairing off, the component does not answer the *pump's* security
// request either. That is deliberate and documented at the DECLINE branch --
// and worth pinning, because "we only ignore strangers" is the natural
// assumption and it is wrong.
void test_pairing_disabled_declines_even_the_pump() {
  std::cout << "\n=== With pairing disabled, even the pump is declined ==="
            << std::endl;

  Rig r;  // pairing_enabled defaults to false
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  r.client.mock_set_remote_bda(pump);
  r.open(ESP_GATT_OK);

  esp_gap_mock().reset();
  esp_ble_gap_cb_param_t p{};
  std::memcpy(p.ble_security.ble_req.bd_addr, pump, 6);
  r.component.gap_event_handler(ESP_GAP_BLE_SEC_REQ_EVT, &p);

  TEST_ASSERT(esp_gap_mock().security_rsp == 0,
              "The pump's own security request is not answered while pairing is off");
}

// ── A stranger's AUTH_CMPL must not be taken for the pump's ─────────────────
// GAP events are broadcast to every registered client, not routed (issue
// #201). Taking a stranger's failed pairing for the pump's latched a wrong
// fault at the rank that masks every other cause.
//
// What this asserts is that the connection is *undisturbed* -- link status
// unchanged, still ready. Be clear about what it does not do: it does not by
// itself pin the address filter. Removing that filter leaves both assertions
// here passing, because the fault only reaches Pump Link Status on paths this
// scenario does not take. The filter is pinned by the SEC_REQ test above,
// which does fail when it is removed.
//
// An earlier version watched client_->disconnect() and was worse than weak --
// it was vacuous, since that call is gated on encryption_pending_, needing
// pairing enabled AND a bonded reconnect. It could never fire here at all.
void test_a_strangers_auth_failure_does_not_latch_a_fault() {
  std::cout << "\n=== A stranger's pairing failure latches no fault ===" << std::endl;

  Rig r;
  r.component.set_pairing_enabled(true);
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  const uint8_t stranger[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  r.client.mock_set_remote_bda(pump);
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "Connected and ready first");

  const std::string before = r.link_status.state;

  esp_ble_gap_cb_param_t p{};
  std::memcpy(p.ble_security.auth_cmpl.bd_addr, stranger, 6);
  p.ble_security.auth_cmpl.success = false;
  p.ble_security.auth_cmpl.fail_reason = ESP_AUTH_SMP_CONFIRM_FAIL;
  r.component.gap_event_handler(ESP_GAP_BLE_AUTH_CMPL_EVT, &p);
  r.advance(2000, 20);

  TEST_ASSERT(r.link_status.state == before,
              "Another device's failed pairing does not change Pump Link Status");
  TEST_ASSERT(r.ready_is_on(), "...and leaves the pump connected and ready");
}

// ── setup() registers the node with the client ──────────────────────────────
// If the component never registered, no GATT event would ever reach it and the
// node would sit silent forever with nothing pointing at why.
void test_the_component_registers_with_the_ble_client() {
  std::cout << "\n=== The component registers with the BLE client ===" << std::endl;

  BLEClient client;
  esphome::binary_sensor::BinarySensor ready;
  esphome::text_sensor::TextSensor link_status;
  AlphaHwrComponent component{&client};
  component.set_ready_binary_sensor(&ready);

  TEST_ASSERT(client.mock_registered_nodes() == 1,
              "Construction registers the component as a BLE node exactly once");
}

// ── The whole chain, end to end ─────────────────────────────────────────────
// This is what the earlier rounds could not reach: a GATT link brought up event
// by event and the initial read chain driven to the point where `Pump Ready`
// turns on. Nothing is called on the component's behalf -- every step goes in
// through a real handler, and every reply is a CRC-valid frame through the real
// transport.
void test_the_full_connection_reaches_pump_ready() {
  std::cout << "\n=== The full chain reaches Pump Ready ===" << std::endl;

  Rig r;
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(!r.ready_is_on(), "Subscribed, but not ready -- nothing has been read yet");

  // The stabilize window is quiet. This is half of what replaced the opening
  // sequence: between the CCCD write and the session being declared ready,
  // nothing goes on the wire at all. (`writes` records characteristic writes,
  // so the CCCD descriptor write is not among them.)
  //
  // This assertion is what kills the stabilize-window-is-not-waited-out
  // mutation. With SESSION_STABILIZE_MS at 0 the read chain starts immediately
  // and the run still reaches Pump Ready, so the end-state assertion below
  // would pass on its own.
  TEST_ASSERT(esp_gattc_mock().writes.empty(),
              "Subscribed, and nothing has been written -- the stabilize window "
              "sends no frames");

  const bool became_ready = r.run_until_ready();

  // Assert the opening sequence by CONTENT, not by count. `>= 10` was vacuous:
  // the run makes tens of writes in total, so the initial read chain satisfied
  // any count assertion on its own.
  //
  // What is pinned now is an ABSENCE, and it is checkable because those three
  // classes were unique to the removed sequence -- nothing else in this
  // component ever queues a Class 2, Class 5 or Class 11 command. So the scan
  // covers the WHOLE run rather than a window: a version that merely relocated
  // the packets later would pass a windowed check and fails this one.
  const auto &w = esp_gattc_mock().writes;
  int class2 = 0, class5 = 0, class11 = 0;
  for (const auto &req : w) {
    if (req.size() < 6) continue;
    if (req[4] == 0x02) class2++;
    else if (req[4] == 0x05) class5++;
    else if (req[4] == 0x0B) class11++;
  }
  TEST_ASSERT(class2 == 0 && class5 == 0 && class11 == 0,
              "No Class 2 identity read and no Class 5 or Class 11 INFO query is "
              "sent, anywhere in the connection");
  TEST_ASSERT(became_ready,
              "Pump Ready turns on once the session is ready and both caches "
              "are populated");

  // Telemetry polling actually started. Pump Ready does NOT depend on it --
  // the control cache is filled by the read chain and the schedule cache by
  // the overview read -- so a build where telemetry_service_.start() is never
  // called reaches Pump Ready with every live sensor frozen at nothing, and
  // every assertion above still passes. That is what the
  // ready-never-starts-telemetry mutation showed, and this is what kills it.
  //
  // Motor state (0x570045) is the first register of the poll set and is read
  // from nowhere else.
  int motor_state_polls = 0;
  int device_info_reads = 0;
  for (const auto &req : w) {
    if (req.size() >= 9 && req[4] == 0x0A && req[5] == 0x03 &&
        req[6] == 0x57 && req[7] == 0x00 && req[8] == 0x45) {
      motor_state_polls++;
    }
    if (req.size() >= 7 && req[4] == 0x07 && req[6] == 0x01) device_info_reads++;
  }
  TEST_ASSERT(motor_state_polls > 0,
              "Telemetry polling ran -- the session being ready starts it, and "
              "nothing else does once the read chain has latched");
  TEST_ASSERT(device_info_reads == 1,
              "...and the initial read chain ran exactly once");
}

// ── The read chain starts on the ready path, not on the next poll ───────────
// update() re-arms the chain when initial_data_read_done_ is still false, so a
// build where on_session_stabilized_() never triggers it still gets there --
// just up to a whole 10 s poll interval later. That fallback exists for a
// connection that persists through an ESP32 restart, not as the normal path,
// and the difference is invisible to any assertion that only waits for Pump
// Ready. Pinning the timing is what makes ready-never-triggers-the-initial-reads
// a catchable mutation rather than an equivalent one.
void test_the_read_chain_starts_without_waiting_for_a_poll() {
  std::cout << "\n=== The read chain does not wait for the next poll ===" << std::endl;

  Rig r;
  r.setup();
  r.connect_and_subscribe();

  // 2 s stabilize + 1 s to the device-info leg = ~3 s. Stop short of the 10 s
  // poll interval, which is the fallback this is distinguishing from.
  r.advance(6000, 60);

  int device_info_reads = 0;
  for (const auto &req : esp_gattc_mock().writes) {
    if (req.size() >= 7 && req[4] == 0x07 && req[6] == 0x01) device_info_reads++;
  }
  TEST_ASSERT(device_info_reads == 1,
              "The Class 7 device-info read is sent ~3 s after subscribe, off "
              "the ready path -- not up to 10 s later off the next update()");
}

// ── The stabilize timer belongs to its own connection ───────────────────────
// The opening sequence used to be guarded by Authentication's sequence number,
// and tests/test_auth.cpp pinned that. Removing the sequence (issue #174) left
// exactly one thing that can fire into a connection it does not belong to: the
// timer that declares the session ready. If a disconnect inside that window
// does not cancel it, it declares the NEXT connection ready before it has
// stabilized, which is issue #15 in a new costume and silent when it happens.
void test_a_disconnect_inside_the_stabilize_window_cancels_it() {
  std::cout << "\n=== A disconnect inside the stabilize window cancels it ===" << std::endl;

  Rig r;
  r.setup();
  r.connect_and_subscribe();
  r.advance(1000, 10);  // inside the window
  TEST_ASSERT(esp_gattc_mock().writes.empty(),
              "Still inside the stabilize window, so nothing has been sent");

  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(30000, 60);  // well past when the timer would have been due

  TEST_ASSERT(esp_gattc_mock().writes.empty(),
              "The cancelled timer never declares the session ready, so the read "
              "chain never runs against a link that is gone");
  TEST_ASSERT(!r.ready_is_on(), "...and Pump Ready stays off");
}

// ── A reconnect runs the chain once, not twice ──────────────────────────────
// Same hazard from the other side: a stale timer surviving into the next
// connection would interleave a second read chain with the real one.
void test_a_reconnect_reaches_ready_exactly_once() {
  std::cout << "\n=== A reconnect reaches ready exactly once ===" << std::endl;

  Rig r;
  r.setup();
  r.connect_and_subscribe();
  r.advance(1000, 10);          // drop inside the stabilize window
  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(5000, 20);

  esp_gattc_mock().writes.clear();
  r.answered_writes = 0;
  r.connect_and_subscribe();
  const bool became_ready = r.run_until_ready();
  TEST_ASSERT(became_ready, "The reconnect reaches Pump Ready");

  // The Class 7 product-name read is sent once per read chain, so counting it
  // counts chains. Two would mean the dropped connection's timer survived.
  int product_name_reads = 0;
  for (const auto &req : esp_gattc_mock().writes) {
    if (req.size() >= 7 && req[4] == 0x07 && req[6] == 0x01) product_name_reads++;
  }
  TEST_ASSERT(product_name_reads == 1,
              "The initial read chain runs once on the reconnect, not twice "
              "interleaved with a chain the dropped connection left behind");
}

// ── One cache is not enough ─────────────────────────────────────────────────
// Without this, `is_state_synchronized()` could be reduced to `return true`
// and every assertion in the file would still pass: there was no scenario in
// which the session authenticated and the caches did not fill, so the gate was
// only ever observed agreeing.
void test_one_cache_is_not_enough_for_ready() {
  std::cout << "\n=== Ready needs both caches, not one ===" << std::endl;

  Rig r;
  r.answer_overview = false;  // control-mode cache fills; schedule overview does not
  r.setup();
  r.connect_and_subscribe();

  TEST_ASSERT(!r.run_until_ready(60000),
              "The session authenticates and the control cache fills, but Pump "
              "Ready stays off while the schedule overview is unanswered");
}

// ── ...and goes off again when the link drops ───────────────────────────────
void test_ready_clears_on_disconnect() {
  std::cout << "\n=== Pump Ready clears when the link drops ===" << std::endl;

  Rig r;
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "Ready first");

  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  TEST_ASSERT(!r.ready_is_on(), "...and not ready after the link drops");
}

// ---------------------------------------------------------------------------
// The gap histogram's wiring (issue #176 part 1). The pure statistics are
// covered in test_link_watchdog.cpp; what can only be checked here is that the
// component feeds the sampler from the real notification path, and — the part
// that has OOMed this node before — what it costs in API frames.
// ---------------------------------------------------------------------------

void test_link_gap_baseline_is_published_once_at_zero() {
  std::cout << "\n=== Every gap counter reaches Home Assistant at zero ==="
            << std::endl;

  // Not cosmetic. These are total_increasing, and Home Assistant reconstructs
  // a total across reboots by recognising the reset — which it can only do if
  // it sees the zero baseline. An entity that stays `unknown` until something
  // interesting happens has its first run's counts charged to the previous one.
  Rig r;
  r.setup();
  r.advance(2000, 2);

  bool all_zero = true;
  for (auto &s : r.gaps_over) {
    if (!s.has_state() || s.state != 0.0f) all_zero = false;
  }
  TEST_ASSERT(all_zero, "All six rungs publish 0 without waiting for a gap");
  TEST_ASSERT(r.gaps_truncated.has_state() && r.gaps_truncated.state == 0.0f,
              "So does the truncated counter");
  TEST_ASSERT(r.watch_time.has_state() && r.watch_time.state == 0.0f,
              "...and watched time, whose throttle does not delay the baseline");
}

void test_gap_counters_do_not_publish_on_every_tick() {
  std::cout << "\n=== Gap counters cost one frame per change, never a repeat ==="
            << std::endl;

  // The issue #127 gate, asserted as a frame budget. publish_state() does not
  // dedup, so an ungated republish on the ~1 s link tick is a frame per API
  // subscriber per second whether or not anything moved.
  //
  // Asserted as "one frame per increment" rather than "no frames at all",
  // because this rig's pump is not a healthy pump: answer_outstanding_writes()
  // deliberately answers only the requests a test taught it about, so some
  // polls here go unanswered and the lower rungs do move. On hardware they
  // would not. The invariant that matters is rig-independent either way -- a
  // counter is published when it changes and at no other time -- and stating it
  // that way keeps the test honest about what it is actually driving.
  Rig r;
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  float before[6];
  for (size_t i = 0; i < 6; i++) {
    before[i] = r.gaps_over[i].state;
    r.gaps_over[i].publish_count = 0;
  }
  r.gaps_truncated.publish_count = 0;
  r.watch_time.publish_count = 0;

  const int ticks = 600;
  r.advance(600000, ticks);  // ten minutes at one loop tick per second

  bool one_frame_per_change = true;
  int rung_frames = 0;
  for (size_t i = 0; i < 6; i++) {
    rung_frames += r.gaps_over[i].publish_count;
    if (r.gaps_over[i].publish_count !=
        static_cast<int>(r.gaps_over[i].state - before[i]))
      one_frame_per_change = false;
  }
  TEST_ASSERT(one_frame_per_change,
              "Each rung published exactly as many frames as it gained counts");
  TEST_ASSERT(rung_frames < ticks / 10,
              "...nowhere near the per-tick republish the gate exists to prevent");
  TEST_ASSERT(r.gaps_truncated.publish_count == 0,
              "Nothing was truncated, and an unchanged counter is silent");

  // Watched time is the one that genuinely moves every 10 s, so it is throttled
  // rather than silent. Ten minutes crosses one or two 300 s boundaries
  // depending on where the baseline landed; what must not happen is a frame per
  // tick.
  TEST_ASSERT(r.watch_time.publish_count >= 1 && r.watch_time.publish_count <= 3,
              "Watched time publishes on its 300s throttle, not on the 1s tick");
  TEST_ASSERT(r.watch_time.state > 0.0f,
              "...and it is current rather than stuck at the baseline");
}

void test_a_quiet_link_fills_the_rungs_end_to_end() {
  std::cout << "\n=== A quiet interval reaches the rungs through the real "
               "notification path ==="
            << std::endl;

  // The call-site test: it proves the notification callback feeds the whole
  // distribution and not only the running maximum. Fifty seconds of silence,
  // which is past the 45 s rung and short of both the 60 s rung and the 60 s
  // watchdog, so the interval ends on its own.
  Rig r;
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  r.answer_writes = false;
  r.advance(50000, 50);
  r.answer_writes = true;
  r.advance(20000, 20);

  TEST_ASSERT(r.gaps_over[3].state >= 1.0f,
              "The 45s rung counted the quiet interval");
  TEST_ASSERT(r.gaps_over[4].state == 0.0f && r.gaps_over[5].state == 0.0f,
              "...and the 60s and 90s rungs did not, since it never got there");
  TEST_ASSERT(r.gaps_truncated.state == 0.0f,
              "The interval ended on its own, so nothing was truncated");
}

void test_a_recycle_marks_the_interval_truncated_end_to_end() {
  std::cout << "\n=== A watchdog recycle counts, and says it was cut short ==="
            << std::endl;

  // The other call site. A pump that answers nothing at all still reaches the
  // point where the watchdog is the only thing that notices, and the interval
  // it gives up on is exactly the sample that must not be discarded — dropping
  // it is what censored the statistic at the budget the first time round.
  Rig r;
  r.answer_writes = false;
  r.setup();
  r.connect_and_subscribe();
  r.advance(70000, 70);

  TEST_ASSERT(r.gaps_over[4].state >= 1.0f,
              "The 60s rung counted the interval the watchdog gave up on");
  TEST_ASSERT(r.gaps_truncated.state >= 1.0f,
              "...and it is marked truncated, because it did not end on its own");
  TEST_ASSERT(r.gaps_over[5].state == 0.0f,
              "The 90s rung stays empty: a 60s budget cannot let an interval "
              "reach it, which is the censoring the docs warn about");
}


// ── A pump that will not pair, through the real handlers ────────────────────
// Issue #230. The detector's rule is host-tested exhaustively in
// tests/test_pairing_stall.cpp; what these pin is the WIRING -- that the
// component opens and closes a cycle on the right events, passes the disconnect
// reason, and puts the answer on the fault surface at a rank that does not
// trample the causes worth more.
//
// These exist because the first version of this change said, in the new
// header's own comment, that ble_connection_manager.cpp "is compiled by no host
// test" -- copied from three neighbouring headers that all still say it. It has
// not been true since it went into COMPONENT_SRCS, and repeating it was on its
// way to being the excuse for leaving the risky half of the change untested.
static void drive_stall_cycles(Rig &r, int n, esp_gatt_conn_reason_t reason) {
  for (int i = 0; i < n; i++) {
    r.open(ESP_GATT_OK);
    r.advance(500, 10);
    r.disconnect(reason);
    r.advance(500, 10);
  }
}

void test_three_refused_connections_name_the_pump_not_the_radio() {
  std::cout << "\n=== Three refused connections name the pump ===" << std::endl;

  Rig r;
  r.setup();  // enable_pairing defaults to false, which is the reported setup
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  r.client.mock_set_remote_bda(pump);
  esp_gap_mock().bond_device_num = 0;  // nothing bonded: the state after a bond clear

  drive_stall_cycles(r, 2, ESP_GATT_CONN_TERMINATE_PEER_USER);
  TEST_ASSERT(r.link_fault.state != "Pump not accepting pairing",
              "Two dropped links are not a diagnosis -- the disconnect reason "
              "still speaks for itself");

  drive_stall_cycles(r, 1, ESP_GATT_CONN_TERMINATE_PEER_USER);
  TEST_ASSERT(r.link_fault.state == "Pump not accepting pairing",
              "The third says what is actually wrong, instead of leaving "
              "\"Remote Terminated\" to imply a radio problem");
}

void test_a_radio_drop_is_never_reported_as_a_refusal() {
  std::cout << "\n=== A radio drop is not reported as a refusal ===" << std::endl;

  Rig r;
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  r.client.mock_set_remote_bda(pump);
  esp_gap_mock().bond_device_num = 0;

  drive_stall_cycles(r, 8, ESP_GATT_CONN_TIMEOUT);
  TEST_ASSERT(r.link_fault.state != "Pump not accepting pairing",
              "A supervision timeout eight times running is a radio problem, "
              "and telling the user to walk to the pump would be worse than "
              "the silence this replaced");
  TEST_ASSERT(r.link_fault.state == "Connection Timeout (0x08)",
              "...so the true reason keeps the surface");
}

void test_the_pump_offering_to_pair_takes_the_fault_back_off() {
  std::cout << "\n=== The pump offering to pair clears the fault ===" << std::endl;

  Rig r;
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  r.client.mock_set_remote_bda(pump);
  esp_gap_mock().bond_device_num = 0;

  drive_stall_cycles(r, 3, ESP_GATT_CONN_TERMINATE_PEER_USER);
  TEST_ASSERT(r.link_fault.state == "Pump not accepting pairing", "Stalled first");

  // Someone walks to the pump and puts it into pairing mode. The pump asks to
  // secure the link -- the one event that refutes the diagnosis outright.
  r.open(ESP_GATT_OK);
  esp_ble_gap_cb_param_t sec{};
  std::memcpy(sec.ble_security.ble_req.bd_addr, pump, 6);
  r.component.gap_event_handler(ESP_GAP_BLE_SEC_REQ_EVT, &sec);
  r.advance(200, 10);

  TEST_ASSERT(r.link_fault.state != "Pump not accepting pairing",
              "The fault comes off at once. Nothing else would take it off: "
              "with enable_pairing false no AUTH_CMPL ever fires, and a link "
              "the pump keeps dropping never reaches READY");
}

void test_a_stall_does_not_bury_a_bond_erasing_pairing_failure() {
  std::cout << "\n=== A stall does not bury an encryption failure ===" << std::endl;

  Rig r;
  r.component.set_pairing_enabled(true);
  r.setup();
  const uint8_t pump[6] = {0x00, 0x1E, 0x2A, 0x00, 0x3C, 0x4D};
  r.client.mock_set_remote_bda(pump);

  // Issue #14's shape: a bonded reconnect whose encryption fails, which erases
  // the bond. Everything after it is unbonded and unanswered -- the failure
  // manufactures the stall's own precondition.
  esp_gap_mock().bond_device_num = 1;
  std::memcpy(esp_gap_mock().bonded_addr, pump, 6);
  r.open(ESP_GATT_OK);
  esp_ble_gap_cb_param_t fail{};
  std::memcpy(fail.ble_security.auth_cmpl.bd_addr, pump, 6);
  fail.ble_security.auth_cmpl.success = false;
  fail.ble_security.auth_cmpl.fail_reason = ESP_AUTH_SMP_ENC_FAIL;
  r.component.gap_event_handler(ESP_GAP_BLE_AUTH_CMPL_EVT, &fail);
  r.disconnect(ESP_GATT_CONN_TERMINATE_PEER_USER);
  r.advance(500, 10);
  const std::string root_cause = r.link_fault.state;
  TEST_ASSERT(root_cause.find("0x") != std::string::npos,
              "The encryption failure is latched with its code");

  esp_gap_mock().bond_device_num = 0;  // the bond it erased
  drive_stall_cycles(r, 6, ESP_GATT_CONN_TERMINATE_PEER_USER);

  TEST_ASSERT(r.link_fault.state == root_cause,
              "The stall does not replace it. 0x61 is the only pointer to the "
              "reconnect_settle_time mitigation -- the stall is the treatment, "
              "that code is the prevention, and it is the one that recurs");
}


// ── The link that is connected, streaming, and never usable ────────────────
// Issue #211, driven through the real handlers. The pure predicate is tested
// exhaustively in tests/test_readiness_watchdog.cpp; what these pin is the
// wiring, and in particular the part that a plausible implementation gets
// wrong: latching a diagnosis the user cannot see.
// The pump's unsolicited operation-status notification -- a frame it volunteers
// rather than one answering a read. This is what makes the reported failure the
// shape it is: the data watchdog is re-armed by every one of these, so silence
// never accumulates and it never fires, while nothing at all is progressing.
static std::vector<uint8_t> volunteered_telemetry() {
  return with_crc({0x24, 0x12, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01,
                   0x00, 0x00, 0x07, 0x00, 0x01, 0x02, 0x44, 0xCE, 0x40, 0x00,
                   0x00, 0x00});
}

// Advance time while the pump keeps volunteering telemetry, the way it does on
// a real link. Without this the data watchdog fires at 60 s and recycles the
// link for its own reasons -- which is a different fault, and a test that let
// it happen would be watching the wrong watchdog while claiming to test this
// one. (It did, in the first version of this file: disabling the readiness
// watchdog entirely left all but one assertion green.)
static void advance_with_telemetry(Rig &r, uint32_t ms) {
  const uint32_t step = 5000;
  for (uint32_t elapsed = 0; elapsed < ms; elapsed += step) {
    r.notify(volunteered_telemetry());
    r.advance(step, 5);
  }
}

void test_a_link_that_never_becomes_ready_is_recycled() {
  std::cout << "\n=== A link that never becomes ready is recycled ===" << std::endl;

  Rig r;
  r.answer_writes = false;  // the reads go out and nothing answers them
  r.component.set_ready_recycle(true);  // this test is about the opt-in half
  r.setup();
  r.connect_and_subscribe();

  // Well past session-ready and past the 60 s data budget, but the pump is
  // streaming, so the data watchdog is satisfied and nothing recycles.
  advance_with_telemetry(r, 90000);
  TEST_ASSERT(!r.ready_is_on(), "Pump Ready is off, as the reported failure has it");
  const int disconnects_before = r.client.mock_disconnect_calls();
  TEST_ASSERT(r.link_fault.state.find("No data") == std::string::npos,
              "and the data watchdog has NOT fired — telemetry is arriving, "
              "which is exactly why this failure was invisible");

  // Past the 300 s readiness budget, still streaming throughout.
  advance_with_telemetry(r, 300000);
  TEST_ASSERT(r.client.mock_disconnect_calls() > disconnects_before,
              "The readiness watchdog tore the link down — recovery is driven "
              "by the disconnect callback, so that is what has to happen");
  TEST_ASSERT(r.link_fault.state.find("never became ready") != std::string::npos,
              "...and the fault says what was wrong, in the terms an operator "
              "can act on rather than as a BLE error code");
}

void test_the_readiness_fault_is_visible_while_the_session_is_ready() {
  std::cout << "\n=== The readiness fault is not hidden by session-ready ==="
            << std::endl;

  // The trap. evaluate_link_status() used to blank the fault string whenever
  // the SESSION was ready -- and session-ready is reached two seconds after
  // subscribe with no frame exchanged, which is precisely the state issue #211
  // describes. Latching a diagnosis and then publishing "None" over it would
  // have shipped a fault nobody could read.
  Rig r;
  r.answer_writes = false;
  r.setup();
  r.connect_and_subscribe();
  advance_with_telemetry(r, 360000);

  TEST_ASSERT(!r.ready_is_on(), "Still not usable");
  TEST_ASSERT(r.link_fault.state.find("never became ready") != std::string::npos,
              "The fault surface names THIS fault. Asserting merely that it is "
              "not \"None\" passed on the data watchdog's string with the "
              "readiness watchdog switched off entirely");
}

void test_a_healthy_link_never_trips_the_readiness_watchdog() {
  std::cout << "\n=== A healthy link never trips it ===" << std::endl;

  Rig r;
  // Recycling ON for the same reason: a healthy link not being torn down is
  // only evidence when a teardown was possible.
  r.component.set_ready_recycle(true);
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "Reaches Pump Ready normally");

  const int disconnects = r.client.mock_disconnect_calls();
  r.advance(600000, 600);  // twice the budget, healthy throughout
  TEST_ASSERT(r.client.mock_disconnect_calls() == disconnects,
              "Ten minutes on a working link and nothing is recycled — the "
              "watchdog waits for a state, and that state arrived");
  TEST_ASSERT(r.link_fault.state == "None",
              "...and no fault is latched");
}


// ── The two defects that nearly shipped ────────────────────────────────────
// Both were found by a skeptic pass, both were fixed, and neither was pinned by
// anything: a second pass reverted each fix in turn and the whole suite stayed
// green. These are the tests that make the fixes stick.
void test_the_readiness_fault_survives_the_telemetry_that_masks_it() {
  std::cout << "\n=== The readiness fault is not erased by telemetry ===" << std::endl;

  // force_disconnect() used to hardcode FailureHold::DATA for every caller, so
  // the readiness reason was latched at the one rank whose defining property is
  // "released by inbound data" -- in the one failure mode defined by inbound
  // data never stopping. One volunteered frame during the async-close window
  // unheld it, and the next generic disconnect reason took the surface.
  Rig r;
  r.answer_writes = false;
  r.setup();
  r.connect_and_subscribe();
  advance_with_telemetry(r, 400000);
  TEST_ASSERT(r.link_fault.state.find("never became ready") != std::string::npos,
              "Latched at the recycle");

  // Exactly the sequence that erased it: one more volunteered frame, then the
  // link dropping for an ordinary reason.
  r.notify(volunteered_telemetry());
  r.advance(1000, 5);
  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(1000, 5);
  TEST_ASSERT(r.link_fault.state.find("never became ready") != std::string::npos,
              "...and still latched afterwards. Held at DATA rank this read "
              "\"Connection Timeout (0x08)\" -- the generic string the hold "
              "exists to keep out");
}

void test_a_node_without_the_ready_entity_is_not_recycled_forever() {
  std::cout << "\n=== No ready_status entity is not a permanent fault ===" << std::endl;

  // `ready_status` is cv.Optional. Reading readiness back off that entity meant
  // a hand-written config omitting it had pump_ready permanently false, so a
  // perfectly healthy pump was torn down every 5 minutes, then 10, then 20,
  // settling hourly -- each recycle re-entering the encryption-on-open path
  // that can erase the bond. A diagnostic must not depend on a diagnostic
  // being declared.
  Rig r;
  r.component.set_ready_binary_sensor(nullptr);
  // Recycling ON, because that is what makes this test mean anything: with
  // it off "was not recycled" is trivially true and the mutation that reads
  // readiness back off the absent entity survives unnoticed.
  r.component.set_ready_recycle(true);
  r.setup();
  r.connect_and_subscribe();
  // Long enough for the read chain to complete and the caches to fill. There is
  // no entity to observe that through, which is the entire point -- the
  // component's own latch is what has to notice.
  r.advance(120000, 120);
  const int disconnects = r.client.mock_disconnect_calls();
  r.advance(700000, 700);  // past the budget, and past the doubled one
  TEST_ASSERT(r.client.mock_disconnect_calls() == disconnects,
              "A healthy pump is not recycled just because nobody declared the "
              "Pump Ready entity");
  TEST_ASSERT(r.link_fault.state == "None", "...and no fault is asserted");
}

void test_readiness_recycles_reach_the_counter_an_automation_watches() {
  std::cout << "\n=== Readiness recycles count on Pump Link Recycles ===" << std::endl;

  // The reporter of issue #211 named this as their signal from outside the
  // component. It stayed at zero through the failure the change exists for,
  // because a readiness recycle never touches the data counter.
  Rig r;
  r.answer_writes = false;
  r.component.set_ready_recycle(true);  // this test is about the opt-in half
  r.setup();
  r.connect_and_subscribe();
  advance_with_telemetry(r, 400000);
  TEST_ASSERT(r.recycles.state >= 1.0f,
              "The recycle is visible on Pump Link Recycles, not only in a log "
              "line nobody kept");
}


void test_the_fault_is_visible_across_the_reconnect_it_describes() {
  std::cout << "\n=== The fault survives the disconnect it explains ===" << std::endl;

  // docs/configuration.md promises this in so many words: "During the reconnect
  // the Pump Link Fault sensor reads `No data from pump (60s)` -- held there
  // rather than overwritten by the local disconnect that caused it."
  //
  // Moving the display gate from session-ready to pump-ready (issue #211) broke
  // that for every user, at a default where the readiness watchdog is off: the
  // readiness latch was cleared only at connection-OPEN, so it stayed true for
  // the whole disconnected window and the surface read it as healthy. The fault
  // published "None" across exactly the reconnect it exists to explain.
  Rig r;
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "Ready first, so the latch is set");

  // Go deaf and let the data watchdog recycle the link.
  r.answer_writes = false;
  r.advance(120000, 120);
  r.disconnect(ESP_GATT_CONN_TERMINATE_PEER_USER);
  r.advance(30000, 30);

  TEST_ASSERT(r.link_fault.state.find("No data") != std::string::npos,
              "The reason is still on the surface while the link is down. It "
              "read \"None\" when the readiness latch outlived the connection "
              "that set it");
}


void test_the_default_names_the_fault_without_touching_the_link() {
  std::cout << "\n=== The default names it and does not recycle ===" << std::endl;

  // The split (issue #211). Naming costs nothing; recycling takes another run
  // at the encryption-on-open window that can erase a bond, and on a node that
  // never becomes ready it would do that on an escalating schedule for as long
  // as the node is up. So the diagnosis ships on and the remedy is opt-in.
  //
  // This is the shipped configuration: ready_timeout 300s, ready_recycle off.
  Rig r;
  r.answer_writes = false;
  r.setup();
  r.connect_and_subscribe();
  const int disconnects = r.client.mock_disconnect_calls();

  advance_with_telemetry(r, 400000);

  TEST_ASSERT(r.link_fault.state.find("never became ready") != std::string::npos,
              "The fault is named — this is the half the reporter of #211 said "
              "he could not build from outside");
  TEST_ASSERT(r.client.mock_disconnect_calls() == disconnects,
              "...and the link was NOT torn down. A default that reconnects is "
              "a default that gambles a bond on an unobserved configuration");
  TEST_ASSERT(r.recycles.state == 0.0f,
              "...and nothing was counted as a recycle, because nothing was "
              "recycled — that counter is one an automation thresholds on");
}

// ── A disconnect must not publish a drift reading it never took (issue #259) ──
//
// The drift leg of the initial read chain reads the pump's clock. Every other
// leg of that chain captures the read-chain generation and returns if it has
// moved; this one captured only `this`.
//
// That cost nothing while Transport::reset() dropped abandoned callbacks in
// silence. Now that it fails them, the abandoned clock read reports an invalid
// time, and the else-branch publishes NAN -- to a user-facing sensor, on every
// dropped link, for the whole time the read sits queued behind the rest of the
// chain. On a flapping link the sensor reads `unknown` more often than it reads
// a number.
//
// It is also the clobber the leg already guards against one level up: the note
// on its timeout says the figure is "how far out was the pump when we found
// it" and must survive a re-armed chain. NAN is the same overwrite with a
// worse value.
void test_a_disconnect_does_not_publish_a_drift_reading() {
  std::cout << "\n=== A disconnect mid-chain publishes no clock drift ==="
            << std::endl;
  Rig r;
  r.setup();
  r.connect_and_subscribe();

  // Far enough in for the chain to be running and the clock read to be queued,
  // but not so far that it has been answered and published legitimately.
  r.advance(4000);
  const int published_before = r.clock_drift.publish_count;

  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(1000);

  TEST_ASSERT(r.clock_drift.publish_count == published_before,
              "the abandoned clock read publishes nothing -- a link that "
              "dropped mid-read has not measured a drift of NAN, it has not "
              "measured a drift");
}


// ── One clock, one floor: what each caller does without one (issue #270) ─────
//
// The component used to answer "what time is it" in five places with three
// different floors, three of them reading ::time(nullptr) directly. The
// accessors themselves are pinned in tests/test_time_service.cpp, including the
// two binaries that prove the `#ifndef USE_TIME` half. What is pinned HERE is
// the other half of the acceptance criterion -- what each *caller* does when
// the accessor refuses -- for the callers this rig can reach through a public
// door.
//
// Coverage, stated rather than implied:
//   * build_event_window()      -- both paths, directly, below.
//   * read_pump_clock()         -- both paths, directly, below.
//   * the read chain's drift leg -- shares publish_clock_drift_() with
//     read_pump_clock(); its ONE difference from that caller (it must not
//     publish NAN) is what test_the_drift_leg_leaves_a_good_reading_alone
//     covers, and the rest is the same function.
//   * stop_single_event_active_() -- reached only with a warm single-event
//     cache behind a run-state reconcile, which this rig does not stage. Its
//     sentinel is `now_unix() == 0`, swept in test_time_service.cpp.
//   * the Last Clock Sync stamp -- unreachable without a clock BY CONSTRUCTION:
//     check_and_sync_time()'s gate refuses to submit a sync at all unless
//     wall_clock_is_set(), which is pinned in test_clock_sync_gate.cpp. That
//     unreachability is why the libc fallback there was deleted rather than
//     converted.

// The pump clock the rig answers with, as an instant. Same local fields as the
// fixture body, resolved the way TimeService::parse_clock_response() resolves
// them, so the expected drift is derived rather than copied.
static time_t fixture_pump_epoch() {
  esphome::ESPTime t = esphome::ESPTime::from_epoch_local(0);
  t.year = 2026; t.month = 8; t.day_of_month = 15;
  t.hour = 20; t.minute = 38; t.second = 55;
  t.recalc_timestamp_local();
  return t.timestamp;
}

void test_the_drift_leg_publishes_the_drift_it_measured() {
  std::cout << "\n=== The read chain measures the drift against the node clock ==="
            << std::endl;
  Rig r;
  r.answer_clock = true;
  // 400 s behind the pump's fixture clock -- a value no floor, default or
  // truncation could produce by accident.
  r.attach_node_clock(fixture_pump_epoch() - 400);
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  TEST_ASSERT(r.clock_drift.publish_count > 0, "a drift is published");
  TEST_ASSERT(!std::isnan(r.clock_drift.state) &&
                  std::fabs(r.clock_drift.state - 400.0f) < 1.5f,
              "the pump reads 400 s ahead of the node, and the sign says ahead "
              "rather than behind");
}

// The one way the two drift callers differ, and the reason publish_clock_drift_
// reports rather than decides what to publish on failure. Overwriting "how far
// out was the pump when we found it" with NAN is the defect issue #259 fixed.
// Note the PUMP answers here -- what is missing is the node's own clock, which
// before #270 this leg resolved with its own ::time(nullptr) read against its
// own copy of the literal 1609459200.
void test_the_drift_leg_leaves_a_good_reading_alone() {
  std::cout << "\n=== The read chain's drift leg publishes nothing without a clock ==="
            << std::endl;
  Rig r;
  r.answer_clock = true;  // the PUMP answers; the NODE still has no clock
  r.setup();
  r.connect_and_subscribe();

  const int before = r.clock_drift.publish_count;
  r.run_until_ready();

  TEST_ASSERT(r.clock_drift.publish_count == before,
              "the chain's drift leg publishes nothing at all -- not a number, "
              "and not the NAN that would clobber the last good reading");
}

// The other caller of publish_clock_drift_(), which resolves the same failure
// the opposite way: somebody pressed "Read Pump Clock" and is owed an answer,
// and "unknown" is the true one.
//
// Submitted immediately after the link comes up rather than after the chain has
// settled: the transport queue is FIFO and the control poll keeps it fed, so a
// command submitted late waits behind an unbounded backlog and never reaches
// the wire inside any window a test can afford.
void test_a_manual_clock_read_without_a_node_clock_answers_unknown() {
  std::cout << "\n=== A manual clock read with no node clock publishes NAN ==="
            << std::endl;
  Rig r;
  r.answer_clock = true;
  r.setup();
  r.connect_and_subscribe();

  r.component.read_pump_clock();
  r.run_until_ready();

  TEST_ASSERT(r.clock_drift.publish_count > 0,
              "somebody pressed the button, so the sensor is answered");
  TEST_ASSERT(std::isnan(r.clock_drift.state),
              "and the answer is NAN -- the pump's clock was read fine, the "
              "node simply cannot say how far out it is");
}

// ── build_event_window(): the schedule editor's dated-event helper ───────────

void test_build_event_window_refuses_without_a_node_clock() {
  std::cout << "\n=== A dated event cannot be built without a node clock ==="
            << std::endl;
  Rig r;
  r.setup();

  uint32_t begin = 12345, end = 67890;
  const bool built = r.component.build_event_window("test", 6, 1, 9, 0, 6, 8, 17, 0,
                                                    &begin, &end);
  TEST_ASSERT(!built, "the window is refused");
  TEST_ASSERT(begin == 12345 && end == 67890,
              "and the caller's outputs are untouched, so a caller that ignores "
              "the return value does not get a 1970 window");
}

void test_build_event_window_anchors_to_the_node_clock() {
  std::cout << "\n=== A dated event anchors to the node's year ===" << std::endl;
  Rig r;
  // 2022-06-15 12:00:00 UTC, and the year is deliberately one the machine
  // running this test is NOT in.
  //
  // That is the whole test. The helper takes month/day/hour/minute and has to
  // get the YEAR from somewhere; before #270 it took it from libc, i.e. from
  // the host's own clock. A fixture dated in the CURRENT year cannot tell the
  // two sources apart -- and the first version of this test used one, so it
  // passed with the production line reverted. Any year above the 2021 floor
  // and away from today works.
  r.attach_node_clock(1655294400);
  r.setup();

  uint32_t begin = 0, end = 0;
  const bool built = r.component.build_event_window("test", 6, 1, 9, 0, 6, 8, 17, 0,
                                                    &begin, &end);
  TEST_ASSERT(built, "the window is built");

  esphome::ESPTime b = esphome::ESPTime::from_epoch_local(begin);
  esphome::ESPTime e = esphome::ESPTime::from_epoch_local(end);
  TEST_ASSERT(b.year == 2022 && e.year == 2022,
              "both ends land in the node clock's year, not the host's -- the "
              "fixture year is deliberately not the current one, so this "
              "assertion can actually fail");
  TEST_ASSERT(b.month == 6 && b.day_of_month == 1 && b.hour == 9 && b.minute == 0,
              "the begin carries the fields it was given");
  TEST_ASSERT(e.month == 6 && e.day_of_month == 8 && e.hour == 17 && e.minute == 0,
              "and so does the end");
  TEST_ASSERT(end > begin, "the pair is ordered");
}

// build_event_window() encodes local calendar fields into a UTC epoch, and it
// used mktime() -- which resolves against libc's zone, and on the ESP32 libc
// has none (issue #289). So the schedule editor's buttons produced windows out
// by the node's offset, and no ordinary host test could see it because the host
// build DOES set the zone.
//
// MockZoneOverride makes ESPHome's zone disagree with the process TZ, which is
// the device's split reproduced on the host.
void test_build_event_window_does_not_encode_through_libc() {
  std::cout << "\n=== A dated event is encoded through ESPTime, not libc ===" << std::endl;
  setenv("TZ", "UTC", 1);
  tzset();
  Rig r;
  r.attach_node_clock(1655294400);  // 2022-06-15 12:00 UTC
  r.setup();
  {
    esphome::MockZoneOverride pst(-8 * 3600);  // libc says UTC, ESPHome says PST

    uint32_t begin = 0, end = 0;
    TEST_ASSERT(r.component.build_event_window("test", 6, 1, 9, 0, 6, 1, 17, 0,
                                               &begin, &end),
                "the window is built");

    // 2022-06-01 09:00 LOCAL in a UTC-8 zone is 17:00 UTC.
    struct tm fields {};
    fields.tm_year = 122;  // 2022
    fields.tm_mon = 5;     // June
    fields.tm_mday = 1;
    fields.tm_hour = 9;
    const uint32_t naive = static_cast<uint32_t>(timegm(&fields));

    TEST_ASSERT(begin == naive + 8u * 3600u,
                "the begin is the UTC instant of 09:00 LOCAL -- eight hours "
                "later than the fields read as UTC");
    TEST_ASSERT(begin != naive,
                "and specifically not the fields taken as UTC, which is what "
                "mktime() produced on the device");
    TEST_ASSERT(end - begin == 8u * 3600u,
                "and the window is still the eight hours that were asked for");
  }
}

// The displays render the pump's timestamps for a human, and they went through
// localtime_r -- so on the device the user was shown UTC (issue #289).
void test_the_vacation_display_is_rendered_in_local_time() {
  std::cout << "\n=== The vacation display renders in the node's zone ===" << std::endl;
  setenv("TZ", "UTC", 1);
  tzset();
  Rig r;
  r.setup();
  {
    esphome::MockZoneOverride pst(-8 * 3600);
    // A vacation running 2022-06-01 00:00 UTC -> the display should read the
    // PREVIOUS day at 16:00 local, not 00:00.
    struct tm fields {};
    fields.tm_year = 122;
    fields.tm_mon = 5;
    fields.tm_mday = 1;
    const uint32_t utc_begin = static_cast<uint32_t>(timegm(&fields));

    esphome::ESPTime shown = esphome::ESPTime::from_epoch_local(utc_begin);
    TEST_ASSERT(shown.hour == 16 && shown.day_of_month == 31 && shown.month == 5,
                "ESPTime renders it as 16:00 on May 31 local, which is what a "
                "user in that zone should see");
  }
}

// A clock the node has but that nothing has SET is not a clock. This is the
// case the retired year-2020 floor let through and the one floor now refuses --
// and it is the realistic one, since an ESP32 that boots without SNTP has a
// running clock reading 1970, not a missing one.
void test_build_event_window_refuses_an_unsynced_clock() {
  std::cout << "\n=== A dated event refuses a clock nothing has set ===" << std::endl;
  Rig r;
  r.attach_node_clock(0);  // 1970-01-01: fields in range, nobody set it
  r.setup();

  uint32_t begin = 1, end = 2;
  TEST_ASSERT(!r.component.build_event_window("test", 6, 1, 9, 0, 6, 8, 17, 0,
                                              &begin, &end),
              "an unset clock is refused, rather than dating the event to 1970");
}

// ── The pump's flow limiters (issue #274) ────────────────────────────────────
//
// The limiter constrains the pump BELOW what it was asked for, while every
// signal the component publishes says the write worked -- because it did.
// Measured by @jfriend00 with MaxFlow at 1.6 gpm: 3000 RPM commanded, 1883
// delivered, settled `accepted`, with the pump reporting 3000 back from its
// own 86/7.
//
// The decode and the three-state reporting are pinned in tests/test_limiter.cpp
// against real captured frames. What is pinned here is the wiring: that the
// reads go out, that the answer reaches both entities, that the status is
// re-read as the load changes, and that a node asking for neither entity does
// not pay for any of it.
void test_an_enabled_limiter_reaches_the_entities() {
  std::cout << "\n=== An enabled limiter reaches the entities ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.limiter_max_flow_enabled = true;
  r.limiter_limiting = false;  // on, but not biting yet
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();
  r.advance(30000);

  TEST_ASSERT(r.limiter_reads > 0, "the component read the limiter family");
  TEST_ASSERT(r.flow_limiter.has_state(), "and published a verdict");
  TEST_ASSERT(r.flow_limiter.state.find("MaxFlow enabled") != std::string::npos,
              "naming the limiter that is switched on");
  TEST_ASSERT(r.flow_limiter.state.find("1.60") != std::string::npos,
              "and its cap in gpm, the unit it was entered in");
  TEST_ASSERT(r.flow_limiter.state.find("not limiting") != std::string::npos,
              "while saying it is not biting yet");
  TEST_ASSERT(!r.flow_limited.state,
              "so the one-bit form an automation reads is false");
}

// The state the whole issue exists to make visible.
void test_a_limiter_actively_limiting_raises_the_binary_sensor() {
  std::cout << "\n=== A limiter actively limiting raises the binary sensor ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.limiter_max_flow_enabled = true;
  r.limiter_limiting = true;
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  TEST_ASSERT(r.flow_limited.state,
              "the pump is being held below what it was asked for, and one "
              "entity says so in a form an automation can act on");
  TEST_ASSERT(r.flow_limiter.state.find("MaxFlow limiting") != std::string::npos,
              "and the other names which limiter");
}

// A pump whose firmware does not implement the family. Every read goes
// unanswered, which must leave "unknown" rather than an all-clear -- reporting
// "no limiter enabled" for a pump we could not ask is exactly the false
// reassurance this issue is about.
void test_a_pump_without_the_limiter_family_reports_unknown() {
  std::cout << "\n=== A pump that cannot answer leaves the limiter unknown ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.answer_limiters = false;
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  TEST_ASSERT(r.limiter_reads > 0, "the reads went out");
  const bool silent = !r.flow_limiter.has_state() ||
                      r.flow_limiter.state == "unknown";
  TEST_ASSERT(silent, "and nothing claimed there was no limiter");
  TEST_ASSERT(!r.flow_limited.state, "the binary sensor does not assert limiting either");
}

// Whether a limiter is LIMITING changes with the load, so it has to be re-read.
// This is the one the reviewer on #288 caught: the poll call was written, the
// edit that placed it never landed, and every test here only exercised the
// connect-time read -- so the entities froze at their connection-time state and
// nothing noticed. Which is most of what they exist for.
void test_the_limiter_status_is_re_read_as_the_load_changes() {
  std::cout << "\n=== A limiter that starts limiting is noticed on the poll ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.limiter_max_flow_enabled = true;
  r.limiter_limiting = false;
  r.component.set_control_state_poll_interval(30000);
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  TEST_ASSERT(!r.flow_limited.state, "not limiting at connect");
  const int reads_after_connect = r.limiter_reads;

  // The load rises and the cap starts biting, with nothing written by us.
  // Fine-grained for the same reason as above: at the default step size a 3 s
  // command timeout expires between ticks and nothing is ever answered.
  r.limiter_limiting = true;
  r.advance(300000, 600);

  TEST_ASSERT(r.limiter_reads > reads_after_connect,
              "the status was re-read rather than left at its connect value");
  TEST_ASSERT(r.flow_limited.state,
              "and the entity followed the pump, which is the whole point of "
              "polling it");
  TEST_ASSERT(r.flow_limiter.state.find("limiting") != std::string::npos,
              "the text sensor moved too");
}

// The limiter family belongs to the pump we were talking to. Left standing
// across a disconnect, the entities went on reporting the previous
// connection's caps -- and a limiter changed in the GO app while the link was
// down would have been reported wrongly for as long as the node stayed up.
void test_a_disconnect_drops_the_limiter_state() {
  std::cout << "\n=== A disconnect drops the limiter state ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.limiter_max_flow_enabled = true;
  r.limiter_limiting = true;
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();
  TEST_ASSERT(r.flow_limited.state, "limiting while connected");

  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(2000);

  TEST_ASSERT(!r.flow_limited.state,
              "the link is gone, so we no longer claim the pump is limiting");
  TEST_ASSERT(r.flow_limiter.state == "unknown",
              "and the text falls back to unknown rather than showing the last "
              "connection's answer as though it were current");
}

// The chains stop at the first failure, and this is the observable that shows
// it. A reply carries no request identifier, and the two config records share a
// type code (895 v1) while the three status records share another (896 v1) --
// so a read that has timed out and whose reply arrives late satisfies the NEXT
// request in the chain. Carrying on is not merely wasteful: a late 86/600 reply
// gets cached as MinFlow, and the entity reports a cap against the wrong
// limiter.
//
// Asserted on what was REQUESTED rather than on the entity, because a half-read
// family reads "unknown" whether the chain stopped or shifted -- which is
// exactly why CI's mutation sweep found this uncovered.
void test_the_config_chain_stops_at_the_first_failure() {
  std::cout << "\n=== A failed config read stops the chain ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.fail_limiter_sub = 600;  // MaxFlow goes unanswered; everything else would answer
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();
  // Long enough, at a fine enough tick, that a chain which DID carry on would
  // have got its next read out and been seen. Asserting too early passes
  // against a chain that simply had not reached 86/601 yet.
  r.advance(300000, 600);

  TEST_ASSERT(r.limiter_sub_was_requested(600), "86/600 was asked for");
  TEST_ASSERT(!r.limiter_sub_was_requested(601),
              "and 86/601 was NOT -- a late 600 reply would have been cached as "
              "MinFlow, so the chain stops rather than carrying on into an "
              "ambiguous reply stream");
}

void test_the_status_chain_stops_at_the_first_failure() {
  std::cout << "\n=== A failed status read stops the chain ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  r.fail_limiter_sub = 640;  // the first status record goes unanswered
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();
  r.advance(300000, 600);  // see the note in the config test above

  TEST_ASSERT(r.limiter_sub_was_requested(600) && r.limiter_sub_was_requested(601),
              "the config pair completed, so the status chain was reached");
  TEST_ASSERT(r.limiter_sub_was_requested(640), "86/640 was asked for");
  TEST_ASSERT(!r.limiter_sub_was_requested(641),
              "and 86/641 was NOT, so a late 640 reply cannot be consumed as "
              "MinFlow's status");
  TEST_ASSERT(!r.limiter_sub_was_requested(660),
              "...nor can the records shift down into the manager slot");
}

// The control: with nothing failing, all five are read. Without this the two
// above pass against a component that reads 86/600 and gives up.
void test_a_healthy_pump_is_read_all_the_way_through() {
  std::cout << "\n=== A pump that answers is read all the way through ===" << std::endl;
  Rig r;
  r.attach_limiter_entities();
  // A deliberately aggressive control poll, so a poll certainly falls WHILE the
  // connect-time chain is still running. At the shipped 30 s the chain can
  // finish first and the overlap never arises, which would make this test agree
  // with a component that has no guard at all.
  r.component.set_control_state_poll_interval(5000);
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();
  // The five reads queue behind the ordinary telemetry and control polls, so
  // the chain takes minutes of mock time to drain rather than seconds.
  //
  // The step count matters and is not padding: advance() divides the interval
  // into `steps` ticks, and the pump is only allowed to answer at a tick
  // boundary. At the default 20 steps this would be 15 s per tick, so every
  // command's 3 s transport timeout would expire inside a single step and
  // nothing could ever be answered. 600 steps is 500 ms a tick.
  r.advance(300000, 600);

  // The first five requests, IN ORDER, rather than "was each one ever asked
  // for". The weaker form passes against overlapping chains: the control poll
  // issues 640/641/660 of its own, so every address gets requested by somebody
  // even when the connect chain is being cut off after its third read. Only the
  // contiguous sequence says one chain ran to completion.
  const uint16_t expected[5] = {600, 601, 640, 641, 660};
  TEST_ASSERT(r.limiter_subs_seen.size() >= 5, "at least five reads went out");
  bool in_order = r.limiter_subs_seen.size() >= 5;
  for (size_t i = 0; i < 5 && in_order; i++)
    if (r.limiter_subs_seen[i] != expected[i]) in_order = false;
  if (!in_order) {
    std::cout << "       requested:";
    for (uint16_t x : r.limiter_subs_seen) std::cout << " " << x;
    std::cout << std::endl;
  }
  TEST_ASSERT(in_order,
              "all five addresses were read, in one uninterrupted chain -- a "
              "second chain starting on top of this one would show up here as "
              "a repeated 86/640 partway through");
}

// The restraint that keeps this optional rather than a tax: five frames per
// connection and three per control poll, on a node that displays neither.
void test_a_node_without_limiter_entities_does_not_read_them() {
  std::cout << "\n=== No limiter entity, no limiter reads ===" << std::endl;
  Rig r;
  // deliberately no attach_limiter_entities()
  r.setup();
  r.connect_and_subscribe();
  r.run_until_ready();

  TEST_ASSERT(r.limiter_reads == 0,
              "nothing asked, so the family costs nothing on a node that does "
              "not display it");
}

// ── Diagnostic suspend (issue #243) ──────────────────────────────────────────
//
// The pump holds one BLE connection at a time, so a bonded, connected node owns
// it and the Grundfos GO app cannot. Before this the only way to hand the pump
// over was to remove power from the node.
//
// The two calls are trivial. What needs testing is the guards, because without
// them the component undoes its own switch: the reconnect-settle path
// (`reconnect_settle_time`, on by default) turns auto-connect off on a
// disconnect and schedules a timer that turns it back on. A suspend is a
// disconnect, so it trips that path with its own teardown and the flag comes
// straight back. Reported from a node running the default window: suspended for
// 2.3 seconds, all of it the window.

/// Hand parse_device() an advertisement from the pump, which is what starts the
/// settle timer once the pump reappears after a drop.
static void advertise_pump(Rig &r) {
  esphome::esp32_ble_tracker::ESPBTDevice dev;
  dev.set_address(r.client.get_address());
  r.component.parse_device(dev);
}

/// Put the rig in the state the guards are about, and PROVE it -- both defaults
/// silently make these tests vacuous:
///
///   - the mock's `auto_connect_` starts FALSE, so "suspending clears it" passes
///     against a component that does nothing at all. Real builds default the
///     YAML option to true, which is what this restores.
///   - `bond_device_num` starts 0, and the settle block is gated on
///     `esp_ble_get_bond_device_num() > 0`, so the whole path the guards protect
///     never executes. Both guards could be deleted with the suite still green;
///     that is how the first draft of these tests scored.
static void arm_the_settle_path(Rig &r) {
  esp_gap_mock().bond_device_num = 1;
  r.client.set_auto_connect(true);
  r.component.set_reconnect_settle_time(2000);
}

void test_suspend_drops_the_link_and_stops_reconnecting() {
  std::cout << "\n=== Suspend drops the link and does not reconnect ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);
  TEST_ASSERT(r.client.mock_auto_connect(),
              "precondition: auto-connect is on, so clearing it can be seen");

  const int before = r.client.mock_disconnect_calls();
  r.component.set_suspended(true);

  TEST_ASSERT(r.client.mock_disconnect_calls() == before + 1,
              "suspending drops the BLE link");
  TEST_ASSERT(!r.client.mock_auto_connect(),
              "  ...and clears auto-connect, which is what makes it a suspend "
              "rather than a disconnect the client undoes on the next advert");

  // The disconnect the suspend just caused, then the pump advertising again --
  // exactly the sequence that fed the settle timer and released the switch.
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  advertise_pump(r);
  r.advance(10000);

  // Deliberately not "the settle path did not hand it back": with the
  // disconnect-side guard in place the settle path is never armed here, so
  // there is no timer to decline. What this pins is that NOTHING restored
  // auto-connect over ten seconds and an advertisement.
  TEST_ASSERT(!r.client.mock_auto_connect(),
              "  ...and it is STILL suspended ten seconds and an advertisement "
              "later; nothing handed auto-connect back");
  TEST_ASSERT(r.component.is_suspended(), "  ...and the component says so");
}

void test_release_restores_the_link() {
  std::cout << "\n=== Releasing the suspend reconnects ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  advertise_pump(r);
  r.advance(5000);
  TEST_ASSERT(!r.client.mock_auto_connect(), "suspended");

  r.component.set_suspended(false);
  TEST_ASSERT(r.client.mock_auto_connect(),
              "releasing restores auto-connect, so the client reconnects on the "
              "pump's next advertisement");
  TEST_ASSERT(!r.component.is_suspended(), "  ...and the flag is down");
}

// The guard the first test cannot reach: a suspend arriving while the settle
// timer is ALREADY running. Guard 1 stops the timer being armed during a
// suspend; only guard 2 stops a timer armed before it from firing into one.
void test_a_suspend_during_a_settle_window_is_not_undone_by_the_timer() {
  std::cout << "\n=== A suspend mid-settle survives the timer ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  // An ordinary drop arms the settle path, and the pump reappearing starts the
  // timer. No suspend yet.
  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  advertise_pump(r);
  TEST_ASSERT(!r.client.mock_auto_connect(),
              "precondition: the settle path really is holding auto-connect "
              "down, so the timer below has something to restore");

  // Suspend lands inside the window, then the window elapses.
  r.component.set_suspended(true);
  r.advance(6000);

  TEST_ASSERT(!r.client.mock_auto_connect(),
              "the settle timer fired into a suspended link and left it alone");
  TEST_ASSERT(r.component.is_suspended(), "  ...and it is still suspended");
}

void test_a_suspended_link_reads_as_suspended_not_as_a_fault() {
  std::cout << "\n=== A suspended link is not a fault ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  r.advance(30000);   // well past the Unreachable threshold

  TEST_ASSERT(r.link_status.state == "Suspended",
              "Pump Link Status reads Suspended, not Unreachable -- a link we "
              "took down on purpose is not a broken one");
  TEST_ASSERT(r.link_fault.state == "None",
              "  ...and Pump Link Fault reads None, not the node's own "
              "Local Host Terminated");

  // The mask outlasts the release. Between reconnecting and Pump Ready there is
  // a ~15 s window in which the self-inflicted 0x16 would otherwise be
  // republished -- which is precisely the window an automation watches.
  r.component.set_suspended(false);
  r.advance(3000);
  TEST_ASSERT(r.link_fault.state == "None",
              "  ...and it stays None across the reconnect, rather than "
              "republishing the disconnect the suspend itself caused");
}


// Releasing a LONG suspension must not resume through a spurious "Unreachable".
//
// Reported by @jfriend00, who wrote the same bug and caught it on an 85 second
// suspension. The Unreachable rung is `now - link_last_open_ms_ >
// LINK_UNREACHABLE_MS`, and that stamp only advances while the session is
// READY -- so it stops moving the moment the link goes down. Suspend for longer
// than the threshold and the first status evaluation after release, in the half
// second before the link reopens, lands on Unreachable.
//
// A short bench session cannot see it: the whole failure needs a suspension
// longer than 20 s, which is exactly what this switch is for.
void test_releasing_a_long_suspension_does_not_report_unreachable() {
  std::cout << "\n=== A long suspension does not resume via Unreachable ==="
            << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  r.advance(85000);   // the reporter's figure, and well past LINK_UNREACHABLE_MS
  TEST_ASSERT(r.link_status.state == "Suspended",
              "precondition: still suspended after 85 s, so the Unreachable "
              "clock has been stopped that whole time");

  r.component.set_suspended(false);

  // Named, not `!= "Unreachable"`. That form passes on five other strings
  // including "Suspended" -- i.e. it would hold against a release that never
  // updated the status at all, which is a different bug and a real one.
  TEST_ASSERT(r.link_status.state == "Connecting",
              "releasing restarts the unreachable clock and the status reads "
              "Connecting -- the link was down on purpose, so the countdown "
              "starts from the release, not from the last time the pump was "
              "ready");
}

// A genuine failure AFTER the release must reach the surface.
//
// The first cut masked Pump Link Fault from suspension until the pump was READY
// again, which keeps the self-inflicted 0x16 off the surface across the
// reconnect -- and also hides a failed reconnect, an authentication error or a
// readiness fault, indefinitely if recovery never succeeds. Raised in review.
// Clearing the one expected reason instead does the first without the second.
void test_a_failure_after_release_is_not_hidden() {
  std::cout << "\n=== A failure after release still reports ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  r.advance(3000);
  TEST_ASSERT(r.link_fault.state == "None",
              "precondition: the teardown we asked for is not on the surface");

  r.component.set_suspended(false);
  // The link comes back and then drops on a supervision timeout -- a real
  // fault, nothing to do with us, and exactly the kind the first cut hid.
  r.connect_and_subscribe();
  r.advance(1000);
  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  r.advance(3000);

  // The exact string, because `!= "None"` cannot tell the genuine new fault
  // from the stale 0x16 we failed to clear -- both are non-None, and this file
  // warns about that pattern elsewhere.
  TEST_ASSERT(r.link_fault.state == "Connection Timeout (0x08)",
              "a genuine drop after the release reports ITS OWN reason, rather "
              "than being masked until a readiness that may never come -- or "
              "showing the teardown we caused");
}

// Releasing must not spend someone else's reconnect-settle hold.
//
// That hold exists because a premature encryption request into a not-ready pump
// can fail with 0x61 and make ESP-IDF erase the bond -- and an erased bond
// strands the pump until someone re-pairs at the pump itself. Raised in review:
// suspend and release inside an ordinary disconnect's settle window, and the
// release was handing auto-connect straight back.
void test_releasing_inside_a_settle_window_does_not_reconnect_early() {
  std::cout << "\n=== Releasing respects an active settle hold ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(2000);

  // An ordinary drop, nothing to do with the suspend, arms the hold.
  r.disconnect(ESP_GATT_CONN_TIMEOUT);
  TEST_ASSERT(!r.client.mock_auto_connect(),
              "precondition: the settle hold is in force");

  // Suspend and release, both inside the window.
  r.component.set_suspended(true);
  r.component.set_suspended(false);

  TEST_ASSERT(!r.client.mock_auto_connect(),
              "the release left the settle hold alone rather than reconnecting "
              "into a pump that may not be ready");

  // ...and the settle path still finishes the job on its own.
  advertise_pump(r);
  r.advance(6000);
  TEST_ASSERT(r.client.mock_auto_connect(),
              "  ...and the settle timer restores it when the window elapses");
}

// A suspension is not an outage, and must not contaminate the gap histogram.
//
// LinkGapSampler::on_disconnect() deliberately records the interval up to an
// involuntary drop -- that interval is evidence about the link. A suspension is
// not: it ends because someone clicked. Recording it moves link_gaps_truncated,
// which is the trust check on every other number in that histogram. Raised in
// review, against an issue requirement that said suspension must not read as an
// outage.
void test_suspending_does_not_record_an_outage() {
  std::cout << "\n=== A suspension is not a gap sample ===" << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  r.advance(20000);   // real airtime, so the sampler is armed and running

  const float truncated_before = r.gaps_truncated.state;

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  r.advance(3000);

  TEST_ASSERT(r.gaps_truncated.state == truncated_before,
              "the deliberate teardown is not counted as a truncated interval, "
              "which is the statistic every other gap number is trusted "
              "against");
}

// Three things none of the tests above reach.
//
// 1. `set_suspended()` ends with evaluate_link_status(), which is what makes
//    the status flip on the CLICK rather than up to a poll interval later.
//    Delete that call and every other test still passes, because they all
//    advance time before reading.
// 2. The `Suspended` rung sits ahead of every other rung INCLUDING the ready
//    check. Move it below and the suite stays green, because every other test
//    suspends a session that is not ready yet.
// 3. The idempotence guard. Nothing double-suspends.
void test_suspending_a_ready_link_reports_immediately_and_once() {
  std::cout << "\n=== Suspending a READY link: immediate, and idempotent ==="
            << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(),
              "precondition: the pump reached READY, so the ready rung would "
              "otherwise win");
  TEST_ASSERT(r.link_status.state == "Connected", "  ...and reads Connected");

  const int before = r.client.mock_disconnect_calls();
  r.component.set_suspended(true);

  // No advance(), no loop(), no poll. Read it straight after the call.
  TEST_ASSERT(r.link_status.state == "Suspended",
              "the status flips on the click, and Suspended outranks the ready "
              "rung -- the session is still READY at this instant");

  // A second click RE-ISSUES the teardown. The first is fire-and-forget and can
  // fail to take -- esp_ble_gattc_close() tears down the ACL only if no other
  // client holds it, and can complete with no DISCONNECT at all -- so a switch
  // that reads ON over a live link must still be actionable. Releasing twice is
  // the case that must stay idempotent, and it is asserted below.
  r.component.set_suspended(true);
  TEST_ASSERT(r.client.mock_disconnect_calls() == before + 2,
              "  ...and suspending again retries the teardown rather than "
              "swallowing the click");

  r.component.set_suspended(false);
  const int after_release = r.client.mock_disconnect_calls();
  r.component.set_suspended(false);
  TEST_ASSERT(r.client.mock_disconnect_calls() == after_release,
              "  ...while releasing twice is a no-op, because its side effects "
              "are destructive on repeat");
}

// Disarming the gap sampler at the click is not enough on its own.
//
// Two events re-arm it inside the asynchronous teardown window, and the
// DISCONNECT behind them then samples -- moving `truncated_`, which is the
// trust check on every other number in the histogram and the exact statistic
// the disarm exists to protect.
void test_events_in_the_teardown_window_do_not_re_arm_the_sampler() {
  std::cout << "\n=== A suspension survives events in its teardown window ==="
            << std::endl;

  // Path A: a notification. LinkGapSampler::on_inbound() SELF-ARMS when it
  // finds the sampler disarmed, by design -- so one frame arriving between the
  // click and the disconnect undoes the disarm.
  {
    Rig r;
    arm_the_settle_path(r);
    r.setup();
    r.connect_and_subscribe();
    r.advance(20000);
    const float before = r.gaps_truncated.state;

    r.component.set_suspended(true);
    auto frame = with_crc({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0x00, 0x00});
    r.notify(frame);                       // lands in the teardown window
    r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
    r.advance(20000);                      // long enough for any sample to publish

    TEST_ASSERT(r.gaps_truncated.state == before,
                "a notification arriving between the click and the disconnect "
                "does not re-arm the sampler");
  }

  // Path B: an OPEN. BLEClientBase::disconnect() cannot close a link with no
  // conn_id yet -- it sets want_disconnect_ and returns -- and the OPEN that
  // then arrives is still dispatched to every node. Reachable whenever the
  // switch is flipped between connections.
  {
    Rig r;
    arm_the_settle_path(r);
    r.setup();
    r.connect_and_subscribe();
    r.advance(20000);
    r.disconnect(ESP_GATT_CONN_TIMEOUT);   // ordinary drop; sampler closed
    // Let that drop's count PUBLISH before taking the baseline. The sensor only
    // updates on a poll, so reading it straight after the disconnect captures a
    // stale value and the assertion then measures publish timing rather than
    // the counter -- which is how the first draft of this case "failed".
    r.advance(20000);
    const float before = r.gaps_truncated.state;

    r.component.set_suspended(true);
    r.open(ESP_GATT_OK);                   // the in-flight connection completes
    r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
    r.advance(20000);

    TEST_ASSERT(r.gaps_truncated.state == before,
                "an OPEN completing inside the suspension does not re-arm the "
                "sampler either");
  }
}

// A suspension is not a failed connection attempt.
//
// The disconnect handler counts one whenever the link had not yet carried a
// frame -- which is where suspending a just-opened connection lands. Three of
// them reach LINK_FAIL_K and Pump Link Status publishes "Reconnecting", which
// an automation reads as a fault.
void test_suspending_is_not_counted_as_a_failed_attempt() {
  std::cout << "\n=== Suspending is not a failed connection attempt ==="
            << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();

  // Three cycles, each suspending a connection that opened but never carried a
  // frame. Under the unguarded counter the third publishes "Reconnecting".
  for (int i = 0; i < 3; i++) {
    r.connect_and_subscribe();
    r.component.set_suspended(true);
    r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
    r.advance(1000);
    r.component.set_suspended(false);
    r.advance(1000);
  }

  TEST_ASSERT(r.link_status.state != "Reconnecting",
              "three suspend cycles do not accumulate into a Reconnecting "
              "diagnosis about a pump that refused nothing");
}

// Releasing under an EXISTING fault must not silence the surface for good.
//
// clear_last_failure() empties the string. It has to clear the hold with it --
// every other site that writes last_failure_ resets both, and failure_hold_admits()
// refuses any later write while a hold outranking NONE is in force. Clearing only
// the string leaves a surface that reads "None" and can never be written again,
// because a DATA hold is released only by an inbound notification and a broken
// link never delivers one.
//
// This is the common case rather than the rare one: a latched fault is usually
// WHY the operator is suspending the link to go look at the pump with something
// else.
void test_releasing_under_a_held_fault_does_not_silence_the_surface() {
  std::cout << "\n=== Release under a held fault leaves it writable ==="
            << std::endl;
  Rig r;
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "precondition: ready, so the latch is set");

  // Go deaf and let the data watchdog latch its reason, at DATA rank.
  // The surface shows a held reason only once the link is not healthy, so the
  // watchdog's recycle has to land before the string is visible -- same shape
  // as test_the_fault_is_visible_across_the_reconnect_it_describes().
  r.answer_writes = false;
  r.advance(120000, 120);
  r.disconnect(ESP_GATT_CONN_TERMINATE_PEER_USER);
  r.advance(30000, 30);
  TEST_ASSERT(r.link_fault.state.find("No data") != std::string::npos,
              "precondition: a DATA-rank fault is latched and on the surface");

  // The operator suspends to go poke the pump with something else, then releases.
  r.component.set_suspended(true);
  r.advance(5000);
  r.component.set_suspended(false);
  r.advance(2000);

  // The link keeps failing for real.
  for (int i = 0; i < 3; i++) {
    r.connect_and_subscribe();
    r.advance(2000);
    r.disconnect(ESP_GATT_CONN_TIMEOUT);
    r.advance(2000);
  }

  TEST_ASSERT(r.link_fault.state != "None",
              "a genuine fault after the release still reaches the surface -- "
              "the release cleared the hold along with the string, rather than "
              "leaving a surface nothing can ever write to again");

  // ...and the stronger claim: the pre-existing diagnosis is not erased at all.
  // Releasing clears only the local-host teardown this component caused. The
  // worst case for getting this wrong is `Encryption Start Failed (0x61)`,
  // which docs/configuration.md calls not recoverable over the air and whose
  // remedy is to walk to the pump with the GO app -- so the switch is reached
  // for exactly when that string is showing.
  Rig r2;
  r2.setup();
  r2.connect_and_subscribe();
  TEST_ASSERT(r2.run_until_ready(), "precondition: ready");
  r2.answer_writes = false;
  r2.advance(120000, 120);
  r2.disconnect(ESP_GATT_CONN_TERMINATE_PEER_USER);
  r2.advance(30000, 30);
  const std::string latched = r2.link_fault.state;
  TEST_ASSERT(latched.find("No data") != std::string::npos,
              "precondition: a real diagnosis is on the surface");

  r2.component.set_suspended(true);
  r2.advance(5000);
  r2.component.set_suspended(false);
  r2.advance(5000);

  TEST_ASSERT(r2.link_fault.state == latched,
              "the diagnosis that was already showing survives the toggle -- "
              "the release forgets its own teardown, not somebody else's fault");
}

// A connection that opens while suspended is torn down again, not adopted.
//
// ESPHome reads auto_connect only at advertisement match, and disconnect()
// cannot close a link with no conn_id yet -- so an in-flight connect completes
// and the OPEN reaches the component. Adopting it puts the session connected,
// re-arms the watchdogs, resumes polling against a pump the operator believes
// they have handed over, and leaves the status reading "Suspended" over a live
// link: the exact failure the switch exists to prevent, with the UI asserting
// the opposite.
void test_an_open_during_a_suspension_is_torn_down_not_adopted() {
  std::cout << "\n=== An open during a suspension is not adopted ==="
            << std::endl;
  Rig r;
  arm_the_settle_path(r);
  r.setup();
  r.connect_and_subscribe();
  TEST_ASSERT(r.run_until_ready(), "precondition: ready before we suspend");

  r.component.set_suspended(true);
  r.disconnect(ESP_GATT_CONN_TERMINATE_LOCAL_HOST);
  r.advance(2000);
  TEST_ASSERT(!r.ready_is_on(), "precondition: Pump Ready went off");

  // The in-flight connect completes anyway.
  const int disconnects_before = r.client.mock_disconnect_calls();
  r.open(ESP_GATT_OK);
  r.advance(60000, 60);

  TEST_ASSERT(r.client.mock_disconnect_calls() > disconnects_before,
              "the open is torn down again rather than adopted");
  TEST_ASSERT(!r.ready_is_on(),
              "  ...so the node does not come back READY against a pump it was "
              "asked to let go of");
  TEST_ASSERT(r.link_status.state == "Suspended",
              "  ...and the status still says Suspended, which is true rather "
              "than a claim made over a live link");
}

int main() {
  // Pinned so the local<->UTC mapping in the clock fixtures is the same on
  // every CI machine (issue #270's tests are the first here to have one). The
  // conversion itself is covered at non-zero offsets in
  // tests/test_schedule_service.cpp; nothing else in this file reads a zone.
  setenv("TZ", "UTC", 1);
  tzset();

  std::cout << "===========================================================" << std::endl;
  std::cout << "  Component BLE Wiring Test Suite" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_the_scan_filter_identifies_the_pump();
  test_a_failed_open_does_not_by_itself_stop_the_sequence();
  test_pump_ready_is_off_until_the_chain_completes();
  test_gap_events_from_a_stranger_are_ignored();
  test_pairing_disabled_declines_even_the_pump();
  test_a_strangers_auth_failure_does_not_latch_a_fault();
  test_the_component_registers_with_the_ble_client();
  test_the_full_connection_reaches_pump_ready();
  test_the_read_chain_starts_without_waiting_for_a_poll();
  test_a_disconnect_inside_the_stabilize_window_cancels_it();
  test_a_reconnect_reaches_ready_exactly_once();
  test_one_cache_is_not_enough_for_ready();
  test_ready_clears_on_disconnect();
  test_a_disconnect_does_not_publish_a_drift_reading();
  test_suspend_drops_the_link_and_stops_reconnecting();
  test_release_restores_the_link();
  test_a_suspend_during_a_settle_window_is_not_undone_by_the_timer();
  test_a_suspended_link_reads_as_suspended_not_as_a_fault();
  test_releasing_a_long_suspension_does_not_report_unreachable();
  test_a_failure_after_release_is_not_hidden();
  test_releasing_inside_a_settle_window_does_not_reconnect_early();
  test_suspending_does_not_record_an_outage();
  test_suspending_a_ready_link_reports_immediately_and_once();
  test_events_in_the_teardown_window_do_not_re_arm_the_sampler();
  test_suspending_is_not_counted_as_a_failed_attempt();
  test_releasing_under_a_held_fault_does_not_silence_the_surface();
  test_an_open_during_a_suspension_is_torn_down_not_adopted();
  test_an_enabled_limiter_reaches_the_entities();
  test_a_limiter_actively_limiting_raises_the_binary_sensor();
  test_a_pump_without_the_limiter_family_reports_unknown();
  test_the_limiter_status_is_re_read_as_the_load_changes();
  test_the_config_chain_stops_at_the_first_failure();
  test_the_status_chain_stops_at_the_first_failure();
  test_a_healthy_pump_is_read_all_the_way_through();
  test_a_disconnect_drops_the_limiter_state();
  test_a_node_without_limiter_entities_does_not_read_them();
  test_link_gap_baseline_is_published_once_at_zero();
  test_gap_counters_do_not_publish_on_every_tick();
  test_a_quiet_link_fills_the_rungs_end_to_end();
  test_a_recycle_marks_the_interval_truncated_end_to_end();
  test_three_refused_connections_name_the_pump_not_the_radio();
  test_a_radio_drop_is_never_reported_as_a_refusal();
  test_the_pump_offering_to_pair_takes_the_fault_back_off();
  test_a_stall_does_not_bury_a_bond_erasing_pairing_failure();
  test_a_link_that_never_becomes_ready_is_recycled();
  test_the_readiness_fault_is_visible_while_the_session_is_ready();
  test_a_healthy_link_never_trips_the_readiness_watchdog();
  test_the_readiness_fault_survives_the_telemetry_that_masks_it();
  test_a_node_without_the_ready_entity_is_not_recycled_forever();
  test_readiness_recycles_reach_the_counter_an_automation_watches();
  test_the_fault_is_visible_across_the_reconnect_it_describes();
  test_the_default_names_the_fault_without_touching_the_link();

  test_the_drift_leg_publishes_the_drift_it_measured();
  test_the_drift_leg_leaves_a_good_reading_alone();
  test_a_manual_clock_read_without_a_node_clock_answers_unknown();
  test_build_event_window_refuses_without_a_node_clock();
  test_build_event_window_anchors_to_the_node_clock();
  test_build_event_window_refuses_an_unsynced_clock();
  test_build_event_window_does_not_encode_through_libc();
  test_the_vacation_display_is_rendered_in_local_time();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
