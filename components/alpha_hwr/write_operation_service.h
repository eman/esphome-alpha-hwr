#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "control_service.h"
#include "schedule_codec.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * The kinds of write the operation layer can perform (issue #92).
 * Schedule commands are part of the same enum so schedule writes share the
 * one-terminal-event contract.
 */
enum class WriteCommand : uint8_t {
  SET_PUMP_ENABLED,
  SET_MODE,
  SET_SETPOINT,
  SET_TEMPERATURE_RANGE,
  SET_CYCLE_TIMES,
  SET_SCHEDULE_ENTRY,
  CLEAR_SCHEDULE_ENTRY,
  SET_SCHEDULE_ENABLED,
  SET_SINGLE_EVENT,
  CLEAR_SINGLE_EVENT,
  REFRESH_SCHEDULE,
  REFRESH_SINGLE_EVENTS,
  UPLOAD_SCHEDULE,  // bulk full-state grid upload (RFC-005 / issue #5)
};

/**
 * Terminal status of a write operation. Every submitted operation ends in
 * exactly one of these, reported through the result callback.
 */
enum class WriteStatus : uint8_t {
  ACCEPTED,    // pump confirmed the requested value
  CLAMPED,     // pump stored a different value (reported in the result)
  REJECTED,    // pump kept its old value, or the request was invalid/unsafe
  TIMEOUT,     // no pump confirmation within the operation's budget
  SUPERSEDED,  // replaced by a newer queued write to the same resource
  PARTIAL,     // upload_schedule only: some layers confirmed, some failed
};

const char *write_command_to_string(WriteCommand cmd);
const char *write_status_to_string(WriteStatus status);

/**
 * Terminal result of a write operation. Only the value fields relevant to
 * `command` are populated; the rest keep their "unknown" defaults. For
 * accepted/clamped/rejected the value fields carry the SETTLED values the
 * pump actually holds (from the confirm readback), not the request.
 */
struct WriteResult {
  std::string op_id;   // caller-supplied identifier ("" for entity-originated writes)
  WriteCommand command{WriteCommand::SET_PUMP_ENABLED};
  WriteStatus status{WriteStatus::REJECTED};
  std::string detail;  // short reason when relevant ("pump stored 1650", ...)

  ControlMode mode{ControlMode::NONE};
  float value{NAN};             // setpoint, in display units
  int8_t enabled{-1};           // -1 unknown, 0 off, 1 on
  float temp_min{NAN};
  float temp_max{NAN};
  int8_t autoadapt{-1};         // -1 unknown, 0 off, 1 on
  int16_t on_minutes{-1};
  int16_t off_minutes{-1};

  // Schedule commands
  int16_t layer{-1};
  int16_t day{-1};
  int16_t slot{-1};
  std::string begin_hhmm;
  std::string end_hhmm;
  uint32_t begin_ts{0};
  uint32_t end_ts{0};
  int8_t sched_enabled{-1};
  int16_t event_count{-1};

  // UPLOAD_SCHEDULE: per-layer outcome summary + post-op canonical hash
  std::string layers_written;   // e.g. "0,2"
  std::string layers_skipped;   // e.g. "1,3,4" (already matching, no write)
  std::string schedule_hash;    // "" when rejected before any wire write
};

class ScheduleService;

/**
 * Write-operation layer (issue #92).
 *
 * Owns the lifecycle of every pump write: it serializes multi-step write
 * SEQUENCES (the transport only serializes individual commands), builds each
 * wire step from the arguments passed — never from a possibly-stale cache —
 * schedules the confirm readback, decides the terminal status
 * (accepted/clamped/rejected/timeout/superseded), and reports exactly one
 * terminal result per operation through the result callback.
 *
 * Invariants:
 * - Exactly one operation is in flight at a time; later submissions queue.
 * - A newly submitted operation SUPERSEDES any still-queued operation that
 *   targets the same resource (last write wins). An operation that has begun
 *   its wire sequence is never aborted mid-wire; it runs to its own terminal
 *   status first.
 * - Every operation reaches exactly one terminal status: validation and
 *   readiness failures reject before any wire write, a per-operation watchdog
 *   converts a stuck operation into TIMEOUT, and on_disconnect() terminates
 *   everything pending, so a client waiting on the result can never hang.
 *
 * The service composes ControlService's wire primitives and coordination state
 * (it is a friend of ControlService); the cache and the issue-#91 pending-mode
 * guards stay in ControlService, so entity reads keep working unchanged.
 */
class WriteOperationService {
 public:
  WriteOperationService(ControlService &control, ScheduleService &schedule);

  /** Delegate for scheduling delayed steps (the component's set_timeout). */
  void set_schedule_callback(std::function<void(std::function<void()>, uint32_t)> callback) {
    schedule_callback_ = std::move(callback);
  }

  /**
   * Readiness gate consulted when an operation starts (the component's
   * is_state_synchronized). Not ready => the operation terminates REJECTED
   * before any wire write.
   */
  void set_ready_check(std::function<bool()> check) { ready_check_ = std::move(check); }

