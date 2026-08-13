#include "write_operation_service.h"
#include "schedule_service.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace services {

static const char *TAG = "alpha_hwr.write_op";

const char *write_command_to_string(WriteCommand cmd) {
  switch (cmd) {
    case WriteCommand::SET_PUMP_ENABLED:      return "set_pump_enabled";
    case WriteCommand::SET_MODE:              return "set_mode";
    case WriteCommand::SET_SETPOINT:          return "set_setpoint";
    case WriteCommand::SET_TEMPERATURE_RANGE: return "set_temperature_range";
    case WriteCommand::SET_CYCLE_TIMES:       return "set_cycle_times";
    case WriteCommand::SET_SCHEDULE_ENTRY:    return "set_schedule_entry";
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:  return "clear_schedule_entry";
    case WriteCommand::SET_SCHEDULE_ENABLED:  return "set_schedule_enabled";
    case WriteCommand::SET_SINGLE_EVENT:      return "set_single_event";
    case WriteCommand::CLEAR_SINGLE_EVENT:    return "clear_single_event";
    case WriteCommand::REFRESH_SCHEDULE:      return "refresh_schedule";
    case WriteCommand::REFRESH_SINGLE_EVENTS: return "refresh_single_events";
    case WriteCommand::UPLOAD_SCHEDULE:       return "upload_schedule";
    case WriteCommand::SET_PUMP_STATE:        return "set_pump_state";
  }
  return "unknown";
}

const char *write_status_to_string(WriteStatus status) {
  switch (status) {
    case WriteStatus::ACCEPTED:   return "accepted";
    case WriteStatus::CLAMPED:    return "clamped";
    case WriteStatus::REJECTED:   return "rejected";
    case WriteStatus::TIMEOUT:    return "timeout";
    case WriteStatus::INVALID:    return "invalid";
    case WriteStatus::SUPERSEDED: return "superseded";
    case WriteStatus::PARTIAL:    return "partial";
  }
  return "unknown";
}

static const char *WRITE_OP_DAY_NAMES[7] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                            "Friday",   "Saturday", "Sunday"};

static std::string format_detail(const char *fmt, ...) {
  char buf[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return std::string(buf);
}

WriteOperationService::WriteOperationService(ControlService &control, ScheduleService &schedule)
    : control_(control), schedule_service_(schedule) {}

// ---------------------------------------------------------------------------
// Queue machinery
// ---------------------------------------------------------------------------

void WriteOperationService::schedule_(std::function<void()> fn, uint32_t delay_ms) {
  if (schedule_callback_) {
    schedule_callback_(std::move(fn), delay_ms);
  } else {
    // No scheduler wired (bare construction): run inline, matching the
    // fallback convention of the legacy setters.
    fn();
  }
}

WriteOperationService::Operation *WriteOperationService::find_(uint32_t seq) {
  for (auto &op : queue_) {
    if (op.seq == seq) return &op;
  }
  return nullptr;
}

std::vector<std::string> WriteOperationService::resource_keys_(const Operation &op) {
  switch (op.command) {
    case WriteCommand::SET_PUMP_ENABLED:
      return {"enabled"};
    case WriteCommand::SET_PUMP_STATE:
      // Composed at the api bridge from the two flag writes; never enqueued as
      // an Operation, so it never reaches resource superseding.
      return {};
    case WriteCommand::SET_MODE:
      return {"mode"};
    case WriteCommand::SET_SETPOINT:
      // Per-mode key ONLY: the pump stores an independent setpoint per mode,
      // so queued setpoints for different modes must both run (review
      // feedback on #92). The write still asserts its mode on the wire, but
      // serialization alone yields the correct final mode, so no shared
      // "mode" key is needed (or wanted: it would silently drop a write to
      // an unrelated mode's stored value).
      return {std::string("setpoint:") + ControlService::mode_to_string(op.mode)};
    case WriteCommand::SET_TEMPERATURE_RANGE:
      return {"temp_range"};
    case WriteCommand::SET_CYCLE_TIMES:
      return {"cycle_times"};
    case WriteCommand::SET_SCHEDULE_ENTRY:
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:
      // Per-(layer,day): queuing Monday..Sunday writes doesn't self-cancel.
      return {format_detail("sched_entry:%u:%u", op.layer, op.day_index)};
    case WriteCommand::SET_SCHEDULE_ENABLED:
      return {"sched_enabled"};
    case WriteCommand::SET_SINGLE_EVENT:
    case WriteCommand::CLEAR_SINGLE_EVENT:
      // One shared key: a set's slot is resolved only when the op runs, so
      // per-slot keys can't be computed at submit time. Last write wins
      // across the single-event domain.
      return {"single_event"};
    case WriteCommand::UPLOAD_SCHEDULE: {
      // Full-grid write: collides with every queued per-entry op, the
      // schedule-enabled op, and other uploads (last write wins).
      std::vector<std::string> keys;
      keys.reserve(36);
      for (uint8_t layer = 0; layer < 5; layer++) {
        for (uint8_t day = 0; day < 7; day++) {
          keys.push_back(format_detail("sched_entry:%u:%u", layer, day));
        }
      }
      keys.push_back("sched_enabled");
      return keys;
    }
    case WriteCommand::REFRESH_SCHEDULE:
      return {"refresh_schedule"};
    case WriteCommand::REFRESH_SINGLE_EVENTS:
      return {"refresh_single_events"};
  }
  return {};
}

void WriteOperationService::submit_(Operation op) {
  op.seq = next_seq_++;

  // Supersede queued (not yet started) operations that write the same
  // resource: last write wins, and the superseded caller still gets its one
  // terminal event. In-flight operations are never aborted mid-wire.
  auto new_keys = resource_keys_(op);
  std::vector<uint32_t> superseded;
  for (auto &queued : queue_) {
    if (queued.phase != Phase::QUEUED) continue;
    auto keys = resource_keys_(queued);
    bool collides = false;
    for (const auto &k : keys) {
      if (std::find(new_keys.begin(), new_keys.end(), k) != new_keys.end()) {
        collides = true;
        break;
      }
    }
    if (collides) superseded.push_back(queued.seq);
  }
  for (uint32_t seq : superseded) {
    finish_(seq, WriteStatus::SUPERSEDED,
            op.origin == WriteOrigin::ENTITY
                ? "superseded by entity write"
                : (op.op_id.empty() ? "superseded by service write"
                                    : format_detail("superseded by %s", op.op_id.c_str())));
  }

  ESP_LOGD(TAG, "Queued %s (op_id='%s', seq=%u, queue depth=%zu)",
           write_command_to_string(op.command), op.op_id.c_str(),
           static_cast<unsigned>(op.seq), queue_.size() + 1);
  queue_.push_back(std::move(op));
  start_front_();
}

void WriteOperationService::start_front_() {
  if (queue_.empty()) return;
  Operation &op = queue_.front();
  if (op.phase != Phase::QUEUED) return;  // already running

  uint32_t seq = op.seq;

  if (ready_check_ && !ready_check_()) {
    finish_(seq, WriteStatus::REJECTED, "pump not connected/synchronized");
    return;
  }

  uint32_t budget = WATCHDOG_DEFAULT_MS;
  switch (op.command) {
    case WriteCommand::SET_MODE:              budget = WATCHDOG_SET_MODE_MS; break;
    case WriteCommand::SET_SCHEDULE_ENTRY:
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:  budget = WATCHDOG_SCHED_ENTRY_MS; break;
    case WriteCommand::SET_SCHEDULE_ENABLED:  budget = WATCHDOG_SCHED_ENABLED_MS; break;
    case WriteCommand::SET_SINGLE_EVENT:
    case WriteCommand::CLEAR_SINGLE_EVENT:    budget = WATCHDOG_SINGLE_EVENT_MS; break;
    case WriteCommand::REFRESH_SCHEDULE:      budget = WATCHDOG_REFRESH_SCHEDULE_MS; break;
    case WriteCommand::REFRESH_SINGLE_EVENTS: budget = WATCHDOG_REFRESH_EVENTS_MS; break;
    case WriteCommand::UPLOAD_SCHEDULE:       budget = WATCHDOG_UPLOAD_MS; break;
    default: break;
  }
  arm_watchdog_(seq, budget);

  switch (op.command) {
    case WriteCommand::SET_PUMP_ENABLED:      run_set_enabled_(seq); break;
    case WriteCommand::SET_MODE:              run_set_mode_(seq); break;
    case WriteCommand::SET_SETPOINT:          run_set_setpoint_(seq); break;
    case WriteCommand::SET_TEMPERATURE_RANGE: run_set_temperature_range_(seq); break;
    case WriteCommand::SET_CYCLE_TIMES:       run_set_cycle_times_(seq); break;
    case WriteCommand::SET_SCHEDULE_ENTRY:
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:  run_schedule_entry_(seq); break;
    case WriteCommand::SET_SCHEDULE_ENABLED:  run_schedule_enabled_(seq); break;
    case WriteCommand::SET_SINGLE_EVENT:
    case WriteCommand::CLEAR_SINGLE_EVENT:    run_single_event_(seq); break;
    case WriteCommand::REFRESH_SCHEDULE:      run_refresh_schedule_(seq); break;
    case WriteCommand::REFRESH_SINGLE_EVENTS: run_refresh_single_events_(seq); break;
    case WriteCommand::UPLOAD_SCHEDULE:       run_upload_schedule_(seq); break;
    case WriteCommand::SET_PUMP_STATE:
      // Composed at the api bridge, never enqueued; fail safe if it ever is.
      finish_(seq, WriteStatus::REJECTED, "internal: SET_PUMP_STATE is not enqueueable");
      break;
  }
}

void WriteOperationService::arm_watchdog_(uint32_t seq, uint32_t budget_ms) {
  // Without a scheduler the "delayed" watchdog would fire inline and kill the
  // operation before its first wire step; skip it (production always wires one).
  if (!schedule_callback_) return;
  schedule_([this, seq, budget_ms]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    ESP_LOGW(TAG, "Operation %s (seq=%u) hit its %u ms watchdog",
             write_command_to_string(op->command), static_cast<unsigned>(seq),
             static_cast<unsigned>(budget_ms));
    finish_(seq, WriteStatus::TIMEOUT,
            format_detail("no pump confirmation within %u ms", static_cast<unsigned>(budget_ms)));
  }, budget_ms);
}

