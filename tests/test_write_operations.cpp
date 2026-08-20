// Host tests for the write-operation layer (issue #92).
//
// Style follows test_schedule_service.cpp: real Transport/Session/
// ControlService/WriteOperationService compiled against the mock ESPHome SDK,
// a fake BLE writer capturing outgoing frames, and mock_millis driving the
// transport FSM plus a test-side scheduler standing in for set_timeout.
//
// A small PumpSim answers reads and applies (or ignores, or clamps) writes so
// each terminal status — accepted, clamped, rejected, timeout, superseded —
// can be driven end-to-end, and the one-terminal-event-per-operation
// invariant asserted throughout.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fixture_crc.h"
#include "../components/alpha_hwr/control_service.h"
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/session.h"
#include "../components/alpha_hwr/time_service.h"
#include "../components/alpha_hwr/transport.h"
#include "../components/alpha_hwr/write_operation_service.h"
#include "../components/alpha_hwr/schedule_codec.h"
#include "../components/alpha_hwr/codec.h"

uint32_t mock_millis = 0;
int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

using esphome::alpha_hwr::core::Session;
using esphome::alpha_hwr::core::Transport;
using esphome::alpha_hwr::services::ControlMode;
using esphome::alpha_hwr::services::ControlService;
using esphome::alpha_hwr::services::ScheduleService;
using esphome::alpha_hwr::services::TimeService;
using esphome::alpha_hwr::services::WriteCommand;
using esphome::alpha_hwr::services::WriteOperationService;
using esphome::alpha_hwr::services::WriteOrigin;
using esphome::alpha_hwr::services::WriteResult;
using esphome::alpha_hwr::services::WriteStatus;
using esphome::alpha_hwr::services::result_republishes_schedule;
namespace protocol = esphome::alpha_hwr::protocol;

// ---------------------------------------------------------------------------
// Pump simulator + test harness
// ---------------------------------------------------------------------------

struct PumpSim {
  uint8_t mode_byte{0x02};  // CONSTANT_SPEED
  bool enabled{true};
  // Native-unit setpoints per register sub-id (13 speed, 15 pressure, 39 flow)
  std::map<int, float> setpoints{{13, NAN}, {15, NAN}, {39, NAN}};
  float temp_min{25.0f}, temp_max{55.0f};
  bool autoadapt{false};
  // Trailing limit bytes of the Sub 430 struct (min/max on/off-time limits +
  // version tail; issue #106). Distinctive values so preservation is provable.
  uint8_t temp_limits_tail[5] = {0x05, 0x3C, 0x02, 0x3C, 0x01};
  // DHW on/off config (Obj 91 Sub 421): stored flow setpoint + live periods.
  uint8_t dhw_setpoint_raw[4] = {0x38, 0x84, 0x4F, 0x30};  // 1.0 gal/min native
  int cycle_on{5}, cycle_off{15};

  // Schedule state (Object 84)
  bool sched_enabled{false};
  uint8_t layers[5][42] = {};        // 7 days x 6 bytes per layer, zero = disabled
  uint8_t single_events[35][10] = {};
  // Slot count advertised in the ClockProgramOverview. 35 is the protocol
  // maximum and the historical default here; real pumps report fewer (5 on
  // the bench unit). Tests that let slot reads time out must lower this, or
  // 35 x 3 s of APDU timeouts outruns the 60 s operation watchdog and the
  // operation settles on the watchdog rather than on the read result.
  uint8_t max_single_events{35};
  uint8_t last_overview_write[10] = {};
  int overview_writes{0};

  // Real-time clock (Object 94). Held as a UTC epoch; the sim converts to and
  // from the local fields the wire carries, exactly as the pump would if its
  // clock and the node's timezone agreed. Seeded to 2026-08-07 12:00:00 UTC so
  // a test that never touches it still reads a valid time back.
  time_t clock_epoch{1786104000};   // value at clock_base_ms
  uint32_t clock_base_ms{0};
  // The pump's clock RUNS. Freezing it would make the operation's
  // "written time + elapsed" comparison drift by however long the confirm
  // ladder took, so a retry could turn a correct sync into a mismatch and the
  // tolerance would be silently absorbing a fixture artifact.
  time_t clock_now() const {
    return clock_epoch + static_cast<time_t>((mock_millis - clock_base_ms) / 1000);
  }
  bool respond_clock_reads{true};
  // Swallow this many clock reads before answering again. Lets a test stretch
  // the confirm ladder over real seconds without disabling readback for good.
  int drop_clock_reads{0};
  bool honor_clock_writes{true};
  // Seconds the pump's clock sits ahead of what a write asked for, applied on
  // top of an honored write. Models a pump that stores something other than
  // what it was told.
  int clock_write_skew_s{0};

  // Behavior switches
  bool respond_mode_reads{true};
  bool respond_obj91{true};
  // Swallow this many Obj 91 Sub 430 replies before answering again. Puts a
  // dropped readback in front of the confirm's retry, which is the only thing
  // CONFIG_MAX_ATTEMPTS is for -- and, since issue #234, the only way a write
  // the pump stored but never acknowledged still gets reported as a success.
  int drop_obj91_reads{0};
  bool ack_temp_write{true};
  uint8_t temp_ack_head{0x01};        // 0x81/0xC1/0x40 = the pump refuses the write
  bool honor_temp_writes{true};       // apply the Sub 430 temperature-range write
  // >0: the pump caps the stored max at this. Its own installer limits do
  // exactly this, and it is what tells a clamp apart from a write that never
  // landed (issue #234) -- both leave the readback disagreeing with the
  // request, and only the pre-write values separate them.
  float temp_max_clamp{0};
  bool honor_mode_change{true};       // apply 0x0A01 mode changes
  // Answer the unfused 0x0A01 mode write with a short ACK, and how late.
  //
  // Default ON because the pump does it: 12 mode writes in the de-duplicated
  // reference captures, all 12 acknowledged, 38-85 ms. (They are only visible
  // once the capture is reassembled -- a mode write is a 22-byte frame against a
  // 20-byte ATT payload, so it spans two packets. See
  // resources/traffic_capture/README.md, which also records why this was once
  // written down as 31.)
  //
  // A mock that answers less than the pump does is not the safe default here:
  // it would make the mode command time out on every write, which under the
  // reply-debt guard (issue #248) costs the config write behind it its own
  // acknowledgement -- so the code guarding against a real hazard would look
  // like it was causing one.
  //
  // The delay is what issue #248 is about: a mode reply arriving after
  // CONFIG_STEP2_DELAY_MS lands inside the config write's window and is
  // byte-identical to that write's own acknowledgement.
  bool ack_mode_write{true};
  uint32_t mode_ack_delay_ms{0};
  // The other Class 10 SETs this harness sees, answered for exactly the same
  // reason and defaulted ON for exactly the same reason (issue #253). Every
  // Class 10 SET in resources/traffic_capture is acknowledged -- 195 writes
  // across 20 distinct address shapes, all with the identical nine bytes
  // `24 05 F8 E7 0A 01 00 AE A2`, 36-193 ms. A simulator that stayed silent
  // would make each of these writes time out, and each timeout records a reply
  // debt that costs the NEXT write its acknowledgement.
  bool ack_control_write{true};       // the fused Obj 0601 write
  bool ack_setpoint_write{true};      // the OpSpec 0x88 register write
  bool ack_clock_write{true};         // Obj 94 Sub 100
  // The Object 84 writes -- schedule layer, single event, overview/commit. All
  // three asked the transport for a reply carrying a type, which a SET reply
  // cannot carry, so all three timed out on every write until issue #253.
  bool ack_object84_writes{true};
  bool honor_setpoint_writes{true};   // apply setpoint values from 0601/register writes
  bool obj91_includes_limits{true};   // firmware echoes the 5 limit tail bytes
  bool respond_dhw_reads{true};       // reply to Obj 91 Sub 421 reads
  // Swallow this many Obj 91 Sub 421 replies before answering again -- the
  // Sub 430 counterpart of drop_obj91_reads, for the same retry ladder.
  int drop_dhw_reads{0};
  bool honor_dhw_writes{true};        // apply Sub 421 writes
  bool ack_dhw_write{true};           // answer the Sub 421 write at all
  int dhw_min_off{0};                 // >0: pump clamps off_period to this floor
  bool honor_dhw_flow_writes{true};   // apply asserted setpoint bytes (issue #107)
  float dhw_flow_clamp_native{0};     // >0: pump caps the stored flow (m³/s)
  bool respond_overview_reads{true};
  // Drop layer / single-event read replies so the APDU times out (issue #136).
  // A set of these lets a test model the all-fail and partial-fail cases that
  // the read-all chains used to report as success.
  bool respond_layer_reads{true};
  std::set<uint8_t> drop_layer_reads;        // specific layers to drop
  bool respond_single_event_reads{true};
  bool honor_layer_writes{true};
  bool honor_overview_writes{true};
  // A pump that disables a day by clearing its ENABLED byte and leaving the
  // hour/minute bytes alone, so a cleared cell reads back with stale times
  // rather than zeros. Nothing in the protocol requires a pump to zero them,
  // and confirm_schedule_entry_ deliberately does not look at the times of a
  // day it asked to be off.
  bool keep_times_when_disabling{false};
  // >0: the pump stores this action byte whatever the write asked for, keeping
  // the window and enabled flag intact. Models the one divergence the confirm
  // used to ignore -- a Stop written and a Run stored, or the reverse.
  uint8_t force_single_event_action{0};
  // A pump that disables a single-event slot by clearing its ENABLED byte and
  // leaving the action and timestamps behind, so a cleared slot reads back with
  // stale content rather than zeros. Nothing in the protocol requires zeroing,
  // and the confirm deliberately does not look at the content of a slot it
  // asked to be off.
  bool keep_single_event_content_when_clearing{false};
  // Swallow this many layer writes, applying each one only when the NEXT layer
  // read arrives. Lets a test put a confirm readback in front of the pump's
  // own commit, which is the only thing the mismatch retry ladder is for.
  int defer_layer_writes{0};
  int8_t deferred_layer{-1};
  uint8_t deferred_layer_data[42] = {};
  // Control source as the pump reports it in Object 86 Sub 7: 2 =
  // Remote/Digital, 1 = Local/Panel, 0 = the unrecognized byte that must not
  // move the cached state.
  uint8_t control_source{2};
  bool ack_class3_remote{true};       // reply to Class 3 remote enable/disable
  bool reject_class3_remote{false};   // reply with the [03 01 xx] descriptor nack
  bool apply_class3_remote{true};     // actually change control_source
  bool ack_class3{true};              // reply to Class 3 START/STOP at all
  bool reject_class3{false};          // reply with the [03 01 xx] descriptor nack
  bool apply_class3{true};            // actually change run state on START/STOP
  std::function<float(int sub, float native)> transform;  // clamping hook

  int sub_for_mode() const {
    if (mode_byte == 0x02) return 13;
    if (mode_byte == 0x00 || mode_byte == 0x01) return 15;
    if (mode_byte == 0x08) return 39;
    return -1;
  }
  void apply_setpoint(int sub, float native) {
    if (!honor_setpoint_writes || sub < 0) return;
    setpoints[sub] = transform ? transform(sub, native) : native;
  }
};

// Accumulated across every Harness in this file, so the check below covers
// every write any test in it performs rather than one representative case.
static int g_apdu_length_violations = 0;
static std::vector<std::string> g_apdu_length_violation_detail;

/// What the node's wall clock reads the instant a Harness is built:
/// 2026-08-07 12:00:00 UTC. `PumpSim::clock_epoch` happens to carry the same
/// literal, and the two are unrelated -- nothing below reads the pump's clock,
/// verified by driving the sim's to 2000 and to 2040 without moving a single
/// assertion. Change one and the other does not follow. Named because the single-event tests express their
/// fixtures relative to it -- which slots the picker may recycle is a question
/// about NOW, not about the event being written (issue #262), so a fixture
/// whose timestamps sit in 1970 says something different from what it looks
/// like it says.
static constexpr time_t NODE_EPOCH_AT_BOOT = 1786104000;

struct Harness {
  Transport transport;
  Session session;
  ControlService control{transport, session};
  ScheduleService schedule{transport, session};
  // The node's own wall clock, as ESPHome's time component would supply it.
  // Built with -DUSE_TIME so TimeService::current_time() is the real function
  // here, not its stub -- SET_CLOCK's confirm reads this on every readback.
  esphome::time::RealTimeClock node_clock;
  TimeService time_service{&transport};
  // Epoch the node clock reads at node_clock_base_ms; advance() carries it
  // forward with mock time so the confirm sees two clocks that both run.
  time_t node_epoch{NODE_EPOCH_AT_BOOT};
  uint32_t node_clock_base_ms{0};
  WriteOperationService write_op{control, schedule, time_service};
  PumpSim sim;

  bool ready{true};
  int commit_count{0};

  std::vector<WriteResult> results;

  // Frame statistics for wire-shape assertions
  int frames_0601{0};       // fused control writes
  int frames_layer_write{0};  // whole-layer 42-byte schedule writes
  int frames_0a01{0};       // unfused mode changes
  int frames_register{0};   // 0x84 setpoint register writes
  int frames_class3_run{0}; // Class 3 START/STOP commands
  int frames_class3_remote{0}; // Class 3 remote enable/disable commands
  int frames_dhw_read{0};   // Obj 91 Sub 421 reads
  int frames_dhw_write{0};   // Obj 91 Sub 421 writes
  int frames_temp_write{0};  // Obj 91 Sub 430 temperature-range writes (OpSpec 0x97)
  // When each of those reached the pump. The step-2 timing is a claim the
  // MODE_ACK_TIMEOUT_MS comment makes -- an answered mode write must not delay
  // the config write behind it -- and a test that does not look at the clock
  // cannot check it.
  uint64_t last_0a01_ms{0};
  uint64_t last_temp_write_ms{0};
  uint64_t last_register_write_ms{0};
  uint64_t last_clock_write_ms{0};
  int frames_clock_read{0};   // Obj 94 Sub 101 reads
  int frames_clock_write{0};  // Obj 94 Sub 100 writes
  std::vector<uint8_t> last_clock_write;  // the whole 22-byte clock-write APDU
  std::vector<uint8_t> last_0601_setpoint_bytes;
  std::vector<uint8_t> last_dhw_write_setpoint;
  std::vector<uint8_t> last_temp_write_tail;

  struct Task { uint64_t due; std::function<void()> fn; };
  std::vector<Task> tasks;
  struct Injection { uint64_t due; std::vector<uint8_t> frame; };
  std::vector<Injection> injections;

  std::vector<uint8_t> out_buf;  // outgoing frame reassembly

  Harness() {
    // mock_millis is a file-global that keeps climbing across tests, so the
    // sim's running clock has to be based at whatever "now" this harness was
    // built at, not at zero.
    sim.clock_base_ms = mock_millis;
    node_clock_base_ms = mock_millis;
    time_service.set_time_id(&node_clock);
    tick_node_clock();
    session.on_ready();
    control.set_schedule_callback([this](std::function<void()> fn, uint32_t delay) {
      tasks.push_back({mock_millis + delay, std::move(fn)});
    });
    control.set_config_commit_callback([this]() { commit_count++; });
    write_op.set_schedule_callback([this](std::function<void()> fn, uint32_t delay) {
      tasks.push_back({mock_millis + delay, std::move(fn)});
    });
    write_op.set_ready_check([this]() { return ready; });
    write_op.set_result_callback([this](const WriteResult &r) { results.push_back(r); });
    transport.set_write_callback([this](const uint8_t *data, size_t len) -> bool {
      on_outgoing_chunk(data, len);
      return true;
    });
    // Frames the transport did not match to a command end up here. A short
    // Class 10 acknowledgement reaching this point is an acknowledgement no
    // command claimed -- which is the whole of issue #248, seen from the other
    // end: every SET reply is byte-identical, so one left lying about is one the
    // next Class 10 write can be handed. Counted rather than asserted here,
    // because the reply-debt path deliberately lets a genuinely late frame
    // through after consuming it (issue #254); the tests that assert zero are
    // the ones running against a pump that answers on time.
    transport.set_packet_callback([this](const uint8_t *data, size_t len) {
      if (len >= 6 && data[4] == 0x0A && (data[5] & 0x3F) <= 1) stray_short_acks++;
    });
  }

  /// Short Class 10 acknowledgements that reached the packet callback because
  /// no queued command consumed them. See the constructor.
  int stray_short_acks{0};

  // -- Outgoing traffic: reassemble chunks into frames, hand to the sim
  void on_outgoing_chunk(const uint8_t *data, size_t len) {
    if (len > 0 && data[0] == 0x27) out_buf.clear();
    out_buf.insert(out_buf.end(), data, data + len);
    if (out_buf.size() >= 2) {
      size_t expected = static_cast<size_t>(out_buf[1]) + 4;
      if (out_buf.size() >= expected) {
        handle_frame(out_buf);
        out_buf.clear();
      }
    }
  }