  /** Sink for terminal results (the api bridge fires the HA event from this). */
  void set_result_callback(std::function<void(const WriteResult &)> callback) {
    result_callback_ = std::move(callback);
  }

  // ---- Submission API. `done` is the legacy bool callback used by the
  // entity path; it fires with the terminal result (true for
  // accepted/clamped). Programmatic callers identify their result by op_id
  // through the result callback instead.
  void submit_set_enabled(bool enabled, const std::string &op_id,
                          std::function<void(bool)> done = nullptr);
  void submit_set_mode(ControlMode mode, const std::string &op_id,
                       std::function<void(bool)> done = nullptr);
  void submit_set_setpoint(ControlMode mode, float value, const std::string &op_id,
                           std::function<void(bool)> done = nullptr);
  void submit_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                    const std::string &op_id,
                                    std::function<void(bool)> done = nullptr);
  void submit_set_cycle_times(uint8_t on_minutes, uint8_t off_minutes,
                              const std::string &op_id,
                              std::function<void(bool)> done = nullptr);

  // ---- Schedule writes (same contract; verify readbacks are new — no
  // schedule write had an honest success signal before this layer).
  void submit_set_schedule_entry(uint8_t layer, uint8_t day_index, uint8_t begin_hour,
                                 uint8_t begin_minute, uint8_t end_hour, uint8_t end_minute,
                                 const std::string &op_id,
                                 std::function<void(bool)> done = nullptr);
  void submit_clear_schedule_entry(uint8_t layer, uint8_t day_index, const std::string &op_id,
                                   std::function<void(bool)> done = nullptr);
  void submit_set_schedule_enabled(bool enabled, const std::string &op_id,
                                   std::function<void(bool)> done = nullptr);
  /** slot < 0 auto-resolves the first free slot; the chosen slot is echoed in the result. */
  void submit_set_single_event(uint32_t begin_ts, uint32_t end_ts, const std::string &op_id,
                               std::function<void(bool)> done = nullptr, int slot = -1);
  void submit_clear_single_event(uint8_t slot, const std::string &op_id,
                                 std::function<void(bool)> done = nullptr);
  // Reads, but they contend for the same transport, so they run through the
  // same operation queue and get the same terminal-event guarantee.
  void submit_refresh_schedule(const std::string &op_id,
                               std::function<void(bool)> done = nullptr);
  void submit_refresh_single_events(const std::string &op_id,
                                    std::function<void(bool)> done = nullptr);
  /**
   * Bulk full-state schedule upload (RFC-005). The request expresses the
   * entire 7x5 grid; layers whose fresh readback already matches the
   * desired image are skipped (no BLE write). Terminates ACCEPTED (all
   * layers confirmed, possibly all skipped), PARTIAL (mixed), REJECTED,
   * TIMEOUT or SUPERSEDED — always exactly one terminal event.
   */
  void submit_upload_schedule(codec::UploadRequest request,
                              const std::string &op_id,
                              std::function<void(bool)> done = nullptr);

  /**
   * Terminate every queued and in-flight operation with TIMEOUT
   * ("disconnected"). Wire next to ControlService::invalidate_cache() so a
   * client can never be left waiting across a BLE drop.
   */
  void on_disconnect();

  /** Number of operations not yet terminal (in flight + queued). */
  size_t pending_count() const { return queue_.size(); }

 private:
  enum class Phase : uint8_t { QUEUED, RESOLVING, WRITING, CONFIRMING, DONE };

  struct Operation {
    uint32_t seq{0};
    WriteCommand command{WriteCommand::SET_PUMP_ENABLED};
    std::string op_id;
    Phase phase{Phase::QUEUED};
    uint8_t attempts{0};
    std::function<void(bool)> done;

    // Requested fields; the confirm handlers overwrite them with the SETTLED
    // values before finishing, so finish_() can copy them into the result.
    ControlMode mode{ControlMode::NONE};
    float value{NAN};
    bool enabled{false};
    float temp_min{NAN};
    float temp_max{NAN};
    bool autoadapt{false};
    uint8_t on_minutes{0};
    uint8_t off_minutes{0};
    // Setpoint value before the write (display units): distinguishes
    // "pump kept its old value" (REJECTED) from "pump stored a different
    // value" (CLAMPED) in the confirm comparison.
    float pre_value{NAN};

    // Schedule fields
    uint8_t layer{0};
    uint8_t day_index{0};
    uint8_t begin_hour{0}, begin_minute{0}, end_hour{0}, end_minute{0};
    int16_t slot{-1};
    uint32_t begin_ts{0}, end_ts{0};
    int16_t event_count{-1};

    // UPLOAD_SCHEDULE fields
    codec::UploadRequest upload;
    uint8_t upload_layer{0};        // cursor for the layer loop / confirms
    uint8_t upload_written_mask{0};
    uint8_t upload_skipped_mask{0};
    uint8_t upload_failed_mask{0};
  };

  // ---- Queue machinery
  void submit_(Operation op);
  void start_front_();
  void finish_(uint32_t seq, WriteStatus status, const std::string &detail);
  Operation *find_(uint32_t seq);
  void arm_watchdog_(uint32_t seq, uint32_t budget_ms);
  void schedule_(std::function<void()> fn, uint32_t delay_ms);
  /** Resources an operation writes; queued ops with intersecting keys are superseded. */
  static std::vector<std::string> resource_keys_(const Operation &op);

  // ---- Per-command state machines
  void run_set_enabled_(uint32_t seq);
  void write_enabled_(uint32_t seq);
  void confirm_enabled_(uint32_t seq);
  void run_set_mode_(uint32_t seq);
  void confirm_mode_(uint32_t seq);
  void run_set_setpoint_(uint32_t seq);
  void confirm_setpoint_(uint32_t seq);
  void run_set_temperature_range_(uint32_t seq);
  void confirm_temperature_range_(uint32_t seq);
  void run_set_cycle_times_(uint32_t seq);
  void confirm_cycle_times_(uint32_t seq);
  void run_schedule_entry_(uint32_t seq);
  void confirm_schedule_entry_(uint32_t seq);
  void run_schedule_enabled_(uint32_t seq);
  void confirm_schedule_enabled_(uint32_t seq);
  void run_single_event_(uint32_t seq);
  void write_single_event_(uint32_t seq);
  void confirm_single_event_(uint32_t seq);
  void run_refresh_schedule_(uint32_t seq);
  void run_refresh_single_events_(uint32_t seq);
  void run_upload_schedule_(uint32_t seq);
  void upload_next_layer_(uint32_t seq);
  void upload_apply_enabled_(uint32_t seq);
  void confirm_upload_(uint32_t seq);
  void finish_upload_(uint32_t seq);
  /**
   * Overview precondition shared by every schedule write: the configuration
   * commit silently refuses without a cached ClockProgramOverview, so poll it
   * first and reject (before any wire write) when it cannot be read.
   */
  void ensure_overview_(uint32_t seq, std::function<void()> next);

  // ---- Helpers
  static float to_native_units_(ControlMode mode, float display_value);
  static float setpoint_epsilon_(ControlMode mode);
  static bool is_scalar_mode_(ControlMode mode);
  static uint16_t setpoint_sub_id_(ControlMode mode);

  ControlService &control_;
  ScheduleService &schedule_service_;

  std::deque<Operation> queue_;
  uint32_t next_seq_{1};

  std::function<void(std::function<void()>, uint32_t)> schedule_callback_;
  std::function<bool()> ready_check_;
  std::function<void(const WriteResult &)> result_callback_;

  // Confirm/retry budgets (bench-derived starting points; see the plan).
  static constexpr uint32_t ENABLED_CONFIRM_DELAY_MS = 800;
  static constexpr uint32_t ENABLED_RETRY_DELAY_MS = 1000;
  static constexpr uint8_t ENABLED_MAX_ATTEMPTS = 3;
  static constexpr uint32_t MODE_CONFIRM_DELAY_MS = 1500;
  static constexpr uint32_t MODE_RETRY_DELAY_MS = 2000;
  static constexpr uint8_t MODE_MAX_ATTEMPTS = 4;  // matches issue #99's budget
  static constexpr uint32_t SETPOINT_STEP2_DELAY_MS = 400;
  static constexpr uint32_t SETPOINT_CONFIRM_DELAY_MS = 1200;  // the #82/#85 readback
  static constexpr uint32_t SETPOINT_RETRY_DELAY_MS = 1500;
  static constexpr uint8_t SETPOINT_MAX_ATTEMPTS = 2;
  static constexpr uint32_t CONFIG_STEP2_DELAY_MS = 400;
  static constexpr uint32_t CONFIG_CONFIRM_DELAY_MS = 1200;
  static constexpr uint32_t CONFIG_RETRY_DELAY_MS = 1500;
  static constexpr uint8_t CONFIG_MAX_ATTEMPTS = 1;
  static constexpr uint32_t WATCHDOG_SET_MODE_MS = 15000;
  static constexpr uint32_t WATCHDOG_DEFAULT_MS = 10000;
  // Schedule budgets: entry = poll + fresh layer read + write + commit +
  // settle + verify (+ retry) at 3-5 s per read; single events include a
  // possible full 35-slot cache scan when resolving a free slot.
  static constexpr uint32_t SCHED_SETTLE_DELAY_MS = 1500;  // flash two-phase commit; bench-tune
  static constexpr uint32_t SCHED_RETRY_DELAY_MS = 2000;
  static constexpr uint8_t SCHED_MAX_ATTEMPTS = 1;
  static constexpr uint32_t WATCHDOG_SCHED_ENTRY_MS = 20000;
  static constexpr uint32_t WATCHDOG_SCHED_ENABLED_MS = 12000;
  static constexpr uint32_t WATCHDOG_SINGLE_EVENT_MS = 60000;
  static constexpr uint32_t WATCHDOG_REFRESH_SCHEDULE_MS = 30000;
  static constexpr uint32_t WATCHDOG_REFRESH_EVENTS_MS = 120000;
  // Upload: overview + up to 5 x (read + write + commit) + settle +
  // readbacks + margin (RFC-005 §3.4).
  static constexpr uint32_t WATCHDOG_UPLOAD_MS = 150000;
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