void WriteOperationService::finish_(uint32_t seq, WriteStatus status, const std::string &detail) {
  // Locate the operation; the DONE guard makes this idempotent, which is what
  // upholds the exactly-one-terminal-event contract when a watchdog and a
  // confirm handler race.
  size_t index = queue_.size();
  for (size_t i = 0; i < queue_.size(); i++) {
    if (queue_[i].seq == seq) { index = i; break; }
  }
  if (index == queue_.size()) return;
  Operation &op = queue_[index];
  if (op.phase == Phase::DONE) return;
  bool was_front_running = (index == 0 && op.phase != Phase::QUEUED);
  op.phase = Phase::DONE;

  // Roll back the optimistic run-state cache when the write never got a
  // readback. note_enabled_commanded() sets pump_enabled_valid_ = true with no
  // pending marker (unlike note_mode_commanded), so a TIMEOUT leaves the
  // *commanded* state cached as authoritative. with_resolved_enabled_state()
  // then short-circuits on it and the next unrelated setpoint write folds that
  // unverified run flag into the fused 0x0601 frame -- stopping (or starting)
  // the pump. Clearing it here restores the issue-#45 behaviour: read the pump
  // back, and abort rather than guess.
  //
  // This belongs in finish_(), not confirm_enabled_'s failure branch: the
  // dominant path is the watchdog (arm_watchdog_ -> finish_), which never runs
  // the confirm handler at all. REJECTED is deliberately excluded -- it can only
  // be reached via a successful readback, which already corrected the cache.
  if (op.command == WriteCommand::SET_PUMP_ENABLED &&
      (status == WriteStatus::TIMEOUT || status == WriteStatus::SUPERSEDED)) {
    control_.pump_enabled_valid_ = false;
  }

  WriteResult result;
  result.op_id = op.op_id;
  result.command = op.command;
  result.status = status;
  result.detail = detail;
  result.origin = op.origin;
  result.seq = op.seq;
  // Echo the pristine request alongside the settled values (review feedback
  // on #92): the event stays self-contained for logging and retry decisions.
  result.requested_mode = op.req_mode;
  result.requested_value = op.req_value;
  result.requested_temp_min = op.req_temp_min;
  result.requested_temp_max = op.req_temp_max;
  if (op.command == WriteCommand::SET_CYCLE_TIMES) {
    // Kept (0-sentinel) fields are omitted from the requested echo: -1/NAN
    // suppress the event keys, so the echo shows exactly what was asked for.
    result.requested_on_minutes = op.req_on_minutes > 0 ? op.req_on_minutes : -1;
    result.requested_off_minutes = op.req_off_minutes > 0 ? op.req_off_minutes : -1;
    result.requested_flow = op.req_flow;
  }
  if (op.command == WriteCommand::SET_SCHEDULE_ENTRY) {
    result.requested_begin_hhmm =
        format_detail("%02u:%02u", op.req_begin_hour, op.req_begin_minute);
    result.requested_end_hhmm =
        format_detail("%02u:%02u", op.req_end_hour, op.req_end_minute);
  }
  switch (op.command) {
    case WriteCommand::SET_PUMP_ENABLED:
      result.enabled = op.enabled ? 1 : 0;
      result.mode = op.mode;
      result.value = op.value;
      break;
    case WriteCommand::SET_MODE:
      result.mode = op.mode;
      break;
    case WriteCommand::SET_SETPOINT:
      result.mode = op.mode;
      result.value = op.value;
      result.enabled = op.enabled ? 1 : 0;
      break;
    case WriteCommand::SET_TEMPERATURE_RANGE:
      result.temp_min = op.temp_min;
      result.temp_max = op.temp_max;
      result.autoadapt = op.autoadapt ? 1 : 0;
      break;
    case WriteCommand::SET_CYCLE_TIMES:
      // A resolved operation always has minutes >= 1; 0 only survives to a
      // pre-wire terminal (invalid/rejected before the fresh read) where the
      // kept-sentinel is not a value worth reporting.
      result.on_minutes = op.on_minutes > 0 ? op.on_minutes : -1;
      result.off_minutes = op.off_minutes > 0 ? op.off_minutes : -1;
      result.flow = op.flow;  // settled from the confirm readback when it ran
      break;
    case WriteCommand::SET_SCHEDULE_ENTRY:
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:
      result.layer = op.layer;
      result.day = op.day_index;
      result.begin_hhmm = format_detail("%02u:%02u", op.begin_hour, op.begin_minute);
      result.end_hhmm = format_detail("%02u:%02u", op.end_hour, op.end_minute);
      result.sched_enabled = op.enabled ? 1 : 0;
      break;
    case WriteCommand::SET_SCHEDULE_ENABLED:
      result.sched_enabled = op.enabled ? 1 : 0;
      break;
    case WriteCommand::SET_SINGLE_EVENT:
    case WriteCommand::CLEAR_SINGLE_EVENT:
      result.slot = op.slot;
      result.begin_ts = op.begin_ts;
      result.end_ts = op.end_ts;
      result.sched_enabled = op.enabled ? 1 : 0;
      break;
    case WriteCommand::REFRESH_SCHEDULE:
    case WriteCommand::REFRESH_SINGLE_EVENTS:
      result.event_count = op.event_count;
      break;
    case WriteCommand::UPLOAD_SCHEDULE: {
      auto mask_to_list = [](uint8_t mask) {
        std::string out;
        for (uint8_t layer = 0; layer < 5; layer++) {
          if ((mask & (1 << layer)) == 0) continue;
          if (!out.empty()) out += ",";
          out += std::to_string(layer);
        }
        return out;
      };
      result.layers_written = mask_to_list(op.upload_written_mask);
      result.layers_skipped = mask_to_list(op.upload_skipped_mask);
      // Post-op hash: only meaningful once the layer loop has run; empty on
      // rejection before the first write.
      //
      // The failed mask counts as much as the other two (issue #133 review).
      // A layer that fails confirm has still been read back, and that readback
      // refreshed the cache from the device — so current_hash() describes the
      // pump whether or not the write took. Keying only on written|skipped
      // reported an empty hash for the all-layers-failed rejection, which is
      // after the wire work, not before it, and left consumers with no way to
      // learn what the pump actually holds.
      if (op.upload_written_mask != 0 || op.upload_skipped_mask != 0 ||
          op.upload_failed_mask != 0) {
        result.schedule_hash = schedule_service_.current_hash();
      }
      result.sched_enabled = op.upload.enabled;
      break;
    }
    case WriteCommand::SET_PUMP_STATE:
      break;  // never enqueued; its aggregate settle event is built at the api bridge
  }

  ESP_LOGI(TAG, "%s (op_id='%s') settled: %s%s%s",
           write_command_to_string(op.command), op.op_id.c_str(),
           write_status_to_string(status), detail.empty() ? "" : " — ", detail.c_str());

  auto done = op.done;
  auto status_done = op.status_done;
  queue_.erase(queue_.begin() + index);

  if (result_callback_) result_callback_(result);
  if (done) done(status == WriteStatus::ACCEPTED || status == WriteStatus::CLAMPED);
  if (status_done) status_done(status);

  if (was_front_running || index == 0) start_front_();
}