  /// Every APDU must declare the payload length it actually carries.
  ///
  /// Byte 1 is `0booLLLLLL`: the operation in bits 7-6, the payload byte count
  /// in bits 5-0. The payload is everything after the class and OpSpec bytes,
  /// so `(opspec & 0x3F) == apdu_len - 2` for every request this component
  /// sends -- GET, SET and command alike.
  ///
  /// Checked here rather than asserted per-write because that is what makes it
  /// a net: it catches any frame the component builds, including ones added
  /// later. It was added after a single-event schedule write shipped for a long
  /// time declaring 51 bytes while carrying 19, which the existing tests could
  /// not see because their mock matched on the wrong OpSpec value too.
  void check_apdu_length(const uint8_t *apdu, size_t apdu_len) {
    if (apdu_len < 2) return;
    const size_t declared = apdu[1] & 0x3F;
    const size_t actual = apdu_len - 2;
    if (declared == actual) return;
    g_apdu_length_violations++;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "class 0x%02X opspec 0x%02X declares %zu payload bytes, carries %zu",
             apdu[0], apdu[1], declared, actual);
    // Distinct shapes only -- one bad frame sent by fifty tests is one defect.
    const std::string entry(buf);
    for (const auto &seen : g_apdu_length_violation_detail)
      if (seen == entry) return;
    g_apdu_length_violation_detail.push_back(entry);
  }

  void handle_frame(const std::vector<uint8_t> &frame) {
    if (frame.size() < 6) return;
    const uint8_t *apdu = frame.data() + 4;
    size_t apdu_len = frame.size() - 6;
    check_apdu_length(apdu, apdu_len);

    // Class 3 run-state commands: [0x03, 0x81, 0x06 START | 0x05 STOP].
    if (apdu_len >= 3 && apdu[0] == 0x03 && apdu[1] == 0x81 &&
        (apdu[2] == 0x05 || apdu[2] == 0x06)) {
      frames_class3_run++;
      if (sim.reject_class3) {
        inject({0x24, 0x05, 0xF8, 0xE7, 0x03, 0x01, 0xAC, 0xAA, 0xBB});
        return;
      }
      if (sim.apply_class3) sim.enabled = (apdu[2] == 0x06);
      if (sim.ack_class3) {
        inject({0x24, 0x04, 0xF8, 0xE7, 0x03, 0x00, 0xAA, 0xBB});
      }
      return;
    }

    // Class 3 remote-mode commands: [0x03, 0x81, 0x07 enable | 0x08 disable].
    if (apdu_len >= 3 && apdu[0] == 0x03 && apdu[1] == 0x81 &&
        (apdu[2] == 0x07 || apdu[2] == 0x08)) {
      frames_class3_remote++;
      if (sim.reject_class3_remote) {
        inject({0x24, 0x05, 0xF8, 0xE7, 0x03, 0x01, 0xAC, 0xAA, 0xBB});
        return;
      }
      if (sim.apply_class3_remote) sim.control_source = (apdu[2] == 0x07) ? 2 : 1;
      if (sim.ack_class3_remote) {
        inject({0x24, 0x04, 0xF8, 0xE7, 0x03, 0x00, 0xAA, 0xBB});
      }
      return;
    }

    if (apdu_len < 2 || apdu[0] != 0x0A) return;

    uint8_t opspec = apdu[1];
    if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 0x56 && apdu[3] == 0x00 && apdu[4] == 0x07) {
      // Mode read (Obj 86 Sub 7)
      if (sim.respond_mode_reads) inject_mode_notification();
    } else if (opspec == 0x90 && apdu_len >= 18 && apdu[2] == 0x56 && apdu[4] == 0x06 && apdu[5] == 0x01) {
      // Fused control write (0x0601): flag + mode + setpoint
      frames_0601++;
      last_0601_setpoint_bytes.assign(apdu + 14, apdu + 18);
      sim.enabled = (apdu[12] == 0x00);
      sim.mode_byte = apdu[13];
      float sp = protocol::decode_float_be(apdu + 14);
      if (!std::isnan(sp)) sim.apply_setpoint(sim.sub_for_mode(), sp);
      if (sim.ack_control_write) inject_short_ack();
    } else if (opspec == 0x90 && apdu_len >= 18 && apdu[2] == 0x56 && apdu[4] == 0x0A && apdu[5] == 0x01) {
      // Unfused mode change (0x0A01, PR #98)
      frames_0a01++;
      last_0a01_ms = mock_millis;
      if (sim.honor_mode_change) sim.mode_byte = apdu[13];
      if (sim.ack_mode_write) {
        if (sim.mode_ack_delay_ms == 0) {
          inject_short_ack();
        } else {
          tasks.push_back({mock_millis + sim.mode_ack_delay_ms,
                           [this]() { inject_short_ack(); }});
        }
      }
    } else if (opspec == 0x88 && apdu_len >= 10) {
      // Setpoint register write
      frames_register++;
      last_register_write_ms = mock_millis;
      int sub = (apdu[2] << 8) | apdu[3];
      sim.apply_setpoint(sub, protocol::decode_float_be(apdu + 6));
      if (sim.ack_setpoint_write) inject_short_ack();
    } else if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 91 && apdu[3] == 0x01 && apdu[4] == 0xAE) {
      // Obj 91 Sub 430 config read
      if (sim.drop_obj91_reads > 0) {
        sim.drop_obj91_reads--;
      } else if (sim.respond_obj91) {
        inject_obj91_response();
      }
    } else if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 91 && apdu[3] == 0x01 && apdu[4] == 0xA5) {
      // Obj 91 Sub 421 DHW config read (issue #106)
      frames_dhw_read++;
      if (sim.drop_dhw_reads > 0) {
        sim.drop_dhw_reads--;
      } else if (sim.respond_dhw_reads) {
        inject_dhw_response();
      }
    } else if (opspec == 0x97 && apdu_len >= 25) {
      // Temperature-range config write; capture the limits tail for the
      // preservation assertion (issue #106).
      frames_temp_write++;
      last_temp_write_ms = mock_millis;
      bool aa = apdu[11] != 0;
      float mn = protocol::decode_float_be(apdu + 12);
      float mx = protocol::decode_float_be(apdu + 16);
      if (sim.honor_temp_writes) {
        sim.autoadapt = aa;
        sim.temp_min = mn;
        if (sim.temp_max_clamp > 0 && mx > sim.temp_max_clamp) mx = sim.temp_max_clamp;
        sim.temp_max = mx;
      }
      last_temp_write_tail.assign(apdu + 20, apdu + 25);
      if (sim.ack_temp_write) inject_short_ack(sim.temp_ack_head);
    } else if (opspec == 0x8F && apdu_len >= 17 && apdu[2] == 0x5B && apdu[3] == 0x01 && apdu[4] == 0xA5) {
      // DHW config write (Obj 91 Sub 421, type 985, GO-app frame shape)
      frames_dhw_write++;
      last_dhw_write_setpoint.assign(apdu + 11, apdu + 15);
      if (sim.honor_dhw_writes) {
        sim.cycle_on = apdu[15];
        sim.cycle_off = apdu[16];
        if (sim.dhw_min_off > 0 && sim.cycle_off < sim.dhw_min_off) sim.cycle_off = sim.dhw_min_off;
        if (sim.honor_dhw_flow_writes) {
          float native = protocol::decode_float_be(apdu + 11);
          if (sim.dhw_flow_clamp_native > 0 && native > sim.dhw_flow_clamp_native) {
            native = sim.dhw_flow_clamp_native;
          }
          // Re-encode after the cap so the sim's stored bytes stay exact.
          protocol::encode_float_be(native, sim.dhw_setpoint_raw);
        }
      }
      if (sim.ack_dhw_write) inject_short_ack();
    } else if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 0x5E && apdu[3] == 0x00 &&
               apdu[4] == 0x65) {
      // Obj 94 Sub 101 DateTimeActual read
      frames_clock_read++;
      if (sim.drop_clock_reads > 0) {
        sim.drop_clock_reads--;
      } else if (sim.respond_clock_reads) {
        inject_clock_response();
      }
    } else if (opspec == 0x94 && apdu_len >= 22 && apdu[2] == 0x5E && apdu[3] == 0x00 &&
               apdu[4] == 0x64) {
      // Obj 94 Sub 100 DateTimeConfig write. The pump DOES answer it -- nine
      // instances in resources/traffic_capture, all acknowledged in 38-90 ms --
      // and the firmware waits for that answer since issue #253. The operation
      // still confirms by reading Sub 101 back; the acknowledgement is not the
      // verdict, it is just spent on the write that earned it.
      frames_clock_write++;
      last_clock_write_ms = mock_millis;
      last_clock_write.assign(apdu, apdu + 22);
      if (sim.honor_clock_writes) {
        struct tm t {};
        t.tm_year = ((apdu[12] << 8) | apdu[13]) - 1900;
        t.tm_mon = apdu[14] - 1;
        t.tm_mday = apdu[15];
        t.tm_hour = apdu[16];
        t.tm_min = apdu[17];
        t.tm_sec = apdu[18];
        t.tm_isdst = -1;
        sim.clock_epoch = ::mktime(&t) + sim.clock_write_skew_s;
        sim.clock_base_ms = mock_millis;
      }
      if (sim.ack_clock_write) inject_short_ack();
    } else if (apdu[2] == 84 && apdu_len >= 5) {
      uint16_t sub = (apdu[3] << 8) | apdu[4];
      if (opspec == 0x03) {
        // Object 84 reads
        if (sub == 1) {
          if (sim.respond_overview_reads) inject_overview_frame();
        } else if (sub >= 1000 && sub <= 1004) {
          uint8_t layer = static_cast<uint8_t>(sub - 1000);
          // A deferred write lands here: the read that follows it answers with
          // the OLD image, and the write takes effect only afterwards, so the
          // read after that one sees it.
          bool answer_stale = sim.deferred_layer == static_cast<int8_t>(layer);
          if (sim.respond_layer_reads && !sim.drop_layer_reads.count(layer))
            inject_layer_frame(layer);
          if (answer_stale) {
            memcpy(sim.layers[layer], sim.deferred_layer_data, 42);
            sim.deferred_layer = -1;
          }
        } else if (sub >= 900 && sub < 935) {
          if (sim.respond_single_event_reads)
            inject_single_event_frame(static_cast<uint8_t>(sub - 900));
        }
      } else if (opspec == 0xB3 && sub >= 1000 && sub <= 1004 && apdu_len >= 53) {
        // Whole-layer write (42 bytes at apdu+11)
        frames_layer_write++;
        if (sim.defer_layer_writes > 0) {
          // Model a pump that takes the frame and applies it a readback later,
          // so the first confirm sees the OLD image. Only the confirm retry
          // ladder gets the operation past that.
          sim.defer_layer_writes--;
          sim.deferred_layer = static_cast<int8_t>(sub - 1000);
          memcpy(sim.deferred_layer_data, apdu + 11, 42);
        } else if (sim.honor_layer_writes) {
          uint8_t was[42];
          memcpy(was, sim.layers[sub - 1000], 42);
          uint8_t *img = sim.layers[sub - 1000];
          memcpy(img, apdu + 11, 42);
          if (sim.keep_times_when_disabling) {
            // Model a pump that clears a day's ENABLED byte but leaves the
            // hour/minute bytes as they were -- a disabled cell whose payload
            // is stale rather than zeroed. The confirm must not read those
            // leftover times as a failed clear.
            for (int d = 0; d < 7; d++) {
              if (was[d * 6] != 0 && img[d * 6] == 0) {
                memcpy(img + d * 6 + 2, was + d * 6 + 2, 4);
              }
            }
          }
        }
        // A short ACK, not the object back. Every SET this pump answers, it
        // answers with the same nine bytes: "the SET operation never returns
        // anything but the APDU Head" (App. Prog. Manual fig 3.5 note 1), and
        // resources/traffic_capture has 20 layer writes, all answered that way
        // in 36-142 ms. This simulator used to echo the layer object back,
        // because the firmware asked for type 0xDE01 -- which is how a write
        // that timed out on every attempt looked healthy from here (issue #253).
        if (sim.ack_object84_writes) inject_short_ack();
      } else if (opspec == 0x93 && sub >= 900 && sub < 935 && apdu_len >= 21) {
        // Single-event write (10 bytes at apdu+11)
        if (sim.honor_layer_writes) {
          uint8_t was[10];
          memcpy(was, sim.single_events[sub - 900], 10);
          uint8_t *slot = sim.single_events[sub - 900];
          memcpy(slot, apdu + 11, 10);
          if (sim.force_single_event_action != 0) {
            slot[1] = sim.force_single_event_action;
          }
          if (sim.keep_single_event_content_when_clearing && was[0] != 0 && slot[0] == 0) {
            memcpy(slot + 1, was + 1, 9);
          }
        }
        if (sim.ack_object84_writes) inject_short_ack();
      } else if (opspec == 0x93 && sub == 1 && apdu_len >= 21) {
        // ClockProgramOverview write (set_state / configuration commit)
        sim.overview_writes++;
        memcpy(sim.last_overview_write, apdu + 11, 10);
        if (sim.honor_overview_writes) sim.sched_enabled = apdu[15] != 0;
        if (sim.ack_object84_writes) inject_short_ack();
      }
    }
  }

  // -- Response builders. Every fixture below writes a placeholder in its last
  // two bytes; inject() stamps the real CRC over it, so the fixtures do not
  // each have to carry a hand-computed checksum. Transport drops a bad-CRC
  // frame, so without this every response here would simply vanish.
  void inject(std::vector<uint8_t> frame, uint32_t delay = 20) {
    injections.push_back({mock_millis + delay, with_crc(std::move(frame))});
  }

  void inject_mode_notification() {
    // OpSpec 0x0E notification: Sub 0x0001 (bytes 6-7), Obj 0x2F01 (bytes 8-9)
    // payload [00 00 07][control_source][operation_mode][control_mode][setpoint f32be]
    std::vector<uint8_t> f = {0x24, 18, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01,
                              0x00, 0x00, 0x07, sim.control_source,
                              static_cast<uint8_t>(sim.enabled ? 0x00 : 0x01),
                              sim.mode_byte, 0, 0, 0, 0, 0xAA, 0xBB};
    int sub = sim.sub_for_mode();
    float sp = (sub >= 0) ? sim.setpoints[sub] : NAN;
    protocol::encode_float_be(sp, f.data() + 16);
    inject(std::move(f));
  }

  void inject_obj91_response() {
    // OpSpec 0x15 workaround path: payload at byte 10.
    // payload [00 00 0E][autoadapt][min f32be][max f32be][limits tail x5]
    std::vector<uint8_t> payload = {0x00, 0x00, 0x0E, static_cast<uint8_t>(sim.autoadapt ? 1 : 0),
                                    0, 0, 0, 0, 0, 0, 0, 0};
    protocol::encode_float_be(sim.temp_min, payload.data() + 4);
    protocol::encode_float_be(sim.temp_max, payload.data() + 8);
    if (sim.obj91_includes_limits) {
      payload.insert(payload.end(), sim.temp_limits_tail, sim.temp_limits_tail + 5);
    }
    std::vector<uint8_t> f = {0x24, 0, 0xF8, 0xE7, 0x0A, 0x15, 0x00, 0x5B, 0x01, 0xAE};
    f.insert(f.end(), payload.begin(), payload.end());
    f.push_back(0xAA);
    f.push_back(0xBB);
    f[1] = static_cast<uint8_t>(f.size() - 4);
    inject(std::move(f));
  }

  void inject_dhw_response() {
    // Capture-verified Sub 421 reply: OpSpec 0x0D, [00][type 03D9][ver 01]
    // [size 00 00 06][setpoint f32][on][off].
    std::vector<uint8_t> f = {0x24, 0x11, 0xF8, 0xE7, 0x0A, 0x0D, 0x00, 0x03, 0xD9, 0x01,
                              0x00, 0x00, 0x06,
                              sim.dhw_setpoint_raw[0], sim.dhw_setpoint_raw[1],
                              sim.dhw_setpoint_raw[2], sim.dhw_setpoint_raw[3],
                              static_cast<uint8_t>(sim.cycle_on),
                              static_cast<uint8_t>(sim.cycle_off),
                              0xAA, 0xBB};
    inject(std::move(f));
  }

  // Obj 94 Sub 101 DateTimeActual reply, transcribed from the bench pump
  // (2026-08-15). Two captured samples, byte for byte:
  //
  //   00 00 0C 07 EA 08 0F 14 26 37 48 00 06 00 01   -> 2026-08-15 20:38:55
  //   00 00 0C 07 EA 08 0F 14 27 08 13 00 06 00 01   -> 2026-08-15 20:39:08
  //
  // Fifteen bytes as the command callback receives them: a three-byte size
  // header (00 00 0C = 12, and twelve bytes do follow), then year big-endian,
  // month, day, hour, minute, second, then five more. The first of those five
  // moved between the samples (0x48, 0x13) and the remaining four did not;
  // it is not identified here and the parser reads none of them.
  //
  // The tail is the reason this fixture is transcribed rather than trimmed to
  // the seven bytes the parser actually consumes. A fixture carrying only what
  // the parser reads proves the parser can read its own fixture; carrying what
  // the pump sends proves it tolerates the real reply.
  void inject_clock_response() {
    const time_t epoch = sim.clock_now();
    struct tm t {};
    ::localtime_r(&epoch, &t);
    const uint16_t year = static_cast<uint16_t>(t.tm_year + 1900);
    // Identity bytes 6-9 are 00 01 42 01, which is the measured value for
    // 94/101 and NOT what inject_data_object_frame() can express -- it pins
    // bytes 6-7 to 00 00, right for Object 84 and wrong here. Read as the
    // transport reads them, byte 6 is 00, bytes 7-8 are the type (0x0142 =
    // 322) and byte 9 is the version (1).
    //
    // Byte 5 is 0x13. In a real frame that field is the APDU body length, and
    // this frame is 27 bytes: 10 header + 15 body + 2 CRC, so 27 - 8 = 19 =
    // 0x13. It is the true value here rather than a constant that happens to
    // miss the register-read length blocklist.
    std::vector<uint8_t> f = {0x24, 0x17, 0xF8, 0xE7, 0x0A, 0x13,
                              0x00, 0x01, 0x42, 0x01,
                              0x00, 0x00, 0x0C,
                              static_cast<uint8_t>((year >> 8) & 0xFF),
                              static_cast<uint8_t>(year & 0xFF),
                              static_cast<uint8_t>(t.tm_mon + 1),
                              static_cast<uint8_t>(t.tm_mday),
                              static_cast<uint8_t>(t.tm_hour),
                              static_cast<uint8_t>(t.tm_min),
                              static_cast<uint8_t>(t.tm_sec),
                              // The captured tail, kept verbatim.
                              0x48, 0x00, 0x06, 0x00, 0x01,
                              0xAA, 0xBB};
    inject(std::move(f));
  }

  void inject_short_ack(uint8_t head = 0x01) {
    inject({0x24, 0x05, 0xF8, 0xE7, 0x0A, head, 0x00, 0xAA, 0xBB});
  }

  // Object 84 and Object 94 replies share one DataObject shape: OpSpec 0x13
  // (not in the register-read set), type at bytes 8-9, payload at byte 10 with
  // a [00 00 XX] size header the parsers skip. Byte 5 is the APDU body length
  // in real frames rather than the constant used here; nothing in the matching
  // path reads it except the register-read length blocklist, which 0x13 misses.
  void inject_data_object_frame(uint8_t obj_hi, uint8_t obj_lo, const uint8_t *body, size_t body_len) {
    std::vector<uint8_t> f = {0x24, 0, 0xF8, 0xE7, 0x0A, 0x13, 0x00, 0x00, obj_hi, obj_lo,
                              0x00, 0x00, static_cast<uint8_t>(body_len)};
    f.insert(f.end(), body, body + body_len);
    f.push_back(0xAA);
    f.push_back(0xBB);
    f[1] = static_cast<uint8_t>(f.size() - 4);
    inject(std::move(f));
  }

  void inject_overview_frame() {
    uint8_t overview[10] = {0x8C, sim.max_single_events, 0x05, 0x05,
                            static_cast<uint8_t>(sim.sched_enabled ? 1 : 0),
                            0x01, 0x00, 0x00, 0x00, 0x00};
    inject_data_object_frame(0xDA, 0x01, overview, 10);
  }

  void inject_layer_frame(uint8_t layer) {
    inject_data_object_frame(0xDE, 0x01, sim.layers[layer], 42);
  }

  void inject_single_event_frame(uint8_t slot) {
    inject_data_object_frame(0xDC, 0x01, sim.single_events[slot], 10);
  }

  // -- Time driver
  void tick_node_clock() {
    node_clock.set_epoch_for_test(node_epoch +
                                  static_cast<time_t>((mock_millis - node_clock_base_ms) / 1000));
  }

  /** Point the node's wall clock at `epoch` as of now; it runs from there. */
  void set_node_time(time_t epoch) {
    node_epoch = epoch;
    node_clock_base_ms = mock_millis;
    tick_node_clock();
  }

  void advance(uint32_t ms) {
    uint64_t end = mock_millis + ms;
    while (mock_millis < end) {
      mock_millis += 10;
      tick_node_clock();
      for (size_t i = 0; i < injections.size();) {
        if (injections[i].due <= mock_millis) {
          auto frame = injections[i].frame;
          injections.erase(injections.begin() + i);
          transport.on_notification(frame.data(), frame.size());
        } else {
          i++;
        }
      }
      transport.loop();
      bool ran = true;
      while (ran) {
        ran = false;
        for (size_t i = 0; i < tasks.size(); i++) {
          if (tasks[i].due <= mock_millis) {
            auto fn = tasks[i].fn;
            tasks.erase(tasks.begin() + i);
            fn();
            ran = true;
            break;
          }
        }
      }
    }
  }

  // Prime the control cache from the sim with a MODE read only.
  //
  // The parenthetical here used to claim "a mode read + obj91 read round trip".
  // It does not reach the Obj 91 read: get_mode_async() does not chain into it,
  // and temp_limits_known() is false after this returns. Use prime_temp_limits()
  // when a test needs the pump's on/off-time limits.
  /// Seed one slot on the simulated pump with an enabled event.
  ///
  /// The ten bytes are the wire layout -- enabled, action, begin BE32, end
  /// BE32 -- and writing them by hand is how a fixture ends up claiming a
  /// "live" event whose timestamps are in 1970. Which slots the picker may
  /// recycle is now judged against the node's wall clock (issue #262), so a
  /// fixture's timestamps have to mean what the test says they mean.
  void seed_single_event(uint8_t slot, uint8_t action, uint32_t begin, uint32_t end) {
    uint8_t *s = sim.single_events[slot];
    s[0] = 0x01;
    s[1] = action;
    s[2] = static_cast<uint8_t>(begin >> 24);
    s[3] = static_cast<uint8_t>(begin >> 16);
    s[4] = static_cast<uint8_t>(begin >> 8);
    s[5] = static_cast<uint8_t>(begin);
    s[6] = static_cast<uint8_t>(end >> 24);
    s[7] = static_cast<uint8_t>(end >> 16);
    s[8] = static_cast<uint8_t>(end >> 8);
    s[9] = static_cast<uint8_t>(end);
  }

  /// The begin timestamp the pump holds for @p slot, as the sim stored it.
  uint32_t sim_single_event_begin(uint8_t slot) const {
    const uint8_t *s = sim.single_events[slot];
    return (static_cast<uint32_t>(s[2]) << 24) | (static_cast<uint32_t>(s[3]) << 16) |
           (static_cast<uint32_t>(s[4]) << 8) | s[5];
  }

  void prime_cache() {
    control.get_mode_async(nullptr);
    advance(100);
  }

  /// Read Obj 91 Sub 430, which is what supplies the pump's own on/off-time
  /// LIMITS tail. prime_cache() does not reach it: the config read is a
  /// separate call, and get_mode_async() alone does not chain into it.
  ///
  /// Kept separate rather than folded into prime_cache() because the extra
  /// simulated time it needs perturbs the clock-drift assertions elsewhere in
  /// this file -- so only the tests that write a temperature range pay for it.
  ///
  /// Be precise about what this buys. test_temp_range_preserves_limits()
  /// already primed inline and already asserted the pump's real tail is echoed
  /// back -- it is the only test that kills a mutation of the echo itself, on
  /// this branch and before it. So calling this from the other two tests adds
  /// no coverage of the echo; it exists to stop the new limits guard rejecting
  /// writes those tests are about. It is a fixture repair the guard forced,
  /// not a coverage win.
  ///
  /// What was true before: those two tests were writing the historical
  /// constants (00 00 00 16 00) and nothing looked.
  void prime_temp_limits() {
    control.sync_cache_async(nullptr);
    advance(500);
  }

  int events_for(const std::string &op_id) {
    int n = 0;
    for (const auto &r : results) {
      if (r.op_id == op_id) n++;
    }
    return n;
  }
  const WriteResult *result_for(const std::string &op_id) {
    for (const auto &r : results) {
      if (r.op_id == op_id) return &r;
    }
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// The Object 86 Sub 7 read is ControlService's, so its response-matching
// arguments are pinned here, where the real call site runs -- not only at the
// transport. They were passed in the wrong order for a long time and the read
// matched anyway, through a fallback branch in Transport that has since been
// removed; with the fallback gone a swap turns every mode-dependent write into
// a timeout. Those failures are diffuse (36 assertions across this file), so
// this states the cause directly.
static void test_mode_read_arguments_are_not_swapped() {
  std::cout << "\n=== Obj 86 Sub 7: matching arguments in the right order ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();

  // prime_cache() drives a mode read to completion. If the call site's two
  // arguments were swapped, no reply would match and the cache would stay cold.
  TEST_ASSERT(h.control.is_mode_valid(),
              "the mode read matched its reply (arguments not swapped)");
  TEST_ASSERT(h.control.is_pump_enabled_valid(),
              "  ...and populated the run-state cache it feeds");
}

static void test_set_mode_accepted() {
  std::cout << "\n=== set_mode: accepted, unfused ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x00;  // CONSTANT_PRESSURE
  h.prime_cache();

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "m1");
  h.advance(12000);

  TEST_ASSERT(h.events_for("m1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("m1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->command == WriteCommand::SET_MODE, "command is set_mode");
  TEST_ASSERT(r && r->mode == ControlMode::CONSTANT_SPEED, "settled mode is constant_speed");
  TEST_ASSERT(h.frames_0a01 == 1, "mode change used the unfused 0x0A01 object");
  TEST_ASSERT(h.frames_0601 == 0, "no fused 0x0601 write was sent");
  TEST_ASSERT(h.frames_register == 0, "no setpoint register write was sent");
  TEST_ASSERT(h.control.get_current_mode() == ControlMode::CONSTANT_SPEED, "cache adopted the new mode");
}

static void test_set_mode_rejected() {
  std::cout << "\n=== set_mode: pump keeps old mode -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x00;
  h.sim.honor_mode_change = false;
  h.prime_cache();

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "m2");
  h.advance(30000);

  TEST_ASSERT(h.events_for("m2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("m2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("kept") != std::string::npos, "detail says the pump kept its mode");
  TEST_ASSERT(r && r->mode == ControlMode::CONSTANT_PRESSURE, "settled mode is the pump's actual mode");
  TEST_ASSERT(h.control.get_current_mode() == ControlMode::CONSTANT_PRESSURE,
              "cache recovered to the pump-reported mode");
}

static void test_set_setpoint_accepted() {
  std::cout << "\n=== set_setpoint: accepted ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 1700.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "s1");
  h.advance(12000);

  TEST_ASSERT(h.events_for("s1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("s1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && std::fabs(r->value - 2000.0f) < 0.5f, "settled value is 2000 RPM");
  TEST_ASSERT(r && r->enabled == 1, "settled enabled state reported");
  TEST_ASSERT(h.frames_register == 1, "one setpoint register write was sent");
}

static void test_set_setpoint_clamped() {
  std::cout << "\n=== set_setpoint: pump clamps -> clamped ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 2000.0f;
  h.sim.transform = [](int sub, float v) { return (sub == 13 && v < 1650.0f) ? 1650.0f : v; };
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 1500.0f, "s2");
  h.advance(12000);

  TEST_ASSERT(h.events_for("s2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("s2");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED, "status is clamped");
  TEST_ASSERT(r && std::fabs(r->value - 1650.0f) < 0.5f, "settled value is the clamped 1650");
  TEST_ASSERT(r && r->detail.find("1650") != std::string::npos, "detail reports the stored value");
}

static void test_set_setpoint_rejected_kept_old() {
  std::cout << "\n=== set_setpoint: pump keeps old value -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 2000.0f;
  h.sim.honor_setpoint_writes = false;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 1800.0f, "s3");
  h.advance(12000);

  TEST_ASSERT(h.events_for("s3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("s3");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && std::fabs(r->value - 2000.0f) < 0.5f, "settled value is the kept 2000");
}

static void test_set_setpoint_resolve_abort() {
  std::cout << "\n=== set_setpoint: enabled state unresolvable -> rejected (#45) ===" << std::endl;
  Harness h;
  h.sim.respond_mode_reads = false;  // enabled state can never be determined

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "s4");
  h.advance(20000);

  TEST_ASSERT(h.events_for("s4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("s4");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("enabled state") != std::string::npos, "detail explains the #45 abort");
  TEST_ASSERT(h.frames_0601 == 0 && h.frames_register == 0, "no write frames were sent");
}

static void test_set_enabled_unfused_class3() {
  // Class 3 START/STOP (ids from jfriend00's #92 bench findings) carries no
  // mode and no setpoint, so a cold setpoint cache is irrelevant by
  // construction: no 0601 frame is sent at all.
  std::cout << "\n=== set_pump_enabled: accepted via unfused Class 3 STOP ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = NAN;  // pump has a setpoint but never told us (cold cache)
  h.prime_cache();

  h.write_op.submit_set_enabled(false, "e1");
  h.advance(12000);

  TEST_ASSERT(h.events_for("e1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("e1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->enabled == 0, "settled enabled state is off");
  TEST_ASSERT(h.frames_class3_run == 1, "one Class 3 STOP was sent");
  TEST_ASSERT(h.frames_0601 == 0, "no fused 0x0601 write was sent (nothing to clobber)");
  TEST_ASSERT(h.frames_register == 0, "no setpoint register write was sent");
}

static void test_set_enabled_class3_nack() {
  std::cout << "\n=== set_pump_enabled: Class 3 descriptor nack -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.sim.reject_class3 = true;

  h.write_op.submit_set_enabled(true, "e2");
  h.advance(12000);

  TEST_ASSERT(h.events_for("e2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("e2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("rejected") != std::string::npos, "detail reports the nack");
  TEST_ASSERT(!h.sim.enabled || h.frames_class3_run == 1, "command was sent once and not applied");
}

static void test_set_enabled_class3_ack_timeout() {
  // No matchable ACK: the pump may still have applied the command, so the
  // readback decides. The sim applies the state but never acks.
  std::cout << "\n=== set_pump_enabled: ACK window closes -> readback decides ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.sim.ack_class3 = false;

  h.write_op.submit_set_enabled(false, "e3");
  h.advance(15000);

  TEST_ASSERT(h.events_for("e3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("e3");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "readback confirmed despite missing ACK");
  TEST_ASSERT(!h.sim.enabled, "pump is off");
}

static void test_set_enabled_rejected_reports_readback() {
  // Copilot review on #105: a rejected run-state write must carry the pump's
  // actual state from the readback, not the requested value (the request
  // survives in the requested_* echoes).
  std::cout << "\n=== set_pump_enabled: rejected result carries readback state ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.sim.apply_class3 = false;  // pump acks the STOP but stays running

  h.write_op.submit_set_enabled(false, "e4");
  h.advance(30000);

  TEST_ASSERT(h.events_for("e4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("e4");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("running") != std::string::npos, "detail reports the pump's state");
  TEST_ASSERT(r && r->enabled == 1, "settled enabled reflects the readback, not the request");
}

// ---------------------------------------------------------------------------
// SET_REMOTE_MODE (issue #46 / write-path audit).
//
// Before this command existed, remote mode was written by two standalone
// ControlService entry points that talked to the transport directly and
// confirmed themselves from the command ACK. None of that was reachable from
// a host test, so ControlService::handle_remote_mode_ack() -- the function
// that decided whether remote mode had taken effect -- had no coverage
// linked to production anywhere. These drive the shipped operation.
// ---------------------------------------------------------------------------
static void test_remote_mode_accepted() {
  std::cout << "\n=== set_remote_mode: accepted via control_source readback ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;  // Local/Panel
  h.prime_cache();

  h.write_op.submit_set_remote_mode(true, "rm1");
  h.advance(12000);

  TEST_ASSERT(h.events_for("rm1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->enabled == 1, "settled remote state is on");
  TEST_ASSERT(h.frames_class3_remote == 1, "one Class 3 remote-mode command was sent");
  TEST_ASSERT(h.frames_0601 == 0, "no fused control write was sent");
  TEST_ASSERT(h.control.get_remote_enabled(), "the service cache reports remote enabled");
}

static void test_remote_mode_nack() {
  std::cout << "\n=== set_remote_mode: descriptor nack -> rejected ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;
  h.prime_cache();
  h.sim.reject_class3_remote = true;

  h.write_op.submit_set_remote_mode(true, "rm2");
  h.advance(12000);

  TEST_ASSERT(h.events_for("rm2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("rejected") != std::string::npos, "detail reports the nack");
  TEST_ASSERT(h.sim.control_source == 1, "the pump's control source did not change");
  TEST_ASSERT(!h.control.get_remote_enabled(), "cache was not optimistically set");
}

static void test_remote_mode_ack_window_closes() {
  // The ACK is not the verdict. The old code took a missing ACK as "state
  // left unchanged" and never looked; here the pump applies the command and
  // stays silent, and the readback still confirms it.
  std::cout << "\n=== set_remote_mode: ACK window closes -> readback decides ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;
  h.prime_cache();
  h.sim.ack_class3_remote = false;

  h.write_op.submit_set_remote_mode(true, "rm3");
  h.advance(15000);

  TEST_ASSERT(h.events_for("rm3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm3");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "readback confirmed despite missing ACK");
  TEST_ASSERT(h.sim.control_source == 2, "pump is in Remote/Digital");
}

static void test_remote_mode_acked_but_not_applied() {
  // The inverse, and the case the ACK-only design got wrong in the other
  // direction: a clean [03 00] ACK that the pump did not act on.
  std::cout << "\n=== set_remote_mode: acked but not applied -> rejected ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;
  h.prime_cache();
  h.sim.apply_class3_remote = false;

  h.write_op.submit_set_remote_mode(true, "rm4");
  h.advance(30000);

  TEST_ASSERT(h.events_for("rm4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm4");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected despite the clean ACK");
  TEST_ASSERT(r && r->detail.find("Local/Panel") != std::string::npos,
              "detail reports the pump's actual control source");
  TEST_ASSERT(r && r->enabled == 0, "settled value reflects the readback, not the request");
}

static void test_remote_mode_unusable_control_source() {
  // control_source 0 is neither Remote nor Local, so it carries no
  // information and must not confirm anything. Without remote_state_valid_
  // the {false} default reads as an observed Local/Panel and a *disable*
  // settles ACCEPTED having read nothing -- so this asks for a disable.
  std::cout << "\n=== set_remote_mode: unusable control_source cannot confirm ===" << std::endl;
  Harness h;
  h.sim.control_source = 0;
  h.sim.apply_class3_remote = false;  // keep the 0; a disable would write 1
  h.prime_cache();

  h.write_op.submit_set_remote_mode(false, "rm5");
  h.advance(30000);

  TEST_ASSERT(h.events_for("rm5") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm5");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "an unreadable control source is not a confirmation");
  TEST_ASSERT(r && r->detail.find("control_source") != std::string::npos,
              "detail says why it could not confirm");
}

static void test_remote_mode_warm_cache_needs_fresh_observation() {
  // The sticky-validity hole: rm5 above covers a COLD cache, where
  // remote_state_valid_ is false and an uninterpretable control_source
  // cannot confirm anything. Warm the cache first and the same pump reply
  // used to settle ACCEPTED -- confirming a disable against a Local/Panel
  // reading taken before the command was ever sent.
  std::cout << "\n=== set_remote_mode: warm cache still needs a post-command reading ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;           // Local/Panel...
  h.prime_cache();                    // ...observed, so the cache is warm and valid
  h.sim.control_source = 0;           // pump now reports a source we cannot read
  h.sim.apply_class3_remote = false;

  h.write_op.submit_set_remote_mode(false, "rm9");
  h.advance(30000);

  TEST_ASSERT(h.events_for("rm9") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rm9");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "a warm cache does not stand in for a post-command reading");
  TEST_ASSERT(r && r->detail.find("control_source") != std::string::npos,
              "detail says why it could not confirm");
}

static void test_remote_mode_unreadable_source_is_not_rejection() {
  // The mirror case, and the one that would regress against the deleted
  // ACK path: the pump DID switch to Remote, but Sub 7 is the prioritized
  // source and reports some third value (scheduler, alarm, standby). The
  // cache still holds Local/Panel from before. Settling REJECTED here would
  // call a write that took effect a failure; the honest answer is that the
  // outcome is unknown.
  std::cout << "\n=== set_remote_mode: unreadable source is a timeout, not a rejection ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;
  h.prime_cache();
  h.sim.control_source = 101;  // ClockScheduler: neither Remote nor Local
  h.sim.apply_class3_remote = false;

  h.write_op.submit_set_remote_mode(true, "rm10");
  h.advance(30000);

  const WriteResult *r = h.result_for("rm10");
  TEST_ASSERT(h.events_for("rm10") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "status is timeout, not rejected");
  TEST_ASSERT(r && r->detail.find("Local/Panel") == std::string::npos,
              "does not claim the pump reports Local/Panel off a stale cache");
}

static void test_remote_mode_own_resource_key() {
  // Remote mode is orthogonal to run state: neither may supersede the other.
  std::cout << "\n=== set_remote_mode: own resource key ===" << std::endl;
  Harness h;
  h.sim.control_source = 1;
  h.sim.mode_byte = 0x02;
  h.prime_cache();

  h.write_op.submit_set_remote_mode(true, "rm6");   // starts immediately
  h.write_op.submit_set_enabled(false, "rm7");      // queues behind it
  h.write_op.submit_set_remote_mode(true, "rm8");   // supersedes rm7? must not
  h.advance(40000);

  TEST_ASSERT(h.events_for("rm7") == 1, "the queued run-state write settled");
  const WriteResult *r7 = h.result_for("rm7");
  TEST_ASSERT(r7 && r7->status != WriteStatus::SUPERSEDED,
              "a remote-mode write did not supersede the run-state write");
  TEST_ASSERT(h.frames_class3_run == 1, "the run-state command reached the wire");
}

static void test_supersede_detail_uses_origin() {
  // Copilot review on #105: the supersede detail labeled any empty-op_id
  // superseder an "entity write", mislabeling service calls that omit op_id.
  // The label must come from the operation's origin.
  std::cout << "\n=== supersede: detail labels superseder by origin ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 1700.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "a5");  // in flight
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2200.0f, "b5");  // queued
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2400.0f, "");    // service, no op_id
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2600.0f, "c5", nullptr,
                                 WriteOrigin::ENTITY);
  h.advance(20000);

  const WriteResult *b = h.result_for("b5");
  TEST_ASSERT(b && b->status == WriteStatus::SUPERSEDED, "queued op b5 superseded");
  TEST_ASSERT(b && b->detail == "superseded by service write",
              "empty-op_id service superseder labeled as service write");
  const WriteResult *anon = h.result_for("");
  TEST_ASSERT(anon && anon->status == WriteStatus::SUPERSEDED, "queued anonymous op superseded");
  TEST_ASSERT(anon && anon->detail == "superseded by entity write",
              "entity-origin superseder labeled as entity write");
  const WriteResult *c = h.result_for("c5");
  TEST_ASSERT(c && c->status == WriteStatus::ACCEPTED, "last write ran to acceptance");
}

static void test_supersede_queued() {
  std::cout << "\n=== supersede: queued op replaced, in-flight op untouched ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 1700.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "a");  // starts immediately
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2200.0f, "b");  // queued
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2400.0f, "c");  // supersedes b

  TEST_ASSERT(h.events_for("b") == 1, "queued op got its terminal event immediately");
  const WriteResult *rb = h.result_for("b");
  TEST_ASSERT(rb && rb->status == WriteStatus::SUPERSEDED, "queued op is superseded");
  TEST_ASSERT(rb && rb->detail.find("c") != std::string::npos, "supersede detail names the newer op");

  h.advance(30000);
  TEST_ASSERT(h.events_for("a") == 1 && h.events_for("b") == 1 && h.events_for("c") == 1,
              "every op got exactly one terminal event");
  const WriteResult *ra = h.result_for("a");
  const WriteResult *rc = h.result_for("c");
  TEST_ASSERT(ra && ra->status == WriteStatus::ACCEPTED, "in-flight op ran to completion");
  TEST_ASSERT(rc && rc->status == WriteStatus::ACCEPTED, "superseding op ran after it");
  TEST_ASSERT(rc && std::fabs(rc->value - 2400.0f) < 0.5f, "last write won");
}

static void test_watchdog_timeout() {
  std::cout << "\n=== watchdog: unresponsive pump -> exactly one timeout ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x00;
  h.prime_cache();
  h.sim.respond_mode_reads = false;  // mode change confirm reads all fail

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "t1");
  h.advance(40000);

  TEST_ASSERT(h.events_for("t1") == 1, "exactly one terminal event despite late callbacks");
  const WriteResult *r = h.result_for("t1");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "status is timeout");
}

static void test_disconnect_terminates() {
  std::cout << "\n=== disconnect: pending op gets a terminal event ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x00;
  h.prime_cache();
  h.sim.respond_mode_reads = false;

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "d1");
  h.advance(2000);
  h.write_op.on_disconnect();

  TEST_ASSERT(h.events_for("d1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("d1");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "status is timeout");
  TEST_ASSERT(r && r->detail == "disconnected", "detail is 'disconnected'");
  h.advance(40000);
  TEST_ASSERT(h.events_for("d1") == 1, "no duplicate event after late timers fire");
}

static void test_not_ready_rejected() {
  std::cout << "\n=== readiness gate: rejected before any wire write ===" << std::endl;
  Harness h;
  h.ready = false;

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "r1");
  h.advance(100);

  TEST_ASSERT(h.events_for("r1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("r1");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(h.frames_0a01 == 0 && h.frames_0601 == 0, "no wire writes were sent");
}

static void test_validation_invalid() {
  // Malformed requests settle `invalid`, distinct from pump-side `rejected`
  // (review feedback on #92): deterministic, never worth a retry.
  std::cout << "\n=== validation: malformed requests settle invalid, pre-wire ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 9999.0f, "v1");
  h.write_op.submit_set_setpoint(ControlMode::AUTO_ADAPT_RADIATOR, 2000.0f, "v2");
  h.advance(100);

  TEST_ASSERT(h.events_for("v1") == 1 && h.events_for("v2") == 1, "each got exactly one terminal event");
  const WriteResult *r1 = h.result_for("v1");
  const WriteResult *r2 = h.result_for("v2");
  TEST_ASSERT(r1 && r1->status == WriteStatus::INVALID, "out-of-range value settles invalid");
  TEST_ASSERT(r1 && r1->detail.find("range") != std::string::npos, "detail states the valid range");
  TEST_ASSERT(r1 && std::fabs(r1->requested_value - 9999.0f) < 0.5f,
              "event echoes the requested value");
  TEST_ASSERT(r2 && r2->status == WriteStatus::INVALID, "non-scalar mode settles invalid");
  TEST_ASSERT(h.frames_0601 == 0 && h.frames_register == 0, "no write frames were sent");
}

static void test_setpoints_different_modes_both_run() {
  // Review feedback on #92: the pump stores an independent setpoint per
  // mode, so a queued constant_speed setpoint must NOT be superseded by a
  // subsequent constant_flow setpoint. Both run; only same-mode pairs
  // supersede.
  std::cout << "\n=== supersede: different-mode setpoints both run ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 1700.0f;
  h.sim.setpoints[39] = 1.0f / 3600.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "ms");  // in flight
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2200.0f, "ms2"); // queued
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_FLOW, 1.5f, "mf");      // different mode
  TEST_ASSERT(h.events_for("ms2") == 0, "same-mode queued op not yet superseded by different mode");
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2400.0f, "ms3"); // supersedes ms2

  TEST_ASSERT(h.events_for("ms2") == 1 && h.result_for("ms2")->status == WriteStatus::SUPERSEDED,
              "same-mode queued setpoint superseded");
  TEST_ASSERT(h.events_for("mf") == 0, "different-mode setpoint was NOT superseded");

  h.advance(40000);
  TEST_ASSERT(h.events_for("ms") == 1 && h.events_for("mf") == 1 && h.events_for("ms3") == 1,
              "all surviving ops ran to terminal events");
  TEST_ASSERT(h.result_for("mf")->status == WriteStatus::ACCEPTED, "flow setpoint accepted");
  TEST_ASSERT(h.result_for("ms3")->status == WriteStatus::ACCEPTED, "final speed setpoint accepted");
  TEST_ASSERT(std::fabs(h.sim.setpoints[13] - 2400.0f) < 0.5f, "speed slot holds the last speed write");
  TEST_ASSERT(std::fabs(h.sim.setpoints[39] * 3600.0f - 1.5f) < 0.01f, "flow slot holds the flow write");
}

static void test_origin_and_seq_reported() {
  std::cout << "\n=== event metadata: origin and submission seq ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 1700.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "svc");
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2100.0f, "ent", nullptr,
                                 WriteOrigin::ENTITY);
  h.advance(20000);

  const WriteResult *rs = h.result_for("svc");
  const WriteResult *re = h.result_for("ent");
  TEST_ASSERT(rs && rs->origin == WriteOrigin::SERVICE, "default origin is service");
  TEST_ASSERT(re && re->origin == WriteOrigin::ENTITY, "entity origin reported");
  TEST_ASSERT(rs && re && re->seq > rs->seq, "seq preserves submission order");
  TEST_ASSERT(rs && std::fabs(rs->requested_value - 2000.0f) < 0.5f &&
              std::fabs(rs->value - 2000.0f) < 0.5f,
              "accepted result echoes matching requested and settled values");
}

// The config write echoes the pump's own min/max on/off-time LIMITS back
// verbatim (issue #106). Those five bytes exist only once an Obj 91 Sub 430
// read has landed; before that the cache holds ControlService's historical
// constants, and sending them would overwrite the pump's real limits with a
// fabrication as a side effect of setting a temperature.
//
// The reachable trigger needs no malformed frame and no unusual pump:
// invalidate_cache() clears the flag on every disconnect, and the HA service
// path reaches submit_set_temperature_range() without check_ready() -- the
// entity path is gated, api_bridge.cpp is not. So a service call during the
// initial read chain, in a reconnect window, or after a Sub 430 read that
// timed out arrives here with the limits unknown. That is what this models:
// the Obj 91 read simply has not happened.
//
// An earlier version of this test shortened the reply instead, which was the
// wrong scenario twice over. Opspec 0x15 *declares* 21 body bytes, and the
// transport trims every frame to the length its header states, so a matched
// Sub 430 reply always yields payload_len 17 -- the short reply could not
// occur, and the fixture that produced it contradicted its own opspec. A pump
// that genuinely answered shorter would send a different opspec, fail the
// match, and time out, leaving the cache invalid anyway.
//
// The scenario it modelled -- "the Obj 91 read simply has not happened" -- also
// stopped being reachable once the write started with a mandatory read of its
// own. What remains reachable is a reply that LANDS and carries no limits tail,
// which is what obj91_includes_limits models, and the guard has to hold for it.
static void test_temperature_range_refused_when_limits_unknown() {
  std::cout << "\n=== set_temperature_range: refused when the reply carries no limits ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.obj91_includes_limits = false;  // the reply lands; the tail is not in it
  h.prime_cache();

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_nolimits");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_nolimits") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_nolimits");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "the write is refused rather than sent with fabricated limits");
  TEST_ASSERT(r && r->detail.find("limits not read") != std::string::npos,
              "and the detail says why, rather than blaming a missing ACK");
  TEST_ASSERT(h.frames_temp_write == 0,
              "no temperature-range config frame reached the pump at all");
  TEST_ASSERT(h.frames_0a01 == 0,
              "and the pump was not switched into temperature-range mode either -- "
              "a refusal must not leave a side effect behind");
}

// The pre-write read is mandatory, so a pump that will not answer it stops the
// write rather than letting it proceed on whatever the cache last held.
static void test_temperature_range_refused_when_the_pre_read_fails() {
  std::cout << "\n=== set_temperature_range: unreadable config -> rejected before any write ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();     // the cache IS populated...
  h.sim.respond_obj91 = false;  // ...and the pump stops answering afterwards

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_preread");
  h.advance(30000);

  TEST_ASSERT(h.events_for("tr_preread") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_preread");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("not attempted") != std::string::npos,
              "detail says the write was never attempted");
  TEST_ASSERT(h.frames_temp_write == 0 && h.frames_0a01 == 0,
              "a populated cache is not a licence to write: nothing reached the pump");
}

static void test_temperature_range_accepted() {
  std::cout << "\n=== set_temperature_range: accepted via unfused mode change ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  int commits_before = h.commit_count;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr1");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && std::fabs(r->temp_min - 30.0f) < 0.2f && std::fabs(r->temp_max - 50.0f) < 0.2f,
              "settled temperatures reported");
  TEST_ASSERT(r && r->autoadapt == 1, "settled autoadapt reported");
  TEST_ASSERT(h.frames_0601 == 0, "temperature range no longer touches the fused 0x0601 object");
  TEST_ASSERT(h.frames_0a01 == 1, "mode switched via the unfused 0x0A01 object");
  TEST_ASSERT(h.commit_count > commits_before, "configuration commit was sent");
}

// An ANSWERED refusal is settled by the readback (issue #208).
//
// Neither a refusal nor silence short-circuits any more (issue #234), so the
// two no longer differ in STATUS. They still differ, and this pins where: the
// callback reports "the pump answered", so a refusal leaves config_unacked
// false and the settle does not claim the write went unacknowledged. Reporting
// a refusal as silence would put that claim on an event about a pump that
// replied in milliseconds.
//
// Why the readback has to be the one to decide either way: transport.cpp's
// short-ACK branch matches "some queued write of this shape", with no sequence
// number and no object echo, so the fire-and-forget mode write a few hundred ms
// earlier can be answered inside this write's window. Making refusals visible
// (#208) is what surfaced this -- a misattributed refusal would otherwise
// report failure for a write that landed. The capture behind #208 is that exact
// shape: an 0x81 mid-write, and the value landed.
static void test_temperature_range_refusal_is_settled_by_the_readback() {
  std::cout << "\n=== set_temperature_range: an answered refusal defers to the readback ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.temp_ack_head = 0x81;  // Unknown Data Item -- the captured frame's head

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_refused");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_refused") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_refused");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "accepted -- the pump holds the requested values, so the refusal was "
              "not this write's; a short-circuit here would report failure for a "
              "write that landed");
  TEST_ASSERT(r && std::fabs(r->temp_min - 30.0f) < 0.2f && std::fabs(r->temp_max - 50.0f) < 0.2f,
              "and the settled values come from the pump, not from the request");
  TEST_ASSERT(r && r->detail.find("not acknowledged") == std::string::npos,
              "and the event does not claim the write went unacknowledged -- the pump "
              "answered, it just answered no");
}

// The confirm's retry ladder, which the fix now depends on.
//
// A dropped first readback used to be indistinguishable from a failed write:
// the operation watchdog fired at 10 s -- before CONFIG_MAX_ATTEMPTS could send
// a second read -- and settled `timeout`. Harmless while a missing ACK had
// already settled REJECTED at 3.4 s without any readback; not harmless once the
// readback is the only thing that decides, because this is a write the pump
// stored and the settle has to say so.
static void test_temperature_range_unacked_survives_a_dropped_readback() {
  std::cout << "\n=== set_temperature_range: no ACK, first readback dropped -> accepted on the retry ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.ack_temp_write = false;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_unacked_retry");
  // The mandatory pre-write read has to land; only the confirm's first read is
  // dropped. Arming the drop before submit would spend it on the pre-read, and
  // the operation would settle REJECTED without ever reaching the ladder.
  h.advance(1000);
  h.sim.drop_obj91_reads = 1;
  h.advance(40000);

  TEST_ASSERT(h.events_for("tr_unacked_retry") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_unacked_retry");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the retry reaches the pump and reports what it holds, rather than the "
              "operation watchdog reporting that nothing was heard");
  TEST_ASSERT(r && std::fabs(r->temp_min - 30.0f) < 0.2f && std::fabs(r->temp_max - 50.0f) < 0.2f,
              "settled values come from the readback");
  TEST_ASSERT(h.sim.drop_obj91_reads == 0,
              "the drop was spent on a CONFIRM read -- otherwise this passes without "
              "the retry ever running");
}

// ...and the ladder still terminates. A pump that never answers the readback
// settles once, as a failure, inside the budget -- the watchdog is what makes
// the deferral safe to do at all.
static void test_temperature_range_unacked_and_unreadable_still_settles() {
  std::cout << "\n=== set_temperature_range: no ACK and no readback ever -> one timeout ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.ack_temp_write = false;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_unreadable");
  // Again the pre-write read lands; it is the CONFIRM readback that never
  // answers, which is the only way to reach the ladder's own timeout.
  h.advance(1000);
  h.sim.respond_obj91 = false;
  h.advance(60000);

  TEST_ASSERT(h.events_for("tr_unreadable") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_unreadable");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "a write nobody can confirm settles timeout, not a guess at rejected");
  TEST_ASSERT(r && r->detail.find("readback failed") != std::string::npos,
              "and the confirm ladder is what reports it, having run to the end inside "
              "the budget -- on the old 10 s default the watchdog got there first and "
              "this branch could not be reached");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos,
              "the missing ACK is still named");
}

// The confirm's three-way split, independent of the ACK.
//
// Until issue #234 this comparison had only two outcomes -- the requested
// values or CLAMPED -- so a pump that took the mode change and then ignored
// the config write reported "the pump stored something else" about a pump
// that had stored nothing. confirm_setpoint_ and confirm_cycle_times_ both
// drew the distinction already; this one did not, which is also why there was
// nothing for an unacknowledged write to defer to.
static void test_temperature_range_ignored_write_is_rejected() {
  std::cout << "\n=== set_temperature_range: answered but not stored -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();  // pre-write values: 25.0-55.0 °C, autoadapt off
  h.sim.honor_temp_writes = false;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_ignored");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_ignored") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_ignored");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "the pump kept every value it had, which is a refusal and not a clamp");
  TEST_ASSERT(r && r->detail.find("pump kept") != std::string::npos,
              "and the detail says kept, not stored");
  TEST_ASSERT(r && std::fabs(r->temp_min - 25.0f) < 0.2f && std::fabs(r->temp_max - 55.0f) < 0.2f,
              "settled values are the pump's, not the request's");
}

