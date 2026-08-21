#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "control_service.h"
#include "schedule_codec.h"
#include "esphome/core/time.h"

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
  SET_REMOTE_MODE,
  SET_CLOCK,        // pump RTC sync; internal-only, no service, event-surface
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
// "ok"; a coupled op like set_pump_state reports its most-severe leg so an
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
  // Single events only: 0x01 = Stop (the pump held off across the window --
  // what a vacation is), 0x02 = a one-time Run. 0 means "not a single event",
  // which is what keeps the key off every other command's settle event.
  uint8_t single_event_action{0};
  std::string begin_hhmm;
  std::string end_hhmm;
  uint32_t begin_ts{0};
  uint32_t end_ts{0};
  int8_t sched_enabled{-1};
  int16_t event_count{-1};

  // SET_CLOCK: the pump's clock minus the node's, in seconds, measured by the
  // confirm readback AFTER the write. Positive = the pump runs ahead. NAN when
  // the readback never produced a usable time.
  float clock_offset_s{NAN};

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
class TimeService;

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
  WriteOperationService(ControlService &control, ScheduleService &schedule, TimeService &time);

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
  /**
   * Remote/Digital vs Local/Panel control source (issue #46). Confirmed from
   * the control_source byte of an Object 86 Sub 7 readback, not from the
   * command ACK -- see ControlService::send_remote_mode_command().
   */
  void submit_set_remote_mode(bool enabled, const std::string &op_id,
                              std::function<void(bool)> done = nullptr,
                              WriteOrigin origin = WriteOrigin::SERVICE);
  /**
   * Write the pump's real-time clock (Object 94 Sub 100) and confirm it by
   * reading Sub 101 back.
   *
   * The time is a PARAMETER, not something this layer reads from a clock of
   * its own: only the component owns `time_id`, and §8.4 wants the wire step
   * built from what the caller asked for. The confirm compares the readback
   * against `local_now` advanced by the milliseconds that have since elapsed,
   * so a slow confirm does not read as drift.
   *
   * ACCEPTED means the pump now holds the right time, which is the question
   * worth answering -- not "our frame is what put it there". A pump that was
   * already correct settles ACCEPTED, and so it should.
   */
  void submit_set_clock(const ESPTime &local_now, const std::string &op_id,
                        std::function<void(bool)> done = nullptr,
                        WriteOrigin origin = WriteOrigin::INTERNAL);
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
    // accepted/not). Used by the coupled set_pump_state composition so it can
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
    // Temperature-range values as the cache held them before the write, for
    // the same REJECTED-vs-CLAMPED distinction on SET_TEMPERATURE_RANGE. The
    // gate above the write (temp_limits_known()) guarantees an Obj 91 Sub 430
    // reply has landed, so these are the pump's own values rather than
    // ControlService's cold-start constants -- but they are cache-fresh, not
    // read-fresh, so a NAN here just falls through to CLAMPED, as pre_value
    // does.
    float pre_temp_min{NAN}, pre_temp_max{NAN};
    int8_t pre_autoadapt{-1};
    // The config write went out and nothing answered within its 3 s window.
    //
    // Not a verdict on its own (issue #234): the pump may have stored the
    // values and lost the acknowledgement -- a dropped notification, a
    // reassembly failure, a frame a moment late -- so the readback decides,
    // and this only survives into the settle detail so the silence is still
    // reported. See run_set_temperature_range_() for why the ACK cannot be
    // attributed to a particular write in the first place.
    bool config_unacked{false};
    // SET_REMOTE_MODE: ControlService::remote_source_observations_ as it stood
    // once the Class 3 command had been answered -- NOT when it was sent. The
    // send callback runs either on the ACK or on the ACK window closing, and
    // both are after the pump has had the command, so snapshotting there is
    // what makes a later observation evidence about the post-command pump
    // rather than about the moment before it. The confirm requires this to
    // have MOVED, not merely for the cached state to be valid -- see the
    // counter's declaration for why sticky validity is not enough.
    uint32_t pre_remote_observations{0};

    // SET_CLOCK: the local time written, and what the readback measured
    // against the node's clock afterwards.
    ESPTime clock_now{};
    // millis() when the wire step ran. Not used to project the written time
    // forward -- the confirm compares two clocks at one instant -- but to bound
    // how far the pump may legitimately lag us: the frame cannot have reached
    // it before this, so it cannot be behind by more than the operation's age.
    uint32_t clock_submitted_ms{0};
    float clock_offset_s{NAN};

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
    // clear_vacation clears EVERY enabled Stop event covering now, not just the
    // best-ranked one (issue #290). These carry that walk across the
    // write -> settle -> confirm cycle, which runs once per slot; finish_() is
    // still called exactly once, at the end, so the one-terminal-event contract
    // holds across a multi-slot write.
    std::vector<uint8_t> vacation_slots;
    uint8_t vacation_cursor{0};
    // Slots that were covering now but did not fit in MAX_VACATIONS_PER_CLEAR.
    // Reported in the settle detail so "cleared 8, 2 remain" is never mistaken
    // for "the vacation is over".
    uint8_t vacation_unhandled{0};
    // Set by the auto-slot resolver when the slot it picked still held an
    // enabled event that had already ended. Empty otherwise. Carried into the
    // ACCEPTED settle detail so recycling a slot is stated rather than
    // inferred from a slot number that changed (issue #262).
    std::string slot_note;

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

  /**
   * "cleared N of M vacations, the rest still cover now", or empty (issue #290).
   *
   * A multi-slot clear that stops part-way has already disabled some slots, and
   * the pump is still holding itself off under the ones it did not reach. Every
   * terminal path after the first slot completes has to say so -- not only the
   * mismatching-readback one, which is where the first cut of this put it.
   * A readback timeout, the watchdog and a disconnect all leave the same
   * half-done state, and all three used to report a generic failure with no hint
   * that anything had changed on the pump.
   */
  static std::string vacation_progress_note_(const Operation &op);
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
  void run_remote_mode_(uint32_t seq);
  void confirm_remote_mode_(uint32_t seq);
  void run_set_clock_(uint32_t seq);
  void confirm_set_clock_(uint32_t seq);
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

  ControlService &control_;
  ScheduleService &schedule_service_;
  TimeService &time_service_;

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
  // How long after the fused control request the setpoint readback goes out.
  //
  // 1600, and it used to be spelled 400 + 1200: a SETPOINT_STEP2_DELAY_MS wait
  // for a second "step 2" register write, then SETPOINT_CONFIRM_DELAY_MS from
  // there. That write is gone (issue #258) and with it the step it was named
  // after, so the two collapse into the one number that was always the real
  // one -- how long the pump gets to store the value before we read it back.
  // #82/#85 settled the total; #250 asks whether it should be larger, since the
  // Grundfos app waits 2500 ms after any write.
  static constexpr uint32_t SETPOINT_CONFIRM_DELAY_MS = 1600;
  static constexpr uint32_t SETPOINT_RETRY_DELAY_MS = 1500;
  static constexpr uint8_t SETPOINT_MAX_ATTEMPTS = 2;
  static constexpr uint32_t CONFIG_STEP2_DELAY_MS = 400;
  static constexpr uint32_t CONFIG_CONFIRM_DELAY_MS = 1200;
  static constexpr uint32_t CONFIG_RETRY_DELAY_MS = 1500;
  static constexpr uint8_t CONFIG_MAX_ATTEMPTS = 1;
  static constexpr uint32_t WATCHDOG_SET_MODE_MS = 15000;
  static constexpr uint32_t WATCHDOG_DEFAULT_MS = 10000;
  // The two config writes get their own budget, sized to the whole path they
  // can legitimately take -- both now open with a mandatory read of their
  // config object, and both settle on a confirm ladder with one retry:
  //
  //   5000 pre-write read + 400 mode-ACK wait + 400 step-2 delay
  //   + 3000 ACK window + 1200 confirm delay + 5000 readback timeout
  //   + 1500 retry delay + 5000 readback timeout = 21.5 s
  //
  // The mode-ACK wait (issue #248) is in that sum but costs nothing in practice:
  // it is equal to the step-2 delay, so an answered mode write frees the queue
  // long before step 2 is due and an unanswered one expires just as it becomes
  // due. The 5000s are APDU timeouts, so each is a bound rather than a cost; a
  // pump answering in the observed p50 of 54 ms takes well under 6 s end to end. The
  // budget has to cover the bound anyway, because the slow cases are exactly
  // the ones the retry exists for.
  //
  // On WATCHDOG_DEFAULT_MS the ladder could not finish: the watchdog fired at
  // 10 s, so CONFIG_MAX_ATTEMPTS never ran a second readback and the confirm's
  // own "readback failed" verdict was unreachable. That was survivable while a
  // missing ACK settled REJECTED at 3.4 s without any readback at all, and is
  // not now the readback decides every case (issue #234): a write the pump
  // stored but did not acknowledge, whose first readback is dropped, has to
  // reach the retry to be reported as the success it is -- otherwise the fix
  // just trades one wrong failure for another.
  static constexpr uint32_t WATCHDOG_CONFIG_WRITE_MS = 26000;
  // Schedule budgets: entry = poll + fresh layer read + write + commit +
  // settle + verify (+ retry) at 3-5 s per read; single events include a
  // possible full 35-slot cache scan when resolving a free slot.
  // How long after a schedule write before the confirm read is taken.
  //
  // MEASURED, on the pump, 2026-08-19. Until issue #253 every Object 84 write
  // waited out a full 3 s timeout for a reply the protocol cannot produce, and
  // this delay is scheduled from that callback -- so the interval that actually
  // shipped was 1500 + 3000, and nobody had established which part of it the
  // pump needed.
  //
  // A probe build set this to 100 ms with a 200 ms retry ladder and wrote to a
  // spare schedule layer. Four writes, set and clear, in both directions: the
  // FIRST confirm read matched every time. The pump makes an Object 84 write
  // visible to a read within 100 ms of acknowledging it -- write to settle was
  // 350-515 ms end to end.
  //
  // So 1500 is 15x a delay demonstrated to be sufficient, and it restores the
  // number this constant always claimed rather than the 4500 the broken timeout
  // was silently adding. With SCHED_RETRY_DELAY_MS behind it the ladder covers
  // 3500 ms before any REJECTED verdict, which is comfortably past the 2500 ms
  // the Grundfos GO app holds the bus quiet after a SET (issue #250).
  //
  // Scope of the measurement: taken on the layer image (Sub 1000+). The
  // overview/enable path (Sub 1) is the same object and a smaller write and is
  // assumed to behave the same -- assumed, not measured, which is why both
  // paths use this one constant rather than the overview getting a tighter one.
  // It says NOTHING about the Obj 91 config writes, whose own interval is
  // CONFIG_CONFIRM_DELAY_MS and is what #250 is actually about.
  static constexpr uint32_t SCHED_SETTLE_DELAY_MS = 1500;
  static constexpr uint32_t SCHED_RETRY_DELAY_MS = 2000;
  static constexpr uint8_t SCHED_MAX_ATTEMPTS = 1;
  static constexpr uint32_t WATCHDOG_SCHED_ENTRY_MS = 20000;
  static constexpr uint32_t WATCHDOG_SCHED_ENABLED_MS = 12000;
  // Same shape as SET_PUMP_ENABLED (Class 3 send + delayed readback with up
  // to ENABLED_MAX_ATTEMPTS retries), so it gets the same budget: the retry
  // ladder is 800 + 2x1000 ms of delay on top of the APDU timeouts.
  static constexpr uint32_t WATCHDOG_REMOTE_MODE_MS = 12000;
  // SET_CLOCK: the write's own acknowledgement window (awaited since issue
  // #253, and at most Transport::SET_ACK_TIMEOUT_MS = 400 ms -- the pump
  // answers this write in 38-90 ms across the captures, so the wait normally
  // costs a fraction of that), then the settle delay and, only while the
  // readback will not decode, up to CLOCK_MAX_ATTEMPTS retries. That is
  // CLOCK_MAX_ATTEMPTS + 1 = three reads at the 5 s timeout get_clock_async()
  // asks for: 400 + 1500 + 3 x (5000 + 1500) = 19900 ms with an idle transport.
  // WATCHDOG_SET_CLOCK_MS is 30 s, so the added wait is well inside budget.
  static constexpr uint32_t CLOCK_CONFIRM_DELAY_MS = 1500;
  static constexpr uint32_t CLOCK_RETRY_DELAY_MS = 1500;
  static constexpr uint8_t CLOCK_MAX_ATTEMPTS = 2;
  // How far the pump's clock may sit from the node's and still count as
  // synced. The pump stores whole seconds and the readback is a second BLE
  // round trip after the write, so a correct sync lands a second or two out as
  // a matter of course; 5 s leaves room for that without being able to hide a
  // write the pump ignored, which shows up as the accumulated drift that made
  // the sync due in the first place. Applied tightly when the pump reads AHEAD
  // and with the operation's own age added when it reads behind -- see the
  // confirm for why those two directions are not symmetric.
  static constexpr int32_t CLOCK_TOLERANCE_S = 5;
  // 30 s, not 25: the ladder above measures 19.56 s with the transport to
  // itself, and it does not have the transport to itself. Every readback queues
  // behind whatever telemetry or schedule read is already in flight, and at
  // 25 s the margin was under two blocked commands -- close enough that a
  // confirm which would have succeeded settles on the watchdog instead, which
  // reports TIMEOUT and waits 15 minutes.
  static constexpr uint32_t WATCHDOG_SET_CLOCK_MS = 30000;
  static constexpr uint32_t WATCHDOG_SINGLE_EVENT_MS = 60000;
  /**
   * Most vacations one `clear_vacation` will clear (issue #290).
   *
   * A policy cap, and stated as one rather than dressed up as a derived bound.
   * Its whole job is to make the watchdog budget below computable: the watchdog
   * cannot be re-armed once an operation is running (arming again adds a second
   * timer rather than replacing the first), so the budget has to be fixed before
   * the slots are resolved.
   *
   * It is not expected to bind. Only vacations that are live AT THE SAME INSTANT
   * can be multiple here, they all have to fit in the pump's single-event slots,
   * and this bench unit has five of those. If it ever does bind, the operation
   * clears what it can and the settle detail names how many are left, so a
   * second call finishes the job -- which is the honest failure for a cap,
   * rather than reporting the pump un-held while it is not.
   */
  static constexpr uint8_t MAX_VACATIONS_PER_CLEAR = 8;
  /**
   * Budget for a vacation clear, which may walk several slots.
   *
   * The expensive preamble -- the overview read and the full single-event read --
   * happens once for the whole operation; each additional slot costs only
   * write + settle + confirm readbacks, which is where the smaller per-slot
   * increment comes from. A backstop, not an expectation: the ordinary
   * one-vacation case still settles in seconds.
   */
  static constexpr uint32_t WATCHDOG_CLEAR_VACATION_MS =
      WATCHDOG_SINGLE_EVENT_MS + (MAX_VACATIONS_PER_CLEAR - 1) * 20000;
  static constexpr uint32_t WATCHDOG_REFRESH_SCHEDULE_MS = 30000;
  static constexpr uint32_t WATCHDOG_REFRESH_EVENTS_MS = 120000;
  // Upload: overview + up to 5 x (read + write + commit) + settle +
  // readbacks + margin.
  static constexpr uint32_t WATCHDOG_UPLOAD_MS = 150000;
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