void WriteOperationService::on_disconnect() {
  // Terminal-event everything pending. Collect seqs first: finish_() mutates
  // the queue and may start the next op, which the ready check then rejects —
  // either way every op ends terminal.
  std::vector<uint32_t> seqs;
  seqs.reserve(queue_.size());
  for (auto &op : queue_) seqs.push_back(op.seq);
  for (uint32_t seq : seqs) {
    finish_(seq, WriteStatus::TIMEOUT, "disconnected");
  }
}

// ---------------------------------------------------------------------------
// Submission API
// ---------------------------------------------------------------------------

void WriteOperationService::submit_set_enabled(bool enabled, const std::string &op_id,
                                               std::function<void(bool)> done, WriteOrigin origin,
                                               std::function<void(WriteStatus)> on_status) {
  Operation op;
  op.command = WriteCommand::SET_PUMP_ENABLED;
  op.op_id = op_id;
  op.origin = origin;
  op.enabled = enabled;
  op.done = std::move(done);
  op.status_done = std::move(on_status);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_mode(ControlMode mode, const std::string &op_id,
                                            std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::SET_MODE;
  op.op_id = op_id;
  op.origin = origin;
  op.mode = mode;
  op.req_mode = mode;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_setpoint(ControlMode mode, float value,
                                                const std::string &op_id,
                                                std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::SET_SETPOINT;
  op.op_id = op_id;
  op.origin = origin;
  op.mode = mode;
  op.value = value;
  op.req_mode = mode;
  op.req_value = value;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                                         const std::string &op_id,
                                                         std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::SET_TEMPERATURE_RANGE;
  op.op_id = op_id;
  op.origin = origin;
  op.temp_min = min_c;
  op.temp_max = max_c;
  op.autoadapt = autoadapt;
  op.req_temp_min = min_c;
  op.req_temp_max = max_c;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_cycle_times(uint8_t on_minutes, uint8_t off_minutes,
                                                   float flow, const std::string &op_id,
                                                   std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::SET_CYCLE_TIMES;
  op.op_id = op_id;
  op.origin = origin;
  op.on_minutes = on_minutes;
  op.off_minutes = off_minutes;
  if (!std::isnan(flow) && flow == 0.0f) flow = NAN;  // 0 sentinel -> keep existing
  op.flow = flow;
  op.req_on_minutes = on_minutes;
  op.req_off_minutes = off_minutes;
  op.req_flow = flow;
  op.done = std::move(done);
  submit_(std::move(op));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool WriteOperationService::is_scalar_mode_(ControlMode mode) {
  return mode == ControlMode::CONSTANT_PRESSURE || mode == ControlMode::PROPORTIONAL_PRESSURE ||
         mode == ControlMode::CONSTANT_SPEED || mode == ControlMode::CONSTANT_FLOW;
}

float WriteOperationService::to_native_units_(ControlMode mode, float display_value) {
  switch (mode) {
    case ControlMode::CONSTANT_PRESSURE:
    case ControlMode::PROPORTIONAL_PRESSURE:
      return display_value * 9806.65f;  // meters -> Pascals
    case ControlMode::CONSTANT_FLOW:
      return display_value / 3600.0f;   // m³/h -> m³/s (issue #88/#90)
    default:
      return display_value;             // RPM is native
  }
}

float WriteOperationService::setpoint_epsilon_(ControlMode mode) {
  // Unit-roundtrip tolerances: the confirm comparison happens in display
  // units after the native-unit float conversions.
  switch (mode) {
    case ControlMode::CONSTANT_SPEED: return 1.0f;    // RPM
    case ControlMode::CONSTANT_FLOW:  return 0.005f;  // m³/h
    default:                          return 0.01f;   // meters (pressure modes)
  }
}

uint16_t WriteOperationService::setpoint_sub_id_(ControlMode mode) {
  switch (mode) {
    case ControlMode::CONSTANT_SPEED: return ControlService::SUB_SPEED_SETPOINT;
    case ControlMode::CONSTANT_FLOW:  return ControlService::SUB_FLOW_SETPOINT;
    default:                          return ControlService::SUB_PRESSURE_SETPOINT;
  }
}

// ---------------------------------------------------------------------------
// SET_PUMP_ENABLED
// ---------------------------------------------------------------------------

void WriteOperationService::run_set_enabled_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;
  op->phase = Phase::WRITING;

  // The unfused Class 3 START/STOP commands (ids bench-verified by jfriend00
  // in issue #92) carry no mode and no setpoint, so this operation asserts
  // nothing it would have to resolve first: no mode pre-read, no cached
  // setpoint reuse, no NaN sentinel. The mode/value below are recorded for
  // the settle event only.
  op->mode = control_.current_mode_;
  op->value = control_.get_setpoint_for_mode(op->mode);

  control_.send_run_command(op->enabled, [this, seq](bool /*acked*/, bool rejected) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    if (rejected) {
      finish_(seq, WriteStatus::REJECTED,
              format_detail("pump rejected %s command", op->enabled ? "START" : "STOP"));
      return;
    }
    // Acked, or the ACK window closed without a match: either way the pump
    // sends no unsolicited notification for Class 3 commands, so the run
    // state must be read back — that readback is the authoritative verdict.
    control_.note_enabled_commanded(op->enabled);
    op->phase = Phase::CONFIRMING;
    schedule_([this, seq]() { confirm_enabled_(seq); }, ENABLED_CONFIRM_DELAY_MS);
  });
}

void WriteOperationService::confirm_enabled_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  control_.get_mode_async([this, seq](bool success, ControlMode /*mode*/) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    bool confirmed = success && control_.pump_enabled_valid_ && control_.pump_enabled_ == op->enabled;
    if (confirmed) {
      op->mode = control_.current_mode_;
      op->value = control_.get_setpoint_for_mode(op->mode);
      finish_(seq, WriteStatus::ACCEPTED, "");
      return;
    }
    if (op->attempts < ENABLED_MAX_ATTEMPTS) {
      op->attempts++;
      schedule_([this, seq]() { confirm_enabled_(seq); }, ENABLED_RETRY_DELAY_MS);
      return;
    }
    if (!success) {
      finish_(seq, WriteStatus::TIMEOUT, "run-state readback failed");
    } else {
      // Rejected results carry the settled state from the readback, same as
      // the accepted path; the request survives in the requested_* echoes.
      op->mode = control_.current_mode_;
      op->value = control_.get_setpoint_for_mode(op->mode);
      op->enabled = control_.pump_enabled_;
      finish_(seq, WriteStatus::REJECTED,
              format_detail("pump still reports %s", control_.pump_enabled_ ? "running" : "stopped"));
    }
  });
}

