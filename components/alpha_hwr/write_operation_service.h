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
  UPLOAD_SCHEDULE,  // bulk full-state grid upload
  SET_PUMP_STATE,   // coupled run-state+schedule selector; composed at the api
                    // bridge from SET_PUMP_ENABLED + SET_SCHEDULE_ENABLED, so it
                    // is never enqueued as an Operation — only used to label its
                    // one aggregate settle event.
};

/**
 * Terminal status of a write operation. Every submitted operation ends in
 * exactly one of these, reported through the result callback.
 */
enum class WriteStatus : uint8_t {
  ACCEPTED,    // pump confirmed the requested value
  CLAMPED,     // pump stored a different value (reported in the result)
  REJECTED,    // the pump or its state refused: kept its old value, nacked
               // the command, or a required precondition was unreadable
  INVALID,     // the request itself is malformed or out of range; decided
               // before any wire write and deterministic (never worth a retry)
  TIMEOUT,     // no pump confirmation within the operation's budget
  SUPERSEDED,  // replaced by a newer queued write to the same resource
  PARTIAL,     // upload_schedule only: some layers confirmed, some failed
};

/** Where a write operation originated (reported in the settle event). */
enum class WriteOrigin : uint8_t {
  SERVICE,   // programmatic API call (op_id may still be empty)
  ENTITY,    // dashboard entity / helper button
  INTERNAL,  // autonomous self-repair by the component; nobody asked for it
             // (issue #124's dead-schedule reconciliation). Distinct so a
             // client watching write_settled can tell "the node fixed itself"
             // from a write it or a user actually made.
};

const char *write_command_to_string(WriteCommand cmd);
const char *write_status_to_string(WriteStatus status);

// Severity ordering used to fold several sub-write results into one terminal
// status (higher = worse / more informative to surface). ACCEPTED/CLAMPED are
// "ok"; a coupled op like pump_set_state reports its most-severe leg so an
// automation can still tell TIMEOUT from REJECTED from SUPERSEDED.
inline int write_status_severity(WriteStatus s) {
  switch (s) {
    case WriteStatus::ACCEPTED:   return 0;
    case WriteStatus::CLAMPED:    return 1;
    case WriteStatus::SUPERSEDED: return 2;
    case WriteStatus::TIMEOUT:    return 3;
    case WriteStatus::PARTIAL:    return 4;
    case WriteStatus::REJECTED:   return 5;
    case WriteStatus::INVALID:    return 6;
  }
  return 0;
}

/**
 * Terminal result of a write operation. Only the value fields relevant to
 * `command` are populated; the rest keep their "unknown" defaults. For
 * accepted/clamped/rejected the value fields carry the SETTLED values the
 * pump actually holds (from the confirm readback), not the request.
 */
struct WriteResult {
  std::string op_id;   // caller-supplied identifier (may be empty)
  WriteCommand command{WriteCommand::SET_PUMP_ENABLED};
  WriteStatus status{WriteStatus::REJECTED};
  std::string detail;  // short reason when relevant ("pump stored 1650", ...)
  WriteOrigin origin{WriteOrigin::SERVICE};  // service call vs entity write
  // Submission-order sequence number (monotonic per boot). Settle events for
  // superseded operations fire at submission time, i.e. before earlier
  // in-flight operations settle, so event arrival order is NOT submission
  // order; this field lets log analysis reconstruct it.
  uint32_t seq{0};

  ControlMode mode{ControlMode::NONE};
  float value{NAN};             // setpoint, in display units
  int8_t enabled{-1};           // -1 unknown, 0 off, 1 on
  float temp_min{NAN};
  float temp_max{NAN};
  int8_t autoadapt{-1};         // -1 unknown, 0 off, 1 on
  int16_t on_minutes{-1};
  int16_t off_minutes{-1};
  float flow{NAN};              // cycle-mode stored flow, m³/h (issue #107)

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

  // The originally requested values, echoed so the event is self-contained
  // for logging and retry decisions (review feedback on #92). Populated
  // alongside their settled counterparts; for accepted results they match.
  ControlMode requested_mode{ControlMode::NONE};
  float requested_value{NAN};
  float requested_temp_min{NAN};
  float requested_temp_max{NAN};
  int16_t requested_on_minutes{-1};
  int16_t requested_off_minutes{-1};
  float requested_flow{NAN};    // NAN when the request kept the stored flow
  std::string requested_begin_hhmm;
  std::string requested_end_hhmm;
};