// Why the pre-write read is a READ and not a cache lookup.
//
// read_obj91_config() runs once per connection, in the initial read chain -- it
// is not in the periodic control poll that refreshes the mode and setpoints
// (issue #54). So on a link that has been up for hours the cached range is that
// old, and the GO app can have moved it in between. Taking the baseline from
// the cache would then compare the readback against values nobody holds any
// more, and an ignored write -- the pump kept what it had -- would report
// CLAMPED, which claims the pump chose something.
//
// The out-of-band edit here is exactly that: the sim's values move with no read
// in between, so the cache and the pump disagree until the write reads again.
static void test_temperature_range_baseline_is_read_not_remembered() {
  std::cout << "\n=== set_temperature_range: an out-of-band edit does not turn a refusal into a clamp ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();  // cache now holds the sim's 25.0-55.0, autoadapt off

  // Another controller moves the range. Nothing reads it, so the cache is stale.
  h.sim.temp_min = 28.0f;
  h.sim.temp_max = 52.0f;
  h.sim.autoadapt = true;
  h.sim.honor_temp_writes = false;  // and our write is then ignored

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_stale");
  h.advance(30000);

  TEST_ASSERT(h.events_for("tr_stale") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_stale");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "the pump kept what it had, so this is a refusal -- against the stale "
              "cache it would read as CLAMPED, blaming the pump for a choice it "
              "never made");
  TEST_ASSERT(r && std::fabs(r->temp_min - 28.0f) < 0.2f && std::fabs(r->temp_max - 52.0f) < 0.2f,
              "and the settled values are the pump's current ones");
}