// ---------------------------------------------------------------------------
// SET_MODE
// ---------------------------------------------------------------------------

void WriteOperationService::run_set_mode_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  ControlService::ControlModeMapping mapping;
  if (!ControlService::get_class10_mapping(op->mode, mapping)) {
    finish_(seq, WriteStatus::INVALID,
            format_detail("unsupported mode %d", static_cast<int>(op->mode)));
    return;
  }

  op->phase = Phase::WRITING;
  // The unfused mode-change object (86/sub-10, PR #98): touches neither the
  // run state nor any mode's stored setpoint.
  if (!control_.send_set_mode_request(op->mode)) {
    finish_(seq, WriteStatus::REJECTED, "failed to queue mode command");
    return;
  }
  control_.note_mode_commanded(op->mode);

  op->phase = Phase::CONFIRMING;
  schedule_([this, seq]() { confirm_mode_(seq); }, MODE_CONFIRM_DELAY_MS);
}

void WriteOperationService::confirm_mode_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  control_.get_mode_async([this, seq](bool success, ControlMode reported) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    if (success && reported == op->mode) {
      // get_mode_async has already cleared the issue-#91 pending flag on a
      // matching readback.
      finish_(seq, WriteStatus::ACCEPTED, "");
      return;
    }
    if (op->attempts < MODE_MAX_ATTEMPTS) {
      op->attempts++;
      schedule_([this, seq]() { confirm_mode_(seq); }, MODE_RETRY_DELAY_MS);
      return;
    }
    if (!success) {
      finish_(seq, WriteStatus::TIMEOUT, "mode readback failed");
      return;
    }
    // The pump never applied the command. Adopt the reported mode (the same
    // recovery sync_cache_async performs after its retry cap) so cached state
    // reflects reality, then report honestly.
    control_.mode_command_pending_ = false;
    control_.mode_confirm_attempts_ = 0;
    control_.current_mode_ = reported;
    control_.mode_valid_ = true;
    if (control_.mode_change_callback_) {
      control_.mode_change_callback_(reported, control_.cached_operation_mode_,
                                     control_.get_setpoint_for_mode(reported));
    }
    op->mode = reported;
    finish_(seq, WriteStatus::REJECTED,
            format_detail("pump kept %s", ControlService::get_mode_name(reported)));
  });
}

// ---------------------------------------------------------------------------
// SET_SETPOINT
// ---------------------------------------------------------------------------

void WriteOperationService::run_set_setpoint_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  if (!is_scalar_mode_(op->mode)) {
    finish_(seq, WriteStatus::INVALID,
            format_detail("%s has no scalar setpoint", ControlService::mode_to_string(op->mode)));
    return;
  }
  // Range validation mirrors the legacy setters.
  float lo = 0.5f, hi = 10.0f;
  const char *unit = "m";
  if (op->mode == ControlMode::CONSTANT_SPEED) { lo = 500.0f; hi = 4500.0f; unit = "RPM"; }
  if (op->mode == ControlMode::CONSTANT_FLOW) { lo = 0.1f; hi = 10.0f; unit = "m³/h"; }
  if (std::isnan(op->value) || op->value < lo || op->value > hi) {
    finish_(seq, WriteStatus::INVALID,
            format_detail("value out of range %.1f-%.1f %s", lo, hi, unit));
    return;
  }

  op->phase = Phase::RESOLVING;
  op->pre_value = control_.get_setpoint_for_mode(op->mode);

  // The 0x0601 frame fuses an on/off flag with the setpoint, so resolve the
  // pump's actual run state first — aborting if it cannot be determined,
  // rather than guessing either way (issue #45).
  control_.with_resolved_enabled_state([this, seq](bool resolved, bool enabled) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    if (!resolved) {
      finish_(seq, WriteStatus::REJECTED, "could not determine pump enabled state");
      return;
    }
    op->phase = Phase::WRITING;
    op->enabled = enabled;
    float native = to_native_units_(op->mode, op->value);
    if (!control_.send_control_request(op->mode, enabled, native, false)) {
      finish_(seq, WriteStatus::REJECTED, "failed to queue control request");
      return;
    }
    control_.note_mode_commanded(op->mode);

    schedule_([this, seq, native]() {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      control_.set_class10_setpoint(native, setpoint_sub_id_(op->mode));
      control_.cache_setpoint_for_mode(op->mode, native);
      op->phase = Phase::CONFIRMING;
      schedule_([this, seq]() { confirm_setpoint_(seq); }, SETPOINT_CONFIRM_DELAY_MS);
    }, SETPOINT_STEP2_DELAY_MS);
  });
}

void WriteOperationService::confirm_setpoint_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  control_.get_mode_async([this, seq](bool success, ControlMode reported) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    float stored = control_.get_setpoint_for_mode(op->mode);
    bool readback_usable = success && reported == op->mode && !std::isnan(stored);

    if (!readback_usable) {
      if (op->attempts < SETPOINT_MAX_ATTEMPTS) {
        op->attempts++;
        schedule_([this, seq]() { confirm_setpoint_(seq); }, SETPOINT_RETRY_DELAY_MS);
        return;
      }
      if (success && reported != op->mode) {
        finish_(seq, WriteStatus::REJECTED,
                format_detail("pump did not enter %s (reports %s)",
                              ControlService::get_mode_name(op->mode),
                              ControlService::get_mode_name(reported)));
      } else {
        finish_(seq, WriteStatus::TIMEOUT, "setpoint readback failed");
      }
      return;
    }

    float requested = op->value;
    float eps = setpoint_epsilon_(op->mode);
    op->value = stored;  // report the settled value in all cases below
    if (std::fabs(stored - requested) <= eps) {
      finish_(seq, WriteStatus::ACCEPTED, "");
    } else if (!std::isnan(op->pre_value) && std::fabs(stored - op->pre_value) <= eps) {
      finish_(seq, WriteStatus::REJECTED, format_detail("pump kept %.4g", stored));
    } else {
      finish_(seq, WriteStatus::CLAMPED, format_detail("pump stored %.4g", stored));
    }
  });
}

// ---------------------------------------------------------------------------
// SET_TEMPERATURE_RANGE
// ---------------------------------------------------------------------------

void WriteOperationService::run_set_temperature_range_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  if (std::isnan(op->temp_min) || std::isnan(op->temp_max) ||
      op->temp_min < 20.0f || op->temp_min > 70.0f || op->temp_max < 20.0f || op->temp_max > 70.0f) {
    finish_(seq, WriteStatus::INVALID, "temperatures out of range 20-70 °C");
    return;
  }
  if (op->temp_min >= op->temp_max) {
    finish_(seq, WriteStatus::INVALID,
            format_detail("min %.1f must be below max %.1f", op->temp_min, op->temp_max));
    return;
  }

  op->phase = Phase::WRITING;
  // Post-#98 there is no reason to touch the run state or a setpoint just to
  // enter temperature-range mode: use the unfused mode change, then write the
  // mode's own config object.
  if (!control_.send_set_mode_request(ControlMode::TEMPERATURE_RANGE)) {
    finish_(seq, WriteStatus::REJECTED, "failed to queue mode command");
    return;
  }
  control_.note_mode_commanded(ControlMode::TEMPERATURE_RANGE);

  schedule_([this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    control_.write_temp_range_config(op->temp_min, op->temp_max, op->autoadapt,
      [this, seq](bool acked) {
        Operation *op = find_(seq);
        if (op == nullptr || op->phase == Phase::DONE) return;
        if (!acked) {
          finish_(seq, WriteStatus::REJECTED, "config write not acknowledged");
          return;
        }
        control_.send_configuration_commit();
        op->phase = Phase::CONFIRMING;
        schedule_([this, seq]() { confirm_temperature_range_(seq); }, CONFIG_CONFIRM_DELAY_MS);
      });
  }, CONFIG_STEP2_DELAY_MS);
}

