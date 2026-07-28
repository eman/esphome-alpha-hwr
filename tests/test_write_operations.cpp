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
#include <string>
#include <vector>

#include "../components/alpha_hwr/control_service.h"
#include "../components/alpha_hwr/schedule_service.h"
#include "../components/alpha_hwr/session.h"
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
  uint8_t last_overview_write[10] = {};
  int overview_writes{0};

  // Behavior switches
  bool respond_mode_reads{true};
  bool respond_obj91{true};
  bool ack_temp_write{true};
  bool honor_mode_change{true};       // apply 0x0A01 mode changes
  bool honor_setpoint_writes{true};   // apply setpoint values from 0601/register writes
  bool obj91_includes_limits{true};   // firmware echoes the 5 limit tail bytes
  bool respond_dhw_reads{true};       // reply to Obj 91 Sub 421 reads
  bool honor_dhw_writes{true};        // apply Sub 421 writes
  int dhw_min_off{0};                 // >0: pump clamps off_period to this floor
  bool honor_dhw_flow_writes{true};   // apply asserted setpoint bytes (issue #107)
  float dhw_flow_clamp_native{0};     // >0: pump caps the stored flow (m³/s)
  bool respond_overview_reads{true};
  bool honor_layer_writes{true};
  bool honor_overview_writes{true};
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

struct Harness {
  Transport transport;
  Session session;
  ControlService control{transport, session};
  ScheduleService schedule{transport, session};
  WriteOperationService write_op{control, schedule};
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
  int frames_dhw_read{0};   // Obj 91 Sub 421 reads
  int frames_dhw_write{0};  // Obj 91 Sub 421 writes
  std::vector<uint8_t> last_0601_setpoint_bytes;
  std::vector<uint8_t> last_dhw_write_setpoint;
  std::vector<uint8_t> last_temp_write_tail;

  struct Task { uint64_t due; std::function<void()> fn; };
  std::vector<Task> tasks;
  struct Injection { uint64_t due; std::vector<uint8_t> frame; };
  std::vector<Injection> injections;

  std::vector<uint8_t> out_buf;  // outgoing frame reassembly

  Harness() {
    session.on_authenticated();
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
  }

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