// Issue #248: a mode reply must never be read as the config write's ACK, at any
// delay. Swept rather than sampled, because the first fix passed at one delay
// and failed across a whole band nobody had tried.
//
// run_set_temperature_range_ sends two Class 10 writes CONFIG_STEP2_DELAY_MS
// apart, and only the second carries a callback. transport.cpp's short-ACK
// branch can test only the QUEUED command's shape -- a GENIbus reply carries no
// sequence number and no object echo -- so the first write's reply, arriving
// with no owner, is byte-identical to what the second is waiting for.
//
// The scenario makes the two verdicts differ: the pump stores the values but
// never answers the config write, so the truthful settle is ACCEPTED with the
// missing-ACK note. If the mode reply is stolen, the write looks acknowledged
// and the note disappears -- an event claiming the pump answered a frame it
// never answered.
//
// COVERAGE HAS A BOUND, and it is asserted here rather than left to be
// discovered. The two guards compose: the wait consumes anything up to
// MODE_ACK_TIMEOUT_MS (400 ms), and the reply debt covers
// STALE_REPLY_WINDOW_MS (500 ms) beyond that. So a mode reply is attributable
// out to 900 ms and not past it -- after that the debt has expired, and the
// config write's own 3 s window is open to whatever arrives.
//
// 900 ms is three times the slowest reply the captures contain (295 ms, n~12k
// after reassembly and de-duplication; nothing exceeds 400). The uncovered band
// is therefore populated by no observation, which is the argument for stopping
// there rather than widening the window until the sweep goes quiet. Widening is
// nearly free under a debt -- it is spent once, not per frame -- so if a pump is
// ever seen replying past 900 ms, raise the window rather than redesign.
//
// The first fix covered only 0-1000 ms with a single guard and left 1100-3900
// wide open; that band is what this sweep exists to keep shut.
static void test_temperature_range_mode_ack_is_never_stolen() {
  std::cout << "\n=== set_temperature_range: a mode ACK is not read as the config write's ==="
            << std::endl;
  // Up to the composed bound of MODE_ACK_TIMEOUT_MS + STALE_REPLY_WINDOW_MS,
  // less the ~20 ms the harness's inject() adds on top of the requested delay --
  // so the last case sits just inside 900, not on it. A case AT the boundary
  // would be testing the harness's arithmetic rather than the guard's.
  const uint32_t delays[] = {0, 54, 121, 200, 295, 401, 500, 700, 850};
  int stolen = 0;
  for (uint32_t d : delays) {
    Harness h;
    h.sim.mode_byte = 0x02;
    h.prime_cache();
    h.prime_temp_limits();
    h.sim.ack_mode_write = true;
    h.sim.mode_ack_delay_ms = d;
    h.sim.ack_temp_write = false;   // the config write itself is never answered

    h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_sweep");
    h.advance(40000);
    const WriteResult *r = h.result_for("tr_sweep");
    const bool claimed_an_ack = r && r->detail.find("not acknowledged") == std::string::npos;
    if (claimed_an_ack) {
      stolen++;
      std::cout << "    delay=" << d << " ms: the config write claimed an acknowledgement" << std::endl;
    }
  }
  TEST_ASSERT(stolen == 0,
              "out to the composed 900 ms bound, the config write never reports an "
              "acknowledgement it did not receive -- the mode write's reply belongs "
              "to the mode write");
}

// The wait is free when the pump answers, which is the claim
// MODE_ACK_TIMEOUT_MS rests on: it is set equal to CONFIG_STEP2_DELAY_MS so an
// answered mode write frees the queue long before step 2 is due, and an
// unanswered one expires exactly when step 2 was going to run anyway.
//
// This asserts on the CLOCK. Its predecessor asserted only that the settle was
// accepted with an empty detail, which is true on an unmodified tree as well --
// it passed with the entire production change reverted.
static void test_mode_ack_does_not_delay_step_two() {
  std::cout << "\n=== set_temperature_range: the mode ACK wait does not delay step 2 ===" << std::endl;
  uint64_t answered_gap = 0, unanswered_gap = 0;
  for (int answered = 1; answered >= 0; answered--) {
    Harness h;
    h.sim.mode_byte = 0x02;
    h.prime_cache();
    h.prime_temp_limits();
    h.sim.ack_mode_write = answered != 0;
    h.sim.mode_ack_delay_ms = 54;  // the corpus p50

    h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_timing");
    h.advance(30000);
    TEST_ASSERT(h.frames_0a01 == 1 && h.frames_temp_write == 1, "both writes went out");
    const uint64_t gap = h.last_temp_write_ms - h.last_0a01_ms;
    (answered ? answered_gap : unanswered_gap) = gap;
  }
  TEST_ASSERT(answered_gap < 500,
              "an answered mode write leaves step 2 at its usual delay, not pushed out "
              "by the wait");
  TEST_ASSERT(unanswered_gap < 500,
              "and an unanswered one costs nothing either -- the wait expires exactly "
              "when step 2 was due, which is why it is set to that delay");
}

static void test_temperature_range_clamped() {
  std::cout << "\n=== set_temperature_range: pump caps the max -> clamped ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.temp_max_clamp = 45.0f;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_clamped");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_clamped") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_clamped");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED,
              "the write moved the min, so something landed -- clamp, not refusal");
  TEST_ASSERT(r && r->detail.find("pump stored") != std::string::npos,
              "and the detail says stored, not kept");
  TEST_ASSERT(r && std::fabs(r->temp_max - 45.0f) < 0.2f, "settled max reports the cap");
}

// --- The unacknowledged config write (issue #234) -------------------------
//
// A missing ACK used to settle REJECTED on the spot, with no readback. REJECTED
// asserts the pump did not take the write, and nothing at that point knew it:
// the only evidence was that no frame arrived inside 3 s. The three tests below
// are the three things that silence can actually mean, and the pump -- not the
// transport -- decides which.
//
// Why the ACK cannot carry the verdict even when it does arrive: transport.cpp's
// short-ACK branch matches "some queued Class 10 command of this shape", with no
// sequence number and no object echo, and the fire-and-forget mode write a few
// hundred ms earlier is on the same class. Silence is less attributable still --
// there is no frame to reason about at all.

static void test_temperature_range_unacked_but_stored() {
  std::cout << "\n=== set_temperature_range: no ACK but the values landed -> accepted ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.ack_temp_write = false;  // the pump stores it; the acknowledgement is lost

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_unacked_ok");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_unacked_ok") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_unacked_ok");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the pump holds the requested values, so the write did not fail -- "
              "an automation retrying on `rejected` here would rewrite correct values");
  TEST_ASSERT(r && std::fabs(r->temp_min - 30.0f) < 0.2f && std::fabs(r->temp_max - 50.0f) < 0.2f,
              "settled values come from the readback");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos,
              "the silence is still reported, in the detail rather than the status");
  TEST_ASSERT(h.commit_count > 0,
              "the configuration commit is still sent -- it is what persists a write "
              "whose acknowledgement was lost");
}

static void test_temperature_range_unacked_and_not_stored() {
  std::cout << "\n=== set_temperature_range: no ACK and nothing stored -> rejected ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.ack_temp_write = false;
  h.sim.honor_temp_writes = false;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_unacked_lost");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_unacked_lost") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_unacked_lost");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "a write that neither landed nor was answered still settles as a failure");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos &&
              r->detail.find("pump kept") != std::string::npos,
              "and the detail carries both halves: no ACK, and the pump kept what it had");
}

static void test_temperature_range_unacked_and_clamped() {
  std::cout << "\n=== set_temperature_range: no ACK and the pump capped it -> clamped ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.prime_temp_limits();
  h.sim.ack_temp_write = false;
  h.sim.temp_max_clamp = 45.0f;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tr_unacked_clamp");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr_unacked_clamp") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr_unacked_clamp");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED,
              "a clamp stays distinguishable from a write that never landed, "
              "even with no ACK to tell them apart");
  TEST_ASSERT(r && std::fabs(r->temp_max - 45.0f) < 0.2f, "settled max reports the cap");
}