void WriteOperationService::confirm_temperature_range_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  control_.read_obj91_config([this, seq](bool ok) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    if (!ok) {
      if (op->attempts < CONFIG_MAX_ATTEMPTS) {
        op->attempts++;
        schedule_([this, seq]() { confirm_temperature_range_(seq); }, CONFIG_RETRY_DELAY_MS);
        return;
      }
      finish_(seq, WriteStatus::TIMEOUT, "config readback failed");
      return;
    }

    float stored_min = control_.get_cached_temp_min();
    float stored_max = control_.get_cached_temp_max();
    int8_t stored_aa = control_.get_cached_autoadapt();
    bool match = !std::isnan(stored_min) && !std::isnan(stored_max) &&
                 std::fabs(stored_min - op->temp_min) <= 0.1f &&
                 std::fabs(stored_max - op->temp_max) <= 0.1f &&
                 stored_aa == (op->autoadapt ? 1 : 0);
    std::string detail;
    if (!match) {
      detail = format_detail("pump stored %.1f-%.1f °C, autoadapt %s", stored_min, stored_max,
                             stored_aa == 1 ? "on" : "off");
    }
    op->temp_min = stored_min;
    op->temp_max = stored_max;
    op->autoadapt = stored_aa == 1;
    finish_(seq, match ? WriteStatus::ACCEPTED : WriteStatus::CLAMPED, detail);
  });
}

// ---------------------------------------------------------------------------
// SET_CYCLE_TIMES
// ---------------------------------------------------------------------------

void WriteOperationService::run_set_cycle_times_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  // Keep-existing sentinels (issue #107): 0 for either minute field, NAN for
  // flow (the API's 0 was normalized to NAN at submit). Asserted fields are
  // range-checked; kept fields are resolved from the mandatory fresh read.
  const bool on_asserted = op->on_minutes != 0;
  const bool off_asserted = op->off_minutes != 0;
  const bool flow_asserted = !std::isnan(op->flow);
  if ((on_asserted && (op->on_minutes < 1 || op->on_minutes > 60)) ||
      (off_asserted && (op->off_minutes < 1 || op->off_minutes > 60))) {
    finish_(seq, WriteStatus::INVALID, "cycle times must be 1-60 minutes (0 = keep)");
    return;
  }
  if (flow_asserted && (op->flow < 0.1f || op->flow > 10.0f)) {
    finish_(seq, WriteStatus::INVALID, "flow out of range 0.1-10.0 m3/h (0 = keep)");
    return;
  }
  if (!on_asserted && !off_asserted && !flow_asserted) {
    finish_(seq, WriteStatus::INVALID, "nothing to write: all fields are keep-existing");
    return;
  }

  // RESOLVING: the DHW config struct (Obj 91 Sub 421, issue #106) fuses the
  // mode's stored flow setpoint with the on/off periods, so a mandatory fresh
  // read supplies the kept fields (kept flow is echoed back byte-verbatim).
  op->phase = Phase::RESOLVING;
  control_.read_dhw_config([this, seq](bool ok) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    if (!ok) {
      finish_(seq, WriteStatus::REJECTED, "could not read DHW config; write not attempted");
      return;
    }

    op->pre_on_minutes = control_.cached_cycle_time_on_;
    op->pre_off_minutes = control_.cached_cycle_time_off_;
    op->pre_flow = control_.get_cached_cycle_flow();
    if ((op->on_minutes == 0 && op->pre_on_minutes == -1) ||
        (op->off_minutes == 0 && op->pre_off_minutes == -1)) {
      finish_(seq, WriteStatus::REJECTED, "stored cycle periods unreadable; write not attempted");
      return;
    }
    if (op->on_minutes == 0) op->on_minutes = static_cast<uint8_t>(op->pre_on_minutes);
    if (op->off_minutes == 0) op->off_minutes = static_cast<uint8_t>(op->pre_off_minutes);

    op->phase = Phase::WRITING;
    if (!control_.send_set_mode_request(ControlMode::DHW_ON_OFF)) {
      finish_(seq, WriteStatus::REJECTED, "failed to queue mode command");
      return;
    }
    control_.note_mode_commanded(ControlMode::DHW_ON_OFF);

    schedule_([this, seq]() {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      // Capture-verified: the GO app sends this write with no configuration
      // commit and the value persists, so none is sent here either.
      uint8_t setpoint_be4[4];
      const uint8_t *sp = nullptr;
      if (!std::isnan(op->req_flow)) {
        protocol::encode_float_be(to_native_units_(ControlMode::CONSTANT_FLOW, op->req_flow),
                                  setpoint_be4);
        sp = setpoint_be4;
      }
      bool queued = control_.write_dhw_config(op->on_minutes, op->off_minutes,
        [this, seq](bool acked) {
          Operation *op = find_(seq);
          if (op == nullptr || op->phase == Phase::DONE) return;
          if (!acked) {
            finish_(seq, WriteStatus::REJECTED, "config write not acknowledged");
            return;
          }
          op->phase = Phase::CONFIRMING;
          schedule_([this, seq]() { confirm_cycle_times_(seq); }, CONFIG_CONFIRM_DELAY_MS);
        }, sp);
      if (!queued) {
        finish_(seq, WriteStatus::REJECTED, "DHW setpoint unavailable; write not attempted");
      }
    }, CONFIG_STEP2_DELAY_MS);
  });
}

void WriteOperationService::confirm_cycle_times_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  control_.read_dhw_config([this, seq](bool ok) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    int8_t stored_on = control_.cached_cycle_time_on_;
    int8_t stored_off = control_.cached_cycle_time_off_;
    if (!ok || stored_on == -1 || stored_off == -1) {
      if (op->attempts < CONFIG_MAX_ATTEMPTS) {
        op->attempts++;
        schedule_([this, seq]() { confirm_cycle_times_(seq); }, CONFIG_RETRY_DELAY_MS);
        return;
      }
      finish_(seq, WriteStatus::TIMEOUT, "DHW config readback failed");
      return;
    }
    // Per-asserted-field comparison (issue #107): kept fields were resolved
    // FROM the pump, so they are excluded from the accept decision (a
    // concurrent GO-app edit on a kept field is last-writer-wins). REJECTED
    // requires every asserted field to still sit at its pre-read value,
    // mirroring the setpoint confirm's kept-vs-clamped distinction.
    float stored_flow = control_.get_cached_cycle_flow();
    const bool flow_asserted = !std::isnan(op->req_flow);
    const float flow_eps = setpoint_epsilon_(ControlMode::CONSTANT_FLOW);
    bool on_ok = op->req_on_minutes == 0 || stored_on == static_cast<int8_t>(op->on_minutes);
    bool off_ok = op->req_off_minutes == 0 || stored_off == static_cast<int8_t>(op->off_minutes);
    bool flow_ok = !flow_asserted ||
                   (!std::isnan(stored_flow) && std::fabs(stored_flow - op->req_flow) <= flow_eps);
    bool all_kept_old =
        (op->req_on_minutes == 0 || stored_on == op->pre_on_minutes) &&
        (op->req_off_minutes == 0 || stored_off == op->pre_off_minutes) &&
        (!flow_asserted || (!std::isnan(op->pre_flow) &&
                            std::fabs(stored_flow - op->pre_flow) <= flow_eps));
    op->on_minutes = static_cast<uint8_t>(stored_on);
    op->off_minutes = static_cast<uint8_t>(stored_off);
    op->flow = stored_flow;
    if (on_ok && off_ok && flow_ok) {
      finish_(seq, WriteStatus::ACCEPTED, "");
    } else if (all_kept_old) {
      finish_(seq, WriteStatus::REJECTED,
              format_detail("pump kept on=%d off=%d flow=%.3f", stored_on, stored_off, stored_flow));
    } else {
      finish_(seq, WriteStatus::CLAMPED,
              format_detail("pump stored on=%d off=%d flow=%.3f", stored_on, stored_off, stored_flow));
    }
  });
}

// ---------------------------------------------------------------------------
// Schedule submissions
// ---------------------------------------------------------------------------