  void handle_frame(const std::vector<uint8_t> &frame) {
    if (frame.size() < 6) return;
    const uint8_t *apdu = frame.data() + 4;
    size_t apdu_len = frame.size() - 6;

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
    } else if (opspec == 0x90 && apdu_len >= 18 && apdu[2] == 0x56 && apdu[4] == 0x0A && apdu[5] == 0x01) {
      // Unfused mode change (0x0A01, PR #98)
      frames_0a01++;
      if (sim.honor_mode_change) sim.mode_byte = apdu[13];
    } else if (opspec == 0x84 && apdu_len >= 10) {
      // Setpoint register write
      frames_register++;
      int sub = (apdu[2] << 8) | apdu[3];
      sim.apply_setpoint(sub, protocol::decode_float_be(apdu + 6));
    } else if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 91 && apdu[3] == 0x01 && apdu[4] == 0xAE) {
      // Obj 91 Sub 430 config read
      if (sim.respond_obj91) inject_obj91_response();
    } else if (opspec == 0x03 && apdu_len >= 5 && apdu[2] == 91 && apdu[3] == 0x01 && apdu[4] == 0xA5) {
      // Obj 91 Sub 421 DHW config read (issue #106)
      frames_dhw_read++;
      if (sim.respond_dhw_reads) inject_dhw_response();
    } else if (opspec == 0x97 && apdu_len >= 25) {
      // Temperature-range config write; capture the limits tail for the
      // preservation assertion (issue #106).
      bool aa = apdu[11] != 0;
      float mn = protocol::decode_float_be(apdu + 12);
      float mx = protocol::decode_float_be(apdu + 16);
      sim.autoadapt = aa;
      sim.temp_min = mn;
      sim.temp_max = mx;
      last_temp_write_tail.assign(apdu + 20, apdu + 25);
      if (sim.ack_temp_write) inject_short_ack();
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
      inject_short_ack();
    } else if (apdu[2] == 84 && apdu_len >= 5) {
      uint16_t sub = (apdu[3] << 8) | apdu[4];
      if (opspec == 0x03) {
        // Object 84 reads
        if (sub == 1) {
          if (sim.respond_overview_reads) inject_overview_frame();
        } else if (sub >= 1000 && sub <= 1004) {
          inject_layer_frame(static_cast<uint8_t>(sub - 1000));
        } else if (sub >= 900 && sub < 935) {
          inject_single_event_frame(static_cast<uint8_t>(sub - 900));
        }
      } else if (opspec == 0xB3 && sub >= 1000 && sub <= 1004 && apdu_len >= 53) {
        // Whole-layer write (42 bytes at apdu+11)
        frames_layer_write++;
        if (sim.honor_layer_writes) memcpy(sim.layers[sub - 1000], apdu + 11, 42);
        inject_layer_frame(static_cast<uint8_t>(sub - 1000));
      } else if (opspec == 0xB3 && sub >= 900 && sub < 935 && apdu_len >= 21) {
        // Single-event write (10 bytes at apdu+11)
        if (sim.honor_layer_writes) memcpy(sim.single_events[sub - 900], apdu + 11, 10);
        inject_single_event_frame(static_cast<uint8_t>(sub - 900));
      } else if (opspec == 0x93 && sub == 1 && apdu_len >= 21) {
        // ClockProgramOverview write (set_state / configuration commit)
        sim.overview_writes++;
        memcpy(sim.last_overview_write, apdu + 11, 10);
        if (sim.honor_overview_writes) sim.sched_enabled = apdu[15] != 0;
        if (sim.respond_overview_reads) inject_overview_frame();
      }
    }
  }

  // -- Response builders (CRC bytes are not checked by the transport)
  void inject(std::vector<uint8_t> frame, uint32_t delay = 20) {
    injections.push_back({mock_millis + delay, std::move(frame)});
  }

  void inject_mode_notification() {
    // OpSpec 0x0E notification: Sub 0x0001 (bytes 6-7), Obj 0x2F01 (bytes 8-9)
    // payload [00 00 07][control_source][operation_mode][control_mode][setpoint f32be]
    std::vector<uint8_t> f = {0x24, 18, 0xF8, 0xE7, 0x0A, 0x0E, 0x00, 0x01, 0x2F, 0x01,
                              0x00, 0x00, 0x07, 0x02, static_cast<uint8_t>(sim.enabled ? 0x00 : 0x01),
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

  void inject_short_ack() {
    inject({0x24, 0x05, 0xF8, 0xE7, 0x0A, 0x01, 0x00, 0xAA, 0xBB});
  }

  // Object 84 response frames use a plain DataObject shape: OpSpec 0x13
  // (not in the register-read set), Obj at bytes 8-9, payload at byte 10
  // with a [00 00 XX] header the parsers skip.
  void inject_obj84_frame(uint8_t obj_hi, uint8_t obj_lo, const uint8_t *body, size_t body_len) {
    std::vector<uint8_t> f = {0x24, 0, 0xF8, 0xE7, 0x0A, 0x13, 0x00, 0x00, obj_hi, obj_lo,
                              0x00, 0x00, static_cast<uint8_t>(body_len)};
    f.insert(f.end(), body, body + body_len);
    f.push_back(0xAA);
    f.push_back(0xBB);
    f[1] = static_cast<uint8_t>(f.size() - 4);
    inject(std::move(f));
  }

  void inject_overview_frame() {
    uint8_t overview[10] = {0x8C, 0x23, 0x05, 0x05,
                            static_cast<uint8_t>(sim.sched_enabled ? 1 : 0),
                            0x01, 0x00, 0x00, 0x00, 0x00};
    inject_obj84_frame(0xDA, 0x01, overview, 10);
  }

  void inject_layer_frame(uint8_t layer) {
    inject_obj84_frame(0xDE, 0x01, sim.layers[layer], 42);
  }

  void inject_single_event_frame(uint8_t slot) {
    inject_obj84_frame(0xDC, 0x01, sim.single_events[slot], 10);
  }

  // -- Time driver
  void advance(uint32_t ms) {
    uint64_t end = mock_millis + ms;
    while (mock_millis < end) {
      mock_millis += 10;
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

  // Prime the control cache from the sim (a mode read + obj91 read round trip).
  void prime_cache() {
    control.get_mode_async(nullptr);
    advance(100);
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

static void test_temperature_range_accepted() {
  std::cout << "\n=== set_temperature_range: accepted via unfused mode change ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
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

static void test_temperature_range_no_ack() {
  std::cout << "\n=== set_temperature_range: no ACK -> rejected ===" << std::endl;
  Harness h;
  h.sim.mode_byte = 0x02;
  h.prime_cache();
  h.sim.ack_temp_write = false;

  h.write_op.submit_set_temperature_range(30.0f, 50.0f, false, "tr2");
  h.advance(15000);

  TEST_ASSERT(h.events_for("tr2") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("tr2");
  TEST_ASSERT(r && r->status == WriteStatus::REJECTED, "status is rejected");
  TEST_ASSERT(r && r->detail.find("not acknowledged") != std::string::npos, "detail reports the missing ACK");
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

static void test_single_event_auto_slot() {
  std::cout << "\n=== set_single_event: auto slot skips occupied slot 0 ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 0 holds a LIVE event (ends after the new event begins); the cache
  // is cold, so the op must read the slots first instead of blindly picking
  // slot 0.
  h.sim.single_events[0][0] = 1;
  h.sim.single_events[0][1] = 0x02;
  // end_ts = 3000000 (BE) — still in the future relative to begin 1000000
  h.sim.single_events[0][6] = 0x00;
  h.sim.single_events[0][7] = 0x2D;
  h.sim.single_events[0][8] = 0xC6;
  h.sim.single_events[0][9] = 0xC0;

  h.write_op.submit_set_single_event(1000000, 2000000, "ev1");
  h.advance(60000);

  TEST_ASSERT(h.events_for("ev1") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev1");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 1, "auto-resolved slot 1 (slot 0 occupied) echoed");
  TEST_ASSERT(r && r->begin_ts == 1000000 && r->end_ts == 2000000, "settled timestamps reported");
  TEST_ASSERT(h.sim.single_events[1][0] == 1, "pump slot 1 holds the enabled event");
  TEST_ASSERT(h.sim.single_events[0][0] == 1, "slot 0 was not overwritten");
}


static void test_single_event_reuses_expired_slot() {
  std::cout << "\n=== set_single_event: expired slot is reused, pool never exhausts ===" << std::endl;
  Harness h;
  h.prime_cache();
  // Slot 0 holds an ENABLED but EXPIRED event (ended at 500000, before the
  // new event's begin 1000000). It must not count as occupied.
  h.sim.single_events[0][0] = 1;
  h.sim.single_events[0][1] = 0x02;
  h.sim.single_events[0][6] = 0x00;
  h.sim.single_events[0][7] = 0x07;
  h.sim.single_events[0][8] = 0xA1;
  h.sim.single_events[0][9] = 0x20;  // 500000 BE

  h.write_op.submit_set_single_event(1000000, 2000000, "ev3");
  h.advance(60000);

  TEST_ASSERT(h.events_for("ev3") == 1, "exactly one terminal event");
  const WriteResult *r = h.result_for("ev3");
  TEST_ASSERT(r && r->status == WriteStatus::ACCEPTED, "status is accepted");
  TEST_ASSERT(r && r->slot == 0, "expired slot 0 was reused");
  TEST_ASSERT(h.sim.single_events[0][0] == 1 &&
                  h.sim.single_events[0][9] == (2000000 & 0xFF),
              "pump slot 0 now holds the new event");
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
// upload_schedule (RFC-005 / dhw-sensor-apps issue #5)
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
  test_supersede_detail_uses_origin();
  test_supersede_queued();
  test_watchdog_timeout();
  test_disconnect_terminates();
  test_not_ready_rejected();
  test_validation_invalid();
  test_setpoints_different_modes_both_run();
  test_origin_and_seq_reported();
  test_temperature_range_accepted();
  test_temperature_range_no_ack();
  test_temperature_range_invalid();
  test_cycle_times_accepted();
  test_cycle_times_pump_clamps();
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
  test_schedule_enabled_verified_rmw();
  test_single_event_auto_slot();
  test_single_event_reuses_expired_slot();
  test_set_vacation_writes_stop_event();
  test_clear_vacation_targets_stop_slot();
  test_clear_vacation_none_active();
  test_clear_single_event();
  test_schedule_supersede_keys();
  test_refresh_ops();
  test_mode_strings();
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

  std::cout << "\n===========================================================" << std::endl;
  std::cout << "  Test Results" << std::endl;
  std::cout << "===========================================================" << std::endl;
  std::cout << "Tests passed: " << tests_passed << std::endl;
  std::cout << "Tests failed: " << tests_failed << std::endl;

  return tests_failed == 0 ? 0 : 1;
}