static void test_temperature_range_invalid() {
  std::cout << "\n=== set_temperature_range: min >= max rejected ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_temperature_range(50.0f, 30.0f, false, "tr3");
  h.advance(100);

  TEST_ASSERT(h.events_for("tr3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr3");
  TEST_ASSERT(r && r->status == WriteStatus::INVALID, "status is invalid (malformed request)");
}

static void test_cycle_times_accepted() {
  // Issue #106: reads and writes go to Obj 91 Sub 421 (the live DHW config),
  // never Sub 430, and the stored flow setpoint is echoed back verbatim.
  std::cout << "\n=== set_cycle_times: verified via Sub 421, setpoint preserved ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 5;
  h.sim.cycle_off = 15;
  h.prime_cache();
  int commits_before = h.commit_count;

  h.write_op.submit_set_cycle_times(10, 20, NAN, "ct1");
  h.advance(15000);

  TEST_ASSERT(h.events_for("ct1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->on_minutes == 10 && r->off_minutes == 20, "settled cycle times reported");
  TEST_ASSERT(r && std::fabs(r->flow - 0.2271f) < 0.005f,
              "settled flow reported even on a kept-flow write");
  TEST_ASSERT(r && std::isnan(r->requested_flow), "kept flow omitted from the requested echo");
  TEST_ASSERT(h.sim.cycle_on == 10 && h.sim.cycle_off == 20, "pump holds the new periods");
  TEST_ASSERT(h.frames_dhw_write == 1, "one Sub 421 write was sent");
  TEST_ASSERT(h.frames_dhw_read >= 2, "fresh pre-read plus verify readback of Sub 421");
  TEST_ASSERT(h.last_dhw_write_setpoint.size() == 4 &&
              memcmp(h.last_dhw_write_setpoint.data(), h.sim.dhw_setpoint_raw, 4) == 0,
              "stored DHW flow setpoint echoed back verbatim");
  TEST_ASSERT(h.commit_count == commits_before, "no configuration commit (capture-verified)");
  TEST_ASSERT(h.control.get_cached_cycle_time_on() == 10 &&
              h.control.get_cached_cycle_time_off() == 20,
              "entity-facing cache reflects the pump values");
}

static void test_cycle_times_pump_clamps() {
  std::cout << "\n=== set_cycle_times: pump clamps OFF floor -> clamped ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.dhw_min_off = 5;  // the GO-app-observed minimum-off behavior
  h.prime_cache();

  h.write_op.submit_set_cycle_times(1, 3, NAN, "ct2");
  h.advance(15000);

  TEST_ASSERT(h.events_for("ct2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct2");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED, "status is clamped");
  TEST_ASSERT(r && r->on_minutes == 1 && r->off_minutes == 5, "settled values report the clamp");
  TEST_ASSERT(r && r->detail.find("off=5") != std::string::npos, "detail reports the stored value");
}

// The same three meanings of silence on the DHW config write (issue #234).
// This confirm already told a kept value from a clamp -- the mandatory
// pre-write read gives it the "before" values -- so all the change needed here
// was to stop short-circuiting past it.
static void test_cycle_times_unacked_but_stored() {
  std::cout << "\n=== set_cycle_times: no ACK but the periods landed -> accepted ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 5;
  h.sim.cycle_off = 15;
  h.prime_cache();
  h.sim.ack_dhw_write = false;

  h.write_op.submit_set_cycle_times(10, 20, NAN, "ct_unacked_ok");
  h.advance(15000);

  TEST_ASSERT(h.events_for("ct_unacked_ok") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct_unacked_ok");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the pump holds the requested periods, so the write did not fail");
  TEST_ASSERT(r && r->on_minutes == 10 && r->off_minutes == 20, "settled periods reported");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos,
              "the silence is reported in the detail rather than the status");
}

static void test_cycle_times_unacked_and_not_stored() {
  std::cout << "\n=== set_cycle_times: no ACK and nothing stored -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 5;
  h.sim.cycle_off = 15;
  h.prime_cache();
  h.sim.ack_dhw_write = false;
  h.sim.honor_dhw_writes = false;

  h.write_op.submit_set_cycle_times(10, 20, NAN, "ct_unacked_lost");
  h.advance(15000);

  TEST_ASSERT(h.events_for("ct_unacked_lost") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct_unacked_lost");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "a write that neither landed nor was answered still settles as a failure");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos &&
              r->detail.find("pump kept") != std::string::npos,
              "and the detail carries both halves");
  TEST_ASSERT(r && r->on_minutes == 5 && r->off_minutes == 15,
              "settled values are the pump's, not the request's");
}

static void test_cycle_times_unacked_and_clamped() {
  std::cout << "\n=== set_cycle_times: no ACK and the pump clamped -> clamped ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.dhw_min_off = 5;
  h.prime_cache();
  h.sim.ack_dhw_write = false;

  h.write_op.submit_set_cycle_times(1, 3, NAN, "ct_unacked_clamp");
  h.advance(15000);

  TEST_ASSERT(h.events_for("ct_unacked_clamp") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct_unacked_clamp");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED,
              "a clamp stays distinguishable from a write that never landed");
  TEST_ASSERT(r && r->off_minutes == 5, "settled values report the clamp");
}

// The DHW side of the retry ladder, measured rather than computed. Its budget
// is the same constant, but its timeline is not: SET_CYCLE_TIMES makes a
// mandatory pre-write read first, so the confirm starts later than the
// temperature range's does. The drop is applied to the CONFIRM read, not the
// pre-write one -- a pre-read that fails settles REJECTED before any write.
static void test_cycle_times_unacked_survives_a_dropped_readback() {
  std::cout << "\n=== set_cycle_times: no ACK, first readback dropped -> accepted on the retry ==="
            << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 5;
  h.sim.cycle_off = 15;
  h.prime_cache();
  h.sim.ack_dhw_write = false;

  h.write_op.submit_set_cycle_times(10, 20, NAN, "ct_unacked_retry");
  // The pre-write read has to land; only the confirm's first read is dropped.
  h.advance(1000);
  h.sim.drop_dhw_reads = 1;
  h.advance(30000);

  TEST_ASSERT(h.events_for("ct_unacked_retry") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct_unacked_retry");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the retry reaches the pump inside the budget and reports what it holds");
  TEST_ASSERT(r && r->on_minutes == 10 && r->off_minutes == 20, "settled periods reported");
  TEST_ASSERT(h.sim.drop_dhw_reads == 0 && h.frames_dhw_read >= 3,
              "the drop was spent on a CONFIRM read, and a third read followed it -- "
              "otherwise this passes without the retry ever running");
}

static void test_cycle_times_read_unavailable() {
  std::cout << "\n=== set_cycle_times: Sub 421 unreadable -> rejected before any write ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.sim.respond_dhw_reads = false;

  h.write_op.submit_set_cycle_times(10, 20, NAN, "ct3");
  h.advance(20000);

  TEST_ASSERT(h.events_for("ct3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ct3");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("not attempted") != std::string::npos,
              "detail says the write was never attempted");
  TEST_ASSERT(h.frames_dhw_write == 0, "no blind Sub 421 write was sent");
}

// --- Cycle flow setpoint (issue #107) ------------------------------------

static void test_cycle_flow_only_accepted() {
  std::cout << "\n=== cycle flow: flow-only write, periods preserved ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 5;
  h.sim.cycle_off = 15;
  h.prime_cache();
  int commits_before = h.commit_count;

  h.write_op.submit_set_cycle_times(0, 0, 0.5f, "cf1");
  h.advance(15000);

  TEST_ASSERT(h.events_for("cf1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("cf1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(h.sim.cycle_on == 5 && h.sim.cycle_off == 15, "kept periods untouched on the pump");
  TEST_ASSERT(r && r->on_minutes == 5 && r->off_minutes == 15,
              "settled periods report the pump's (kept) values");
  TEST_ASSERT(r && r->requested_on_minutes == -1 && r->requested_off_minutes == -1,
              "kept periods omitted from the requested echo");
  TEST_ASSERT(r && std::fabs(r->flow - 0.5f) < 0.005f, "settled flow reported");
  TEST_ASSERT(r && std::fabs(r->requested_flow - 0.5f) < 0.0001f, "requested flow echoed");
  float stored = protocol::decode_float_be(h.sim.dhw_setpoint_raw) * 3600.0f;
  TEST_ASSERT(std::fabs(stored - 0.5f) < 0.005f, "pump stores the encoded flow (m³/h)");
  TEST_ASSERT(h.frames_dhw_write == 1 && h.frames_dhw_read >= 2,
              "one write between the fresh read and the verify readback");
  TEST_ASSERT(h.commit_count == commits_before, "no configuration commit");
}

static void test_cycle_combined_write() {
  std::cout << "\n=== cycle flow: combined periods + flow write ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();

  h.write_op.submit_set_cycle_times(10, 20, 0.5f, "cf2");
  h.advance(15000);

  const WriteResult *r = h.result_for("cf2");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(h.sim.cycle_on == 10 && h.sim.cycle_off == 20, "periods applied");
  float stored = protocol::decode_float_be(h.sim.dhw_setpoint_raw) * 3600.0f;
  TEST_ASSERT(std::fabs(stored - 0.5f) < 0.005f, "flow applied");
  TEST_ASSERT(r && r->requested_on_minutes == 10 && r->requested_off_minutes == 20 &&
              std::fabs(r->requested_flow - 0.5f) < 0.0001f,
              "all three asserted fields echoed as requested");
}

static void test_cycle_flow_clamped() {
  std::cout << "\n=== cycle flow: pump caps the flow -> clamped ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.dhw_flow_clamp_native = 0.3f / 3600.0f;  // installer limit at 0.3 m³/h
  h.prime_cache();

  h.write_op.submit_set_cycle_times(0, 0, 0.5f, "cf3");
  h.advance(15000);

  TEST_ASSERT(h.events_for("cf3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("cf3");
  TEST_ASSERT(r && r->status == WriteStatus::CLAMPED, "status is clamped");
  TEST_ASSERT(r && std::fabs(r->flow - 0.3f) < 0.005f, "settled flow reports the cap");
  TEST_ASSERT(r && r->detail.find("flow=0.300") != std::string::npos,
              "detail reports the stored flow");
  TEST_ASSERT(h.sim.cycle_on == 5 && h.sim.cycle_off == 15, "kept periods untouched");
}

static void test_cycle_flow_rejected_kept() {
  std::cout << "\n=== cycle flow: pump keeps its old flow -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.honor_dhw_flow_writes = false;  // pump ignores the asserted setpoint bytes
  h.prime_cache();

  h.write_op.submit_set_cycle_times(0, 0, 0.5f, "cf4");
  h.advance(15000);

  const WriteResult *r = h.result_for("cf4");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected (pump kept old value)");
  TEST_ASSERT(r && r->detail.find("pump kept") != std::string::npos, "detail says kept");
  TEST_ASSERT(r && std::fabs(r->flow - 0.2271f) < 0.005f, "settled flow is the pump's old value");
}

static void test_cycle_flow_invalid_ranges() {
  std::cout << "\n=== cycle flow: out-of-range fields -> invalid, no wire traffic ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  int reads_before = h.frames_dhw_read;

  h.write_op.submit_set_cycle_times(0, 0, 0.05f, "iv1");   // below the 0.1 floor
  h.write_op.submit_set_cycle_times(0, 0, -1.0f, "iv2");   // negative
  h.write_op.submit_set_cycle_times(0, 0, 11.0f, "iv3");   // above the 10.0 cap
  h.write_op.submit_set_cycle_times(61, 0, NAN, "iv4");    // minutes out of range
  h.advance(5000);

  for (const char *id : {"iv1", "iv2", "iv3", "iv4"}) {
    const WriteResult *r = h.result_for(id);
    TEST_ASSERT(r && r->status == WriteStatus::INVALID, "out-of-range field settles invalid");
  }
  TEST_ASSERT(h.frames_dhw_write == 0 && h.frames_dhw_read == reads_before,
              "no wire traffic for invalid requests");
}

static void test_cycle_all_kept_invalid() {
  std::cout << "\n=== cycle flow: all fields keep-existing -> invalid ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  int reads_before = h.frames_dhw_read;

  h.write_op.submit_set_cycle_times(0, 0, NAN, "ak1");
  h.write_op.submit_set_cycle_times(0, 0, 0.0f, "ak2");  // API 0 sentinel
  h.advance(5000);

  for (const char *id : {"ak1", "ak2"}) {
    const WriteResult *r = h.result_for(id);
    TEST_ASSERT(r && r->status == WriteStatus::INVALID, "nothing-to-write settles invalid");
    TEST_ASSERT(r && r->detail.find("nothing to write") != std::string::npos, "detail explains");
  }
  TEST_ASSERT(h.frames_dhw_write == 0 && h.frames_dhw_read == reads_before, "zero wire traffic");
}

static void test_cycle_kept_minutes_unreadable() {
  std::cout << "\n=== cycle flow: kept periods unreadable -> rejected, no write ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.cycle_on = 0;  // parses to the -1 "unknown" sentinel
  h.prime_cache();

  h.write_op.submit_set_cycle_times(0, 0, 0.5f, "ku1");
  h.advance(15000);

  const WriteResult *r = h.result_for("ku1");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("not attempted") != std::string::npos,
              "detail says the write was never attempted");
  TEST_ASSERT(h.frames_dhw_write == 0, "no Sub 421 write was sent");
}

static void test_cycle_flow_entity_origin() {
  std::cout << "\n=== cycle flow: entity-origin flow-only submit ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();

  bool done_result = false, done_fired = false;
  h.write_op.submit_set_cycle_times(0, 0, 0.4f, "",
                                    [&](bool ok) { done_fired = true; done_result = ok; },
                                    WriteOrigin::ENTITY);
  h.advance(15000);

  TEST_ASSERT(h.events_for("") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->origin == WriteOrigin::ENTITY, "origin is entity");
  TEST_ASSERT(done_fired && done_result, "entity callback fired with success");
  float stored = protocol::decode_float_be(h.sim.dhw_setpoint_raw) * 3600.0f;
  TEST_ASSERT(std::fabs(stored - 0.4f) < 0.005f, "flow applied");
}

static void test_temp_range_preserves_limits() {
  // Issue #106 (adjacent bug): the Sub 430 struct's tail bytes are the pump's
  // on/off-time limits; a temperature write must echo them, not zero them.
  std::cout << "\n=== set_temperature_range: limit tail bytes preserved ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.control.sync_cache_async(nullptr);  // populates the cached limits tail
  h.advance(2000);

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, true, "tl1");
  h.advance(15000);

  const WriteResult *r = h.result_for("tl1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "temperature write accepted");
  TEST_ASSERT(h.last_temp_write_tail.size() == 5 &&
              memcmp(h.last_temp_write_tail.data(), h.sim.temp_limits_tail, 5) == 0,
              "pump's limit tail bytes echoed back verbatim (not zeroed)");
}

static void test_interleaved_poll() {
  std::cout << "\n=== #54 poll interleaved with an op: no duplicate events ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x00;
  h.prime_cache();

  h.write_op.submit_set_mode(ControlMode::CONSTANT_SPEED, "p1");
  h.advance(500);
  h.control.sync_cache_async(nullptr);  // the out-of-band control-state poll
  h.advance(15000);

  TEST_ASSERT(h.events_for("p1") == 1, "exactly one terminal event with a poll interleaved");
  const WriteResult *r = h.result_for("p1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "op still settled accepted");
  TEST_ASSERT(h.control.get_current_mode() == ControlMode::CONSTANT_SPEED, "cache holds the new mode");
}

static void test_issue92_collision() {
  // The originating incident from issue #92: set the speed, then immediately
  // turn the pump off. On the legacy path the off command raced the setpoint
  // write and folded a stale cached speed into the fused frame, reverting the
  // value on the pump. With the op layer the off operation queues until the
  // setpoint operation settles, so the value its frame carries is the
  // settled one (or the NaN keep-existing sentinel on a cold cache) — the
  // new speed survives.
  std::cout << "\n=== #92 collision: setpoint then immediate off ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.sim.setpoints[13] = 2000.0f;
  h.prime_cache();

  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 1650.0f, "sp");
  h.write_op.submit_set_enabled(false, "off");  // entity-style: fired right behind it
  h.advance(30000);

  TEST_ASSERT(h.events_for("sp") == 1 && h.events_for("off") == 1,
              "both ops got exactly one terminal event");
  const WriteResult *rsp = h.result_for("sp");
  const WriteResult *roff = h.result_for("off");
  TEST_ASSERT(rsp && rsp->status == WriteStatus::ACCEPTED, "setpoint settled accepted");
  TEST_ASSERT(roff && roff->status == WriteStatus::ACCEPTED, "off settled accepted");
  TEST_ASSERT(std::fabs(h.sim.setpoints[13] - 1650.0f) < 0.5f,
              "pump still holds 1650 after the off (no revert to 2000)");
  TEST_ASSERT(!h.sim.enabled, "pump is off");
}

static void test_schedule_entry_accepted() {
  std::cout << "\n=== set_schedule_entry: verified accepted ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_schedule_entry(0, 2, 6, 30, 8, 15, "se1");  // Wednesday 06:30-08:15
  h.advance(20000);

  TEST_ASSERT(h.events_for("se1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("se1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->layer == 0 && r->day == 2, "layer/day echoed");
  TEST_ASSERT(r && r->begin_hhmm == "06:30" && r->end_hhmm == "08:15", "settled times reported");
  const uint8_t *day_bytes = h.sim.layers[0] + 2 * 6;
  TEST_ASSERT(day_bytes[0] == 1 && day_bytes[2] == 6 && day_bytes[3] == 30 &&
              day_bytes[4] == 8 && day_bytes[5] == 15,
              "pump layer holds the written entry");
  TEST_ASSERT(h.sim.overview_writes >= 1, "configuration commit was written");
}

static void test_schedule_entry_verify_mismatch() {
  std::cout << "\n=== set_schedule_entry: pump ignores write -> rejected ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.honor_layer_writes = false;

  h.write_op.submit_set_schedule_entry(1, 0, 6, 0, 8, 0, "se2");
  h.advance(25000);

  TEST_ASSERT(h.events_for("se2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("se2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("does not match") != std::string::npos,
              "detail reports the verify mismatch");
  TEST_ASSERT(r && r->sched_enabled == 0, "settled state reports the pump's actual (disabled) entry");
}

static void test_schedule_entry_no_overview() {
  std::cout << "\n=== schedule write: overview unreadable -> rejected, no write frames ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.respond_overview_reads = false;

  h.write_op.submit_set_schedule_entry(0, 0, 6, 0, 8, 0, "se3");
  h.advance(25000);

  TEST_ASSERT(h.events_for("se3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("se3");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("not attempted") != std::string::npos,
              "detail says the write was never attempted");
  TEST_ASSERT(h.sim.overview_writes == 0, "no overview/commit write was sent");
}

// CLEAR_SCHEDULE_ENTRY had no case in this file — service, submit_*, facade
// passthrough, watchdog budget, resource key and event fields all wired, and
// nothing exercising any of it. AGENTS §9 step 6 asks for accepted, one
// failure status and the one-terminal-event invariant per command; these
// three are that, for a command that had none.
//
// (SET_PUMP_STATE is equally caseless here, so this was not "the only one".
// It is never enqueued as an Operation -- the bridge composes it from two
// flag writes -- so it has no run/confirm path to test; test_pump_schedule_ux
// covers its state parsing. Its aggregate settle event, built in
// api_bridge.cpp, is genuinely untested, but no host test compiles that file.)
//
// The clear path is not simply "set with the enabled bit off": it composes a
// blank ScheduleEntry rather than the requested one, and its confirm
// comparator matches on the enabled flag alone (the times are meaningless
// once the day is off). Those two branches in run_schedule_entry_ and
// confirm_schedule_entry_ were reached by nothing before this — verified by
// instrumenting both and running the whole pre-change suite: zero hits.
static void test_clear_schedule_entry_accepted() {
  std::cout << "\n=== clear_schedule_entry: verified accepted ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Seed layer 1 with two enabled days, so "cleared" is distinguishable from
  // "was never set", and so the whole-layer write has a neighbour to preserve.
  uint8_t *thursday = h.sim.layers[1] + 3 * 6;
  thursday[0] = 0x01; thursday[1] = 0x02;
  thursday[2] = 6; thursday[3] = 0; thursday[4] = 8; thursday[5] = 0;
  uint8_t *friday = h.sim.layers[1] + 4 * 6;
  friday[0] = 0x01; friday[1] = 0x02;
  friday[2] = 17; friday[3] = 30; friday[4] = 19; friday[5] = 45;

  h.write_op.submit_clear_schedule_entry(1, 3, "ce1");
  h.advance(20000);

  TEST_ASSERT(h.events_for("ce1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ce1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->layer == 1 && r->day == 3, "layer/day echoed");
  // Deliberately weak wording: on the ACCEPTED path confirm_schedule_entry_
  // does not write op->enabled back from the readback, so this pins the
  // request echo, not the pump. The pump-side claim is the next assertion.
  TEST_ASSERT(r && r->sched_enabled == 0, "the event reports the entry as off");
  TEST_ASSERT(h.sim.layers[1][3 * 6] == 0x00, "pump day cell is disabled");
  // The clear is a read-modify-write of the whole 42-byte layer, so the day
  // next to the target is the thing a bad patch destroys.
  TEST_ASSERT(friday[0] == 0x01 && friday[2] == 17 && friday[3] == 30 &&
              friday[4] == 19 && friday[5] == 45,
              "the untargeted day in the same layer survives");
  TEST_ASSERT(h.sim.overview_writes >= 1, "configuration commit was written");
}

static void test_clear_schedule_entry_pump_keeps_the_entry() {
  std::cout << "\n=== clear_schedule_entry: pump ignores write -> rejected ===" << std::endl;
  Harness h;
  h.prime_cache();
  uint8_t *saturday = h.sim.layers[2] + 5 * 6;
  saturday[0] = 0x01; saturday[1] = 0x02;
  saturday[2] = 7; saturday[3] = 15; saturday[4] = 9; saturday[5] = 45;
  h.sim.honor_layer_writes = false;

  h.write_op.submit_clear_schedule_entry(2, 5, "ce2");
  h.advance(25000);

  TEST_ASSERT(h.events_for("ce2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ce2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("does not match") != std::string::npos,
              "detail reports the verify mismatch");
  // The settled fields must describe the entry the pump still holds, not the
  // blank one that was sent — that is what tells a client the clear failed.
  TEST_ASSERT(r && r->sched_enabled == 1, "settled state reports the surviving entry");
  TEST_ASSERT(r && r->begin_hhmm == "07:15" && r->end_hhmm == "09:45",
              "and the times it still holds");
}

static void test_clear_schedule_entry_out_of_range_is_invalid() {
  std::cout << "\n=== clear_schedule_entry: layer 5 -> invalid, no wire write ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_clear_schedule_entry(5, 0, "ce3");
  // Asserted BEFORE any time passes, and that ordering is the test. "No write
  // frames were sent" cannot fail here, as a skeptic pass showed: removing the
  // bounds guard does not produce a write, it produces a 20 s stall, because
  // read_entries_async refuses layer 5 by returning false without invoking its
  // callback. What the guard buys is a verdict before any of that.
  TEST_ASSERT(h.events_for("ce3") == 1, "settled synchronously, before any wire traffic");

  h.advance(5000);
  TEST_ASSERT(h.events_for("ce3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ce3");
  TEST_ASSERT(r && r->status == WriteStatus::INVALID, "status is invalid");
}

// The confirm compares the enabled flag ALONE for a clear -- the
// `!want_enabled ||` short-circuit. Nothing pinned that: after a normal clear
// the pump's day cell is all zeros and the operation's own begin/end are zero
// too, so comparing the times as well is trivially true and deleting the
// short-circuit changes nothing.
//
// It stops mattering only if every pump zeroes the payload when it disables a
// day. Nothing requires that, and a pump that leaves the old times behind in a
// disabled cell would have every clear settle REJECTED, with the times it
// "still holds" quoted back as evidence -- a clear that worked, reported as a
// failure, forever.
static void test_clear_schedule_entry_ignores_stale_times_in_a_disabled_cell() {
  std::cout << "\n=== clear_schedule_entry: a disabled cell's leftover times are not a mismatch ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  uint8_t *tuesday = h.sim.layers[3] + 1 * 6;
  tuesday[0] = 0x01; tuesday[1] = 0x02;
  tuesday[2] = 7; tuesday[3] = 15; tuesday[4] = 9; tuesday[5] = 45;
  h.sim.keep_times_when_disabling = true;

  h.write_op.submit_clear_schedule_entry(3, 1, "ce4");
  h.advance(25000);

  TEST_ASSERT(h.events_for("ce4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ce4");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the day is off, so the clear is accepted");
  TEST_ASSERT(h.sim.layers[3][1 * 6] == 0x00, "pump day cell is disabled");
  TEST_ASSERT(h.sim.layers[3][1 * 6 + 2] == 7 && h.sim.layers[3][1 * 6 + 3] == 15,
              "and the pump really did keep the old times -- the fixture bites");
}

// The mismatch retry ladder in confirm_schedule_entry_, which no assertion
// named. A pump that acks the layer write but commits it a beat later answers
// the first confirm read with the pre-write image. Without the ladder that is
// a REJECTED for a write that took.
static void test_clear_schedule_entry_retries_a_late_commit() {
  std::cout << "\n=== clear_schedule_entry: pump commits late -> retry, not rejection ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  uint8_t *wednesday = h.sim.layers[4] + 2 * 6;
  wednesday[0] = 0x01; wednesday[1] = 0x02;
  wednesday[2] = 6; wednesday[3] = 0; wednesday[4] = 8; wednesday[5] = 0;
  h.sim.defer_layer_writes = 1;

  h.write_op.submit_clear_schedule_entry(4, 2, "ce5");
  h.advance(30000);

  TEST_ASSERT(h.events_for("ce5") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ce5");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the retry sees the committed clear and accepts");
  TEST_ASSERT(h.sim.layers[4][2 * 6] == 0x00, "pump day cell ended up disabled");
}

static void test_schedule_enabled_verified_rmw() {
  std::cout << "\n=== set_schedule_enabled: verified, preserves overview bytes ===" << std::endl;
  Harness h;
  h.prime_cache();
  TEST_ASSERT(!h.sim.sched_enabled, "sim starts disabled");

  h.write_op.submit_set_schedule_enabled(true, "en1");
  h.advance(15000);

  TEST_ASSERT(h.events_for("en1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("en1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->sched_enabled == 1, "settled enabled state reported");
  TEST_ASSERT(h.sim.sched_enabled, "pump schedule is enabled");
  // RMW preservation: every non-flag byte of the written overview must match
  // the pump's real overview, not hardcoded defaults.
  const uint8_t expect[10] = {0x8C, 0x23, 0x05, 0x05, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00};
  TEST_ASSERT(memcmp(h.sim.last_overview_write, expect, 10) == 0,
              "overview write preserved the pump's structure bytes");
}

// SET_SCHEDULE_ENABLED had the accepted case above and nothing else, so its
// confirm comparator was only ever asked a question it answered yes to. The
// failure status §9 step 6 requires is this one: the pump takes the frame and
// keeps its old flag.
static void test_schedule_enabled_pump_keeps_its_flag() {
  std::cout << "\n=== set_schedule_enabled: pump ignores the flag -> rejected ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.honor_overview_writes = false;

  h.write_op.submit_set_schedule_enabled(true, "en2");
  h.advance(30000);

  TEST_ASSERT(h.events_for("en2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("en2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("still reports schedule disabled") != std::string::npos,
              "detail names the state the pump kept");
  TEST_ASSERT(r && r->sched_enabled == 0,
              "settled flag is the pump's actual state, not the requested one");
  TEST_ASSERT(!h.sim.sched_enabled, "pump schedule stayed disabled");
}

// ---------------------------------------------------------------------------
// Auto-slot resolution (issue #262)
//
// The resolver picks a slot by asking which stored events have expired, and it
// used to ask that question against the NEW EVENT'S BEGIN rather than against
// the clock. For an event a few minutes out the two agree, which is what the
// Lovelace card's Quick Run presets produce. For an event years out they do
// not: a 2040 event makes everything in the next thirteen-odd years look
// expired, so the picker hands back a slot holding a live event and the write
// destroys it -- observed on the bench, with four slots free.
//
// Every fixture below therefore states its timestamps relative to
// NODE_EPOCH_AT_BOOT, and that is not cosmetic. Written as bare small integers
// they land in 1970, and then a fixture says something other than what it looks
// like it says: test_single_event_auto_slot's "live" event ended at 3000000, so
// it was live only relative to the new event's begin and expired against any
// real clock. Its slot-1 assertion held under the old comparison and failed
// under the fixed one -- the fixture, not the code, was what had to change.
// ---------------------------------------------------------------------------

// Windows used by the single-event slot tests, all relative to the node clock.
static constexpr uint32_t EVENT_ENDED_BEGIN = NODE_EPOCH_AT_BOOT - 7200;   // ended 1 h ago
static constexpr uint32_t EVENT_ENDED_END = NODE_EPOCH_AT_BOOT - 3600;
static constexpr uint32_t EVENT_TOMORROW_BEGIN = NODE_EPOCH_AT_BOOT + 86400;
static constexpr uint32_t EVENT_TOMORROW_END = NODE_EPOCH_AT_BOOT + 86400 + 3600;
static constexpr uint32_t EVENT_NEXT_WEEK_BEGIN = NODE_EPOCH_AT_BOOT + 7 * 86400;
static constexpr uint32_t EVENT_NEXT_WEEK_END = NODE_EPOCH_AT_BOOT + 7 * 86400 + 3600;
// The pair from the bench transcript in issue #262: 2040-06-01 10:00-10:05 UTC.
static constexpr uint32_t EVENT_2040_BEGIN = 2222157600;
static constexpr uint32_t EVENT_2040_END = 2222157900;

static void test_single_event_auto_slot() {
  std::cout << "\n=== set_single_event: auto slot skips occupied slot 0 ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 0 holds a LIVE event -- it has not begun yet, let alone ended. The
  // cache is cold, so the op must read the slots first instead of blindly
  // picking slot 0.
  h.seed_single_event(0, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "ev1");
  h.advance(60000);

  TEST_ASSERT(h.events_for("ev1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 1, "auto-resolved slot 1 (slot 0 occupied) echoed");
  TEST_ASSERT(r && r->begin_ts == EVENT_NEXT_WEEK_BEGIN && r->end_ts == EVENT_NEXT_WEEK_END,
              "settled timestamps reported");
  TEST_ASSERT(h.sim.single_events[1][0] == 1, "pump slot 1 holds the enabled event");
  TEST_ASSERT(h.sim.single_events[0][0] == 1, "slot 0 was not overwritten");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "and still holds its own window, not the new one");
}

// The counterpart: an event that really is over does not hold its slot, so the
// pool cannot fill up with history. Every other slot here holds a LIVE event,
// which is what forces the recycle -- an empty slot would be taken first.
static void test_single_event_reuses_expired_slot() {
  std::cout << "\n=== set_single_event: expired slot is reused, pool never exhausts ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  for (uint8_t i = 0; i < 4; i++)
    h.seed_single_event(i, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);
  h.seed_single_event(4, 0x02, EVENT_ENDED_BEGIN, EVENT_ENDED_END);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "ev3");
  h.advance(60000);

  TEST_ASSERT(h.events_for("ev3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev3");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "a pool whose only spare slot has expired is not a full pool");
  TEST_ASSERT(r && r->slot == 4, "the expired slot was reused");
  TEST_ASSERT(h.sim_single_event_begin(4) == EVENT_NEXT_WEEK_BEGIN,
              "pump slot 4 now holds the new event");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "and the four live events are untouched");
  // Recycling a slot destroys what was in it. That is legitimate here, and it
  // used to be silent: the operation settles ACCEPTED because the write did
  // land, and nothing said the slot had been occupied. Diagnosing #262 cost
  // hours for exactly that reason, so the settle event says so.
  TEST_ASSERT(r && r->detail.find("reused slot 4") != std::string::npos,
              "the settle detail states that a slot was recycled");
  TEST_ASSERT(r && r->detail.find(std::to_string(EVENT_ENDED_END)) != std::string::npos,
              "and names the window it replaced");
}

// ...but only when it has to. Recycling costs the stored record of an event
// that ran, and the picker used to spend it while four slots sat empty: it took
// the first index no LIVE event held, which is slot 0 whenever slot 0 has
// expired. On a five-slot pump that meant repeated one-time runs cycled through
// slot 0 forever and never touched slots 1-4, and every one of them carried the
// "this slot was recycled" warning, which is how a warning stops meaning
// anything.
static void test_single_event_prefers_an_empty_slot_to_an_expired_one() {
  std::cout << "\n=== set_single_event: an empty slot beats an expired one ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  h.seed_single_event(0, 0x02, EVENT_ENDED_BEGIN, EVENT_ENDED_END);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "prefer");
  h.advance(60000);

  const WriteResult *r = h.result_for("prefer");
  TEST_ASSERT(h.events_for("prefer") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 1, "the empty slot 1 was taken, not the expired slot 0");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_ENDED_BEGIN,
              "the expired event is still on the pump, since nothing needed its slot");
  TEST_ASSERT(r && r->detail.empty(),
              "and nothing was recycled, so nothing is reported as recycled");
}

// A pool that really is full: five LIVE events and a working clock. The refusal
// must be the plain one -- naming the clock here would send the reader after a
// problem the node does not have.
static void test_a_pool_of_live_events_is_a_full_pool() {
  std::cout << "\n=== set_single_event: five live events -> plainly full ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  for (uint8_t i = 0; i < 5; i++)
    h.seed_single_event(i, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "full");
  h.advance(60000);

  const WriteResult *r = h.result_for("full");
  TEST_ASSERT(h.events_for("full") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail == "no free single event slots",
              "the plain refusal, with no clock excuse attached to it");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "and no live event was overwritten");
}

// The #262 regression itself. Two events fifteen seconds apart on a pump with
// five free slots: one tomorrow, then one in 2040. Against the old comparison
// the 2040 write resolved to slot 0 -- the tomorrow event's slot -- and wiped
// it, reporting `accepted`.
static void test_a_far_future_event_does_not_evict_a_live_one() {
  std::cout << "\n=== set_single_event: an event years out must not evict a live one (issue #262) ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;  // the bench pump's real slot count
  h.seed_single_event(0, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);

  h.write_op.submit_set_single_event(EVENT_2040_BEGIN, EVENT_2040_END, "ev2040");
  h.advance(60000);

  TEST_ASSERT(h.events_for("ev2040") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev2040");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot != 0, "the 2040 event did not take the occupied slot");
  TEST_ASSERT(h.sim.single_events[0][0] == 1 &&
                  h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "tomorrow's event is still in slot 0, untouched");
  TEST_ASSERT(r && r->slot >= 0 && h.sim_single_event_begin(static_cast<uint8_t>(r->slot)) ==
                                       EVENT_2040_BEGIN,
              "and the 2040 event landed in the free slot it was given");
  // Nothing was recycled, so nothing is claimed to have been.
  TEST_ASSERT(r && r->detail.empty(), "a write to a genuinely free slot settles with no note");
}

// The same defect on the surface most likely to hit it. A vacation is months
// out by nature, and submit_set_vacation resolves through the same picker, so
// booking next summer would have cleared every single event before it.
static void test_a_far_future_vacation_does_not_evict_a_live_event() {
  std::cout << "\n=== set_vacation: a vacation months out must not evict a live event (issue #262) ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  h.seed_single_event(0, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);

  h.write_op.submit_set_vacation(EVENT_2040_BEGIN, EVENT_2040_END, "vac2040");
  h.advance(60000);

  const WriteResult *r = h.result_for("vac2040");
  TEST_ASSERT(h.events_for("vac2040") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot != 0, "the vacation did not take the occupied slot");
  TEST_ASSERT(h.sim.single_events[0][0] == 1 && h.sim.single_events[0][1] == 0x02 &&
                  h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "the live run event survives the vacation write");
}

// With no synced wall clock there is no honest answer to "has this expired", so
// the picker treats every enabled event as holding its slot. The pool looks
// full rather than empty -- the conservative direction, since the other one
// overwrites live events -- and the refusal has to say which of the two
// problems it is, or a node that never synced looks like a full pump.
static void test_without_a_clock_no_slot_is_treated_as_expired() {
  std::cout << "\n=== set_single_event: no node clock -> nothing is expired (issue #262) ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  h.set_node_time(0);  // never synced: the clock reads 1970
  // All five slots hold events that ended long before the new one begins. With
  // a real clock every one of them is reusable; with no clock, none is.
  for (uint8_t i = 0; i < 5; i++)
    h.seed_single_event(i, 0x02, EVENT_ENDED_BEGIN, EVENT_ENDED_END);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "noclock");
  h.advance(60000);

  TEST_ASSERT(h.events_for("noclock") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("noclock");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "the write is refused, not guessed at");
  TEST_ASSERT(r && r->detail.find("node clock not set") != std::string::npos,
              "the refusal names the clock, not just a full pool");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_ENDED_BEGIN,
              "and no slot was overwritten");
}

// The same pump with a clock: the five expired events are all reusable, so the
// write lands. Without this the test above passes against a picker that refuses
// everything, clock or no clock.
//
// The five ended at different times, and the STALEST goes. Recycling by lowest
// index instead would throw away the most recent record and keep the oldest,
// which is backwards, and slot 0 as the answer is also what a picker that had
// simply stopped thinking would return.
static void test_with_a_clock_the_same_expired_slots_are_reusable() {
  std::cout << "\n=== set_single_event: with a clock, the stalest expired slot is reused ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  // Slot 3 ended first, so slot 3 is the one to lose. Deliberately neither the
  // lowest nor the highest index.
  const uint32_t ENDS[5] = {EVENT_ENDED_END, EVENT_ENDED_END - 600, EVENT_ENDED_END - 300,
                            EVENT_ENDED_END - 3600, EVENT_ENDED_END - 900};
  for (uint8_t i = 0; i < 5; i++)
    h.seed_single_event(i, 0x02, ENDS[i] - 60, ENDS[i]);

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "clock_ok");
  h.advance(60000);

  const WriteResult *r = h.result_for("clock_ok");
  TEST_ASSERT(h.events_for("clock_ok") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "a pool of expired events is not a full pool");
  TEST_ASSERT(r && r->slot == 3, "the event that ended first is the one recycled");
  TEST_ASSERT(r && r->detail.find(std::to_string(ENDS[3])) != std::string::npos,
              "and the settle detail names the window it replaced");
}

// ---------------------------------------------------------------------------
// find_free_single_event_slot() directly (issue #262)
//
// Everything above reaches the picker through a whole write operation, at
// whatever "now" the harness happens to be at. That is the right shape for the
// eviction tests and the wrong shape for the picker's own decision table: the
// reference timestamp is the harness's, not the test's, so the interesting
// values -- 0, one second either side of an event's end -- cannot be asked for.
// These call it with the cache warmed through the real read path and the
// reference chosen by hand.
// ---------------------------------------------------------------------------

/// Warm the overview and single-event caches from the sim, the way the boot
/// chain does, so the picker has something real to answer from.
static void warm_single_event_cache(Harness &h) {
  h.schedule.poll_state_async(nullptr);
  h.advance(300);
  h.schedule.read_single_events_async(nullptr);
  h.advance(5000);
}

static void test_slot_picker_decision_table() {
  std::cout << "\n=== find_free_single_event_slot: the decision table, called directly ===" << std::endl;
  const uint32_t T = NODE_EPOCH_AT_BOOT;

  {
    Harness h;
    h.sim.max_single_events = 5;
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T) == -1,
                "a cold cache answers -1 rather than handing out slot 0 (issue #92)");
  }
  {
    // Two expired slots, one live, one empty.
    Harness h;
    h.sim.max_single_events = 5;
    h.seed_single_event(0, 0x02, T - 7200, T - 3600);
    h.seed_single_event(1, 0x02, T - 60, T);
    h.seed_single_event(2, 0x02, T + 3600, T + 7200);
    h.seed_single_event(4, 0x02, T - 10800, T - 7200);
    warm_single_event_cache(h);
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T) == 3,
                "the empty slot is taken while two expired ones sit there");
    TEST_ASSERT(h.schedule.find_free_single_event_slot(0) == 3,
                "and with no clock the empty slot is still the answer");
  }
  {
    // Every slot occupied: two expired, three live.
    Harness h;
    h.sim.max_single_events = 5;
    h.seed_single_event(0, 0x02, T - 7200, T - 3600);
    h.seed_single_event(1, 0x02, T - 60, T);
    h.seed_single_event(2, 0x02, T + 3600, T + 7200);
    h.seed_single_event(3, 0x02, T + 7200, T + 10800);
    h.seed_single_event(4, 0x02, T - 10800, T - 7200);
    warm_single_event_cache(h);
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T) == 4,
                "with nothing empty, the event that ended FIRST is recycled, not the lowest index");
    TEST_ASSERT(h.schedule.find_free_single_event_slot(0) == -1,
                "with no clock nothing has expired, so a full pool it is");
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T - 100000) == -1,
                "and at a reference before any of them ended, likewise");
  }
  {
    // The boundary. Slot 2 ends exactly at the reference instant; everything
    // else is live. "Expired" means ended BEFORE now, so at T it is not yet
    // recyclable and one second later it is. Nothing recorded which way this
    // went, and a `<` that drifted to `<=` would recycle an event in the second
    // it is still running.
    Harness h;
    h.sim.max_single_events = 5;
    for (uint8_t i = 0; i < 5; i++)
      h.seed_single_event(i, 0x02, T + 3600, T + 7200);
    h.seed_single_event(2, 0x02, T - 60, T);
    warm_single_event_cache(h);
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T) == -1,
                "an event ending exactly now has not ended before now, so it keeps its slot");
    TEST_ASSERT(h.schedule.find_free_single_event_slot(T + 1) == 2,
                "one second later it has");
  }
}