void WriteOperationService::submit_set_schedule_entry(uint8_t layer, uint8_t day_index,
                                                      uint8_t begin_hour, uint8_t begin_minute,
                                                      uint8_t end_hour, uint8_t end_minute,
                                                      const std::string &op_id,
                                                      std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::SET_SCHEDULE_ENTRY;
  op.op_id = op_id;
  op.origin = origin;
  op.req_begin_hour = begin_hour;
  op.req_begin_minute = begin_minute;
  op.req_end_hour = end_hour;
  op.req_end_minute = end_minute;
  op.layer = layer;
  op.day_index = day_index;
  op.begin_hour = begin_hour;
  op.begin_minute = begin_minute;
  op.end_hour = end_hour;
  op.end_minute = end_minute;
  op.enabled = true;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_clear_schedule_entry(uint8_t layer, uint8_t day_index,
                                                        const std::string &op_id,
                                                        std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::CLEAR_SCHEDULE_ENTRY;
  op.op_id = op_id;
  op.origin = origin;
  op.layer = layer;
  op.day_index = day_index;
  op.enabled = false;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_schedule_enabled(bool enabled, const std::string &op_id,
                                                        std::function<void(bool)> done, WriteOrigin origin,
                                                        std::function<void(WriteStatus)> on_status) {
  Operation op;
  op.command = WriteCommand::SET_SCHEDULE_ENABLED;
  op.op_id = op_id;
  op.origin = origin;
  op.enabled = enabled;
  op.done = std::move(done);
  op.status_done = std::move(on_status);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_single_event(uint32_t begin_ts, uint32_t end_ts,
                                                    const std::string &op_id,
                                                    std::function<void(bool)> done, int slot,
                                                    WriteOrigin origin, uint8_t action) {
  Operation op;
  op.command = WriteCommand::SET_SINGLE_EVENT;
  op.op_id = op_id;
  op.origin = origin;
  op.begin_ts = begin_ts;
  op.end_ts = end_ts;
  op.slot = static_cast<int16_t>(slot);
  op.enabled = true;
  op.single_event_action = action;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_set_vacation(uint32_t begin_ts, uint32_t end_ts,
                                                const std::string &op_id,
                                                std::function<void(bool)> done, WriteOrigin origin) {
  // A vacation is just a Stop (0x01) single-event over a date range.
  submit_set_single_event(begin_ts, end_ts, op_id, std::move(done), -1, origin, 0x01);
}

void WriteOperationService::submit_clear_vacation(const std::string &op_id,
                                                  std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::CLEAR_SINGLE_EVENT;
  op.op_id = op_id;
  op.origin = origin;
  op.slot = -1;                 // auto-resolve to the active vacation slot
  op.clear_by_vacation = true;
  op.enabled = false;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_clear_single_event(uint8_t slot, const std::string &op_id,
                                                      std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::CLEAR_SINGLE_EVENT;
  op.op_id = op_id;
  op.origin = origin;
  op.slot = slot;
  op.enabled = false;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_refresh_schedule(const std::string &op_id,
                                                    std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::REFRESH_SCHEDULE;
  op.op_id = op_id;
  op.origin = origin;
  op.done = std::move(done);
  submit_(std::move(op));
}

void WriteOperationService::submit_refresh_single_events(const std::string &op_id,
                                                         std::function<void(bool)> done, WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::REFRESH_SINGLE_EVENTS;
  op.op_id = op_id;
  op.origin = origin;
  op.done = std::move(done);
  submit_(std::move(op));
}

// ---------------------------------------------------------------------------
// Schedule state machines
// ---------------------------------------------------------------------------

void WriteOperationService::ensure_overview_(uint32_t seq, std::function<void()> next) {
  if (schedule_service_.is_overview_cache_valid()) {
    next();
    return;
  }
  schedule_service_.poll_state_async([this, seq, next](bool ok) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    if (!ok) {
      finish_(seq, WriteStatus::REJECTED, "overview not readable; write not attempted");
      return;
    }
    next();
  });
}

// SET/CLEAR_SCHEDULE_ENTRY: overview precondition -> ALWAYS fresh-read the
// layer (a stale 42-byte layer image would silently overwrite out-of-band
// edits of other days — the issue-#92 clobber class) -> patch + write +
// commit -> settle delay -> re-read the layer and compare the day's entry.
void WriteOperationService::run_schedule_entry_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  if (op->layer > 4 || op->day_index > 6) {
    finish_(seq, WriteStatus::INVALID, "layer must be 0-4 and day 0-6");
    return;
  }
  if (op->command == WriteCommand::SET_SCHEDULE_ENTRY) {
    ScheduleEntry entry(WRITE_OP_DAY_NAMES[op->day_index], op->begin_hour, op->begin_minute,
                        op->end_hour, op->end_minute, 0x02, op->layer, true);
    std::vector<std::string> errors;
    std::vector<ScheduleEntry> entries{entry};
    if (!ScheduleService::validate_entries(entries, &errors)) {
      finish_(seq, WriteStatus::INVALID,
              errors.empty() ? "invalid schedule entry" : errors.front());
      return;
    }
  }

  op->phase = Phase::RESOLVING;
  ensure_overview_(seq, [this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    schedule_service_.read_entries_async(op->layer,
      [this, seq](bool ok, const std::vector<ScheduleEntry> &) {
        Operation *op = find_(seq);
        if (op == nullptr || op->phase == Phase::DONE) return;
        if (!ok) {
          finish_(seq, WriteStatus::REJECTED, "could not read layer; write not attempted");
          return;
        }
        op->phase = Phase::WRITING;
        ScheduleEntry entry;
        if (op->command == WriteCommand::SET_SCHEDULE_ENTRY) {
          entry = ScheduleEntry(WRITE_OP_DAY_NAMES[op->day_index], op->begin_hour,
                                op->begin_minute, op->end_hour, op->end_minute, 0x02,
                                op->layer, true);
        } else {
          entry.set_enabled(false);
          entry.set_action(0x02);
          entry.set_day(WRITE_OP_DAY_NAMES[op->day_index]);
          entry.set_layer(op->layer);
        }
        // The layer is now cached, so set_entry_async takes its cache-hit
        // path: patch the day's 6 bytes + whole-layer write + commit. Its
        // callback fires true unconditionally (the historical "pump commits
        // on timeout" tolerance) — the honest verdict comes from the verify
        // read below, not from it.
        schedule_service_.set_entry_async(op->layer, op->day_index, entry,
          [this, seq](bool /*always_true*/) {
            Operation *op = find_(seq);
            if (op == nullptr || op->phase == Phase::DONE) return;
            op->phase = Phase::CONFIRMING;
            schedule_([this, seq]() { confirm_schedule_entry_(seq); }, SCHED_SETTLE_DELAY_MS);
          });
      });
  });
}

void WriteOperationService::confirm_schedule_entry_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  schedule_service_.read_entries_async(op->layer,
    [this, seq](bool ok, const std::vector<ScheduleEntry> &) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;

      if (!ok) {
        if (op->attempts < SCHED_MAX_ATTEMPTS) {
          op->attempts++;
          schedule_([this, seq]() { confirm_schedule_entry_(seq); }, SCHED_RETRY_DELAY_MS);
          return;
        }
        finish_(seq, WriteStatus::TIMEOUT, "layer readback failed");
        return;
      }

      ScheduleEntry actual;
      if (!schedule_service_.get_cached_entry(op->layer, op->day_index, &actual)) {
        finish_(seq, WriteStatus::TIMEOUT, "layer readback failed");
        return;
      }
      bool want_enabled = op->command == WriteCommand::SET_SCHEDULE_ENTRY;
      bool match = actual.is_enabled() == want_enabled &&
                   (!want_enabled ||
                    (actual.get_begin_hour() == op->begin_hour &&
                     actual.get_begin_minute() == op->begin_minute &&
                     actual.get_end_hour() == op->end_hour &&
                     actual.get_end_minute() == op->end_minute));
      if (match) {
        finish_(seq, WriteStatus::ACCEPTED, "");
        return;
      }
      if (op->attempts < SCHED_MAX_ATTEMPTS) {
        op->attempts++;
        schedule_([this, seq]() { confirm_schedule_entry_(seq); }, SCHED_RETRY_DELAY_MS);
        return;
      }
      // Report what the pump actually holds.
      op->enabled = actual.is_enabled();
      op->begin_hour = actual.get_begin_hour();
      op->begin_minute = actual.get_begin_minute();
      op->end_hour = actual.get_end_hour();
      op->end_minute = actual.get_end_minute();
      finish_(seq, WriteStatus::REJECTED, "layer readback does not match written entry");
    });
}

// SET_SCHEDULE_ENABLED: overview precondition -> verified RMW write (no
// hardcoded-defaults fallback) -> poll the overview back and compare byte 4.
void WriteOperationService::run_schedule_enabled_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  op->phase = Phase::RESOLVING;
  ensure_overview_(seq, [this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    op->phase = Phase::WRITING;
    schedule_service_.set_state_async(op->enabled, [this, seq](bool sent) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      if (!sent) {
        finish_(seq, WriteStatus::REJECTED, "schedule state write not attempted");
        return;
      }
      op->phase = Phase::CONFIRMING;
      schedule_([this, seq]() { confirm_schedule_enabled_(seq); }, 1000);
    });
  });
}

