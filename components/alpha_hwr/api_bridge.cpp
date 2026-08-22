#include "api_bridge.h"
#ifdef ALPHA_HWR_HAS_API_BRIDGE

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <type_traits>
#include "alpha_hwr.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace alpha_hwr {

static const char *const BRIDGE_TAG = "alpha_hwr.api";
static const char *const WRITE_SETTLED_EVENT = "esphome.alpha_hwr_write_settled";

using services::ControlMode;
using services::ControlService;
using services::WriteCommand;
using services::WriteResult;
using services::WriteStatus;

void AlphaHwrApiBridge::setup(AlphaHwrComponent *component) {
  component_ = component;

  // Service names are not spelled here. Each one is the *same string the
  // settle event reports in `command`*, read from the single function that
  // spells it (issue #159). Both surfaces used to name the write
  // independently, and they disagreed: calling `pump_set_state` settled as
  // `set_pump_state`, so no automation could correlate a call to its own
  // event by name. Deriving one from the other means they can now only
  // disagree by passing the wrong enumerator — visible right here, at the
  // call site, rather than at a user's event listener.
  //
  // Every command string that is registered below is therefore public API on
  // two counts, and test_write_operations.cpp::test_command_strings() pins all
  // sixteen: editing one of those strings in write_command_to_string() renames
  // a Home Assistant service along with the event field. Two commands have no
  // service here — SET_REMOTE_MODE (the Remote Mode switch is entity-only) and
  // SET_CLOCK (the RTC sync is autonomous, submitted by the periodic check with
  // origin INTERNAL) — so their strings move the event field alone.
  //
  // What that test cannot reach: this file is compiled only against the real
  // ESPHome API headers, so no host test builds it and the mutation check has
  // no target here. Pairing a handler with the wrong enumerator below compiles,
  // passes the whole suite and passes the firmware build -- it surfaces only on
  // a bench service listing or in somebody's automation. The pairings are
  // one-per-line and adjacent to their argument lists for that reason; a
  // service listing from a real node is the check.
  const auto name = services::write_command_to_string;

  register_service(&AlphaHwrApiBridge::on_set_enabled, name(WriteCommand::SET_PUMP_ENABLED),
                   {"enabled", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_mode, name(WriteCommand::SET_MODE),
                   {"mode", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_setpoint, name(WriteCommand::SET_SETPOINT),
                   {"mode", "value", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_temperature_range,
                   name(WriteCommand::SET_TEMPERATURE_RANGE),
                   {"min_c", "max_c", "autoadapt", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_cycle_times, name(WriteCommand::SET_CYCLE_TIMES),
                   {"on_minutes", "off_minutes", "flow", "op_id"});
  // The limiter is named rather than numbered (issue #299). "600"/"601" are
  // sub-ids, and a service argument that takes a raw address invites a caller
  // to try a third one -- which the operation layer would then refuse. The two
  // that exist have names the Grundfos app already uses.
  register_service(&AlphaHwrApiBridge::on_set_flow_limiter, name(WriteCommand::SET_FLOW_LIMITER),
                   {"limiter", "enabled", "limit_gpm", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_pump_state, name(WriteCommand::SET_PUMP_STATE),
                   {"state", "op_id"});

  register_service(&AlphaHwrApiBridge::on_upload_schedule, name(WriteCommand::UPLOAD_SCHEDULE),
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_schedule_entry,
                   name(WriteCommand::SET_SCHEDULE_ENTRY), {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_clear_schedule_entry,
                   name(WriteCommand::CLEAR_SCHEDULE_ENTRY), {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_set_schedule_enabled,
                   name(WriteCommand::SET_SCHEDULE_ENABLED), {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_refresh_schedule, name(WriteCommand::REFRESH_SCHEDULE),
                   {"op_id"});
  register_service(&AlphaHwrApiBridge::on_set_single_event, name(WriteCommand::SET_SINGLE_EVENT),
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_clear_single_event,
                   name(WriteCommand::CLEAR_SINGLE_EVENT), {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_refresh_single_events,
                   name(WriteCommand::REFRESH_SINGLE_EVENTS), {"op_id"});

  // The two exceptions, and the only services whose name is written out: a
  // vacation is a composition over the single-event slots rather than a
  // command of its own, so these settle as `set_single_event` /
  // `clear_single_event`. Documented in docs/programmatic-interface.md so the
  // mismatch is stated rather than discovered.
  register_service(&AlphaHwrApiBridge::on_set_vacation, "set_vacation",
                   {"data", "op_id"});
  register_service(&AlphaHwrApiBridge::on_clear_vacation, "clear_vacation", {"op_id"});

  // Terminal results reach fire_write_settled() through the component's
  // central write-result hook (see AlphaHwrComponent::setup()), which also
  // refreshes the schedule displays.
  ESP_LOGI(BRIDGE_TAG, "Programmatic write services registered");
}

// ---------------------------------------------------------------------------
// Strict argument parsing
// ---------------------------------------------------------------------------
//
// These replace sscanf("%d,%d,...") in the handlers below. sscanf was wrong
// three separate ways for input that any Home Assistant user or script can
// send, and all three turned a MALFORMED request into a SUCCESSFUL write to a
// plausible-looking target -- strictly worse than the terminal `invalid` the
// reject_ path exists to produce:
//
//   * Trailing garbage is ignored. "3.9" cleared slot 3; "1abc" cleared slot 1;
//     "0,0,6,0,8,0GARBAGE" was accepted as a schedule entry.
//   * Integer overflow is undefined behaviour, and on the host it WRAPPED into
//     the valid range: clear_single_event "4294967296" wrapped to 0 and cleared
//     slot 0. The range guard let it through because the wrapped value is in
//     range. Which side of the wrap you land on decided whether the guard
//     worked, so "2147483648" happened to be caught and "4294967296" did not.
//   * "%lu" ACCEPTS a leading minus and negates it, so "-2,-1" became a valid
//     ordered pair of huge timestamps.
//
// The file already held the right standard in two places -- on_set_schedule_enabled
// demands exactly "0" or "1", and parse_upload_payload is strict throughout --
// so the sscanf handlers were the outliers.
//
// One more reason not to lean on the host's behaviour here: `unsigned long` is
// 8 bytes on this host and 4 on the ESP32, so a host test can never observe
// what the firmware's sscanf would do with an out-of-range timestamp. Parsing
// into a fixed-width type and range-checking it explicitly makes the two agree.
//
// That paragraph was written and then only half-applied. The parsed value was
// narrowed to a fixed-width type at the end, but the BOUNDS travelled as plain
// `long` -- 8 bytes on this host, 4 on the ESP32-C3 (RISC-V, ILP32). So
// `parse_int_field(s, 0, 4294967295L, &v)` compiled to `hi = -1` on the pump,
// `v > hi` refused every value >= 0, and `set_single_event` and `set_vacation`
// rejected every input any client could send -- for as long as the parser has
// existed -- while this suite asserted the opposite and stayed green (#255).
//
// The width is therefore named once, carried everywhere, and asserted twice --
// because the two assertions catch the regression in different places, and
// only one of them can run where the defect actually appeared.
//
// `long long` is guaranteed at least 64 bits by the standard, so nothing below
// depends on the host and target agreeing; it is also what keeps epochs past
// 2038 parsable, since `std::strtol` on a 32-bit `long` saturates at 2147483647
// and sets ERANGE, which this parser treats as a rejection.
using ParseInt = long long;

// (1) Semantic, and the one that reproduces #255 exactly: it fails only where
// the bound would narrow. On the ESP32-C3 it reduces to `2147483647 >=
// 4294967295` and stops CI's firmware build; on this host it is trivially true,
// which is the whole reason the bug shipped.
static_assert(std::numeric_limits<ParseInt>::max() >= 4294967295LL,
              "ParseInt must hold the wire's uint32 ceiling at every word size; "
              "`long` does not on the ESP32-C3 (issue #255)");

// (2) is not here. It is beside the parse itself, a few lines down, because the
// width story has two halves and asserting only this one leaves the other bare:
// `std::strtol` saturates at 2147483647 on a 32-bit `long` and reports ERANGE,
// which this function treats as a rejection, so a parse narrowed back to
// `strtol` refuses every post-2038 epoch even with the alias left wide. Tying
// the alias TO the parse covers both halves at once, and does it on every
// platform -- see the assert in parse_int_field().
//
// An earlier attempt asserted the alias against an allowlist of
// guaranteed-width spellings, `long long` or `int64_t`. That is wrong in a way
// worth recording, because it verified clean on the author's machine: on glibc
// LP64 -- which is what CI runs the unit tests on -- `int64_t` IS `long`, so
// `ParseInt = long` satisfies the allowlist and the assert passes. It fired only
// on hosts where `int64_t` is `long long`, and the author's is one. A check that
// depends on which spelling a platform picked for a typedef is not a check.

/// Whole-string decimal parse. Rejects an empty field, leading/trailing
/// characters of any kind (including whitespace), and anything outside
/// [lo, hi]. Returns false rather than a clamped value: a request the bridge
/// cannot read exactly is one it must not guess at.
static bool parse_int_field(const std::string &s, ParseInt lo, ParseInt hi, ParseInt *out) {
  if (s.empty()) return false;
  // strtoll skips leading whitespace and accepts '+'/'-'; the explicit check
  // here makes both a rejection, so " 7" and "+7" are refused rather than
  // silently accepted as 7.
  const bool starts_cleanly = (s[0] == '-') || (s[0] >= '0' && s[0] <= '9');
  if (!starts_cleanly) return false;
  errno = 0;
  char *end = nullptr;
  const auto v = std::strtoll(s.c_str(), &end, 10);
  // (2) Structural, and the one every build can make -- host and firmware
  // alike, whatever a platform calls its 64-bit types. It ties the alias to the
  // parse: narrow either one and `long` meets `long long`, which are distinct
  // types even where both are 64 bits wide, so this fails everywhere rather
  // than only where the widths differ. Assert (1) is what reproduces #255 on
  // the target; this is what stops it reaching the target at all.
  static_assert(std::is_same<decltype(v), const ParseInt>::value,
                "the bound type and the parse must be the same wide type: a `long` bound "
                "narrows to -1 on the ESP32-C3, and a `strtol` parse saturates at "
                "2147483647 and reports ERANGE, which this function rejects (issue #255)");
  // Each condition is hoisted into its own name and its own statement so
  // mutation_check.sh has a pipe-free line per rule to anchor to: entries are
  // split with IFS='|', which truncates any search string containing `||`.
  const bool overflowed = (errno == ERANGE);
  const bool consumed_everything = (end != s.c_str()) && (*end == '\0');
  if (overflowed) return false;
  if (!consumed_everything) return false;
  if (v < lo) return false;
  if (v > hi) return false;
  *out = v;
  return true;
}

/// Split on ',' into exactly `want` fields. A missing, extra or empty field is
/// a rejection; so is any field that is not a whole decimal number in range.
static bool parse_int_csv(const std::string &data, size_t want, const ParseInt *lo,
                          const ParseInt *hi, ParseInt *out) {
  size_t start = 0;
  for (size_t i = 0; i < want; i++) {
    const size_t comma = data.find(',', start);
    const bool last = (i + 1 == want);
    if (last != (comma == std::string::npos)) return false;  // too few / too many
    const std::string field =
        last ? data.substr(start) : data.substr(start, comma - start);
    if (!parse_int_field(field, lo[i], hi[i], &out[i])) return false;
    start = comma + 1;
  }
  return true;
}

/// Epoch seconds: a whole non-negative decimal that fits the 32-bit value the
/// wire carries. The width check is explicit rather than a cast, so a value the
/// pump cannot hold is refused instead of truncated into a different instant.
///
/// The ceiling is the wire's, not `time_t`'s. Grundfos' own GENI profile for
/// this pump -- geni_profile_52_7.xml, shipped inside the Grundfos Home app --
/// declares ClockProgramSingleEvent, object type 220, fixed size 10, with
/// `begin` and `end` as `uint32_t`; that is exactly the layout
/// SingleEvent::to_bytes serialises big-endian. The last instant it can hold is
/// 2106-02-07, so a parser that stopped at 2038 would be imposing our
/// limitation rather than the pump's.
static constexpr ParseInt EPOCH_MAX_TS = 4294967295;

/// The floor is 1, not 0. `0` is the single-event wire's disabled/cleared
/// sentinel (schedule_service.h), never shifted in either direction, so an
/// enabled event whose begin is 0 confirms clean -- the readback shifts 0 back
/// to 0 and the comparator agrees -- while describing a slot that says
/// "cleared". Both fields carry the floor: an end of 0 is the same sentinel,
/// and a zero-length window at 0 is refused by the ordering rule anyway.
/// Settled with issue #263's wrap fix, which is the other half of "the confirm
/// agreed with itself about a value nobody asked for".
static bool parse_epoch_field(const std::string &s, uint32_t *out) {
  ParseInt v = 0;
  if (!parse_int_field(s, 1, EPOCH_MAX_TS, &v)) return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

static bool parse_epoch_pair(const std::string &data, uint32_t *begin, uint32_t *end) {
  const size_t comma = data.find(',');
  if (comma == std::string::npos) return false;
  if (data.find(',', comma + 1) != std::string::npos) return false;
  if (!parse_epoch_field(data.substr(0, comma), begin)) return false;
  if (!parse_epoch_field(data.substr(comma + 1), end)) return false;
  // Compared AFTER both are narrowed to the wire's width, so a pair that only
  // looks ordered at 64-bit precision cannot reach the pump reversed.
  return *begin < *end;
}

/// Bound an argument echoed back into `detail`. The event map is copied into an
/// ESPHome API message on a device with tens of KB of usable heap, and every
/// reject_ caller echoes its raw argument -- so an oversized service call was a
/// heap spike the bridge chose to take.
static std::string echo_arg(const std::string &s) {
  constexpr size_t MAX_ECHO = 64;
  if (s.size() <= MAX_ECHO) return s;
  return s.substr(0, MAX_ECHO) + "... (" + std::to_string(s.size()) + " chars)";
}

void AlphaHwrApiBridge::fire_write_settled(const WriteResult &result) {
  std::map<std::string, std::string> data;
  data["op_id"] = result.op_id;
  data["command"] = services::write_command_to_string(result.command);
  data["status"] = services::write_status_to_string(result.status);
  data["detail"] = result.detail;
  data["origin"] = result.origin == services::WriteOrigin::ENTITY      ? "entity"
                   : result.origin == services::WriteOrigin::INTERNAL ? "internal"
                                                                      : "service";
  // Node name (App.get_name()) makes the event self-identifying across a
  // multi-controller install, unlike HA's opaque, re-add-unstable device_id
  // (issue #113). Present on every event, including empty-op_id entity writes.
  data["node"] = App.get_name();
  data["seq"] = std::to_string(result.seq);
  // Echo of the original request, so the event is self-contained for
  // logging/retry (review feedback on #92). Only populated fields are sent.
  if (result.requested_mode != ControlMode::NONE) {
    data["requested_mode"] = ControlService::mode_to_string(result.requested_mode);
  }
  if (std::isfinite(result.requested_value)) {
    char rbuf[48];
    snprintf(rbuf, sizeof(rbuf), "%.4g", result.requested_value);
    data["requested_value"] = rbuf;
  }
  // Guarded independently. Sharing one guard meant a request with a valid min
  // and a NaN max emitted requested_temp_max: "nan" -- reachable from the
  // service, which does not validate its floats.
  if (std::isfinite(result.requested_temp_min)) {
    char rbuf[48];
    snprintf(rbuf, sizeof(rbuf), "%.1f", result.requested_temp_min);
    data["requested_temp_min"] = rbuf;
  }
  if (std::isfinite(result.requested_temp_max)) {
    char rbuf[48];
    snprintf(rbuf, sizeof(rbuf), "%.1f", result.requested_temp_max);
    data["requested_temp_max"] = rbuf;
  }
  // Emitted independently: with the keep-existing sentinels (issue #107) a
  // cycle write may assert any subset of its three fields.
  if (result.requested_on_minutes >= 0) {
    data["requested_on_minutes"] = std::to_string(result.requested_on_minutes);
  }
  if (result.requested_off_minutes >= 0) {
    data["requested_off_minutes"] = std::to_string(result.requested_off_minutes);
  }
  if (std::isfinite(result.requested_flow)) {
    char fbuf[48];
    snprintf(fbuf, sizeof(fbuf), "%.3f", result.requested_flow);
    data["requested_flow"] = fbuf;
  }
  if (!result.requested_begin_hhmm.empty()) {
    data["requested_begin"] = result.requested_begin_hhmm;
  }
  if (!result.requested_end_hhmm.empty()) {
    data["requested_end"] = result.requested_end_hhmm;
  }

  // 48, not 32: "%.1f" of a float near FLT_MAX needs 41 characters, and
  // snprintf truncating it yielded a different, plausible-looking number.
  char buf[48];
  auto put_float = [&](const char *key, float value, const char *fmt) {
    // isfinite, not !isnan: an infinity formatted as "inf" breaks the same
    // contract NaN was excluded for -- a key is either a number the client can
    // parse or absent, never a word.
    if (!std::isfinite(value)) return;
    snprintf(buf, sizeof(buf), fmt, value);
    data[key] = buf;
  };
  auto put_bool = [&](const char *key, int8_t value) {
    if (value < 0) return;
    data[key] = value ? "true" : "false";
  };

  switch (result.command) {
    case WriteCommand::SET_PUMP_ENABLED:
      put_bool("enabled", result.enabled);
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      put_float("value", result.value, "%.4g");
      break;
    case WriteCommand::SET_MODE:
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      break;
    case WriteCommand::SET_SETPOINT:
      if (result.mode != ControlMode::NONE) data["mode"] = ControlService::mode_to_string(result.mode);
      put_float("value", result.value, "%.4g");
      put_bool("enabled", result.enabled);
      break;
    case WriteCommand::SET_TEMPERATURE_RANGE:
      put_float("temp_min", result.temp_min, "%.1f");
      put_float("temp_max", result.temp_max, "%.1f");
      put_bool("autoadapt", result.autoadapt);
      break;
    case WriteCommand::SET_CYCLE_TIMES:
      if (result.on_minutes >= 0) data["on_minutes"] = std::to_string(result.on_minutes);
      if (result.off_minutes >= 0) data["off_minutes"] = std::to_string(result.off_minutes);
      put_float("flow", result.flow, "%.3f");
      break;
    case WriteCommand::SET_FLOW_LIMITER:
      // Which limiter, by name rather than sub-id: the service takes a name,
      // so the event answering it should too (issue #299).
      if (result.limiter_sub == static_cast<int16_t>(services::SUB_LIMITER_CONFIG_MAX_FLOW)) {
        data["limiter"] = "maxflow";
      } else if (result.limiter_sub ==
                 static_cast<int16_t>(services::SUB_LIMITER_CONFIG_MIN_FLOW)) {
        data["limiter"] = "minflow";
      }
      // `limiter_enabled`, not `enabled`, for the reason the remote-mode case
      // below gives: that key already carries the pump's run state and the
      // schedule flag, and a third meaning would be one too many.
      put_bool("limiter_enabled", result.limiter_enabled);
      put_float("limit_gpm", result.limiter_limit_gpm, "%.2f");
      break;
    case WriteCommand::SET_REMOTE_MODE:
      // `remote_enabled`, not `enabled`: that key already carries two
      // meanings -- the pump's run state on the control commands, the
      // schedule flag on every schedule command (the default: branch below
      // fills it from result.sched_enabled) -- and a third would be one too
      // many for anything parsing write_settled.
      put_bool("remote_enabled", result.enabled);
      break;
    case WriteCommand::SET_CLOCK:
      // How far the pump's clock sits from the node's, measured after the
      // write. Absent when no readback decoded a time -- which is every
      // TIMEOUT, since a decoded readback settles the operation on the spot
      // and only undecodable attempts retry.
      put_float("clock_offset_s", result.clock_offset_s, "%.0f");
      break;
    case WriteCommand::SET_PUMP_STATE:
      // Coupled selector: report both underlying flags plus the derived state
      // name, so the settled state is self-contained.
      put_bool("enabled", result.enabled);
      put_bool("schedule_enabled", result.sched_enabled);
      if (result.enabled >= 0 && result.sched_enabled >= 0) {
        data["state"] = ux::state_name(result.enabled != 0, result.sched_enabled != 0);
      }
      break;
    default: {
      // Schedule commands. Settled values come from the verify readbacks.
      static const char *DAY_NAMES[7] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                         "Friday",   "Saturday", "Sunday"};
      if (result.layer >= 0) data["layer"] = std::to_string(result.layer);
      if (result.day >= 0 && result.day <= 6) {
        data["day"] = std::to_string(result.day);
        data["day_name"] = DAY_NAMES[result.day];
      }
      if (result.slot >= 0) data["slot"] = std::to_string(result.slot);
      // Which KIND of single event. A vacation is a Stop event rather than a
      // command of its own, so it settles under `set_single_event` -- and
      // without this key a client could not tell "run the pump once at 6am"
      // from "hold the pump off for a week". They are opposite intents
      // arriving under one name.
      if (result.single_event_action == 0x01) data["event_type"] = "stop";
      if (result.single_event_action == 0x02) data["event_type"] = "run";
      if (!result.begin_hhmm.empty()) data["begin"] = result.begin_hhmm;
      if (!result.end_hhmm.empty()) data["end"] = result.end_hhmm;
      if (result.begin_ts > 0) data["begin_ts"] = std::to_string(result.begin_ts);
      if (result.end_ts > 0) data["end_ts"] = std::to_string(result.end_ts);
      put_bool("enabled", result.sched_enabled);
      if (result.event_count >= 0) data["event_count"] = std::to_string(result.event_count);
      if (result.command == WriteCommand::UPLOAD_SCHEDULE) {
        // Omitted rather than emitted empty, like every other key here. An
        // empty schedule_hash on a payload the bridge rejected before any wire
        // work reads as "the upload ran and the grid hashes to nothing".
        if (!result.layers_written.empty()) data["layers_written"] = result.layers_written;
        if (!result.layers_skipped.empty()) data["layers_skipped"] = result.layers_skipped;
        if (!result.schedule_hash.empty()) data["schedule_hash"] = result.schedule_hash;
      }
      break;
    }
  }

  fire_homeassistant_event(WRITE_SETTLED_EVENT, data);
}

void AlphaHwrApiBridge::reject_(WriteCommand command, const std::string &op_id,
                                const std::string &detail) {
  // Bridge-level failures are malformed requests (unparsable data, unknown
  // mode names): deterministic, so they settle `invalid`, never `rejected`.
  ESP_LOGW(BRIDGE_TAG, "%s (op_id='%s') invalid at the bridge: %s",
           services::write_command_to_string(command), op_id.c_str(), detail.c_str());
  WriteResult result;
  result.op_id = op_id;
  result.command = command;
  result.status = WriteStatus::INVALID;
  result.detail = detail;
  fire_write_settled(result);
}

void AlphaHwrApiBridge::on_set_enabled(bool enabled, std::string op_id) {
  component_->submit_set_enabled(enabled, op_id);
}

void AlphaHwrApiBridge::on_set_mode(std::string mode, std::string op_id) {
  ControlMode parsed;
  if (!ControlService::mode_from_string(mode.c_str(), parsed)) {
    reject_(WriteCommand::SET_MODE, op_id, "unknown mode '" + mode + "'");
    return;
  }
  component_->submit_set_mode(parsed, op_id);
}

void AlphaHwrApiBridge::on_set_setpoint(std::string mode, float value, std::string op_id) {
  ControlMode parsed;
  if (!ControlService::mode_from_string(mode.c_str(), parsed)) {
    reject_(WriteCommand::SET_SETPOINT, op_id, "unknown mode '" + mode + "'");
    return;
  }
  component_->submit_set_setpoint(parsed, value, op_id);
}

void AlphaHwrApiBridge::on_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                                 std::string op_id) {
  component_->submit_set_temperature_range(min_c, max_c, autoadapt, op_id);
}

void AlphaHwrApiBridge::on_set_cycle_times(float on_minutes, float off_minutes, float flow,
                                           std::string op_id) {
  // Minutes arrive as float only because int-typed service variables hit the
  // ESP32-C3 linker bug; the pump takes whole minutes, so fractional values
  // settle invalid here instead of being truncated. 0 means keep-existing
  // (issue #107) for all three fields; the operation layer validates the
  // 1-60 minute and 0.1-10.0 m3/h flow ranges.
  if (!(on_minutes >= 0) || on_minutes > 255 || !(off_minutes >= 0) || off_minutes > 255 ||
      on_minutes != std::floor(on_minutes) || off_minutes != std::floor(off_minutes)) {
    reject_(WriteCommand::SET_CYCLE_TIMES, op_id, "cycle times must be whole minutes 1-60, or 0 to keep");
    return;
  }
  component_->submit_set_cycle_times(static_cast<uint8_t>(on_minutes),
                                     static_cast<uint8_t>(off_minutes), flow, op_id);
}

void AlphaHwrApiBridge::on_set_flow_limiter(std::string limiter, bool enabled, float limit_gpm,
                                            std::string op_id) {
  // Name to sub-id. Case-insensitive on the two the pump declares; anything
  // else is refused here rather than reaching the wire, so the error names what
  // is available instead of echoing an address back.
  std::string key;
  for (char c : limiter) key += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  uint16_t sub = 0;
  if (key == "maxflow" || key == "max_flow" || key == "max") {
    sub = services::SUB_LIMITER_CONFIG_MAX_FLOW;
  } else if (key == "minflow" || key == "min_flow" || key == "min") {
    sub = services::SUB_LIMITER_CONFIG_MIN_FLOW;
  } else {
    reject_(WriteCommand::SET_FLOW_LIMITER, op_id,
            "unknown limiter '" + echo_arg(limiter) + "' (maxflow|minflow)");
    return;
  }

  // 0 means keep-existing, matching set_cycle_times' sentinel (issue #107):
  // a service variable cannot carry NAN, so the API's 0 becomes the operation
  // layer's NAN and only the enable flag changes.
  const float cap = (limit_gpm == 0.0f) ? NAN : limit_gpm;
  component_->submit_set_flow_limiter(sub, enabled, cap, op_id);
}

void AlphaHwrApiBridge::on_set_pump_state(std::string state, std::string op_id) {
  // Coupled three-state selector (off | engaged | scheduled). The component
  // composes it from raw enable/schedule writes and reports one aggregate
  // outcome; we turn that into the single terminal settle event for this op_id.
  ux::PumpScheduleTarget target;
  if (!ux::parse_pump_state(state.c_str(), &target)) {
    reject_(WriteCommand::SET_PUMP_STATE, op_id,
            "unknown state '" + echo_arg(state) + "' (off|engaged|scheduled)");
    return;
  }
  component_->submit_set_pump_state(
      target, [this, op_id](services::WriteStatus status, int8_t actual_engaged,
                            int8_t actual_scheduled, const std::string &detail) {
        // Surface the composed op's most-severe leg (accepted / timeout /
        // superseded / rejected …) so automations can retry/back off correctly,
        // rather than flattening every failure to `rejected`.
        WriteResult result;
        result.op_id = op_id;
        result.command = WriteCommand::SET_PUMP_STATE;
        result.origin = services::WriteOrigin::SERVICE;
        result.status = status;
        result.detail = detail;
        // Tri-state, straight through: -1 means the cache could not tell us,
        // and put_bool()/the `state` guard in fire_write_settled() then omit
        // the key rather than asserting a state nothing read back.
        result.enabled = actual_engaged;
        result.sched_enabled = actual_scheduled;
        fire_write_settled(result);
      });
}

// ---------------------------------------------------------------------------
// Schedule services (data-string formats preserved from the YAML originals)
// ---------------------------------------------------------------------------

void AlphaHwrApiBridge::on_upload_schedule(std::string data, std::string op_id) {
  codec::UploadRequest request;
  std::string err;
  if (!codec::parse_upload_payload(data, &request, &err)) {
    reject_(WriteCommand::UPLOAD_SCHEDULE, op_id, err);
    return;
  }
  component_->submit_upload_schedule(std::move(request), op_id);
}

void AlphaHwrApiBridge::on_set_schedule_entry(std::string data, std::string op_id) {
  static const ParseInt LO[6] = {0, 0, 0, 0, 0, 0};
  static const ParseInt HI[6] = {4, 6, 23, 59, 23, 59};
  ParseInt vals[6];
  if (!parse_int_csv(data, 6, LO, HI, vals)) {
    reject_(WriteCommand::SET_SCHEDULE_ENTRY, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_set_schedule_entry(
      static_cast<uint8_t>(vals[0]), static_cast<uint8_t>(vals[1]), static_cast<uint8_t>(vals[2]),
      static_cast<uint8_t>(vals[3]), static_cast<uint8_t>(vals[4]), static_cast<uint8_t>(vals[5]),
      op_id);
}

void AlphaHwrApiBridge::on_clear_schedule_entry(std::string data, std::string op_id) {
  static const ParseInt LO[2] = {0, 0};
  static const ParseInt HI[2] = {4, 6};
  ParseInt v[2];
  if (!parse_int_csv(data, 2, LO, HI, v)) {
    reject_(WriteCommand::CLEAR_SCHEDULE_ENTRY, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_clear_schedule_entry(static_cast<uint8_t>(v[0]), static_cast<uint8_t>(v[1]),
                                          op_id);
}

void AlphaHwrApiBridge::on_set_schedule_enabled(std::string data, std::string op_id) {
  if (data != "0" && data != "1") {
    reject_(WriteCommand::SET_SCHEDULE_ENABLED, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_set_schedule_enabled(data == "1", op_id);
}

void AlphaHwrApiBridge::on_refresh_schedule(std::string op_id) {
  component_->submit_refresh_schedule(op_id);
}

void AlphaHwrApiBridge::on_set_single_event(std::string data, std::string op_id) {
  uint32_t begin_ts = 0, end_ts = 0;
  if (!parse_epoch_pair(data, &begin_ts, &end_ts)) {
    reject_(WriteCommand::SET_SINGLE_EVENT, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_set_single_event(begin_ts, end_ts, op_id);
}

void AlphaHwrApiBridge::on_clear_single_event(std::string data, std::string op_id) {
  ParseInt idx = 0;
  if (!parse_int_field(data, 0, 99, &idx)) {
    reject_(WriteCommand::CLEAR_SINGLE_EVENT, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_clear_single_event(static_cast<uint8_t>(idx), op_id);
}

void AlphaHwrApiBridge::on_refresh_single_events(std::string op_id) {
  component_->submit_refresh_single_events(op_id);
}

void AlphaHwrApiBridge::on_set_vacation(std::string data, std::string op_id) {
  // A vacation is a multi-day Stop single-event overriding the weekly schedule.
  uint32_t begin_ts = 0, end_ts = 0;
  if (!parse_epoch_pair(data, &begin_ts, &end_ts)) {
    reject_(WriteCommand::SET_SINGLE_EVENT, op_id, "parse error: " + echo_arg(data));
    return;
  }
  component_->submit_set_vacation(begin_ts, end_ts, op_id);
}

void AlphaHwrApiBridge::on_clear_vacation(std::string op_id) {
  component_->submit_clear_vacation(op_id);
}

}  // namespace alpha_hwr
}  // namespace esphome

#endif  // ALPHA_HWR_HAS_API_BRIDGE