static void test_clear_single_event() {
  std::cout << "\n=== clear_single_event: verified accepted ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.single_events[3][0] = 1;

  h.write_op.submit_clear_single_event(3, "ev2");
  h.advance(20000);

  TEST_ASSERT(h.events_for("ev2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev2");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 3, "slot echoed");
  TEST_ASSERT(h.sim.single_events[3][0] == 0, "pump slot 3 is disabled");
}

// A caller-supplied slot is bounded by the count the pump reports, not by the
// 35-slot fallback -- this specimen answers 5. Without the bound, slot 10 goes
// out as SubID 910; a pump that owns only 900-904 never answers it, so the
// operation settles TIMEOUT ~15 s later instead of REJECTED up front, and the
// detail blames the link rather than the argument.
//
// Slot 10 alone kills a hardcoded `slot >= 35` (35 would accept it here), and
// slot 5 alone kills `>` for `>=`. The accepted case in the next test is not
// what makes either fail -- it guards the opposite error, a bound one slot too
// tight, which no mutation here covers.
static void test_single_event_slot_bounded_by_pump_capacity() {
  std::cout << "\n=== single event: slot past the pump's count -> rejected, nothing written ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  // Slot 10 is one the *simulator* would happily answer (it models 35), so a
  // failure here is the bound going missing, not the sim declining to reply.
  h.sim.single_events[10][0] = 1;

  h.write_op.submit_clear_single_event(10, "sb1");
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb1");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("out of range") != std::string::npos,
              "detail names the argument, not a timeout");
  TEST_ASSERT(r && r->detail.find("pump has 5") != std::string::npos,
              "detail reports the pump's own count, not the 35 fallback");
  TEST_ASSERT(h.sim.overview_writes == 0, "no configuration commit reached the pump");
  TEST_ASSERT(h.sim.single_events[10][0] == 1, "slot 10 was left untouched");

  // max_events is a count, so the highest legal slot is max_events - 1. Slot 5
  // on a 5-slot pump is the off-by-one this pins; the sim answers SubID 905
  // happily, so only the bound distinguishes it.
  h.sim.single_events[5][0] = 1;
  h.write_op.submit_clear_single_event(5, "sb1b");
  h.advance(25000);

  const WriteResult *rb = h.result_for("sb1b");
  TEST_ASSERT(h.events_for("sb1b") == 1, "exactly one terminal event for the boundary slot");
  TEST_ASSERT(rb && rb->status == WriteStatus::REJECTED, "slot == max_events is out of range");
  TEST_ASSERT(rb && rb->detail.find("pump has 5") != std::string::npos,
              "the boundary case names the count too, not only slot 10");
  TEST_ASSERT(h.sim.overview_writes == 0, "still no configuration commit");
  TEST_ASSERT(h.sim.single_events[5][0] == 1, "slot 5 was left untouched");
}

// Past SINGLE_EVENT_SLOT_LIMIT the slot is wrong on every pump, not just this
// one: SubID 900+100 is 1000, which is the weekly schedule's layer 0 record.
// So it settles INVALID before the overview read rather than REJECTED after it
// -- which is the whole point of the ordering. With the link down, deferring
// would answer "overview not readable" and blame the link for an argument that
// could never have been right.
static void test_single_event_slot_past_the_protocol_limit_is_invalid() {
  std::cout << "\n=== single event: slot past the SubID space -> invalid, no overview read ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.respond_overview_reads = false;   // the link cannot answer

  h.write_op.submit_clear_single_event(100, "sb4");
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb4");
  TEST_ASSERT(r && r->status == WriteStatus::INVALID,
              "a structurally impossible slot is invalid, not rejected");
  TEST_ASSERT(r && r->detail.find("not a single-event slot") != std::string::npos,
              "detail blames the argument even though the overview is unreadable");
  TEST_ASSERT(r && r->detail.find("overview") == std::string::npos,
              "detail does not blame the link");
}

// The complement, and the honest limit of the split: a slot this *pump* lacks
// but the protocol allows still reports the overview failure when the link is
// down. Asserting "out of range (pump has N)" would mean claiming a count that
// was never read. Pinned so the distinction is deliberate rather than drift.
static void test_device_range_defers_to_the_overview_failure() {
  std::cout << "\n=== single event: device-range slot + dead link -> reports the link ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.respond_overview_reads = false;

  h.write_op.submit_clear_single_event(50, "sb5");
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb5") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb5");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "rejected, not invalid");
  TEST_ASSERT(r && r->detail.find("not attempted") != std::string::npos,
              "detail reports the unreadable overview");
}

// The auto-slot branch resolves through find_vacation_slot(), which has no
// bound of its own -- it returns whatever index the cache holds. What keeps it
// safe is upstream: read_single_events_async() only reads slots 0..max-1, so an
// out-of-range index can never enter the cache to be found.
//
// This pins that upstream property, because it is the entire reason the auto
// path is safe. A Stop event parked past the pump's count is invisible: the
// operation reports no vacation rather than resolving slot 10 and writing to
// SubID 910. If the read loop ever widened, this fails here rather than on a
// pump.
static void test_out_of_range_events_never_enter_the_cache() {
  std::cout << "\n=== clear_vacation: a Stop event past the pump's count is not found ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  h.sim.single_events[10][0] = 1;     // enabled
  h.sim.single_events[10][1] = 0x01;  // Stop -> would be the vacation if visible

  h.write_op.submit_clear_vacation("sb6");
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb6") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb6");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "settles accepted");
  TEST_ASSERT(r && r->detail.find("no active vacation") != std::string::npos,
              "slot 10 was never a candidate -- the read never reached it");
  TEST_ASSERT(h.sim.single_events[10][0] == 1, "slot 10 was left untouched");
}

static void test_single_event_last_valid_slot_still_writes() {
  std::cout << "\n=== single event: the pump's last slot is still writable ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;
  h.sim.single_events[4][0] = 1;

  h.write_op.submit_clear_single_event(4, "sb2");
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb2");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "max_events-1 is in range");
  TEST_ASSERT(h.sim.single_events[4][0] == 0, "pump slot 4 is disabled");
}