void WriteOperationService::confirm_schedule_enabled_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  schedule_service_.poll_state_async([this, seq](bool ok) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    bool actual = false;
    bool have_state = ok && schedule_service_.get_state(&actual);
    if (have_state && actual == op->enabled) {
      finish_(seq, WriteStatus::ACCEPTED, "");
      return;
    }
    if (op->attempts < SCHED_MAX_ATTEMPTS) {
      op->attempts++;
      schedule_([this, seq]() { confirm_schedule_enabled_(seq); }, SCHED_RETRY_DELAY_MS);
      return;
    }
    if (!have_state) {
      finish_(seq, WriteStatus::TIMEOUT, "overview readback failed");
    } else {
      op->enabled = actual;
      finish_(seq, WriteStatus::REJECTED,
              format_detail("pump still reports schedule %s", actual ? "enabled" : "disabled"));
    }
  });
}

// SET/CLEAR_SINGLE_EVENT: overview precondition -> resolve the slot (reading
// the slot cache first if cold) -> write + commit -> settle delay -> re-read
// the one slot and compare the 10 bytes.
void WriteOperationService::run_single_event_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  if (op->command == WriteCommand::SET_SINGLE_EVENT && op->end_ts <= op->begin_ts) {
    finish_(seq, WriteStatus::INVALID, "end timestamp must be after begin timestamp");
    return;
  }

  op->phase = Phase::RESOLVING;
  ensure_overview_(seq, [this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;

    if (op->slot >= 0) {
      // Bound the caller-supplied slot against what this pump actually has.
      // ensure_overview_() just ran, so get_max_single_events() is the device's
      // own count rather than the 35 fallback. Without this an out-of-range
      // slot reaches the wire as SubID 900+idx and settles TIMEOUT instead of
      // being rejected up front.
      uint8_t max_events = schedule_service_.get_max_single_events();
      if (op->slot >= static_cast<int16_t>(max_events)) {
        finish_(seq, WriteStatus::REJECTED,
                "single event slot " + std::to_string(op->slot) +
                    " out of range (pump has " + std::to_string(max_events) +
                    ")");
        return;
      }
      write_single_event_(seq);
      return;
    }
    // Auto-slot: needs a warm single-event cache, else find_free would
    // blindly pick slot 0 and could overwrite a real event.
    auto resolve = [this, seq]() {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      if (op->clear_by_vacation) {
        // clear_vacation: target the active Stop (vacation) single-event.
        int slot = schedule_service_.find_vacation_slot();
        if (slot < 0) {
          finish_(seq, WriteStatus::ACCEPTED, "no active vacation");
          return;
        }
        op->slot = static_cast<int16_t>(slot);
        write_single_event_(seq);
        return;
      }
      // The new event's begin timestamp doubles as "now": any cached
      // event that ended before it is expired and its slot reusable.
      int slot = schedule_service_.find_free_single_event_slot(op->begin_ts);
      if (slot < 0) {
        finish_(seq, WriteStatus::REJECTED, "no free single event slots");
        return;
      }
      op->slot = static_cast<int16_t>(slot);
      write_single_event_(seq);
    };
    if (schedule_service_.is_single_events_cached()) {
      resolve();
      return;
    }
    schedule_service_.read_single_events_async(
        [this, seq, resolve](bool ok, const std::vector<SingleEvent> &) {
          Operation *op = find_(seq);
          if (op == nullptr || op->phase == Phase::DONE) return;
          if (!ok) {
            finish_(seq, WriteStatus::REJECTED, "could not read single events; write not attempted");
            return;
          }
          resolve();
        });
  });
}

void WriteOperationService::write_single_event_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;
  op->phase = Phase::WRITING;

  SingleEvent event;
  event.index = static_cast<uint8_t>(op->slot);
  event.enabled = op->command == WriteCommand::SET_SINGLE_EVENT;
  event.action = op->single_event_action;
  event.begin_timestamp = op->begin_ts;
  event.end_timestamp = op->end_ts;

  schedule_service_.write_single_event_async(event, [this, seq](bool /*always_true*/) {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    op->phase = Phase::CONFIRMING;
    schedule_([this, seq]() { confirm_single_event_(seq); }, SCHED_SETTLE_DELAY_MS);
  });
}

void WriteOperationService::confirm_single_event_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  schedule_service_.read_single_event_async(static_cast<uint8_t>(op->slot),
    [this, seq](bool ok, const SingleEvent &actual) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;

      if (!ok) {
        if (op->attempts < SCHED_MAX_ATTEMPTS) {
          op->attempts++;
          schedule_([this, seq]() { confirm_single_event_(seq); }, SCHED_RETRY_DELAY_MS);
          return;
        }
        finish_(seq, WriteStatus::TIMEOUT, "single event readback failed");
        return;
      }

      bool want_enabled = op->command == WriteCommand::SET_SINGLE_EVENT;
      bool match = actual.enabled == want_enabled &&
                   (!want_enabled || (actual.begin_timestamp == op->begin_ts &&
                                      actual.end_timestamp == op->end_ts));
      if (match) {
        finish_(seq, WriteStatus::ACCEPTED, "");
        return;
      }
      if (op->attempts < SCHED_MAX_ATTEMPTS) {
        op->attempts++;
        schedule_([this, seq]() { confirm_single_event_(seq); }, SCHED_RETRY_DELAY_MS);
        return;
      }
      op->enabled = actual.enabled;
      op->begin_ts = actual.begin_timestamp;
      op->end_ts = actual.end_timestamp;
      finish_(seq, WriteStatus::REJECTED, "slot readback does not match written event");
    });
}

void WriteOperationService::run_refresh_schedule_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;
  op->phase = Phase::WRITING;

  schedule_service_.read_entries_async(-1,
    [this, seq](bool ok, const std::vector<ScheduleEntry> &entries) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      if (!ok) {
        finish_(seq, WriteStatus::TIMEOUT, "schedule read failed");
        return;
      }
      op->event_count = static_cast<int16_t>(entries.size());
      finish_(seq, WriteStatus::ACCEPTED,
              format_detail("%zu entries cached", entries.size()));
    });
}

void WriteOperationService::run_refresh_single_events_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;
  op->phase = Phase::WRITING;

  // Warm the overview first, like every sibling operation. Without it
  // get_max_single_events() falls back to 35, so on a pump with fewer slots the
  // scan spends a 3 s timeout on each slot that does not exist -- ~90 s of
  // monopolised BLE, and the all-or-nothing read then fails and caches nothing.
  ensure_overview_(seq, [this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    schedule_service_.read_single_events_async(
      [this, seq](bool ok, const std::vector<SingleEvent> &events) {
        Operation *op = find_(seq);
        if (op == nullptr || op->phase == Phase::DONE) return;
        if (!ok) {
          finish_(seq, WriteStatus::TIMEOUT, "single event read failed");
          return;
        }
        op->event_count = static_cast<int16_t>(events.size());
        finish_(seq, WriteStatus::ACCEPTED, "");
      });
  });
}

