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

#include <cstring>
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
  // The gap histogram (issue #176 part 1). Attached by default so every test
  // here exercises the publish path, and because the frame-budget assertions
  // below are only meaningful against a rig that has them on.
  esphome::sensor::Sensor gaps_over[6];
  esphome::sensor::Sensor gaps_truncated;
  esphome::sensor::Sensor watch_time;
  AlphaHwrComponent component{&client};

  BLEService service;
  BLECharacteristic characteristic;

  Rig() {
    mock_millis = 1000;
    esp_gattc_mock().reset();
    esp_gap_mock().reset();
    component.set_ready_binary_sensor(&ready);
    component.set_pump_link_status_text_sensor(&link_status);
    component.set_link_gaps_over_15s_sensor(&gaps_over[0]);
    component.set_link_gaps_over_20s_sensor(&gaps_over[1]);
    component.set_link_gaps_over_30s_sensor(&gaps_over[2]);
    component.set_link_gaps_over_45s_sensor(&gaps_over[3]);
    component.set_link_gaps_over_60s_sensor(&gaps_over[4]);
    component.set_link_gaps_over_90s_sensor(&gaps_over[5]);
    component.set_link_gaps_truncated_sensor(&gaps_truncated);
    component.set_link_watch_time_sensor(&watch_time);
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
static std::vector<uint8_t> data_object_frame(uint8_t type_hi, uint8_t type_lo,
                                              const uint8_t *body, size_t body_len) {
  std::vector<uint8_t> f = {0x24, 0x00, 0xF8, 0xE7, 0x0A, 0x13, 0x00, 0x00,
                            type_hi, type_lo, 0x00, 0x00,
                            static_cast<uint8_t>(body_len)};
  f.insert(f.end(), body, body + body_len);
  f.push_back(0x00);
  f.push_back(0x00);
  f[1] = static_cast<uint8_t>(f.size() - 4);
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
      if (obj == 0x56) {
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

int main() {
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
  test_link_gap_baseline_is_published_once_at_zero();
  test_gap_counters_do_not_publish_on_every_tick();
  test_a_quiet_link_fills_the_rungs_end_to_end();
  test_a_recycle_marks_the_interval_truncated_end_to_end();

  std::cout << "\n==========================================" << std::endl;
  std::cout << "Results: " << tests_passed << " passed, " << tests_failed
            << " failed" << std::endl;
  std::cout << "==========================================" << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