// The bound sits in run_single_event_(), which both commands share, so the
// entity path's explicit-slot write (AlphaHwrComponent::write_single_event)
// is covered by the same check rather than by a second copy of it.
static void test_set_single_event_explicit_slot_is_bounded() {
  std::cout << "\n=== set_single_event: explicit slot past the pump's count -> rejected ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;

  h.write_op.submit_set_single_event(1000000, 2000000, "sb3", nullptr, 10,
                                     WriteOrigin::ENTITY);
  h.advance(25000);

  TEST_ASSERT(h.events_for("sb3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("sb3");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("out of range") != std::string::npos,
              "detail names the argument");
  TEST_ASSERT(h.sim.single_events[10][0] == 0, "no event was written to slot 10");
}

static void test_set_vacation_writes_stop_event() {
  std::cout << "\n=== set_vacation: writes a Stop single-event ===" << std::endl;
  Harness h;
  h.prime_cache();

  // Multi-day range: begin 1000000, end 2000000.
  h.write_op.submit_set_vacation(1000000, 2000000, "vac1");
  h.advance(60000);

  TEST_ASSERT(h.events_for("vac1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("vac1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  int slot = r ? r->slot : -1;
  TEST_ASSERT(slot >= 0 && h.sim.single_events[slot][0] == 1, "slot holds an enabled event");
  TEST_ASSERT(slot >= 0 && h.sim.single_events[slot][1] == 0x01,
              "action byte is Stop (0x01), not Auto");
}

// The confirm compared the enabled flag and the window but NOT the action, so
// a pump that stored the opposite kind of event settled ACCEPTED. The two are
// opposites: 0x01 Stop holds the pump off across the window (a vacation),
// 0x02 Run turns it on. A vacation confirmed as set while the pump was in fact
// scheduled to RUN for the whole week is the failure this pins -- and it
// reported success, so nothing downstream could notice either.
static void test_a_vacation_stored_as_a_run_event_is_rejected() {
  std::cout << "\n=== set_vacation: pump stores a Run where a Stop was written -> rejected ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.force_single_event_action = 0x02;  // pump keeps everything else, flips the kind

  h.write_op.submit_set_vacation(1000000, 2000000, "vac_bad");
  h.advance(60000);

  TEST_ASSERT(h.events_for("vac_bad") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("vac_bad");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "an inverted event kind is a rejection, not an accepted vacation");
  // The window matched exactly, so only the action comparison can have caught
  // it -- and the settled fields must report what the pump actually holds.
  TEST_ASSERT(r && r->single_event_action == 0x02,
              "the settled action reports the Run the pump kept");
}

// The mirror: a one-time run stored as a Stop would hold the pump off over a
// window the user asked it to run in.
static void test_a_run_event_stored_as_a_stop_is_rejected() {
  std::cout << "\n=== set_single_event: pump stores a Stop where a Run was written -> rejected ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.force_single_event_action = 0x01;

  h.write_op.submit_set_single_event(1000000, 2000000, "run_bad");
  h.advance(60000);

  TEST_ASSERT(h.events_for("run_bad") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("run_bad");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "an inverted event kind is a rejection");
  TEST_ASSERT(r && r->single_event_action == 0x01,
              "the settled action reports the Stop the pump kept");
}

// The counterpart, so the two above cannot pass by rejecting everything: an
// honest pump still settles ACCEPTED, and the action reaches the result.
static void test_a_matching_action_still_settles_accepted() {
  std::cout << "\n=== set_vacation: a pump that stores the Stop settles accepted ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_vacation(1000000, 2000000, "vac_ok");
  h.advance(60000);

  const WriteResult *r = h.result_for("vac_ok");
  TEST_ASSERT(h.events_for("vac_ok") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->single_event_action == 0x01,
              "and the result carries Stop, so a client can tell a vacation from a run");
}

// The confirm skips the content comparison for a CLEAR -- a slot we asked to
// be off has no meaningful window or action. Nothing pinned that: after a
// normal clear the slot reads back all zeros and the operation's own
// begin/end are zero too, so comparing them changes nothing and the skip could
// be deleted unnoticed. It matters for a pump that clears the enabled byte and
// leaves the rest, which would then have every clear settle REJECTED.
static void test_clear_single_event_ignores_stale_content_in_a_disabled_slot() {
  std::cout << "\n=== clear_single_event: leftover content in a disabled slot is not a mismatch ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 1 holds an enabled Stop event with a real window.
  h.sim.single_events[1][0] = 1;
  h.sim.single_events[1][1] = 0x01;
  h.sim.single_events[1][5] = 0x10;
  h.sim.single_events[1][9] = 0x20;
  h.sim.keep_single_event_content_when_clearing = true;

  h.write_op.submit_clear_single_event(1, "cse_stale");
  h.advance(60000);

  TEST_ASSERT(h.events_for("cse_stale") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("cse_stale");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the slot is off, so the clear is accepted");
  TEST_ASSERT(h.sim.single_events[1][0] == 0, "pump slot is disabled");
  TEST_ASSERT(h.sim.single_events[1][1] == 0x01,
              "and the pump really did keep the old action -- the fixture bites");
}

static void test_clear_vacation_targets_stop_slot() {
  std::cout << "\n=== clear_vacation: clears the Stop slot, leaves run events ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 1 = a one-time RUN event (action Auto); slot 2 = a vacation (Stop).
  h.sim.single_events[1][0] = 1; h.sim.single_events[1][1] = 0x02;
  h.sim.single_events[1][9] = 0x40;  // some future end
  h.sim.single_events[2][0] = 1; h.sim.single_events[2][1] = 0x01;  // Stop = vacation
  h.sim.single_events[2][9] = 0x40;

  h.write_op.submit_clear_vacation("vac2");
  h.advance(60000);

  TEST_ASSERT(h.events_for("vac2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("vac2");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 2, "auto-resolved the vacation (Stop) slot 2");
  TEST_ASSERT(h.sim.single_events[2][0] == 0, "vacation slot 2 is now disabled");
  TEST_ASSERT(h.sim.single_events[1][0] == 1, "run event in slot 1 left untouched");
}

static void test_clear_vacation_none_active() {
  std::cout << "\n=== clear_vacation: no active vacation settles accepted ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Only a RUN event present, no Stop event.
  h.sim.single_events[0][0] = 1; h.sim.single_events[0][1] = 0x02;
  h.sim.single_events[0][9] = 0x40;

  h.write_op.submit_clear_vacation("vac3");
  h.advance(60000);

  const WriteResult *r = h.result_for("vac3");
  TEST_ASSERT(h.events_for("vac3") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "accepted (nothing to clear)");
  TEST_ASSERT(h.sim.single_events[0][0] == 1, "run event untouched");
}

static void test_schedule_supersede_keys() {
  std::cout << "\n=== schedule supersede: per-(layer,day) keys ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_schedule_entry(0, 0, 6, 0, 8, 0, "mon");   // runs
  h.write_op.submit_set_schedule_entry(0, 1, 6, 0, 8, 0, "tue");   // queued, different day
  h.write_op.submit_set_schedule_entry(0, 1, 7, 0, 9, 0, "tue2");  // supersedes "tue"

  TEST_ASSERT(h.events_for("tue") == 1, "same-day queued op superseded immediately");
  const WriteResult *rt = h.result_for("tue");
  TEST_ASSERT(rt && rt->status == WriteStatus::SUPERSEDED, "tue is superseded");

  h.advance(60000);
  TEST_ASSERT(h.events_for("mon") == 1 && h.events_for("tue2") == 1,
              "different-day ops both ran to a terminal event");
  const WriteResult *rm = h.result_for("mon");
  const WriteResult *rt2 = h.result_for("tue2");
  TEST_ASSERT(rm && rm->status == WriteStatus::ACCEPTED, "monday accepted");
  TEST_ASSERT(rt2 && rt2->status == WriteStatus::ACCEPTED, "tuesday (last write) accepted");
  const uint8_t *tue_bytes = h.sim.layers[0] + 1 * 6;
  TEST_ASSERT(tue_bytes[2] == 7 && tue_bytes[4] == 9, "tuesday holds the superseding times");
}

static void test_refresh_ops() {
  std::cout << "\n=== refresh_schedule / refresh_single_events: terminal accepted ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.layers[2][0] = 1;  // Monday on layer 2 enabled
  h.sim.layers[2][2] = 6;
  h.sim.layers[2][4] = 8;
  h.sim.single_events[5][0] = 1;

  h.write_op.submit_refresh_schedule("rs1");
  h.write_op.submit_refresh_single_events("re1");
  h.advance(120000);

  TEST_ASSERT(h.events_for("rs1") == 1 && h.events_for("re1") == 1,
              "both refreshes got exactly one terminal event");
  const WriteResult *rs = h.result_for("rs1");
  const WriteResult *re = h.result_for("re1");
  TEST_ASSERT(rs && rs->status == WriteStatus::ACCEPTED, "refresh_schedule accepted");
  TEST_ASSERT(rs && rs->event_count == 1, "refresh_schedule counted the entry");
  TEST_ASSERT(re && re->status == WriteStatus::ACCEPTED, "refresh_single_events accepted");
  TEST_ASSERT(re && re->event_count == 1, "refresh_single_events counted the event");
}

// ---------------------------------------------------------------------------
// Issue #136: the read-all chains used to report success unconditionally.
// ---------------------------------------------------------------------------

static void test_refresh_schedule_all_layers_fail() {
  std::cout << "\n=== refresh_schedule: every layer read fails (issue #136) ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Layers hold real data, but the pump answers no layer read at all. Before
  // #136 the chain reported success with an empty grid, so refresh settled
  // ACCEPTED and an automation could not tell "schedule is genuinely empty"
  // from "every read failed".
  h.sim.layers[2][0] = 1;
  h.sim.layers[2][2] = 6;
  h.sim.layers[2][4] = 8;
  h.sim.respond_layer_reads = false;

  h.write_op.submit_refresh_schedule("rs_fail");
  h.advance(120000);  // 5 layers x 3 s APDU timeout, with headroom

  TEST_ASSERT(h.events_for("rs_fail") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rs_fail");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "all layers failing settles TIMEOUT, not ACCEPTED");
}

static void test_refresh_schedule_partial_layer_failure() {
  std::cout << "\n=== refresh_schedule: one layer fails (issue #136) ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.layers[0][0] = 1;
  h.sim.layers[0][2] = 6;
  h.sim.layers[0][4] = 8;
  // Layers 0-3 answer; layer 4 does not. A partial grid cannot support the
  // claim the success boolean makes ("the pump's schedule is now known"), so
  // the all-or-nothing rule fails the whole read.
  h.sim.drop_layer_reads.insert(4);

  h.write_op.submit_refresh_schedule("rs_part");
  h.advance(120000);

  TEST_ASSERT(h.events_for("rs_part") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("rs_part");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "a single failed layer fails the whole read");
}

// ---------------------------------------------------------------------------
// SET_CLOCK (Object 94). Every one of these drives the shipped operation
// against the simulator; before it existed the clock write had no host test at
// all and reported success from having formatted a packet.
// ---------------------------------------------------------------------------

// The node's wall clock, as ESPHome would hand it over. main() pins TZ=UTC, so
// the local fields on the wire and the UTC epochs the sim keeps are the same
// numbers and no test here is measuring the timezone engine.
static esphome::ESPTime node_time(time_t epoch) {
  return esphome::ESPTime::from_epoch_local(epoch);
}

static void test_set_clock_accepted() {
  std::cout << "\n=== set_clock: pump takes the write and confirms ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;      // 2026-08-07 12:00:00 UTC
  h.sim.clock_epoch = now - 3600;     // pump an hour behind
  h.sim.clock_base_ms = mock_millis;

  h.write_op.submit_set_clock(node_time(now), "clk1");
  h.advance(10000);

  TEST_ASSERT(h.events_for("clk1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "a confirmed sync settles ACCEPTED");
  TEST_ASSERT(r && !std::isnan(r->clock_offset_s) && std::fabs(r->clock_offset_s) <= 1.0f,
              "the settled offset is the measured post-write drift, near zero");
  TEST_ASSERT(h.frames_clock_write == 1, "exactly one Sub 100 write went out");
  TEST_ASSERT(h.frames_clock_read >= 1, "the confirm read Sub 101 back");
  TEST_ASSERT(r && r->command == WriteCommand::SET_CLOCK, "reported as set_clock");
}

// ── No Class 10 write leaves its acknowledgement lying about ──────────────
// Issue #253. Four Class 10 sends used to go out with no callback, so the
// transport never entered AWAITING_RESPONSE for them and nothing consumed their
// replies. Three of the four are reachable from here: the fused Obj 0601
// control request and the OpSpec 0x88 register write, both inside SET_SETPOINT,
// and the Obj 94 Sub 100 clock write inside SET_CLOCK. (The ClockProgram commit
// and the layer write are ScheduleService's, and test_schedule_service.cpp
// covers those.)
//
// The assertion is deliberately not "the operation still works" -- it did
// before, which is why this went unnoticed for so long. It is that the
// acknowledgement is SPENT: the harness counts every short Class 10 frame that
// reaches the packet callback, which is where a reply lands when no command
// claimed it. Reverting any of these call sites to a null callback puts its
// reply there and turns this red.
//
// The simulator answers all three because the pump does: 195 Class 10 SETs in
// resources/traffic_capture, every one acknowledged, none of them
// distinguishable from another.
static void test_no_class10_write_leaves_an_unclaimed_ack() {
  std::cout << "\n=== Every Class 10 write consumes its own acknowledgement ===" << std::endl;

  {
    Harness h;
    h.prime_cache();
    h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000.0f, "ff1");
    h.advance(10000);
    TEST_ASSERT(h.events_for("ff1") == 1, "set_setpoint settled");
    TEST_ASSERT(h.frames_0601 == 1 && h.frames_register == 1,
                "both of its Class 10 writes went out -- the fused control request "
                "and the register write");
    TEST_ASSERT(h.stray_short_acks == 0,
                "and neither left an acknowledgement for the next write to be handed");
  }

  {
    Harness h;
    h.prime_cache();
    const time_t now = 1786104000;
    h.sim.clock_epoch = now - 3600;
    h.sim.clock_base_ms = mock_millis;
    h.write_op.submit_set_clock(node_time(now), "ff2");
    h.advance(10000);
    TEST_ASSERT(h.events_for("ff2") == 1, "set_clock settled");
    TEST_ASSERT(h.frames_clock_write == 1, "its Sub 100 write went out");
    TEST_ASSERT(h.stray_short_acks == 0,
                "and the clock write's acknowledgement was consumed by the clock write");
  }

  // The Object 84 writes -- schedule entry (a layer write plus an overview
  // commit), schedule enable (the overview write directly) and a single event.
  // None of these was fire-and-forget; each asked the transport for a reply
  // carrying a type, which a SET reply cannot carry, so each timed out on every
  // attempt and left its real acknowledgement with no owner.
  {
    Harness h;
    h.prime_cache();
    h.write_op.submit_set_schedule_entry(1, 0, 6, 0, 8, 0, "ff3");
    h.advance(30000);
    TEST_ASSERT(h.events_for("ff3") == 1, "set_schedule_entry settled");
    TEST_ASSERT(h.frames_layer_write >= 1, "a layer write went out");
    TEST_ASSERT(h.sim.overview_writes >= 1, "and the commit behind it");
    TEST_ASSERT(h.stray_short_acks == 0,
                "neither the layer write nor the commit left an acknowledgement behind");
  }

  {
    Harness h;
    h.prime_cache();
    h.write_op.submit_set_schedule_enabled(true, "ff4");
    h.advance(30000);
    TEST_ASSERT(h.events_for("ff4") == 1, "set_schedule_enabled settled");
    TEST_ASSERT(h.sim.overview_writes >= 1, "the overview write went out");
    TEST_ASSERT(h.stray_short_acks == 0,
                "and its acknowledgement was consumed by it");
  }

  {
    Harness h;
    h.prime_cache();
    h.write_op.submit_set_single_event(1786104000, 1786107600, "ff5");
    h.advance(60000);
    TEST_ASSERT(h.events_for("ff5") == 1, "set_single_event settled");
    TEST_ASSERT(h.stray_short_acks == 0,
                "the single-event write and its commit each consumed their own "
                "acknowledgement");
  }
}

static void test_set_clock_wire_struct() {
  std::cout << "\n=== set_clock: the APDU on the wire ===" << std::endl;
  Harness h;
  h.prime_cache();
  // 2026-08-07 12:34:56 UTC. Year 2026 = 0x07EA, so a little-endian slip or a
  // shifted field shows up as a specific wrong byte rather than as "invalid".
  h.set_node_time(1786106096);
  h.write_op.submit_set_clock(node_time(1786106096), "clk_wire");
  h.advance(10000);

  // The whole APDU, not just the struct. The eleven header bytes carry the
  // object, sub-id, type, version and declared size, and until this asserted
  // them a skeptic could corrupt the type, the version or the size byte and
  // watch the entire suite stay green -- the simulator only matches on
  // apdu[1..4] and only reads apdu[11..].
  //
  // Shape bench-captured from the pump on 2026-08-15; this fixture writes
  // 2026-08-07 12:34:56 rather than the captured instant.
  static const uint8_t EXPECTED[22] = {
      0x0A,                    // Class 10
      0x94,                    // OpSpec: SET + 20 body bytes
      0x5E,                    // Object 94
      0x00, 0x64,              // Sub-ID 100 (DateTimeConfig)
      0x01, 0x41,              // Type 321
      0x02,                    // Object version 2
      0x00, 0x00, 0x0B,        // Declared size: 11 data bytes
      0x01,                    // leading struct byte
      0x07, 0xEA,              // year 2026, big-endian (AGENTS §3)
      0x08, 0x07,              // month, day
      0x0C, 0x22, 0x38,        // hour 12, minute 34, second 56
      0x00, 0x00, 0x00};       // tail padding
  TEST_ASSERT(h.last_clock_write.size() == 22, "22-byte APDU");
  if (h.last_clock_write.size() == 22) {
    bool same = true;
    for (size_t i = 0; i < 22; i++) {
      if (h.last_clock_write[i] != EXPECTED[i]) {
        same = false;
        std::cout << "  byte " << i << ": got 0x" << std::hex
                  << static_cast<int>(h.last_clock_write[i]) << " want 0x"
                  << static_cast<int>(EXPECTED[i]) << std::dec << std::endl;
      }
    }
    TEST_ASSERT(same, "every byte matches the documented frame");
    // Stated separately, so an edit that moves a field and keeps the frame
    // self-consistent still fails the byte compare, and one that breaks the
    // self-consistency fails here too.
    const auto &w = h.last_clock_write;
    TEST_ASSERT((w[1] & 0x7F) == 20, "OpSpec declares 20 body bytes");
    TEST_ASSERT(w[10] == w.size() - 11, "declared size equals the data that follows");
  }
}

static void test_set_clock_ignored_write_is_rejected() {
  std::cout << "\n=== set_clock: pump ignores the write ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;
  h.sim.honor_clock_writes = false;   // the frame lands nowhere
  h.sim.clock_epoch = now - 600;      // and the pump stays ten minutes behind
  h.sim.clock_base_ms = mock_millis;

  h.write_op.submit_set_clock(node_time(now), "clk2");
  h.advance(20000);

  TEST_ASSERT(h.events_for("clk2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "a pump that kept its old clock settles REJECTED, not ACCEPTED");
  TEST_ASSERT(r && r->detail.find("-600 s off") != std::string::npos,
              "the detail carries the measured offset");
  TEST_ASSERT(r && r->clock_offset_s < -599.0f && r->clock_offset_s > -601.0f,
              "clock_offset_s reports the drift the pump still holds");
  // This is the case the old code reported as success: it called back true on
  // the line after send, so "Last Clock Sync" advanced and the next attempt was
  // suppressed for 24 h with the pump ten minutes out.
}

static void test_set_clock_already_correct_is_accepted() {
  std::cout << "\n=== set_clock: pump was already right ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;
  h.sim.honor_clock_writes = false;   // nothing applies the write...
  h.sim.clock_epoch = now;            // ...but the pump already holds the time
  h.sim.clock_base_ms = mock_millis;

  h.write_op.submit_set_clock(node_time(now), "clk3");
  h.advance(10000);

  const WriteResult *r = h.result_for("clk3");
  TEST_ASSERT(h.events_for("clk3") == 1, "exactly one terminal event");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "the confirm answers 'does the pump hold the right time', not "
              "'did our frame put it there'");
}

static void test_set_clock_unreadable_is_timeout() {
  std::cout << "\n=== set_clock: readback never answers ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.respond_clock_reads = false;

  h.write_op.submit_set_clock(node_time(1786104000), "clk4");
  h.advance(30000);

  TEST_ASSERT(h.events_for("clk4") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk4");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "an undecodable readback is TIMEOUT, not REJECTED -- nothing is "
              "known about the pump's clock");
  TEST_ASSERT(r && r->detail == "pump clock unreadable",
              "settled on the read result, not the operation watchdog");
  TEST_ASSERT(r && std::isnan(r->clock_offset_s), "no offset is claimed");
  TEST_ASSERT(h.frames_clock_read == 3, "the initial confirm plus two retries");
}

static void test_set_clock_slow_ladder_is_not_drift() {
  std::cout << "\n=== set_clock: a slow confirm is not drift ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;
  h.sim.clock_epoch = now - 3600;
  h.sim.clock_base_ms = mock_millis;
  // Swallow the first two readbacks so each costs a full 5 s APDU timeout on
  // top of the 1.5 s retry delay. By the time the third answers, ~14 s of mock
  // time has passed and the pump's clock -- which runs -- has moved with it.
  h.sim.drop_clock_reads = 2;

  const uint32_t started = mock_millis;
  h.write_op.submit_set_clock(node_time(now), "clk5");
  h.advance(24000);

  TEST_ASSERT(mock_millis - started > 10000, "the ladder really did take over 10 s");
  TEST_ASSERT(h.events_for("clk5") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk5");
  // Both clocks run, and the confirm reads them at one instant, so however long
  // the ladder took cancels. An implementation that instead projects the
  // submitted time forward has to get the elapsed interval exactly right --
  // measured from the moment the frame was actually SENT, which nothing here
  // can observe -- and every millisecond it is wrong by lands in the offset.
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "a slow confirm ladder does not manufacture drift");
  TEST_ASSERT(r && std::fabs(r->clock_offset_s) <= 1.0f, "the measured offset stays near zero");
}

static void test_set_clock_pre_2019_pump_clock_is_rejected_not_timeout() {
  std::cout << "\n=== set_clock: pump RTC reset to 2000 ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;          // node: 2026-08-07 12:00:00 UTC
  h.sim.honor_clock_writes = false;       // the write does not take
  h.sim.clock_epoch = 946684800;          // pump: 2000-01-01, an RTC cleared by a power cut
  h.sim.clock_base_ms = mock_millis;

  h.write_op.submit_set_clock(node_time(now), "clk_old");
  h.advance(20000);

  TEST_ASSERT(h.events_for("clk_old") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk_old");
  // ESPTime::is_valid() floors the year at 2019, so testing the readback with
  // it would call a perfectly decoded 2000-01-01 "unreadable" -- reporting the
  // one case where the offset matters most as though nothing came back.
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "a decoded but ancient clock is REJECTED, not TIMEOUT");
  TEST_ASSERT(r && !std::isnan(r->clock_offset_s) && r->clock_offset_s < -8.0e8f,
              "and the event carries the ~26-year offset it measured");
}

static void test_set_clock_survives_the_dst_fall_back_fold() {
  std::cout << "\n=== set_clock: inside the DST fall-back hour ===" << std::endl;
  // 2026-11-01 01:30 US Pacific happens twice: once at 01:30 PDT (epoch
  // 1793521800) and again an hour later at 01:30 PST (1793525400). ESPHome
  // resolves such a local time toward standard time, so a readback of the wall
  // clock we just wrote comes back as the LATER instant.
  //
  // The node's own clock knows which one it is. Comparing that exact epoch
  // against the readback's re-resolved one differs by exactly 3600 inside the
  // fold, which settles a correct sync REJECTED and retries every 15 minutes
  // until 02:00. Both sides are resolved by the same rule now, so the
  // ambiguity cancels.
  setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
  {
    // Which of the two instants libc re-derives from those local fields is a
    // platform decision -- glibc and macOS disagree -- and the node's clock
    // must be set to the OTHER one for this to detect anything. Otherwise the
    // exact epoch and the re-resolved one coincide, the missing conversion
    // makes no difference, and the test passes against the bug. That is not
    // hypothetical: this mutation was caught on macOS and survived on Linux CI
    // until the fold was resolved here rather than assumed.
    const time_t fold_pdt = 1793521800;  // 01:30 PDT, the first pass
    const time_t fold_pst = 1793525400;  // 01:30 PST, an hour later
    struct tm fields {};
    ::localtime_r(&fold_pdt, &fields);
    fields.tm_isdst = -1;
    const time_t preferred = ::mktime(&fields);
    const time_t node_epoch = (preferred == fold_pdt) ? fold_pst : fold_pdt;

    Harness h;
    h.prime_cache();
    h.set_node_time(node_epoch);
    h.sim.clock_epoch = node_epoch - 900;  // pump 15 min behind, same fold
    h.sim.clock_base_ms = mock_millis;

    h.write_op.submit_set_clock(node_time(node_epoch), "clk_dst");
    h.advance(15000);

    TEST_ASSERT(h.events_for("clk_dst") == 1, "exactly one terminal event");
    const WriteResult *r = h.result_for("clk_dst");
    TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
                "a sync inside the ambiguous hour still confirms");
    TEST_ASSERT(r && std::fabs(r->clock_offset_s) <= 1.0f,
                "and reports no offset -- not the 3600 s the fold would inject");
  }
  // Every other test in this file assumes the UTC pin main() set.
  setenv("TZ", "UTC", 1);
  tzset();
}

static void test_set_clock_queue_latency_is_not_the_pumps_drift() {
  std::cout << "\n=== set_clock: the frame waits behind a busy transport ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;
  h.sim.clock_epoch = now;
  h.sim.clock_base_ms = mock_millis;
  h.set_node_time(now);

  // Park two commands the pump will never answer in front of the clock write.
  // The transport is strictly FIFO and runs one command at a time, so the SET
  // frame does not reach the wire until both have timed out -- while the local
  // fields inside it still say when it was built.
  h.sim.respond_mode_reads = false;
  h.control.get_mode_async(nullptr);
  h.control.get_mode_async(nullptr);

  h.write_op.submit_set_clock(node_time(now), "clk_busy");
  h.advance(40000);

  TEST_ASSERT(h.frames_clock_write == 1, "the write did go out, late");
  TEST_ASSERT(h.events_for("clk_busy") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk_busy");
  // The pump applied exactly what it was sent, and is now behind by however
  // long the frame waited -- a real residual, caused by us. Against a flat
  // +/-5 s window that settled REJECTED with one command parked ahead and
  // worse with two, then retried every 15 minutes for as long as the link
  // stayed busy, since the ladder never re-sends the write.
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED,
              "a slow send is not reported as a failed sync");
  // The offset is still REPORTED -- the pump really did end up behind by the
  // time the frame spent in the queue, and hiding that would be the other kind
  // of lie. What changes is the verdict: a lag no larger than the operation's
  // own age is ours, not the pump's.
  TEST_ASSERT(r && r->clock_offset_s < -1.0f,
              "the real residual lag is measured and reported, not zeroed");
}

static void test_set_clock_below_the_sntp_floor_never_writes() {
  std::cout << "\n=== set_clock: node clock below the 2021 floor ===" << std::endl;
  Harness h;
  h.prime_cache();

  // 2020-06-01. Passes ESPTime::is_valid(), whose floor is 2019, and fails the
  // operation's own 2021 floor -- an ESP32 that partially restored time rather
  // than one that has none. Without the second half of that guard this reaches
  // the wire and writes a year-old clock into the pump.
  h.write_op.submit_set_clock(node_time(1590969600), "clk_2020");
  h.advance(3000);

  TEST_ASSERT(h.events_for("clk_2020") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk_2020");
  TEST_ASSERT(r && r->status == WriteStatus::INVALID, "settles INVALID before any wire write");
  TEST_ASSERT(h.frames_clock_write == 0, "and nothing reached the wire");
}

static void test_set_clock_invalid_time_never_writes() {
  std::cout << "\n=== set_clock: no system time to write ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_clock(esphome::ESPTime{}, "clk6");
  h.advance(2000);

  TEST_ASSERT(h.events_for("clk6") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("clk6");
  TEST_ASSERT(r && r->status == WriteStatus::INVALID,
              "an unset time is INVALID -- deterministic, never worth a retry");
  TEST_ASSERT(h.frames_clock_write == 0, "and nothing reached the wire");
}

static void test_set_clock_supersedes_only_other_clock_writes() {
  std::cout << "\n=== set_clock: supersede scope ===" << std::endl;
  Harness h;
  h.prime_cache();
  const time_t now = 1786104000;

  // First one starts immediately and runs to its own terminal status; the
  // second and third queue behind it on the same "clock" key.
  h.write_op.submit_set_clock(node_time(now), "clk_a");
  h.write_op.submit_set_clock(node_time(now + 1), "clk_b");
  h.write_op.submit_set_setpoint(ControlMode::CONSTANT_SPEED, 2000, "sp_x");
  h.write_op.submit_set_clock(node_time(now + 2), "clk_c");
  h.advance(30000);

  TEST_ASSERT(h.events_for("clk_a") == 1 && h.events_for("clk_b") == 1 &&
                  h.events_for("clk_c") == 1 && h.events_for("sp_x") == 1,
              "one terminal event each");
  const WriteResult *b = h.result_for("clk_b");
  TEST_ASSERT(b && b->status == WriteStatus::SUPERSEDED,
              "a queued clock sync is superseded by a newer one");
  const WriteResult *x = h.result_for("sp_x");
  TEST_ASSERT(x && x->status != WriteStatus::SUPERSEDED,
              "a queued setpoint is untouched -- the clock is its own resource");
}

static void test_set_clock_disconnect_mid_op() {
  std::cout << "\n=== set_clock: disconnect during the confirm ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_set_clock(node_time(1786104000), "clk7");
  // Past the 1500 ms settle delay, so a Sub 101 readback is genuinely in
  // flight rather than merely scheduled -- the harder of the two windows, and
  // the one the test's name claims.
  h.sim.drop_clock_reads = 1;
  h.advance(1700);
  h.write_op.on_disconnect();
  h.advance(20000);

  TEST_ASSERT(h.events_for("clk7") == 1,
              "exactly one terminal event across the drop");
  const WriteResult *r = h.result_for("clk7");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "terminated by the disconnect");
}

static void test_refresh_single_events_all_slots_fail() {
  std::cout << "\n=== refresh_single_events: every slot read fails (issue #136) ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.max_single_events = 5;  // stay inside the 60 s operation watchdog
  h.sim.single_events[3][0] = 1;
  // refresh_single_events has no overview precondition of its own, so cache
  // the overview first -- otherwise get_max_single_events() falls back to the
  // 35-slot protocol maximum. Production caches it during the boot chain.
  h.schedule.poll_state_async(nullptr);
  h.advance(200);
  h.sim.respond_single_event_reads = false;

  h.write_op.submit_refresh_single_events("re_fail");
  h.advance(45000);  // 5 slots x 3 s of timeouts, with headroom

  TEST_ASSERT(h.events_for("re_fail") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("re_fail");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT,
              "all slots failing settles TIMEOUT, not ACCEPTED");
  // Pin the reason: without this the test also passes when the operation
  // watchdog fires first, which would not exercise the #136 fix at all.
  TEST_ASSERT(r && r->detail == "single event read failed",
              "settled on the read result, not the operation watchdog");
  TEST_ASSERT(!h.schedule.is_single_events_cached(),
              "a failed read does not mark the single-event cache valid");
}

static void test_single_event_read_failure_blocks_slot_allocation() {
  std::cout << "\n=== set_single_event: unread slots must not be allocated (issue #136) ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 0 holds a live, future event on the pump -- but no slot read is
  // answered. The old code cached an empty vector and set the cached flag, so
  // find_free_single_event_slot() returned slot 0 and the write clobbered a
  // live event. That is the class issue #92 exists to prevent.
  //
  // The event is live against the node's CLOCK, which is what decides expiry
  // (issue #262). It used to end in 1970, so it was live only relative to the
  // new event's begin -- fine for the branch this test is on, and meaningless
  // once the picker started asking the right question.
  h.seed_single_event(0, 0x02, EVENT_TOMORROW_BEGIN, EVENT_TOMORROW_END);
  h.sim.max_single_events = 5;  // stay inside the 60 s operation watchdog
  h.sim.respond_single_event_reads = false;

  h.write_op.submit_set_single_event(EVENT_NEXT_WEEK_BEGIN, EVENT_NEXT_WEEK_END, "ev_fail");
  h.advance(45000);

  TEST_ASSERT(h.events_for("ev_fail") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev_fail");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED,
              "the write is rejected rather than attempted blind");
  TEST_ASSERT(r && r->detail == "could not read single events; write not attempted",
              "settled on the read result, not the operation watchdog");
  TEST_ASSERT(h.sim_single_event_begin(0) == EVENT_TOMORROW_BEGIN,
              "the live event in slot 0 was not clobbered");
  TEST_ASSERT(!h.schedule.is_single_events_cached(),
              "the single-event cache is still not valid");
}

static void test_mode_strings() {
  std::cout << "\n=== mode string round trip ===" << std::endl;
  ControlMode m;
  TEST_ASSERT(ControlService::mode_from_string("constant_speed", m) && m == ControlMode::CONSTANT_SPEED,
              "constant_speed parses");
  TEST_ASSERT(ControlService::mode_from_string("temperature_range", m) && m == ControlMode::TEMPERATURE_RANGE,
              "temperature_range parses");
  TEST_ASSERT(!ControlService::mode_from_string("warp_drive", m), "unknown string rejected");
  TEST_ASSERT(strcmp(ControlService::mode_to_string(ControlMode::CONSTANT_FLOW), "constant_flow") == 0,
              "mode_to_string round trips");
}

// ---------------------------------------------------------------------------
// Command strings (issue #159)
//
// Every string write_command_to_string() returns is the `command` field of an
// esphome.alpha_hwr_write_settled event. Fourteen of the fifteen are *also* the
// name api_bridge.cpp registers a Home Assistant service under, so editing one
// of those renames a service somebody's automation calls. Either way the rename
// should come with a failing test rather than arriving silently.
//
// SET_REMOTE_MODE is the exception: the Remote Mode switch is an entity-only
// write with no service, so `set_remote_mode` is an event string only. It is
// pinned here all the same — it is still public API, just on one surface.
//
// Before #159 the two surfaces spelled the name independently and disagreed:
// the service was `pump_set_state`, the event said `set_pump_state`. Nothing
// caught it because nothing asserted either spelling.
// ---------------------------------------------------------------------------

static void test_command_strings() {
  std::cout << "\n=== command strings (event field AND service name) ===" << std::endl;
  const auto to_string = esphome::alpha_hwr::services::write_command_to_string;

  struct Expected {
    WriteCommand cmd;
    const char *name;
  };
  static const Expected EXPECTED[] = {
      {WriteCommand::SET_PUMP_ENABLED, "set_pump_enabled"},
      {WriteCommand::SET_MODE, "set_mode"},
      {WriteCommand::SET_SETPOINT, "set_setpoint"},
      {WriteCommand::SET_TEMPERATURE_RANGE, "set_temperature_range"},
      {WriteCommand::SET_CYCLE_TIMES, "set_cycle_times"},
      {WriteCommand::SET_SCHEDULE_ENTRY, "set_schedule_entry"},
      {WriteCommand::CLEAR_SCHEDULE_ENTRY, "clear_schedule_entry"},
      {WriteCommand::SET_SCHEDULE_ENABLED, "set_schedule_enabled"},
      {WriteCommand::SET_REMOTE_MODE, "set_remote_mode"},
      {WriteCommand::SET_CLOCK, "set_clock"},
      {WriteCommand::SET_SINGLE_EVENT, "set_single_event"},
      {WriteCommand::CLEAR_SINGLE_EVENT, "clear_single_event"},
      {WriteCommand::REFRESH_SCHEDULE, "refresh_schedule"},
      {WriteCommand::REFRESH_SINGLE_EVENTS, "refresh_single_events"},
      {WriteCommand::UPLOAD_SCHEDULE, "upload_schedule"},
      {WriteCommand::SET_PUMP_STATE, "set_pump_state"},
  };
  const size_t expected_count = sizeof(EXPECTED) / sizeof(EXPECTED[0]);

  for (const auto &e : EXPECTED) {
    TEST_ASSERT(strcmp(to_string(e.cmd), e.name) == 0,
                std::string("command string is \"") + e.name + "\"");
  }

  // Exhaustiveness: a new enumerator returns its own string, so the count of
  // named commands stops matching the table above and this fails until the new
  // command is pinned here too. Without it the table only proves that the
  // commands somebody remembered to list are right.
  //
  // It covers one of the two ways to add a command. An enumerator added with
  // no case in write_command_to_string() returns "unknown", leaving the count
  // where the table left it, so this assert stays green -- that half is caught by -Wswitch and
  // CI's warnings-are-errors build, not here. Weakening either leaves the gap
  // uncovered.
  size_t named = 0;
  for (int v = 0; v < 256; v++) {
    if (strcmp(to_string(static_cast<WriteCommand>(v)), "unknown") != 0)
      named++;
  }
  TEST_ASSERT(named == expected_count,
              "every WriteCommand is pinned above (pin new commands here too)");

  // Uniqueness matters more since #159 than it did before: two commands sharing
  // a string would now register two Home Assistant services under one name.
  // Inserting what the function RETURNS rather than what the table above says
  // is the whole point -- over the table's own literals this would only catch a
  // typo in the fixture, which is not a property of the shipped code.
  std::set<std::string> unique;
  for (const auto &e : EXPECTED)
    unique.insert(to_string(e.cmd));
  TEST_ASSERT(unique.size() == expected_count,
              "command strings are unique (duplicates would collide as service names)");
}

// ---------------------------------------------------------------------------
// upload_schedule (bulk full-state grid upload)
// ---------------------------------------------------------------------------

namespace upcodec = esphome::alpha_hwr::codec;

static upcodec::UploadRequest make_upload(std::initializer_list<upcodec::UploadEntry> entries,
                                          int8_t enabled = 1) {
  upcodec::UploadRequest req;
  req.entries = entries;
  req.enabled = enabled;
  return req;
}

static void test_upload_accepted() {
  std::cout << "\n=== upload_schedule: happy path ===" << std::endl;
  Harness h;
  h.prime_cache();

  auto req = make_upload({{0, 0, 6, 54, 7, 0}, {0, 1, 7, 24, 7, 30}, {1, 0, 17, 54, 18, 0}});
  h.write_op.submit_upload_schedule(req, "up1");
  h.advance(60000);

  TEST_ASSERT(h.events_for("up1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("up1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->layers_written == "0,1", "layers 0,1 written");
  TEST_ASSERT(r && r->layers_skipped == "2,3,4", "untouched layers skipped");
  const uint8_t *cell = h.sim.layers[0] + 0 * 6;
  TEST_ASSERT(cell[0] == 1 && cell[2] == 6 && cell[3] == 54 && cell[4] == 7 && cell[5] == 0,
              "pump layer 0 day 0 holds 06:54-07:00");
  // Expected hash from the codec directly (cross-checked against the
  // Python golden vector V3)
  uint8_t images[5][42];
  for (uint8_t layer = 0; layer < 5; layer++) upcodec::build_layer_image(req, layer, images[layer]);
  TEST_ASSERT(r && r->schedule_hash == upcodec::schedule_hash(images, true),
              "settled hash matches codec-computed hash");
  TEST_ASSERT(r && r->schedule_hash == "v1:673dd2d1104de5b5",
              "settled hash matches the RFC golden vector V3");
}

static void test_upload_skip_identical() {
  std::cout << "\n=== upload_schedule: identical re-upload writes nothing ===" << std::endl;
  Harness h;
  h.prime_cache();

  auto req = make_upload({{2, 3, 6, 30, 7, 15}});
  h.write_op.submit_upload_schedule(req, "up2a");
  h.advance(60000);
  TEST_ASSERT(h.result_for("up2a") && h.result_for("up2a")->status == WriteStatus::ACCEPTED,
              "first upload accepted");

  int writes_before = h.frames_layer_write;
  h.write_op.submit_upload_schedule(req, "up2b");
  h.advance(60000);

  TEST_ASSERT(h.events_for("up2b") == 1, "exactly one terminal event for re-upload");
  const WriteResult *r = h.result_for("up2b");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "re-upload accepted");
  TEST_ASSERT(r && r->detail == "no-op", "re-upload reports no-op");
  TEST_ASSERT(r && r->layers_skipped == "0,1,2,3,4", "all layers skipped");
  TEST_ASSERT(h.frames_layer_write == writes_before,
              "no layer write frames on identical re-upload");
}

static void test_upload_partial() {
  std::cout << "\n=== upload_schedule: pump ignores writes -> partial ===" << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.honor_layer_writes = false;

  auto req = make_upload({{0, 0, 6, 0, 7, 0}});
  h.write_op.submit_upload_schedule(req, "up3");
  h.advance(60000);

  TEST_ASSERT(h.events_for("up3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("up3");
  TEST_ASSERT(r && r->status == WriteStatus::PARTIAL,
              "status is partial (layer 0 failed, others matched)");
  TEST_ASSERT(r && r->layers_written.empty(), "no layer confirmed written");
  TEST_ASSERT(r && r->layers_skipped == "1,2,3,4", "matching layers still skipped");
}

static void test_upload_supersedes_queued() {
  std::cout << "\n=== upload_schedule: newer upload supersedes queued one ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 0, 7, 0}}), "up4a");
  h.write_op.submit_upload_schedule(make_upload({{0, 0, 7, 0, 8, 0}}), "up4b");
  h.write_op.submit_upload_schedule(make_upload({{0, 0, 8, 0, 9, 0}}), "up4c");
  h.advance(120000);

  TEST_ASSERT(h.events_for("up4a") == 1 && h.events_for("up4b") == 1 && h.events_for("up4c") == 1,
              "every submission got exactly one terminal event");
  TEST_ASSERT(h.result_for("up4b") && h.result_for("up4b")->status == WriteStatus::SUPERSEDED,
              "queued middle upload superseded");
  TEST_ASSERT(h.result_for("up4c") && h.result_for("up4c")->status == WriteStatus::ACCEPTED,
              "latest upload accepted");
  const uint8_t *cell = h.sim.layers[0];
  TEST_ASSERT(cell[2] == 8 && cell[4] == 9, "pump holds the latest schedule");
}

static void test_upload_supersedes_entry_ops() {
  std::cout << "\n=== upload_schedule: supersedes queued per-entry ops ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 0, 7, 0}}), "up5a");  // in flight
  h.write_op.submit_set_schedule_entry(1, 1, 9, 0, 10, 0, "se5");                // queued
  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 30, 7, 30}}), "up5b");
  h.advance(120000);

  TEST_ASSERT(h.result_for("se5") && h.result_for("se5")->status == WriteStatus::SUPERSEDED,
              "queued per-entry op superseded by the upload");
  TEST_ASSERT(h.result_for("up5b") && h.result_for("up5b")->status == WriteStatus::ACCEPTED,
              "final upload accepted");
}

static void test_upload_disconnect_mid_op() {
  std::cout << "\n=== upload_schedule: disconnect mid-op -> one timeout event ===" << std::endl;
  Harness h;
  h.prime_cache();

  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 0, 7, 0}}), "up6");
  h.advance(200);  // op under way
  h.write_op.on_disconnect();

  TEST_ASSERT(h.events_for("up6") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("up6");
  TEST_ASSERT(r && r->status == WriteStatus::TIMEOUT, "status is timeout");
  TEST_ASSERT(r && r->detail == "disconnected", "detail reports disconnect");
  h.advance(200000);
  TEST_ASSERT(h.events_for("up6") == 1, "no second event after watchdog window");
}

static void test_upload_enabled_flag() {
  std::cout << "\n=== upload_schedule: applies schedule-enabled flag ===" << std::endl;
  Harness h;
  h.prime_cache();
  TEST_ASSERT(!h.sim.sched_enabled, "sim starts disabled");

  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 0, 7, 0}}, 1), "up7");
  h.advance(60000);

  const WriteResult *r = h.result_for("up7");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "upload accepted");
  TEST_ASSERT(h.sim.sched_enabled, "pump schedule enabled by the upload");
}