// ---------------------------------------------------------------------------
// UPLOAD_SCHEDULE (bulk full-state grid upload)
//
// Full-state bulk upload: overview precondition, then per layer 0..4 a
// mandatory fresh read, a memcmp against the desired 42-byte image (layers
// already matching are SKIPPED — no BLE write), a whole-layer write + commit
// for the rest, the shared settle delay, and a readback confirm of every
// written layer. Exactly one terminal event; PARTIAL when some layers
// confirmed and some failed.
// ---------------------------------------------------------------------------

void WriteOperationService::submit_upload_schedule(codec::UploadRequest request,
                                                   const std::string &op_id,
                                                   std::function<void(bool)> done,
                                                   WriteOrigin origin) {
  Operation op;
  op.command = WriteCommand::UPLOAD_SCHEDULE;
  op.op_id = op_id;
  op.origin = origin;
  op.done = std::move(done);
  op.upload = std::move(request);
  submit_(std::move(op));
}

void WriteOperationService::run_upload_schedule_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  op->phase = Phase::RESOLVING;
  ensure_overview_(seq, [this, seq]() {
    Operation *op = find_(seq);
    if (op == nullptr || op->phase == Phase::DONE) return;
    op->phase = Phase::WRITING;
    op->upload_layer = 0;
    upload_next_layer_(seq);
  });
}

void WriteOperationService::upload_next_layer_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  if (op->upload_layer >= codec::UPLOAD_LAYERS) {
    upload_apply_enabled_(seq);
    return;
  }
  uint8_t layer = op->upload_layer;

  // Mandatory fresh read — never trust a stale cache before deciding to
  // skip or write (same rule as the per-entry op).
  schedule_service_.read_entries_async(layer,
    [this, seq, layer](bool ok, const std::vector<ScheduleEntry> &) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;

      if (!ok) {
        ESP_LOGW(TAG, "upload: layer %u read failed; layer not written", layer);
        op->upload_failed_mask |= (1 << layer);
        op->upload_layer++;
        upload_next_layer_(seq);
        return;
      }

      uint8_t desired[codec::LAYER_IMAGE_BYTES];
      codec::build_layer_image(op->upload, layer, desired);
      uint8_t actual[codec::LAYER_IMAGE_BYTES];
      if (schedule_service_.build_cached_layer_image(layer, actual) &&
          memcmp(desired, actual, codec::LAYER_IMAGE_BYTES) == 0) {
        ESP_LOGD(TAG, "upload: layer %u already matches — skipped", layer);
        op->upload_skipped_mask |= (1 << layer);
        op->upload_layer++;
        upload_next_layer_(seq);
        return;
      }

      // Whole-layer write of the desired image (one 42-byte GENI frame +
      // configuration commit), via the same proven path as set_entry_async.
      schedule_service_.write_layer_image_async(layer, desired,
        [this, seq, layer](bool /*always_true*/) {
          Operation *op = find_(seq);
          if (op == nullptr || op->phase == Phase::DONE) return;
          op->upload_written_mask |= (1 << layer);
          op->upload_layer++;
          upload_next_layer_(seq);
        });
    });
}

void WriteOperationService::upload_apply_enabled_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  // Snapshot the request before the confirm readback overwrites op->upload
  // with the pump's actual state; the verdict needs both.
  op->upload_enabled_requested = op->upload.enabled;

  bool current = false;
  bool have_state = schedule_service_.get_state(&current);
  if (op->upload.enabled < 0 ||
      (have_state && current == (op->upload.enabled == 1))) {
    op->phase = Phase::CONFIRMING;
    op->upload_layer = 0;  // reuse as the confirm cursor
    schedule_([this, seq]() { confirm_upload_(seq); }, SCHED_SETTLE_DELAY_MS);
    return;
  }
  schedule_service_.set_state_async(op->upload.enabled == 1,
    [this, seq](bool /*sent*/) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;
      // Read the flag back before confirming. confirm_upload_ only walks the
      // written *layers*, and the enabled flag lives in Sub 1, which no layer
      // readback carries -- so without this the operation settled ACCEPTED and
      // reported the requested value even when the enable write was dropped
      // entirely (AGENTS §8.4 rule 3: the readback decides, not the ACK).
      // Overwrite the operation's field with what the pump actually holds so
      // the settle event and its schedule_hash describe the device.
      schedule_service_.poll_state_async([this, seq](bool ok) {
        Operation *op = find_(seq);
        if (op == nullptr || op->phase == Phase::DONE) return;
        bool actual = false;
        if (ok && schedule_service_.get_state(&actual)) {
          op->upload_enabled_mismatch =
              (op->upload_enabled_requested >= 0) &&
              (actual != (op->upload_enabled_requested == 1));
          op->upload.enabled = actual ? 1 : 0;  // report what the pump holds
        } else {
          // Cannot tell whether the write took. Reporting ACCEPTED here is the
          // exact false-accept this readback exists to prevent.
          op->upload_enabled_unreadable = true;
        }
        op->phase = Phase::CONFIRMING;
        op->upload_layer = 0;  // reuse as the confirm cursor
        schedule_([this, seq]() { confirm_upload_(seq); }, SCHED_SETTLE_DELAY_MS);
      });
    });
}

void WriteOperationService::confirm_upload_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  // Readback-confirm each WRITTEN layer; upload_layer was reset to 0 on
  // the transition into CONFIRMING and now serves as the confirm cursor.
  while (op->upload_layer < codec::UPLOAD_LAYERS &&
         (op->upload_written_mask & (1 << op->upload_layer)) == 0) {
    op->upload_layer++;
  }
  if (op->upload_layer >= codec::UPLOAD_LAYERS) {
    finish_upload_(seq);
    return;
  }
  uint8_t layer = op->upload_layer;
  schedule_service_.read_entries_async(layer,
    [this, seq, layer](bool ok, const std::vector<ScheduleEntry> &) {
      Operation *op = find_(seq);
      if (op == nullptr || op->phase == Phase::DONE) return;

      uint8_t desired[codec::LAYER_IMAGE_BYTES];
      codec::build_layer_image(op->upload, layer, desired);
      uint8_t actual[codec::LAYER_IMAGE_BYTES];
      bool match = ok &&
                   schedule_service_.build_cached_layer_image(layer, actual) &&
                   memcmp(desired, actual, codec::LAYER_IMAGE_BYTES) == 0;
      if (!match) {
        ESP_LOGW(TAG, "upload: layer %u readback does not match", layer);
        op->upload_written_mask &= ~(1 << layer);
        op->upload_failed_mask |= (1 << layer);
      }
      op->upload_layer++;
      confirm_upload_(seq);
    });
}

void WriteOperationService::finish_upload_(uint32_t seq) {
  Operation *op = find_(seq);
  if (op == nullptr || op->phase == Phase::DONE) return;

  uint8_t confirmed = op->upload_written_mask | op->upload_skipped_mask;

  // The enabled flag lives in ClockProgramOverview Sub 1, which no layer
  // readback carries, so the layer masks alone cannot see a dropped enable
  // write. Without this an enable-only upload settled ACCEPTED "no-op".
  if (op->upload_enabled_unreadable) {
    finish_(seq, WriteStatus::TIMEOUT,
            "could not read the schedule-enabled state back");
    return;
  }
  if (op->upload_enabled_mismatch) {
    const char *want = op->upload_enabled_requested == 1 ? "enabled" : "disabled";
    if (op->upload_failed_mask == 0 && confirmed != 0) {
      finish_(seq, WriteStatus::PARTIAL,
              format_detail("layers written but pump still reports schedule %s",
                            op->upload.enabled == 1 ? "enabled" : "disabled"));
    } else {
      finish_(seq, WriteStatus::REJECTED,
              format_detail("pump did not take schedule %s", want));
    }
    return;
  }

  if (op->upload_failed_mask == 0) {
    finish_(seq, WriteStatus::ACCEPTED,
            op->upload_written_mask == 0 ? "no-op" : "");
  } else if (confirmed != 0) {
    finish_(seq, WriteStatus::PARTIAL,
            format_detail("failed layers mask 0x%02x", op->upload_failed_mask));
  } else {
    finish_(seq, WriteStatus::REJECTED, "no layer could be written");
  }
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