/**
 * Does this settled write require republishing the schedule display — the
 * canonical hash sensor plus the five per-layer read-back sensors?
 *
 * Pure and header-resident so the host test drives the production rule rather
 * than a copy of it; the component's result callback is the only caller.
 *
 * UPLOAD_SCHEDULE is keyed on the post-op hash being present rather than on the
 * terminal status (issue #133). An upload is five independent layer writes, so
 * a run that fails partway has still moved the device grid, and the sensor has
 * to track the device rather than the verdict.
 *
 * `schedule_hash` is populated once the layer loop has run — whether each layer
 * was written, skipped as already-matching, or failed confirm. What those three
 * have in common is a readback, which refreshes the cache from the device, so
 * the hash describes the pump in all of them. It is empty only when the upload
 * was rejected before the first layer, where nothing was read and the cache is
 * untouched. That is the same condition the write_settled event uses, so the
 * sensor and the event cannot disagree.
 */
inline bool result_republishes_schedule(const WriteResult &r) {
  const bool applied =
      r.status == WriteStatus::ACCEPTED || r.status == WriteStatus::CLAMPED;
  switch (r.command) {
    case WriteCommand::SET_SCHEDULE_ENTRY:
    case WriteCommand::CLEAR_SCHEDULE_ENTRY:
    case WriteCommand::SET_SCHEDULE_ENABLED:
    case WriteCommand::REFRESH_SCHEDULE:
      return applied;
    case WriteCommand::UPLOAD_SCHEDULE:
      return !r.schedule_hash.empty();
    default:
      return false;
  }
}

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
                          std::function<void(bool)> done = nullptr,
                          WriteOrigin origin = WriteOrigin::SERVICE,
                          std::function<void(WriteStatus)> on_status = nullptr);
  void submit_set_mode(ControlMode mode, const std::string &op_id,
                       std::function<void(bool)> done = nullptr,
                       WriteOrigin origin = WriteOrigin::SERVICE);
  void submit_set_setpoint(ControlMode mode, float value, const std::string &op_id,
                           std::function<void(bool)> done = nullptr,
                           WriteOrigin origin = WriteOrigin::SERVICE);
  void submit_set_temperature_range(float min_c, float max_c, bool autoadapt,
                                    const std::string &op_id,
                                    std::function<void(bool)> done = nullptr,
                                    WriteOrigin origin = WriteOrigin::SERVICE);
  // Each field has a keep-existing sentinel resolved from the mandatory
  // fresh Sub 421 read: 0 for the minute fields, 0/NAN for flow (m³/h,
  // 0.1-10.0 when asserted; issue #107). All three kept settles `invalid`.
  void submit_set_cycle_times(uint8_t on_minutes, uint8_t off_minutes, float flow,
                              const std::string &op_id,
                              std::function<void(bool)> done = nullptr,
                              WriteOrigin origin = WriteOrigin::SERVICE);

  // ---- Schedule writes (same contract; verify readbacks are new — no
  // schedule write had an honest success signal before this layer).
  void submit_set_schedule_entry(uint8_t layer, uint8_t day_index, uint8_t begin_hour,
                                 uint8_t begin_minute, uint8_t end_hour, uint8_t end_minute,
                                 const std::string &op_id,
                                 std::function<void(bool)> done = nullptr,
                                 WriteOrigin origin = WriteOrigin::SERVICE);
  void submit_clear_schedule_entry(uint8_t layer, uint8_t day_index, const std::string &op_id,
                                   std::function<void(bool)> done = nullptr,
                                   WriteOrigin origin = WriteOrigin::SERVICE);
  void submit_set_schedule_enabled(bool enabled, const std::string &op_id,
                                   std::function<void(bool)> done = nullptr,
                                   WriteOrigin origin = WriteOrigin::SERVICE,
                                   std::function<void(WriteStatus)> on_status = nullptr);
  /** slot < 0 auto-resolves the first free slot; the chosen slot is echoed in the result.
   *  action: 0x02 = Auto (one-time run), 0x01 = Stop (vacation / pump-off period). */
  void submit_set_single_event(uint32_t begin_ts, uint32_t end_ts, const std::string &op_id,
                               std::function<void(bool)> done = nullptr, int slot = -1,
                               WriteOrigin origin = WriteOrigin::SERVICE,
                               uint8_t action = 0x02);
  void submit_clear_single_event(uint8_t slot, const std::string &op_id,
                                 std::function<void(bool)> done = nullptr,
                                 WriteOrigin origin = WriteOrigin::SERVICE);
  /** Vacation = a multi-day Stop single-event overriding the weekly schedule. */
  void submit_set_vacation(uint32_t begin_ts, uint32_t end_ts, const std::string &op_id,
                           std::function<void(bool)> done = nullptr,
                           WriteOrigin origin = WriteOrigin::SERVICE);
  /** Clears the active vacation (auto-resolves the Stop single-event slot). */
  void submit_clear_vacation(const std::string &op_id,
                             std::function<void(bool)> done = nullptr,
                             WriteOrigin origin = WriteOrigin::SERVICE);
  // Reads, but they contend for the same transport, so they run through the
  // same operation queue and get the same terminal-event guarantee.
  void submit_refresh_schedule(const std::string &op_id,
                               std::function<void(bool)> done = nullptr,
                               WriteOrigin origin = WriteOrigin::SERVICE);
  void submit_refresh_single_events(const std::string &op_id,
                                    std::function<void(bool)> done = nullptr,
                                    WriteOrigin origin = WriteOrigin::SERVICE);
  /**
   * Bulk full-state schedule upload. The request expresses the
   * entire 7x5 grid; layers whose fresh readback already matches the
   * desired image are skipped (no BLE write). Terminates ACCEPTED (all
   * layers confirmed, possibly all skipped), PARTIAL (mixed), REJECTED,
   * TIMEOUT or SUPERSEDED — always exactly one terminal event.
   */
  void submit_upload_schedule(codec::UploadRequest request,
                              const std::string &op_id,
                              std::function<void(bool)> done = nullptr,
                              WriteOrigin origin = WriteOrigin::SERVICE);

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
    WriteOrigin origin{WriteOrigin::SERVICE};
    Phase phase{Phase::QUEUED};
    uint8_t attempts{0};
    std::function<void(bool)> done;
    // Like `done` but carries the full terminal WriteStatus (not just
    // accepted/not). Used by the coupled pump_set_state composition so it can
    // report TIMEOUT / SUPERSEDED / REJECTED distinctly instead of flattening
    // every failure to one bool.
    std::function<void(WriteStatus)> status_done;

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
    float flow{NAN};  // cycle flow, m³/h; NAN = keep existing (issue #107)
    // Setpoint value before the write (display units): distinguishes
    // "pump kept its old value" (REJECTED) from "pump stored a different
    // value" (CLAMPED) in the confirm comparison.
    float pre_value{NAN};
    // Cycle-config values from the mandatory pre-write read: resolve the
    // kept (0-sentinel) fields and drive the same REJECTED-vs-CLAMPED
    // distinction for SET_CYCLE_TIMES.
    int8_t pre_on_minutes{-1}, pre_off_minutes{-1};
    float pre_flow{NAN};

    // Schedule fields
    uint8_t layer{0};
    uint8_t day_index{0};
    uint8_t begin_hour{0}, begin_minute{0}, end_hour{0}, end_minute{0};
    int16_t slot{-1};
    uint32_t begin_ts{0}, end_ts{0};
    int16_t event_count{-1};
    // Single-event action (SchedulingActionType): 0x02 = Auto (one-time run),
    // 0x01 = Stop (vacation / pump-off period). clear_by_vacation makes a
    // CLEAR_SINGLE_EVENT auto-resolve to the active Stop (vacation) slot.
    uint8_t single_event_action{0x02};
    bool clear_by_vacation{false};

    // UPLOAD_SCHEDULE fields
    codec::UploadRequest upload;
    uint8_t upload_layer{0};        // cursor for the layer loop / confirms
    uint8_t upload_written_mask{0};
    uint8_t upload_skipped_mask{0};
    uint8_t upload_failed_mask{0};
    // The enabled state the caller asked for, preserved across the confirm
    // readback. upload.enabled is overwritten with what the pump actually
    // holds so the settle event reports the truth; without a separate copy of
    // the request there is nothing left to compare it against, and a dropped
    // enable write settles ACCEPTED.
    int8_t upload_enabled_requested{-1};
    // Set when the readback disagreed with the request, or could not be read.
    bool upload_enabled_mismatch{false};
    bool upload_enabled_unreadable{false};

    // Pristine copies of the request, captured at submit and never
    // overwritten, so the settle event can echo what was asked for.
    ControlMode req_mode{ControlMode::NONE};
    float req_value{NAN};
    float req_temp_min{NAN};
    float req_temp_max{NAN};
    uint8_t req_on_minutes{0}, req_off_minutes{0};
    float req_flow{NAN};  // NAN = request kept the stored flow
    uint8_t req_begin_hour{0}, req_begin_minute{0}, req_end_hour{0}, req_end_minute{0};
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
  // readbacks + margin.
  static constexpr uint32_t WATCHDOG_UPLOAD_MS = 150000;
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