static void test_upload_republishes_schedule_display() {
  std::cout << "\n=== upload_schedule: republishes the schedule display (#133) ==="
            << std::endl;
  Harness h;
  h.prime_cache();

  // An upload that puts real layers on the wire.
  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 30, 7, 30}}), "rp1");
  h.advance(120000);
  // Copied by value: result_for() points into the harness's results vector,
  // which reallocates when the second upload settles.
  const WriteResult *first_ptr = h.result_for("rp1");
  TEST_ASSERT(first_ptr != nullptr, "first upload settled");
  WriteResult first = first_ptr ? *first_ptr : WriteResult{};
  TEST_ASSERT(first.status == WriteStatus::ACCEPTED, "first upload accepted");
  TEST_ASSERT(!first.layers_written.empty(), "first upload wrote layers");
  TEST_ASSERT(!first.schedule_hash.empty(), "post-op hash is reported");
  TEST_ASSERT(result_republishes_schedule(first),
              "a writing upload republishes the schedule display");

  // The same grid again: every layer already matches, so nothing goes on the
  // wire. This is the case from the report — accepted, detail "no-op", every
  // layer skipped — where the event carried the right hash and the sensor kept
  // its old value. The bridge's cache and the published sensor had genuinely
  // diverged, so "nothing changed" was true of the pump but not of HA.
  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 30, 7, 30}}), "rp2");
  h.advance(120000);
  const WriteResult *noop = h.result_for("rp2");
  TEST_ASSERT(noop && noop->status == WriteStatus::ACCEPTED, "no-op upload accepted");
  TEST_ASSERT(noop && noop->layers_written.empty(), "no-op upload wrote nothing");
  TEST_ASSERT(noop && noop->layers_skipped == "0,1,2,3,4",
              "no-op upload skipped every layer");
  TEST_ASSERT(noop && !noop->schedule_hash.empty(),
              "no-op upload still reports the post-op hash");
  TEST_ASSERT(noop && result_republishes_schedule(*noop),
              "a no-op upload republishes too — the sensor may still be stale");
  TEST_ASSERT(noop && first.schedule_hash == noop->schedule_hash,
              "the hash is unchanged across the no-op");
}

static void test_partial_upload_republishes_schedule_display() {
  std::cout << "\n=== upload_schedule: a partial upload still republishes (#133) ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.honor_layer_writes = false;

  h.write_op.submit_upload_schedule(make_upload({{0, 0, 6, 0, 7, 0}}), "rp3");
  h.advance(60000);

  const WriteResult *r = h.result_for("rp3");
  TEST_ASSERT(r && r->status == WriteStatus::PARTIAL, "status is partial");
  TEST_ASSERT(r && !r->schedule_hash.empty(), "a partial upload still reports a hash");
  // PARTIAL is neither ACCEPTED nor CLAMPED, so the `applied` gate the
  // single-entry writes use would have skipped this — yet an upload is five
  // independent layer writes and a partial run is precisely the case where the
  // device grid and the published sensor drift apart.
  TEST_ASSERT(r && result_republishes_schedule(*r),
              "a partial upload republishes despite not being 'applied'");
}

static void test_all_layers_failed_upload_republishes() {
  std::cout << "\n=== upload_schedule: all layers fail -> still republishes (#133) ==="
            << std::endl;
  Harness h;
  h.prime_cache();
  h.sim.honor_layer_writes = false;

  // Every layer differs from the cache, so every one is attempted as a write
  // and every one fails confirm. written_mask is cleared bit by bit and
  // skipped_mask never gets set, so the upload ends REJECTED with both masks
  // zero — the case the review flagged, where the old written|skipped test
  // reported an empty hash despite the readbacks having refreshed the cache.
  h.write_op.submit_upload_schedule(
      make_upload({{0, 0, 6, 0, 7, 0},
                   {1, 0, 8, 0, 9, 0},
                   {2, 0, 10, 0, 11, 0},
                   {3, 0, 12, 0, 13, 0},
                   {4, 0, 14, 0, 15, 0}}),
      "rp4");
  h.advance(120000);

  const WriteResult *r = h.result_for("rp4");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail == "no layer could be written", "detail names the cause");
  TEST_ASSERT(r && r->layers_written.empty(), "no layer confirmed as written");
  TEST_ASSERT(r && r->layers_skipped.empty(), "no layer was already matching");
  TEST_ASSERT(r && !r->schedule_hash.empty(),
              "the hash is still reported — the readbacks refreshed the cache");
  TEST_ASSERT(r && result_republishes_schedule(*r),
              "an all-failed upload republishes so the sensor tracks the device");
}

static void test_republish_predicate_arms() {
  std::cout << "\n=== schedule-display republish predicate (#133) ===" << std::endl;
  WriteResult r;

  // Rejected before the first layer: nothing was read, the cache is untouched,
  // and schedule_hash is empty. Note the status is not what decides this — an
  // all-layers-failed upload is REJECTED too and does republish
  // (test_all_layers_failed_upload_republishes); the empty hash is.
  r.command = WriteCommand::UPLOAD_SCHEDULE;
  r.status = WriteStatus::REJECTED;
  r.schedule_hash = "";
  TEST_ASSERT(!result_republishes_schedule(r),
              "upload rejected before the first layer does not republish");

  // Failed partway. An upload is five independent layer writes, so the device
  // grid moved even though the verdict is a failure — the sensor tracks the
  // device, not the verdict.
  r.status = WriteStatus::TIMEOUT;
  r.schedule_hash = "v1:e7197ac9e42729dd";
  TEST_ASSERT(result_republishes_schedule(r),
              "upload that failed partway still republishes what landed");

  // Non-schedule writes never touch the schedule display.
  r.command = WriteCommand::SET_SETPOINT;
  r.status = WriteStatus::ACCEPTED;
  TEST_ASSERT(!result_republishes_schedule(r),
              "a setpoint write does not republish the schedule");

  // Single-entry schedule writes stay gated on the terminal status: unlike an
  // upload they are one wire write, so a rejection means nothing moved.
  r.command = WriteCommand::SET_SCHEDULE_ENTRY;
  r.status = WriteStatus::ACCEPTED;
  TEST_ASSERT(result_republishes_schedule(r), "accepted schedule entry republishes");
  r.status = WriteStatus::REJECTED;
  TEST_ASSERT(!result_republishes_schedule(r),
              "rejected schedule entry does not republish");

  r.command = WriteCommand::REFRESH_SCHEDULE;
  r.status = WriteStatus::ACCEPTED;
  TEST_ASSERT(result_republishes_schedule(r), "refresh_schedule republishes");
}

int main() {
  // Pin the timezone to UTC so single-event timestamp assertions are
  // deterministic across CI machines: schedule_service shifts single-event
  // timestamps by the local UTC offset (the pump's clock is local Unix time),
  // which under UTC is a no-op. The offset math itself is covered by
  // test_schedule_service (utc_to_local_unix with explicit offsets).
  setenv("TZ", "UTC", 1);
  tzset();

  std::cout << "===========================================================" << std::endl;
  std::cout << "  Write Operation Service Test Suite (issue #92)" << std::endl;
  std::cout << "===========================================================" << std::endl;

  test_mode_read_arguments_are_not_swapped();
  test_set_mode_accepted();
  test_set_mode_rejected();
  test_set_setpoint_accepted();
  test_set_setpoint_clamped();
  test_set_setpoint_rejected_kept_old();
  test_set_setpoint_resolve_abort();
  test_set_enabled_unfused_class3();
  test_set_enabled_class3_nack();
  test_set_enabled_class3_ack_timeout();
  test_set_enabled_rejected_reports_readback();
  test_remote_mode_accepted();
  test_remote_mode_nack();
  test_remote_mode_ack_window_closes();
  test_remote_mode_acked_but_not_applied();
  test_remote_mode_unusable_control_source();
  test_remote_mode_warm_cache_needs_fresh_observation();
  test_remote_mode_unreadable_source_is_not_rejection();
  test_remote_mode_own_resource_key();
  test_supersede_detail_uses_origin();
  test_supersede_queued();
  test_watchdog_timeout();
  test_disconnect_terminates();
  test_not_ready_rejected();
  test_validation_invalid();
  test_setpoints_different_modes_both_run();
  test_origin_and_seq_reported();
  test_temperature_range_refused_when_limits_unknown();
  test_temperature_range_refused_when_the_pre_read_fails();
  test_temperature_range_accepted();
  test_temperature_range_ignored_write_is_rejected();
  test_temperature_range_baseline_is_read_not_remembered();
  test_temperature_range_mode_ack_is_never_stolen();
  test_mode_ack_does_not_delay_step_two();
  test_temperature_range_clamped();
  test_temperature_range_unacked_but_stored();
  test_temperature_range_unacked_and_not_stored();
  test_temperature_range_unacked_and_clamped();
  test_temperature_range_unacked_survives_a_dropped_readback();
  test_temperature_range_unacked_and_unreadable_still_settles();
  test_temperature_range_refusal_is_settled_by_the_readback();
  test_temperature_range_invalid();
  test_cycle_times_accepted();
  test_cycle_times_pump_clamps();
  test_cycle_times_unacked_but_stored();
  test_cycle_times_unacked_and_not_stored();
  test_cycle_times_unacked_and_clamped();
  test_cycle_times_unacked_survives_a_dropped_readback();
  test_cycle_times_read_unavailable();
  test_cycle_flow_only_accepted();
  test_cycle_combined_write();
  test_cycle_flow_clamped();
  test_cycle_flow_rejected_kept();
  test_cycle_flow_invalid_ranges();
  test_cycle_all_kept_invalid();
  test_cycle_kept_minutes_unreadable();
  test_cycle_flow_entity_origin();
  test_temp_range_preserves_limits();
  test_interleaved_poll();
  test_issue92_collision();

  test_schedule_entry_accepted();
  test_schedule_entry_verify_mismatch();
  test_schedule_entry_no_overview();
  test_clear_schedule_entry_accepted();
  test_clear_schedule_entry_pump_keeps_the_entry();
  test_clear_schedule_entry_out_of_range_is_invalid();
  test_clear_schedule_entry_ignores_stale_times_in_a_disabled_cell();
  test_clear_schedule_entry_retries_a_late_commit();
  test_schedule_enabled_verified_rmw();
  test_schedule_enabled_pump_keeps_its_flag();
  test_single_event_auto_slot();
  test_single_event_reuses_expired_slot();
  test_a_far_future_event_does_not_evict_a_live_one();
  test_a_far_future_vacation_does_not_evict_a_live_event();
  test_without_a_clock_no_slot_is_treated_as_expired();
  test_with_a_clock_the_same_expired_slots_are_reusable();
  test_single_event_prefers_an_empty_slot_to_an_expired_one();
  test_a_pool_of_live_events_is_a_full_pool();
  test_slot_picker_decision_table();
  test_set_vacation_writes_stop_event();
  test_a_vacation_stored_as_a_run_event_is_rejected();
  test_a_run_event_stored_as_a_stop_is_rejected();
  test_a_matching_action_still_settles_accepted();
  test_clear_single_event_ignores_stale_content_in_a_disabled_slot();
  test_clear_vacation_targets_stop_slot();
  test_clear_vacation_none_active();
  test_clear_single_event();
  test_single_event_slot_bounded_by_pump_capacity();
  test_single_event_last_valid_slot_still_writes();
  test_set_single_event_explicit_slot_is_bounded();
  test_single_event_slot_past_the_protocol_limit_is_invalid();
  test_device_range_defers_to_the_overview_failure();
  test_out_of_range_events_never_enter_the_cache();
  test_schedule_supersede_keys();
  test_refresh_ops();
  test_refresh_schedule_all_layers_fail();
  test_refresh_schedule_partial_layer_failure();
  test_set_clock_accepted();
  test_no_class10_write_leaves_an_unclaimed_ack();
  test_set_clock_wire_struct();
  test_set_clock_ignored_write_is_rejected();
  test_set_clock_already_correct_is_accepted();
  test_set_clock_unreadable_is_timeout();
  test_set_clock_slow_ladder_is_not_drift();
  test_set_clock_queue_latency_is_not_the_pumps_drift();
  test_set_clock_below_the_sntp_floor_never_writes();
  test_set_clock_pre_2019_pump_clock_is_rejected_not_timeout();
  test_set_clock_survives_the_dst_fall_back_fold();
  test_set_clock_invalid_time_never_writes();
  test_set_clock_supersedes_only_other_clock_writes();
  test_set_clock_disconnect_mid_op();
  test_refresh_single_events_all_slots_fail();
  test_single_event_read_failure_blocks_slot_allocation();
  test_mode_strings();
  test_command_strings();
  test_upload_accepted();
  test_upload_skip_identical();
  test_upload_partial();
  test_upload_supersedes_queued();
  test_upload_supersedes_entry_ops();
  test_upload_disconnect_mid_op();
  test_upload_enabled_flag();
  test_upload_republishes_schedule_display();
  test_partial_upload_republishes_schedule_display();
  test_all_layers_failed_upload_republishes();
  test_republish_predicate_arms();

  // Every APDU any test above sent, checked against its own declared length.
  std::cout << "\n=== Every APDU declares the payload length it carries ==="
            << std::endl;
  for (const auto &d : g_apdu_length_violation_detail)
    std::cout << "  " << d << std::endl;
  TEST_ASSERT(g_apdu_length_violations == 0,
              "No frame declares a payload length it does not carry");

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}
