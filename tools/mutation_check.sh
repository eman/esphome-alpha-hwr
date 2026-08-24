#!/usr/bin/env bash
# ==============================================================================
# Mutation check: does the test suite actually test the shipped code?
# ==============================================================================
#
# A test that validates a *copy* of production logic passes no matter what the
# firmware does. That is not hypothetical here: corrupting codec.cpp's CRC
# initial value and swapping the CRC bytes in frame_builder.cpp -- a total
# protocol break where every frame the device emits carries a wrong checksum --
# once left `make test` reporting 21/21 with byte-identical output, because
# tests/protocol.h carried its own copy of both.
#
# This script breaks production code on purpose and asserts the suite notices.
# Each mutation is applied on its own, the suite is run, and the mutation is
# reverted. A mutation the suite does not catch is a coverage hole and fails
# this check.
#
# Usage:
#   ./tools/mutation_check.sh              # run every mutation
#   ./tools/mutation_check.sh --list       # just show them
#   ./tools/mutation_check.sh --verify     # do they all still point at real code?
#   ./tools/mutation_check.sh continuation # only names containing "continuation"
#   JOBS=8 ./tools/mutation_check.sh       # more parallel build jobs (default 4)
#   SCOPED=0 ./tools/mutation_check.sh     # rebuild/run everything per mutation
#
# The filter exists because a full sweep rebuilds and re-runs the whole suite
# once per mutation and takes the better part of an hour. Use it while adding
# or repairing entries; the unfiltered run is what CI and a merge want, and the
# summary says loudly when a filter was in force so a partial run cannot be
# mistaken for a clean sweep.
#
# ==============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_DIR="$PROJECT_DIR/tests"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# Parallel build jobs. Each mutation does a full clean rebuild of all 22 test
# binaries and that build, not the mutation, is where the wall-clock goes: the
# `test` target lists them as prerequisites and only *runs* them in its recipe,
# so -j parallelises compilation while leaving execution serial and ordered.
# Override with JOBS=1 to serialise if a build ever looks order-dependent.
JOBS="${JOBS:-4}"

# Wall-clock limit on a single suite run, in seconds, and the exit status used
# to report one that blew through it (124, the value GNU timeout uses).
#
# Why this exists (issue #237). A mutation that makes a test loop forever is
# neither caught nor survived: the run never returns an answer. Before this the
# sweep simply stopped advancing -- no output, because progress is printed per
# entry; the script itself at 0% CPU with a test binary at 99%; and nothing
# anywhere naming which entry was in force, because the mutated file is restored
# per entry and the name lives only in this script's memory. It reads exactly
# like a slow sweep. One such mutation cost 34 minutes of silence before anyone
# noticed the log had stopped growing.
#
# It is a class rather than one bad entry: any "the thing never fires" mutation,
# against any test that drives to a condition instead of a fixed step count,
# produces it. Drive-to-condition is the established pattern here -- see
# run_until_ready() in tests/test_component_wiring.cpp, whose comment explains
# why the fixed-window alternative was wrong twice -- so the vulnerable shape is
# the one the tests are supposed to use.
#
# The limit is deliberately enormous rather than tight. A full `make test` runs
# in about 7 s on a developer machine and the slowest single binary in about 2,
# so 300 s is a factor of forty. That asymmetry is on purpose: a false HUNG
# would blame a timeout for someone's correct change and send them debugging the
# wrong thing, while a real hang caught late merely costs five minutes instead
# of the rest of the day. Override for a slow machine with
# `TEST_TIMEOUT=600 ./tools/mutation_check.sh`.
TEST_TIMEOUT="${TEST_TIMEOUT:-300}"
# The baseline builds every binary from a cleared object cache before running
# them, so it gets its own much larger budget. Bounded for the same reason:
# an unbounded loop in an UNMUTATED test hangs here, before a single mutation is
# applied, with even less to go on than the per-entry case.
BASELINE_TIMEOUT="${BASELINE_TIMEOUT:-1800}"
TIMEOUT_EXIT=124

# Run a command with a wall-clock limit, returning its status, or TIMEOUT_EXIT
# if it had to be killed.
#
# Hand-rolled because `timeout` is GNU coreutils and macOS does not ship it --
# development here is on darwin and CI is ubuntu, and a limit that engaged only
# in CI would not have caught the case that prompted this.
#
# `set -m` around the launch is the part that matters and the part that is easy
# to leave out. It puts the background job in its own process group, so the kill
# below can signal the group. Without it, killing the job kills the subshell
# while the test binary it launched survives as an orphan, still spinning --
# which is exactly what had to be cleaned up by hand when this was first hit.
# TERM first so a test can die normally, then KILL for one that cannot.
#
# Polls rather than racing a watchdog against `wait`, because distinguishing
# "killed by the watchdog" from "exited 143 on its own" would need a flag file
# and this does not. 200 ms granularity keeps the rounding overhead near a tenth
# of a second per run, against a sweep where each entry costs seconds to build.
run_bounded() {
  local limit=$1; shift
  local monitor_was_on=0
  case "$-" in *m*) monitor_was_on=1 ;; esac
  set -m
  "$@" &
  local pid=$!
  [ "$monitor_was_on" = "1" ] || set +m

  local ticks=0
  local limit_ticks=$((limit * 5))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$ticks" -ge "$limit_ticks" ]; then
      kill -TERM -"$pid" 2>/dev/null || kill -TERM "$pid" 2>/dev/null
      sleep 2
      kill -KILL -"$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      return "$TIMEOUT_EXIT"
    fi
    sleep 0.2
    ticks=$((ticks + 1))
  done
  wait "$pid"
  return $?
}

# The three things run_bounded is pointed at, as functions rather than inline
# compound commands, because a backgrounded function is what it can launch.
run_one_test() { cd "$TESTS_DIR" && ./"$1" >/dev/null 2>&1; }
run_whole_suite() { cd "$TESTS_DIR" && make test >/dev/null 2>&1; }

# Each mutation: name | file | python-repr search | python-repr replace
# The search string must appear exactly once; the script fails loudly if not,
# so a refactor that moves the code is reported rather than silently skipped.
#
# Also deliberately absent: deleting the `if (current_ms == 0) return 0;` guard
# from link_data_timeout_next(). It is an equivalent mutant, verified by
# experiment after this check flagged it as surviving: with current_ms == 0 the
# remaining arithmetic already returns 0 (0 >= cap is false, doubled is 0, and
# 0 > cap is false), so removing the guard changes no output for any cap. The
# guard is kept because it states the intent -- a disabled watchdog stays
# disabled -- and because it keeps that property true independently of the
# arithmetic below it, which a later edit could change. Listing it here would
# be an uncatchable entry rather than a coverage gap.
MUTATIONS=(
"crc-initial-value|components/alpha_hwr/codec.cpp|uint16_t crc = init;|uint16_t crc = init ^ 0x0001;"
"crc-byte-order|components/alpha_hwr/frame_builder.cpp|packet_out[9] = (crc >> 8) & 0xFF;|packet_out[9] = crc & 0xFF;"
"class-match-equality|components/alpha_hwr/response_match.h|if (incoming_class != queued_class) return false;|if (false) return false;"
"frame-length-guard|components/alpha_hwr/frame_parser.cpp|if (len < expected_total) {|if (false) {"
"schedule-day-bound|components/alpha_hwr/schedule_codec.cpp|if (v[1] > 6) return fail|if (v[1] > 99) return fail"
# Restores the DST readback defect: resolving the offset from the local value
# instead of from an approximate UTC. That is the single-line difference between
# a seven-to-eight-hour error window at both transitions and a one-hour residual
# at one of them -- and the visible symptom was a write settling REJECTED while
# the pump held exactly the right value.
"dst-offset-resolved-from-local|components/alpha_hwr/schedule_service.h|  const int32_t refined = local_utc_offset_seconds(\n      static_cast<time_t>(static_cast<int64_t>(local) - approx));\n  return local_unix_to_utc(local, refined);|  return local_unix_to_utc(local, approx);"
# DELETED, not lost: dst-offset-ignores-sub-hour-zones and
# dst-offset-year-rollover-branch. They mutated the minutes term and the
# year-rollover branch of local_utc_offset_seconds()'s hand-rolled offset
# arithmetic, and issue #289 deleted that arithmetic -- the function now asks
# ESPTime, which is the only implementation that is right on the ESP32 as well
# as the host. There is no line left to mutate.
#
# The PROPERTIES they proved are still tested, and by the same assertions as
# before: test_schedule_service's ACST fixture (a +9:30 zone, so a whole-hour
# implementation fails it) and its New Year fixture (local and UTC in different
# calendar years). Those tests did not change; only what they exercise did.
# Recorded here because --verify checks entries against code and never code
# against entries, so a deletion is invisible to it (see the note at the top).
"ignore-unrelated-gate|components/alpha_hwr/response_match.h|  if (!is_wildcard_matched_class(queued_class)) return false;\n  return !is_wildcard_matched_class(incoming_class);|  return false;"
# The wildcard-matched class set (issue #174). Membership is the whole safety
# argument for admitting classes 2, 5 and 11, so both directions are pinned:
# dropping a member silently stops matching that class's replies and sends the
# sequence back to advancing on nothing, and admitting Class 10 lets an
# unsolicited telemetry notification answer any queued Class 10 command. Written
# without `||` on purpose -- this script splits its entries on `|`, so a search
# or replacement string containing one is truncated and the mutation never runs.
"class-set-drop-class2|components/alpha_hwr/response_match.h|  if (class_byte == CLASS_2_MEASURED_DATA) return true;|  if (false) return true;"
"class-set-drop-class5|components/alpha_hwr/response_match.h|  if (class_byte == CLASS_5_REFERENCE_VALUES) return true;|  if (false) return true;"
"class-set-admit-class10|components/alpha_hwr/response_match.h|  return class_byte == CLASS_11_MEASURED_16BIT;|  return class_byte == CLASS_11_MEASURED_16BIT ? true : (class_byte == 0x0A);"
"remote-mode-confirm-fresh|components/alpha_hwr/write_operation_service.cpp|bool confirmed = success && fresh && control_.remote_state_valid_ &&|bool confirmed = success && control_.remote_state_valid_ &&"
# Restores the exact naming defect issue #159 reported: the service was called
# `pump_set_state`, its settle event answered `set_pump_state`. Since api_bridge
# now registers each service *by* its command string, this one mutation renames
# a Home Assistant service and the event field together -- a silent break of two
# public surfaces. It went unnoticed for as long as it did precisely because no
# test asserted either spelling.
"command-string-service-name-drift|components/alpha_hwr/write_operation_service.cpp|    case WriteCommand::SET_PUMP_STATE:        return \"set_pump_state\";|    case WriteCommand::SET_PUMP_STATE:        return \"pump_set_state\";"
# A Class 10 request is addressed OBJECT FIRST -- [Obj][SubH][SubL] -- in all 20
# distinct address shapes across the 420 SETs in resources/traffic_capture. Lay
# it out sub-first and the pump answers Unknown Data Item, quoting the first
# payload byte back. That is not hypothetical: a "dedicated" setpoint write did
# exactly this for as long as it existed, and nobody noticed because the send was
# fire-and-forget so the refusal was never read (issue #258).
#
# The simulator used to accept any address it recognised the OpSpec for, which is
# why the old write looked healthy from the host suite. It now answers an
# unrecognised Class 10 SET the way the pump does, and main() fails the run if
# any test provoked one. This entry is the proof that net works: reversing the
# fused control request's address turns it red.
"control-request-addressed-sub-first|components/alpha_hwr/control_service.cpp|  apdu[2] = 0x56;  // Object id 86, the start/stop request\n  apdu[3] = 0x00;  // Sub-id high\n  apdu[4] = 0x06;  // Sub-id low -- 86/6, overall_operation_local_request_obj|  apdu[2] = 0x00;  // mutated: the sub-first layout the deleted write used\n  apdu[3] = 0x06;\n  apdu[4] = 0x00;"
# The pump's flow limiters (issue #274). The limiter constrains the pump BELOW
# what it was asked for while every signal we publish says the write worked --
# because it did. Measured with MaxFlow at 1.6 gpm: 3000 RPM commanded, 1883
# delivered, settled `accepted`, with the pump reporting 3000 back from its own
# 86/7. Every fixture in tests/test_limiter.cpp is a real frame.
"limiter-enable-byte-ignored|components/alpha_hwr/limiter.h|  c.enabled = body[1] != 0;|  c.enabled = true;"
"limiter-status-byte-ignored|components/alpha_hwr/limiter.h|  s.limiting = body[1] != 0;|  s.limiting = false;"
# "Not read" must stay distinguishable from "no limiter". Reporting an all-clear
# for a pump we could not ask is the same false reassurance the issue is about.
"limiter-unread-reads-as-all-clear|components/alpha_hwr/limiter.h|  bool known() const {\n    return max_flow.valid |  bool known() const {\n    return true; //"
# An enabled limiter that is not biting yet is its own state: it starts biting
# the moment the setpoint rises, and a user told "no limiter" will not
# understand the clamp when it arrives.
"limiter-enabled-but-idle-reported-as-clear|components/alpha_hwr/limiter.h|  if (s.any_enabled()) {|  if (false) {"
# The config record is 18 bytes and the status 6. Reading a short one past its
# end is the memory-safety half.
"limiter-config-short-frame-decoded|components/alpha_hwr/limiter.h|  const bool long_enough = len >= LIMITER_CONFIG_BODY_LEN;|  const bool long_enough = true;"
"limiter-status-short-frame-decoded|components/alpha_hwr/limiter.h|  const bool long_enough = len >= LIMITER_STATUS_BODY_LEN;|  const bool long_enough = true;"
# Each record is published as it lands rather than when the five-read chain
# finishes: a reconnect mid-chain resets the transport and drops the rest, which
# left the entities empty even though most of the family had been read.
"limiter-published-only-on-chain-completion|components/alpha_hwr/control_service.cpp|        } else if (on_limiter_update_) {|        } else if (false) {"
# ...which is exactly why an all-clear needs BOTH configuration records. One
# enabled limiter is enough to say "a limiter is enabled"; saying "no limiter is
# enabled" is a claim about both, and a link that drops between 86/600 and
# 86/601 must not produce it.
"limiter-half-read-config-reads-as-all-clear|components/alpha_hwr/limiter.h|  if (!s.config_complete())\n    return \"unknown\";|  if (false)\n    return \"unknown\";"
# The read chains stop at the first failure. A reply carries no request
# identifier and the records within each family share a type code, so a late
# reply to a timed-out read satisfies the NEXT request -- caching MaxFlow's cap
# as MinFlow's.
"limiter-config-chain-continues-past-a-failure|components/alpha_hwr/control_service.cpp|      ESP_LOGD(TAG, \"Limiter config chain stopped at 86/600\");\n      limiters_reading_ = false;\n      if (callback) callback(false);\n      return;|      ESP_LOGD(TAG, \"Limiter config chain stopped at 86/600\");\n      limiters_reading_ = false;"
"limiter-status-chain-continues-past-a-failure|components/alpha_hwr/control_service.cpp|      ESP_LOGD(TAG, \"Limiter status chain stopped at 86/640\");\n      limiters_reading_ = false;\n      if (callback) callback(false);\n      return;|      ESP_LOGD(TAG, \"Limiter status chain stopped at 86/640\");\n      limiters_reading_ = false;"
# One limiter chain at a time. The connect read is five requests and the control
# poll starts another three; without the guard the two overlap, and every record
# within a family shares a type code, so the poll's reply satisfies whichever
# request is at the head of the queue. Found by a host test asserting all five
# addresses are read on a healthy pump -- they were not, the poll's chain cut
# the connect chain off after its third read, every time.
"limiter-overlapping-chains-allowed|components/alpha_hwr/control_service.cpp|  if (limiters_reading_) {\n    ESP_LOGD(TAG, \"Limiter read already in flight; skipping this poll\");|  if (false) {\n    ESP_LOGD(TAG, \"Limiter read already in flight; skipping this poll\");"

# The family belongs to the pump we were talking to. Left standing across a
# disconnect the entities reported the previous connection's caps indefinitely.
"limiter-state-survives-a-disconnect|components/alpha_hwr/control_service.h|     limiters_ = LimiterState{};|     // mutated: the previous pump's limiters stay"

# The pump's published range EXPLAINS a clamp; it does not gate the write
# (issue #276). #273/#275 made it a gate, against the pump's own type-301
# min_set_point / max_set_point. The bounds were right and the gate was still
# wrong: with a flow limiter enabled there is no maximum speed -- the pump takes
# the setpoint and manages run speed to hold the flow bound, and where it lands
# is a property of the loop's hydraulics. Measured, constant speed with MaxFlow
# at 1.6 gpm: 3000 commanded, 1883 delivered. 1883 is not in the type-301 range,
# not in the limiter record, and not anywhere else.
#
# So the range survives only in the settle detail, and only when the pump is the
# source -- the constants this code used to fall back on were wrong in both
# directions on all four modes, and printing one as though the pump had said it
# turns an explanation into a fabrication.
"setpoint-clamp-detail-drops-the-range|components/alpha_hwr/write_operation_service.cpp|      if (control_.get_setpoint_range(op->mode, pump_lo, pump_hi)) {|      if (false) {"
"setpoint-clamp-detail-quotes-a-range-never-read|components/alpha_hwr/write_operation_service.cpp|      if (control_.get_setpoint_range(op->mode, pump_lo, pump_hi)) {|      if (true) {"
# A NaN is still refused before the wire, and not for a reason about range: the
# all-ones float doubles as the SETPOINT_KEEP sentinel, so a NaN reads as "leave
# the setpoint alone" -- a write that silently does nothing rather than fails.
"setpoint-nan-reaches-the-wire|components/alpha_hwr/write_operation_service.cpp|  if (std::isnan(op->value)) {|  if (false) {"
# The range read must reject a degenerate answer rather than caching it, so that
# setpoint_ranges_known() does not claim a complete set off the back of one.
# Note what this does NOT do: get_setpoint_range() re-checks max > min on every
# call, so a cached degenerate range would not actually bound anything -- the
# write layer would fall back and accept. The two checks are belt and braces and
# only the completeness flag can tell them apart, which is why exactly one
# assertion catches this.
"setpoint-range-accepts-an-inverted-range|components/alpha_hwr/control_service.cpp|        const bool usable = !std::isnan(lo) && !std::isnan(hi) && hi > lo;|        const bool usable = true;"
# The ranges belong to the pump on the other end. A reconnect may be a different
# pump, and a stale range would bound the new one.
"setpoint-ranges-survive-a-disconnect|components/alpha_hwr/control_service.h|     setpoint_ranges_valid_ = false;|     setpoint_ranges_valid_ = setpoint_ranges_valid_;"
# The range chain must stop at the first failure. All four objects are type 301
# version 1, so all four reads declare the same expectation and the transport --
# which matches on object TYPE and never on the instance -- cannot tell their
# replies apart. Carrying on after a timeout hands read N's late reply to read
# N+1 and shifts every remaining range by one slot: constant pressure ends up
# bounded by constant speed's 1650-3671 read as Pascals, and a 1.5 m setpoint is
# refused as INVALID blaming the pump, for the rest of the connection.
# The pump reports flow in m3/s and the range is kept in m3/h. Dropping the
# conversion leaves constant flow bounded by 3.2e-05 to 6.9e-04 m3/h, which
# refuses every realistic setpoint -- a total loss of the mode, and one that
# nothing in the suite could see until constant flow got a range assertion of
# its own. The pressure conversion was covered from the start; this one was not.
# Deliberate absence, as of issue #259: the in-flight guard released in
# invalidate_cache() is now an EQUIVALENT MUTANT, and the entry that covered it
# has been removed rather than left to survive.
#
# It was written for issue #273 and it was load-bearing then. The defect: the
# range chain sets a flag so a second chain cannot start, and only `finish`
# cleared it -- but Transport::reset() dropped a queued command WITHOUT invoking
# its callback, so a link drop mid-chain killed the chain silently. The guard
# stayed set, every later read answered "already in flight", and every setpoint
# write was permanently back on the fallback constants for the life of the node,
# visible only as a DEBUG line. One ordinary BLE drop inside a ~200 ms window.
#
# #259 removed the mechanism. reset() now fails what it abandons, so the chain's
# own callback runs `finish` and releases the flag on every path that can strand
# it. Verified in two steps rather than assumed: with the guard deleted the suite
# passes (619/619), and with the guard deleted AND reset() put back to
# command_queue_.clear() the same three assertions fail again. There is no
# reachable input left that distinguishes the line.
#
# The line stays. It is one assignment, it is correct, and the failure it guards
# is silent and permanent -- deleting correct defensive code because a test can
# no longer see it is the wrong trade. What goes is the entry that claimed to
# prove it.
"setpoint-range-flow-conversion-dropped|components/alpha_hwr/control_service.cpp|      return native * 3600.0f;   // m³/s -> m³/h|      return native;"
"setpoint-range-chain-continues-past-a-failure|components/alpha_hwr/control_service.cpp|    if (!a) { finish(false); return; }|    (void) a;"
# The setpoint readback waits SETPOINT_CONFIRM_DELAY_MS after the write so the
# pump has time to store the value (#82/#85). That delay used to be unfalsifiable:
# the simulator applied a setpoint the instant the frame arrived, so a confirm at
# 1600 ms, at 1200 or at 0 all passed, and the whole rationale survived only as a
# comment. PumpSim::setpoint_apply_delay_ms models a pump that needs a moment,
# which is what turns the delay into a claim. Two ways to break it: read back too
# early by the amount the deleted step-2 write used to cost, and read back
# immediately.
"setpoint-confirm-does-not-wait|components/alpha_hwr/write_operation_service.cpp|    schedule_([this, seq]() { confirm_setpoint_(seq); }, SETPOINT_CONFIRM_DELAY_MS);|    schedule_([this, seq]() { confirm_setpoint_(seq); }, 0);"
"setpoint-confirm-loses-the-step2-lead|components/alpha_hwr/write_operation_service.h|  static constexpr uint32_t SETPOINT_CONFIRM_DELAY_MS = 1600;|  static constexpr uint32_t SETPOINT_CONFIRM_DELAY_MS = 1200;"
# The single-event slot bound is the last stop before a caller's index becomes
# SubID 900+idx on the wire. Two ways to get it wrong, and the pair is the point:
# hardcoding 35 still rejects absurd values while quietly accepting slot 10 on a
# 5-slot pump, and < vs <= lets slot == max_events through. Neither shows up
# against a 35-slot simulator, which is why the tests model a 5-slot one.
#
# The predicate is hoisted into `slot_in_range` in production so these two have
# a pipe-free line to anchor to: the entries are split with IFS='|', so a search
# string containing `||` is truncated and the mutation is scored as though the
# code were untouched.
"single-event-slot-bound-hardcoded|components/alpha_hwr/write_operation_service.cpp|  const bool slot_in_range = op->slot >= 0 && op->slot < max_events;|  const bool slot_in_range = op->slot >= 0 && op->slot < 35;"
"single-event-slot-bound-off-by-one|components/alpha_hwr/write_operation_service.cpp|  const bool slot_in_range = op->slot >= 0 && op->slot < max_events;|  const bool slot_in_range = op->slot >= 0 && op->slot <= max_events;"
# The protocol envelope is a separate check from the device bound, and its
# ORDERING is the behaviour: it runs before ensure_overview_() so that a slot
# wrong on every pump is named as such even when the link cannot answer. Moving
# it after leaves the device bound still catching the slot -- the operation is
# still refused -- so only the settle status and detail change. That is exactly
# the regression worth pinning: the user stops being told which of the two
# things is broken.
"single-event-envelope-after-overview|components/alpha_hwr/write_operation_service.cpp|  if (op->slot >= static_cast<int16_t>(services::SINGLE_EVENT_SLOT_LIMIT)) {|  if (false && op->slot >= static_cast<int16_t>(services::SINGLE_EVENT_SLOT_LIMIT)) {"
# CLEAR_SCHEDULE_ENTRY, whose four branches had no test at all until the §9
# step 6 gap was closed. SET and CLEAR share run_schedule_entry_() and
# confirm_schedule_entry_(), so the parts only CLEAR reaches are exactly the
# parts that were unexercised: the blank entry it composes instead of the
# requested one, and the enabled-flag-only comparison its confirm makes.
"clear-entry-writes-an-enabled-day|components/alpha_hwr/write_operation_service.cpp|          entry.set_enabled(false);|          entry.set_enabled(true);"
"clear-entry-confirm-wants-the-day-on|components/alpha_hwr/write_operation_service.cpp|      bool want_enabled = op->command == WriteCommand::SET_SCHEDULE_ENTRY;|      bool want_enabled = true;"
# A separate branch from the one above, and it needed a test of its own. The
# entry-times comparison is SKIPPED for a clear; with a pump that zeroes the
# payload when it disables a day -- which the simulator did, and which nothing
# in the protocol requires -- op->begin/end and the readback are both 0, the
# extra comparison is trivially true, and deleting the skip changes nothing.
# The fixture now models a pump that keeps the old times in a disabled cell,
# which is the case the skip exists for.
"clear-entry-confirm-compares-a-cleared-days-times|components/alpha_hwr/write_operation_service.cpp|      const bool times_are_a_verdict = want_enabled ? times_match : true;|      const bool times_are_a_verdict = times_match;"
# The mismatch retry ladder. A pump that acks the layer write and commits it a
# beat later answers the first confirm read with the pre-write image; without
# the ladder that is a REJECTED for a write that took. Distinguished from the
# !ok ladder above it by indentation (6 spaces vs 8).
"clear-entry-no-mismatch-retry|components/alpha_hwr/write_operation_service.cpp|      if (op->attempts < SCHED_MAX_ATTEMPTS) {\n        op->attempts++;\n        schedule_([this, seq]() { confirm_schedule_entry_(seq); }, SCHED_RETRY_DELAY_MS);\n        return;\n      }\n      // Report what the pump actually holds.|      // mutated: no retry on a mismatch"
# Both rejection paths must report what the PUMP holds, not what was asked
# for. Reporting the request back is the failure that reads as success: the
# event says the entry is gone (or the schedule enabled) while the pump still
# holds the opposite, so a client has no way to see the write did not take.
#
# Only the second of these two was previously unguarded. The entry line is
# shared with SET, and test_schedule_entry_verify_mismatch already killed a
# mutation of it (submit_set_schedule_entry sets op.enabled = true, and that
# test asserts the settled sched_enabled is 0). Kept anyway: it is now killed
# from the CLEAR side as well, and the entry names the invariant rather than
# leaving it implicit in a test about SET.
"clear-entry-reject-reports-the-request|components/alpha_hwr/write_operation_service.cpp|      op->enabled = actual.is_enabled();|      // mutated: keep the requested flag instead of the pump's"
"schedule-enabled-reject-reports-the-request|components/alpha_hwr/write_operation_service.cpp|      op->enabled = actual;|      // mutated: keep the requested flag instead of the pump's"
# The clock confirm, in its load-bearing pieces.
#
# The first restores the defect the operation was written to fix: report the
# write as confirmed without reading anything back. That is what the old
# standalone path did -- callback(true) on the line after send -- and it stamped
# "Last Clock Sync" and suppressed the next attempt for 24 h whether or not the
# pump ever got the frame. If this survives, the confirm is decorative.
"clock-confirm-assumes-success|components/alpha_hwr/write_operation_service.cpp|      if (offset >= -late_allowance && offset <= CLOCK_TOLERANCE_S) {|      if (true) {"
# Makes the accept window symmetric, which is how it started. The pump lags us
# by however long the frame sat in the FIFO transport queue behind other
# commands, carrying a time built before the wait; that lag is ours. A flat
# window calls it the pump's failure as soon as one blocked APDU is in front of
# the write, and retries every 15 minutes while the link stays busy.
"clock-confirm-symmetric-window|components/alpha_hwr/write_operation_service.cpp|      const int64_t late_allowance =\n          static_cast<int64_t>(elapsed_ms / 1000) + CLOCK_TOLERANCE_S;|      const int64_t late_allowance = CLOCK_TOLERANCE_S;"
# Drops the shared local-fields resolution, so the node's exact epoch is compared
# against a readback that ESPHome re-resolved from ambiguous local fields. Equal
# for all but one hour a year, and an hour apart inside the DST fall-back fold --
# where it settles a correct sync REJECTED "3600 s off" until 02:00 local.
"clock-confirm-dst-fold|components/alpha_hwr/write_operation_service.cpp|      node_now.recalc_timestamp_local();|      // mutated: skip the shared resolution"
# Restores the readback test that calls a decoded 2000-01-01 "unreadable": a
# pump whose RTC a power cut has cleared is exactly the state a sync exists to
# repair, and is_valid()'s 2019 floor reports it as nothing having come back.
"clock-confirm-decode-test|components/alpha_hwr/write_operation_service.cpp|    if (pump_time.fields_in_range() && time_service_.current_time(node_now)) {|    if (pump_time.is_valid() && time_service_.current_time(node_now)) {"
# The 2021 floor on what we are willing to write. is_valid() alone stops at 2019,
# so an ESP32 that partially restored time writes a year-old clock to the pump.
"clock-submit-sntp-floor|components/alpha_hwr/write_operation_service.cpp|  const bool have_writable_time = op->clock_now.is_valid() && op->clock_now.year >= 2021;|  const bool have_writable_time = op->clock_now.is_valid();"
# The clock write's APDU header: object, sub-id, type, version and declared size.
# Six bytes this change renamed from an opaque blob, and until the wire test
# asserted the whole frame all three of these survived the suite -- the simulator
# matches on apdu[1..4] and reads apdu[11..], so the middle was unguarded.
"clock-apdu-type|components/alpha_hwr/time_service.cpp|  apdu[5] = 0x01;   // Type high (321 = 0x0141)|  apdu[5] = 0x99;   // Type high (321 = 0x0141)"
"clock-apdu-version|components/alpha_hwr/time_service.cpp|  apdu[7] = 0x02;   // Object version|  apdu[7] = 0x77;   // Object version"
"clock-apdu-declared-size|components/alpha_hwr/time_service.cpp|  apdu[10] = 0x0B;  // Size low (11 bytes)|  apdu[10] = 0x05;  // Size low (11 bytes)"
# The two branches taken when a chunk cannot be handed to the BLE stack. Every
# write callback in the suite returned true unconditionally until now, so a GATT
# failure -- the ordinary consequence of a link dropping mid-write -- was the one
# transport path that had never executed under test. The branches turned out to
# be correct; these keep them that way.
#
# The first drops the failure report, so a caller waits out its own timeout
# instead of being told immediately. The second leaves the failed command at the
# head of the queue, which wedges the link for every command after it -- far
# worse than the one write that was lost.
#
# Both were retargeted for issue #259: the branches call fail_front_command_()
# now instead of inlining callback-then-pop, so each mutation replaces that call
# with a bare pop_front() -- the command still leaves the queue, its caller is
# still never told.
"transport-send-failure-silent|components/alpha_hwr/transport.cpp|          this->peer_resync_started_ms_ = now;\n        }\n        this->fail_front_command_();|          this->peer_resync_started_ms_ = now;\n        }\n        this->command_queue_.pop_front();"
"transport-send-failure-wedges-queue|components/alpha_hwr/transport.cpp|        ESP_LOGE(TAG, \"Failed to send chunk, dropping command\");|        ESP_LOGE(TAG, \"Failed to send chunk\"); if (true) break;"
# The queue-advance property ON ITS OWN. The entry above drops the failure report
# and the pop in one edit, so the report assertions kill it and it proves nothing
# about the pop. This one removes only the pop: the caller is still told, and the
# failed command simply stays at the head of the queue, wedging the link for
# every command behind it.
#
# It is here because the first version of the test could not catch it. Counting
# writes cannot tell the second command being sent from the first being re-sent,
# so five assertions passed against a transport that never advanced. The test
# asserts on the payload byte now.
# Retargeted for issue #259: the three failure paths no longer inline the
# callback-then-pop, they call fail_front_command_(). Removing the pop there
# removes it from all three at once, which is a wider mutation than the original
# but the same defect -- the failed command stays at the head of the queue and
# wedges the link for everything behind it.
"transport-failed-command-stays-queued|components/alpha_hwr/transport.cpp|  Command cmd = std::move(this->command_queue_.front());\n  this->command_queue_.pop_front();|  Command cmd = this->command_queue_.front();"
# The peer-resync hold, in the three ways it can be got wrong. A write that dies
# part-way through a packet leaves the peer holding the head of a frame whose
# length byte promises more; a receiver built like ours appends whatever comes
# next rather than restarting on a frame start, so the following command is
# swallowed and its caller waits out a full timeout in silence.
#
# Dropping the hold restores that. Applying it to EVERY failure loses the
# distinction the fix exists to draw -- a first-chunk failure put nothing on the
# wire and must not buy a second of silence. Shrinking the window to nothing
# leaves the hold in place while making it useless, which is the version that
# would look right in review.
"transport-no-peer-resync-hold|components/alpha_hwr/transport.cpp|        if (cmd.bytes_sent > 0) {|        if (false) {"
"transport-peer-resync-hold-always|components/alpha_hwr/transport.cpp|        if (cmd.bytes_sent > 0) {|        if (true) {"
"transport-peer-resync-window-zero|components/alpha_hwr/transport.h|  static constexpr uint32_t PEER_RESYNC_HOLD_MS = REASSEMBLY_TIMEOUT_MS + 100;|  static constexpr uint32_t PEER_RESYNC_HOLD_MS = 0;"
# The fourth way, and the likeliest one in practice: not zeroing the window but
# TRIMMING it, because 1.1 s feels like a long stall. 700 ms leaves the hold
# plainly visible in the code and useless -- the peer's guard fires at 950 ms
# after the failure, so a shorter hold hands the next command back into the
# partial frame exactly as before. The test probes the boundary at 950 ms rather
# than at some comfortable fraction of it for this reason; with a looser probe
# this mutant survives.
"transport-peer-resync-window-too-short|components/alpha_hwr/transport.h|  static constexpr uint32_t PEER_RESYNC_HOLD_MS = REASSEMBLY_TIMEOUT_MS + 100;|  static constexpr uint32_t PEER_RESYNC_HOLD_MS = 700;"
"transport-missing-writer-silent|components/alpha_hwr/transport.cpp|ESP_LOGW(TAG, \"Write callback not set, dropping command\");\n        this->fail_front_command_();|ESP_LOGW(TAG, \"Write callback not set, dropping command\");\n        this->command_queue_.pop_front();"
# Restores issue #179's off-by-one: a seven-byte Class 7 header instead of six,
# which cost every device-info string its first character. It survived for as
# long as it did because the only test fixture was generated from the same wrong
# assumption -- so the parser and its test agreed, and the pump was blamed. The
# fixtures are transcribed from a capture now, which is what makes this mutation
# fail rather than merely shift both sides together.
"class7-header-length|components/alpha_hwr/class7_string.h|constexpr size_t CLASS7_HEADER_LEN = 6;|constexpr size_t CLASS7_HEADER_LEN = 7;"
# The length guard stands between a runt frame and an unsigned underflow:
# string_bytes is a size_t difference, so a frame under 8 bytes wraps it to
# ~1.8e19 and the copy loop reads ~127 bytes past the frame.
#
# BOTH DIRECTIONS ARE MUTATED AGAIN, and the story is worth keeping because it is
# how an equivalent mutant is made and unmade.
#
# Relaxing it (to `len < 5`) was the original entry: transport.cpp dispatched
# Class 3/7 on len >= 5, so 5-, 6- and 7-byte frames really did reach this
# decode. Issue #278 closed that -- on_notification() now refuses a frame start
# whose length byte is below 4, the structural floor (DA + SA + APDU = 1 + 1 + 2)
# -- so every frame reaching a service is at least 8 bytes, which is exactly what
# this guard checks. The two guards then masked each other and the relaxation
# became an equivalent mutant: CI found it surviving at 266/267.
#
# What un-masked it was not weakening either guard but making this one reachable.
# Issue #282 extracted the decode into protocol::decode_class7_string(), pure and
# callable with arbitrary bytes, so a host test hands it a 7-byte frame with no
# transport in front of it. The transport's floor no longer stands between the
# test and the guard, and `len < 5` fails three assertions again.
#
# The upward mutation is kept alongside it. It was never masked -- a guard set too
# HIGH rejects the captured frames the decode tests use -- and it is the direction
# that stays provable if the transport's floor moves again.
"class7-runt-guard-too-low|components/alpha_hwr/class7_string.h|constexpr size_t CLASS7_MIN_FRAME_LEN = CLASS7_HEADER_LEN + CLASS7_CRC_LEN;|constexpr size_t CLASS7_MIN_FRAME_LEN = 5;"
"class7-runt-guard-too-strict|components/alpha_hwr/class7_string.h|  if (len < CLASS7_MIN_FRAME_LEN) {|  if (len < 20) {"
# The count byte is reported, never trusted to bound the copy. Trusting it is an
# overread waiting to happen from the other direction -- it is radio-supplied.
"class7-count-byte-bounds-the-copy|components/alpha_hwr/class7_string.h|  size_t take = string_bytes;|  size_t take = data[5];"
# ---------------------------------------------------------------------------
# Wire-supplied lengths used as loop bounds (issue #284)
#
# `available_entries` and `max_entries` are both uint16 fields straight off the
# metadata reply and nothing clamped either, so the chain could be 65,535 reads:
# ~1.9 hours at the corpus's reply latency, ~512 KB accumulated on a part with a
# fraction of that free, and a uint16 sub-id that wraps out of the event-log
# address range entirely from idx 55336. The clamp is also what keeps the chain
# at or under Transport::MAX_ABANDON_STEPS, past which an abandoned chain cannot
# tell its caller anything -- the hazard #259 closed.
"event-log-count-unclamped|components/alpha_hwr/event_log_service.cpp|      count = EVENT_LOG_MAX_CHAIN_ENTRIES;|      // mutated: believe the pump"
# One chain at a time. A clamp bounds ONE chain; the re-arm backoff retires
# timers without stopping work already queued in the transport, so without this
# a slow chain gets a duplicate queued behind it and a slow read becomes an
# unbounded one.
"event-log-second-chain-allowed|components/alpha_hwr/event_log_service.cpp|  if (entries_reading_) {|  if (false) {"
# The same shape one service across, and the same omission the sibling client
# had: SINGLE_EVENT_SLOT_LIMIT guarded the WRITE path and was never applied to
# the read. SubID is 900 + slot and the schedule LAYER records start at 1000, so
# an unclamped count sends the read chain into layer 0, reading schedule layers
# as though they were single events.
"single-event-count-unclamped|components/alpha_hwr/schedule_service.h|      return static_cast<uint8_t>(SINGLE_EVENT_SLOT_LIMIT);|      return reported;"
# ---------------------------------------------------------------------------
# clear_vacation clears every vacation covering now (issue #290)
#
# #267 fixed the RANKING of stored vacations, but a ranking orders a set without
# bounding its size: setting a vacation while one is stored writes a second, and
# clearing the best-ranked one settled ACCEPTED while the pump went on holding
# itself off under the other. Killing this leaves that behaviour exactly.
"clear-vacation-stops-at-the-first|components/alpha_hwr/write_operation_service.cpp|        if (more_vacation_slots) {|        if (false) {"
# Only what covers NOW is cleared. Without the covers-now term every stored Stop
# event is collected, so clearing the current absence cancels next month's
# booking too -- which is the destruction "leave creation alone" was chosen to
# avoid.
"clear-vacation-takes-every-stored-stop|components/alpha_hwr/schedule_service.cpp|      if (covers_now)|      if (true)"
# ---------------------------------------------------------------------------
# ready_recycle is a bounded COUNT, not off-or-forever (issue #257)
#
# The reporter's case is a bonded, connected link that never finishes its
# opening GENI reads. One reconnect clears a glitch; fifty will not clear
# anything a glitch did not cause, while each takes another run at the
# encryption-on-open window that can erase a bond (issue #14). Killing this
# makes every configured bound unlimited again.
# ---------------------------------------------------------------------------
"ready-recycle-bound-ignored|components/alpha_hwr/readiness_watchdog.h|  return consecutive < limit;|  return true;"
# ---------------------------------------------------------------------------
# Setting a flow limiter is a read-modify-write (issue #299)
#
# Type 895 is one struct -- [name][enable][limit][kp][ti][td] -- so setting the
# cap rewrites all eighteen bytes. The three PID floats are echoed verbatim from
# a mandatory pre-write read; zeroing them would re-tune the pump's limiter
# control loop invisibly, which is a bigger change than the one the caller asked
# for and one nothing would report.
# ---------------------------------------------------------------------------
"limiter-write-drops-the-pid-terms|components/alpha_hwr/limiter.h|    c.pid_raw[i] = body[LIMITER_CONFIG_PID_OFFSET + i];|    c.pid_raw[i] = 0;"
# And the guard that makes the read mandatory rather than advisory: without a
# cached record there are no PID terms to echo at all.
#
# Retargeted onto the PURE builder after this exact entry survived at 341/342.
# It sat in ControlService's private write primitive, which WriteOperationService
# only calls after its own validity check -- so the outer check masked it, the
# suite could not reach it, and the mutation lived. Issue #282's shape, and its
# remedy: move the decision somewhere a test can call directly. Both guards
# stay; the operation layer's produces a useful settle status, this one makes
# the function safe to call at all.
"limiter-write-without-a-read|components/alpha_hwr/limiter.h|  if (!cached.valid)|  if (false)"
# The settle must report what the pump HOLDS on a non-accepted verdict, not what
# was requested -- otherwise a clamp names the value the caller asked for and
# reads as though the pump complied.
#
# Its sibling hazard has no entry and cannot have a useful one: settling those
# fields BEFORE the retry made every mismatch settle ACCEPTED, because the retry
# then compared the requested value against itself. That was the first cut's
# real bug, caught by the clamp and reject tests. Any mutation expressing it
# has to move a statement rather than replace a line, which this format cannot
# do -- so it is written down here instead.
# The trailing comment is load-bearing for the SEARCH, not for the reader: this
# script matches its search field as a SUBSTRING, so a bare "    settle_fields();"
# also matches the six-space call inside the accepted branch above and the entry
# is refused for matching twice. Anchoring on the comment makes the terminal
# call unique. (Targeting the accepted-branch call instead would be an
# equivalent mutant -- on an accepted write the requested and settled values are
# the same by definition.)
"limiter-settle-reports-the-request-not-the-pump|components/alpha_hwr/write_operation_service.cpp|    settle_fields();  // the pump's values, not the request|    // mutated: leave the requested values in place"
# The bound is judged against the CONSECUTIVE counter, which the pump becoming
# ready resets -- so an allowance is per episode rather than per boot. Reading a
# lifetime counter instead would let a node spend its allowance on unrelated
# episodes months apart and then never recycle again.
"ready-recycle-allowance-never-restored|components/alpha_hwr/alpha_hwr.cpp|        this->link_recycles_without_ready_ = 0;|        // mutated: the allowance is never restored"
"response-crc-enforcement|components/alpha_hwr/transport.cpp|if (!protocol::frame_crc_valid(reassembly_buffer_.data(), frame_len)) {|if (false) {"
"response-crc-trim|components/alpha_hwr/transport.cpp|if (expected_packet_length_ >= 4 && frame_len > expected_packet_length_) {|if (false) {"
"register-read-vetoes-type-match|components/alpha_hwr/transport.cpp|bool wildcard_command = (cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000);|bool wildcard_command = true;"
"register-read-guard-removed|components/alpha_hwr/transport.cpp|if (is_register_read && wildcard_command && !cmd.allow_register_read) {|if (false) {"
# Subscribe-outcome policy (issue #175). The first restores the state where a
# failed CCCD write was indistinguishable from a success. The second is the
# opposite error and the more tempting one: recycling the link on every failure,
# including the CCCD write, which tears down bonded links that were already
# subscribed from an earlier session -- and each recycle re-enters the
# encryption-on-open path where a failure can erase the bond (issue #14).
"subscribe-cccd-failure-reads-as-success|components/alpha_hwr/subscribe_outcome.h|  return o != SubscribeOutcome::OK;|  return o != SubscribeOutcome::OK && o != SubscribeOutcome::CCCD_WRITE_FAILED;"
"subscribe-holds-on-every-failure|components/alpha_hwr/subscribe_outcome.h|  return subscribe_outcome_blocks_session(o);|  return subscribe_failed(o);"
# Found by a skeptic pass, both survived the original tests. The first points
# the operator at the wrong cause, which defeats the whole purpose of naming
# them; the second is the "distinctness is not correspondence" hole.
"subscribe-fault-strings-swapped|components/alpha_hwr/subscribe_outcome.h|    case SubscribeOutcome::NO_SERVICE:\n      return \"Subscribe: service not found\";\n    case SubscribeOutcome::NO_CHARACTERISTIC:\n      return \"Subscribe: characteristic not found\";|    case SubscribeOutcome::NO_SERVICE:\n      return \"Subscribe: characteristic not found\";\n    case SubscribeOutcome::NO_CHARACTERISTIC:\n      return \"Subscribe: service not found\";"
# Fault-hold rank (issue #175 follow-up). The first restores the exception list
# the rank replaced: `!= SUBSCRIBE` reads as "hold the specific reason", but an
# AUTH hold is neither NONE nor SUBSCRIBE, so the watchdog's generic "No data
# from pump" overwrote a bond-erasing pairing failure 60 s later -- and relabelled
# the origin DATA, which the next notification then released. The second drops the
# rank entirely (last writer wins, the pre-#175 state). The third is the tempting
# near-miss: strictly-greater, or the equivalent `held == NONE` one-liner, which
# freezes a fault at its first text and hides the watchdog's backoff escalation
# from the fault surface. The fourth releases a pairing failure on inbound data,
# which an unbonded pump keeps sending after a failed SMP -- erasing exactly the
# diagnostic the hold exists to preserve.
"failure-hold-watchdog-outranks-auth|components/alpha_hwr/failure_hold.h|  return held <= incoming;|  return held != FailureHold::SUBSCRIBE;"
"failure-hold-rank-ignored|components/alpha_hwr/failure_hold.h|  return held <= incoming;|  return true;"
"failure-hold-never-refreshes-its-own-text|components/alpha_hwr/failure_hold.h|  return held <= incoming;|  return held < incoming;"
"failure-hold-auth-released-by-data|components/alpha_hwr/failure_hold.h|    case FailureHold::AUTH:\n      return false;  // see the asymmetry above|    case FailureHold::AUTH:\n      return true;  // see the asymmetry above"
# The READY release is what bounds an AUTH hold. Removing it restores a hold
# with no exit for the rest of the boot on a pump that never pairs -- masking
# every later fault with a pairing string that, past READY, is not even shown.
# The second is the opposite error: releasing the watchdog's hold at READY,
# which a deaf link reaches on every cycle, so the deaf-link reason would be
# cleared on exactly the links it describes.
"failure-hold-auth-not-released-at-ready|components/alpha_hwr/failure_hold.h|    case FailureHold::SUBSCRIBE:\n      return false;  // and a blocking subscribe fault never reaches READY\n    case FailureHold::AUTH:\n      return true;|    case FailureHold::SUBSCRIBE:\n      return false;  // and a blocking subscribe fault never reaches READY\n    case FailureHold::AUTH:\n      return false;"
"failure-hold-watchdog-released-at-ready|components/alpha_hwr/failure_hold.h|      return false;  // READY does not prove the pump is answering|      return true;  // READY does not prove the pump is answering"
# Pairing-stall detection (pairing_stall.h, issue #230). A pump bonded to a
# client that is no longer bonded to it sends no SEC_REQ and drops the link, and
# the node loops on that every ~5 s forever saying "waiting for pump to initiate
# pairing". Nothing here recovers it -- the pump has to be put into pairing mode
# by hand -- so the whole value of this module is that it says so.
#
# The first two are the false negatives: never firing at all, and firing only on
# the reported cycles so the latched fault blinks in and out with the log
# throttle instead of holding.
"pairing-stall-never-reported|components/alpha_hwr/pairing_stall.h|    if (consecutive_ < PAIRING_STALL_CYCLES) return false;|    if (true) return false;"
"pairing-stall-fault-blinks-with-the-log-throttle|components/alpha_hwr/pairing_stall.h|  bool stalled() const { return consecutive_ >= PAIRING_STALL_CYCLES; }|  bool stalled() const { return consecutive_ == PAIRING_STALL_CYCLES; }"
# The rest are the false positives, which are the more damaging half: telling
# someone their pump refuses to pair, when it does not, is worse than the
# silence this replaces. `enable_pairing` defaults to false and passive
# telemetry needs no bond, so a healthy installation runs unbonded forever and
# its ordinary reconnects must not accumulate -- that is what the data term and
# the SEC_REQ term are for. The bonded-open term keeps a bonded reconnect out of
# the count entirely, and the failed-open guard keeps a pump that is powered
# down or out of range from being reported as one that refuses to pair.
"pairing-stall-data-does-not-clear-it|components/alpha_hwr/pairing_stall.h|  void note_data() { note_progress_(); }|  void note_data() {}"
"pairing-stall-a-willing-pump-still-counts|components/alpha_hwr/pairing_stall.h|  void note_security_request() { note_progress_(); }|  void note_security_request() {}"
"pairing-stall-counts-bonded-connections|components/alpha_hwr/pairing_stall.h|    if (bonded_at_open) note_progress_();|    if (false) note_progress_();"
"pairing-stall-counts-a-failed-open|components/alpha_hwr/pairing_stall.h|    if (!saw_open_) return false;  // a failed open is not a cycle|    if (false) return false;  // a failed open is not a cycle"
"pairing-stall-counts-our-own-teardowns|components/alpha_hwr/pairing_stall.h|  void note_local_teardown() { progress_ = true; }|  void note_local_teardown() {}"
"pairing-stall-fires-on-a-single-drop|components/alpha_hwr/pairing_stall.h|inline constexpr uint8_t PAIRING_STALL_CYCLES = 3;|inline constexpr uint8_t PAIRING_STALL_CYCLES = 1;"
# The throttle itself, and the counter it is NOT derived from. A stall lasts
# until someone walks to the pump, which can be days: dropping the throttle puts
# twelve identical warnings a minute in the log forever, and letting the cycle
# counter wrap reads a two-day-old stall as one that has just started.
"pairing-stall-warns-on-every-cycle|components/alpha_hwr/pairing_stall.h|    if (since_report_ >= PAIRING_STALL_REMINDER_CYCLES) {|    if (true) {"
"pairing-stall-cycle-counter-wraps|components/alpha_hwr/pairing_stall.h|    if (consecutive_ < UINT8_MAX) consecutive_++;|    consecutive_++;"
# Found by a skeptic pass, all four SURVIVING against the first version of the
# tests. The first is the sharpest: the branch form of "warn on every cycle" was
# pinned and the CONSTANT form was not, so the mechanism read as covered while
# the number could be retuned to 1 -- twelve warnings a minute, forever, for a
# fault whose remedy needs someone to walk to the pump. The second let one
# ordinary drop plus an out-of-range pump manufacture a pattern, because the
# only test of the failed-open guard used a virgin detector where the flag it
# checks was already false. The third shortens a relapse's first repeat.
"pairing-stall-reminder-window-retuned-to-one|components/alpha_hwr/pairing_stall.h|inline constexpr uint8_t PAIRING_STALL_REMINDER_CYCLES = 12;|inline constexpr uint8_t PAIRING_STALL_REMINDER_CYCLES = 1;"
"pairing-stall-one-open-closes-many-cycles|components/alpha_hwr/pairing_stall.h|    saw_open_ = false;\n    if (progress_|    if (progress_"
"pairing-stall-relapse-skips-its-reminder-window|components/alpha_hwr/pairing_stall.h|      consecutive_ = 0;\n      since_report_ = 0;\n      return false;|      consecutive_ = 0;\n      return false;"
"pairing-stall-a-completed-bond-does-not-clear-it|components/alpha_hwr/pairing_stall.h|  void note_bond_established() { note_progress_(); }|  void note_bond_established() {}"
# The wiring, which the first version left entirely unexercised -- ten hand-made
# mutations of these call sites all survived, including one that switches the
# feature off outright. The rule living in a pure header is why it can be tested
# exhaustively; it is not a reason for the component's use of it to be untested,
# and the claim that ble_connection_manager.cpp "is compiled by no host test"
# (still repeated by three neighbouring headers) stopped being true when it
# joined COMPONENT_SRCS.
"pairing-stall-feature-silently-off|components/alpha_hwr/ble_connection_manager.cpp|  pairing_stall_.on_connection_opened(bonded_at_open_);|  pairing_stall_.on_connection_opened(true);"
"pairing-stall-no-cycle-ever-closes|components/alpha_hwr/ble_connection_manager.cpp|          pairing_stall_.on_disconnected(static_cast<uint16_t>(param->disconnect.reason));|          false;"
"pairing-stall-disconnect-reason-ignored|components/alpha_hwr/ble_connection_manager.cpp|          pairing_stall_.on_disconnected(static_cast<uint16_t>(param->disconnect.reason));|          pairing_stall_.on_disconnected(0x0013);"
"pairing-stall-fault-never-latched|components/alpha_hwr/ble_connection_manager.cpp|      if (pairing_stall_.stalled()) {|      if (false) {"
"pairing-stall-fault-latched-on-every-drop|components/alpha_hwr/ble_connection_manager.cpp|      if (pairing_stall_.stalled()) {|      if (true) {"
"pairing-stall-latched-at-auth-rank|components/alpha_hwr/ble_connection_manager.cpp|          failure_hold_ = FailureHold::PAIRING_STALL;|          failure_hold_ = FailureHold::AUTH;"
"pairing-stall-hold-is-never-withdrawn|components/alpha_hwr/ble_connection_manager.cpp|  if (failure_hold_ == FailureHold::PAIRING_STALL && !pairing_stall_.stalled()) {|  if (false) {"
"pairing-stall-sec-req-not-noted|components/alpha_hwr/ble_connection_manager.cpp|        pairing_stall_.note_security_request();|        (void) 0;"
# GAP security policy (gap_security_policy.h). BLE GAP events are broadcast to
# every client and every node on the device, not routed to the connection that
# caused them, so a handler without an address check answers -- and acts on --
# the pairing traffic of every other BLE peer sharing the node. The first two
# are the auto-accept as it shipped: no address check at all, and the address
# check present but the enable_pairing gate missing. The third is the tempting
# fix that is still wrong, refusing a stranger's request on another component's
# behalf instead of staying out of it. The last two attack the comparison
# itself: stopping after one octet accepts every device with a matching OUI,
# and dropping the unset-peer guard makes an all-zero event address match a
# ble_client that has no address configured yet. The last three came from an
# adversarial pass and were all SURVIVING when first written: every address in
# the suite was zero-free, so "is this peer unset?" could be reduced to its
# first octet and stay green -- while in production it reads a pump on a
# 00:xx:xx OUI as unconfigured and ignores its AUTH_CMPL forever. The length
# over-read is the same blind spot in the other direction: comparing a seventh
# byte compares one object with itself in a test and reads out of bounds on the
# device.
#
# Read the green here narrowly. All five mutate the *rule*; none can mutate its
# *application*, because ble_connection_manager.cpp is compiled by no host test
# -- so this block would still report 5/5 caught on a branch where every one of
# the seven gates in handle_gap_event() had been deleted. What is pinned is that
# the policy is right, not that it is wired up. The wiring is checked by the
# ESP32 compile and on the bench.
"gap-security-accepts-any-device|components/alpha_hwr/gap_security_policy.h|  if (!addr_is_ours) {\n    return GapSecurityAction::IGNORE;\n  }|  if (false) {\n    return GapSecurityAction::IGNORE;\n  }"
"gap-security-ignores-enable-pairing|components/alpha_hwr/gap_security_policy.h|  return pairing_enabled ? GapSecurityAction::ACCEPT : GapSecurityAction::DECLINE;|  return GapSecurityAction::ACCEPT;"
"gap-security-refuses-for-others|components/alpha_hwr/gap_security_policy.h|    return GapSecurityAction::IGNORE;\n  }\n  return pairing_enabled|    return GapSecurityAction::DECLINE;\n  }\n  return pairing_enabled"
"gap-addr-compares-one-octet|components/alpha_hwr/gap_security_policy.h|  for (size_t i = 0; i < BD_ADDR_LEN; i++) {\n    if (event_addr[i] != peer_addr[i]) {|  for (size_t i = 0; i < 1; i++) {\n    if (event_addr[i] != peer_addr[i]) {"
"gap-addr-unset-peer-matches|components/alpha_hwr/gap_security_policy.h|  if (!gap_addr_is_set(peer_addr)) {\n    return false;\n  }\n  if (event_addr == nullptr) {|  if (event_addr == nullptr) {"
"gap-addr-unset-scan-one-octet|components/alpha_hwr/gap_security_policy.h|  for (size_t i = 0; i < BD_ADDR_LEN; i++) {\n    if (addr[i] != 0) {|  for (size_t i = 0; i < 1; i++) {\n    if (addr[i] != 0) {"
"gap-addr-unset-first-octet-only|components/alpha_hwr/gap_security_policy.h|  for (size_t i = 0; i < BD_ADDR_LEN; i++) {\n    if (addr[i] != 0) {\n      return true;\n    }\n  }\n  return false;|  return addr[0] != 0;"
"gap-addr-everything-is-unset|components/alpha_hwr/gap_security_policy.h|    if (addr[i] != 0) {\n      return true;\n    }|    if (addr[i] != 0) {\n      return false;\n    }"
"gap-addr-len-over-read|components/alpha_hwr/gap_security_policy.h|constexpr size_t BD_ADDR_LEN = 6;|constexpr size_t BD_ADDR_LEN = 7;"
# Clock sync gate (clock_sync_gate.h). The pump runs schedule windows off its
# own RTC and nothing else corrects it, so a sync that never happens surfaces
# days later as a schedule firing at the wrong hour. The first two mutations are
# the two permanent causes going unreported again -- no time_id, and a time
# source that never answers, the latter being the likelier one because both
# entry packages set time_id. The third is the false alarm in the other
# direction: accusing a node that is merely still booting. The fourth is the
# boundary, where "settling" turns into "misconfigured". The last collapses
# blocked and warns into one question, which is how the boot false alarm gets
# reintroduced. The last three pin the window itself: every other assertion in
# the suite is written against CLOCK_SOURCE_GRACE_MS, so the constant used to
# certify itself -- 1 ms and 24 h both passed 34/34, and so did a comparison
# that ignored its own parameter for any window under a second.
"clock-gate-grace-far-too-short|components/alpha_hwr/clock_sync_gate.h|constexpr uint32_t CLOCK_SOURCE_GRACE_MS = 15 * 60 * 1000;|constexpr uint32_t CLOCK_SOURCE_GRACE_MS = 1;"
"clock-gate-grace-far-too-long|components/alpha_hwr/clock_sync_gate.h|constexpr uint32_t CLOCK_SOURCE_GRACE_MS = 15 * 60 * 1000;|constexpr uint32_t CLOCK_SOURCE_GRACE_MS = 24 * 60 * 60 * 1000;"
"clock-gate-grace-ignored-when-small|components/alpha_hwr/clock_sync_gate.h|  if (uptime_ms < grace_ms) {|  if (uptime_ms < grace_ms && grace_ms >= 1000u) {"
"clock-gate-missing-time-id-unreported|components/alpha_hwr/clock_sync_gate.h|    return ClockSyncAction::WARN_NO_TIME_ID;\n  }\n  if (wall_clock_set) {|    return ClockSyncAction::WAIT;\n  }\n  if (wall_clock_set) {"
"clock-gate-silent-source-unreported|components/alpha_hwr/clock_sync_gate.h|  return ClockSyncAction::WARN_NO_SOURCE;\n}|  return ClockSyncAction::WAIT;\n}"
"clock-gate-accuses-a-booting-node|components/alpha_hwr/clock_sync_gate.h|  if (uptime_ms < grace_ms) {\n    return ClockSyncAction::WAIT;\n  }|  if (false) {\n    return ClockSyncAction::WAIT;\n  }"
"clock-gate-grace-boundary-off-by-one|components/alpha_hwr/clock_sync_gate.h|  if (uptime_ms < grace_ms) {|  if (uptime_ms <= grace_ms) {"
"clock-gate-every-block-warns|components/alpha_hwr/clock_sync_gate.h|  return a == ClockSyncAction::WARN_NO_TIME_ID |  return a != ClockSyncAction::SYNC; //"

# Local time comes from ESPHome's parsed zone, never from libc (issue #289).
# ESPHome calls setenv("TZ")/tzset() only under USE_HOST, so on the ESP32 libc
# has NO zone: localtime_r/mktime/gmtime_r all answer UTC while ESPTime is
# correct. local_utc_offset_seconds() was written against libc and therefore
# returned 0 on the device, making the whole UTC<->local shift for single
# events a no-op -- a user asking for 07:00 got 00:00.
#
# These are the mutants an ordinary host test cannot catch, because the host
# build DOES set libc's zone. They die only under the mock's embedded mode,
# where ESPTime's zone deliberately disagrees with the process TZ.
"tz-offset-taken-as-utc|components/alpha_hwr/schedule_service.h|  ESPTime local_fields = ESPTime::from_epoch_local(ref);|  ESPTime local_fields = ESPTime::from_epoch_utc(ref);"
"tz-event-window-encoded-as-utc|components/alpha_hwr/alpha_hwr.h|      t.recalc_timestamp_local();|      t.recalc_timestamp_utc(false);"
# The other half of the same distinction, and the one I got wrong first. Event
# log and cycle timestamps come off the wire RAW -- nothing converts them,
# because they are the pump's own clock, which runs local. Rendering them
# "to local" shifts a local wall clock by the offset a SECOND time. Cached
# single events are the opposite case: local_unix_to_utc_resolved() has already
# made them UTC epochs in our domain, so they do want shifting.
"tz-event-log-shifted-twice|components/alpha_hwr/event_log_service.cpp|    const ESPTime lt = ESPTime::from_epoch_utc(static_cast<time_t>(e.timestamp));|    const ESPTime lt = ESPTime::from_epoch_local(static_cast<time_t>(e.timestamp));"

# The pump's own DST rule against the node's timezone (issue #286). The pump
# shifts its own clock twice a year by its own stored rule; schedule windows are
# stored in its LOCAL time and converted using the HOST's offset, so the two
# have to agree or every stored window is an hour out on one side of a
# transition. The symptom appears months after the cause.
"dst-enabled-bit-ignored|components/alpha_hwr/dst_rule.h|  r.enabled = body[0] != 0;|  r.enabled = true;"
"dst-offset-not-compared|components/alpha_hwr/dst_rule.h|  const bool same_amount = pump.offset_minutes == host.offset_minutes;|  const bool same_amount = true;"
"dst-dates-not-compared|components/alpha_hwr/dst_rule.h|  const bool same_start = transitions_coincide(pump.start, host.start, year);|  const bool same_start = true;"
"dst-end-date-not-compared|components/alpha_hwr/dst_rule.h|  const bool same_end = transitions_coincide(pump.end, host.end, year);|  const bool same_end = true;"
# "The last Sunday of October" and "the fourth Sunday of October" are different
# rules that coincide in some years and not others, so both sides are resolved
# to a concrete date in the year being asked about. Ignoring "last" makes a
# correct EU pump in an EU zone read as a mismatch in every year where the month
# has five of that weekday.
"dst-last-occurrence-not-resolved|components/alpha_hwr/dst_rule.h|  if (d.occurrence >= 5) {\n    uint8_t last = first;|  if (false) {\n    uint8_t last = first;"
# ...and the fields have to describe a real date before any of that runs. A
# payload with occurrence 0 or hour 24 would otherwise decode into a
# genuine-looking rule that resolves to nothing, reported to the user as a
# mismatch against their timezone -- blaming the zone for a bad frame.
"dst-impossible-fields-accepted|components/alpha_hwr/dst_rule.h|  const bool occurrence_ok = d.occurrence >= 1 && d.occurrence <= 5;|  const bool occurrence_ok = true;"
# A transition at LOCAL MIDNIGHT: the date and the hour must be advanced
# TOGETHER. Taking the date from one second before while wrapping only the hour
# named a day 24 h early -- 212 start-side wrong answers across the tz database
# (Havana, Cairo, Beirut, the Azores, Santiago, Nuuk), reporting a correct pump
# as broken and a pump misconfigured to the probed rule as AGREE.
"dst-midnight-transition-off-by-a-day|components/alpha_hwr/dst_rule.h|      if (h == 24) {  // the wall clock rolls into the next civil day|      if (false) {  // the wall clock rolls into the next civil day"
# Anything but a clean pair is a rule this cannot express. Emitting half of one
# printed "? ?#0 00:00" to the user for any year a country adopts or abolishes
# DST (Brazil 2019, Fiji 2021, Montevideo 2015).
"dst-half-a-rule-emitted|components/alpha_hwr/dst_rule.h|  if (count != 2)\n    return r;|  if (false)\n    return r;"
# The zone comes from ESPHome, not libc (issue #289) -- the half that made this
# feature inert on the device, finding no transitions and reporting a mismatch
# for every user in a DST zone.
"dst-probe-reads-libc|components/alpha_hwr/dst_rule.h|    ESPTime fields = ESPTime::from_epoch_local(t);|    ESPTime fields = ESPTime::from_epoch_utc(t);"

# The southern hemisphere starts its year in daylight time, so the first
# transition of the calendar year is the one OFF it. Reading them in calendar
# order makes every southern installation a mismatch.
"dst-transitions-read-backwards|components/alpha_hwr/dst_rule.h|  const bool first_is_the_spring_shift = delta[0] > 0;|  const bool first_is_the_spring_shift = true;"
# A frame too short to hold the rule must not be read past the end, and must not
# be reported as a rule.
"dst-short-frame-decoded|components/alpha_hwr/dst_rule.h|  if (body == nullptr |  if (false) return r; //"

# One clock, one floor (issue #270). The component used to resolve "what time is
# it" in five places with three different floors -- year 2020, year 2021, and
# the literal 1609459200 -- three of which read ::time(nullptr) directly, so a
# build without the time component had one subsystem declining to act and three
# acting on an unvalidated clock in an unset zone. Nothing was known to be
# broken by it; #262 was caused by one caller substituting the wrong timestamp
# for "now", and several independent notions of now is what makes that class of
# bug easy to reintroduce.
#
# The first two are the floor itself, which every other assertion in
# tests/test_time_service.cpp is written relative to and could therefore not
# certify on its own. 2019 is ESPTime::is_valid()'s own year rule, so that
# mutant makes the floor a no-op rather than merely a different number.
"clock-floor-is-a-no-op|components/alpha_hwr/time_service.h|constexpr uint16_t CLOCK_SYNCED_YEAR_FLOOR = 2021;|constexpr uint16_t CLOCK_SYNCED_YEAR_FLOOR = 2019;"
"clock-floor-not-applied|components/alpha_hwr/time_service.cpp|  if (!clock_is_synced(now)) return false;|  if (false) return false;"
# ...and that a missing time source is still a refusal, not a fall-through.
"clock-missing-time-id-answered|components/alpha_hwr/time_service.cpp|  if (time_id_ == nullptr) return false;|  if (false) return false;"
# The drift callers. publish_clock_drift_() reports rather than decides, so both
# of its callers can share it while still differing on what to publish when it
# fails -- the read chain leaves the last good reading alone (issue #259), the
# manual button publishes NAN.
"clock-drift-invents-a-node-time|components/alpha_hwr/alpha_hwr.cpp|  if (now_ts == 0) {\n    ESP_LOGI(TAG, \"System time not synced yet; skipping drift calculation\");\n    return false;\n  }|  if (false) {\n    ESP_LOGI(TAG, \"System time not synced yet; skipping drift calculation\");\n    return false;\n  }"
# build_event_window() takes month/day/hour/minute and has to get the YEAR from
# somewhere. Before #270 it took it from libc rather than from the node clock.
"event-window-year-not-from-the-clock|components/alpha_hwr/alpha_hwr.h|    int year = static_cast<int>(local_now.year) - 1900;  // years since 1900|    int year = 120;  // years since 1900"
#
# Deliberately absent, both equivalent mutants rather than gaps:
#   * now_unix()'s `if (now.timestamp <= 0) return 0;` -- unreachable, since
#     clock_is_synced() has already floored the year at 2021. It is there to
#     keep the 0 sentinel unambiguous rather than argued, and mutating it
#     changes no reachable behaviour.
#   * stop_single_event_active_()'s `if (now_ts == 0) return false;` -- NOT an
#     equivalent mutant, and not covered either. Reaching it needs a warm
#     single-event cache behind a run-state reconcile, which no host rig stages
#     today. Its sentinel is the same now_unix() one swept in
#     tests/test_time_service.cpp; the caller's own branch is unpinned. Recorded
#     rather than quietly omitted.
"link-watchdog-never-fires|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return false;"
"link-watchdog-rollover-unsafe|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return now_ms > last_inbound_ms + timeout_ms;"
# Readiness (progress) watchdog, issue #211. The data watchdog above watches
# liveness and is re-armed by every notification; this pump volunteers telemetry
# unprompted, so a session stuck anywhere keeps re-arming it and the failure --
# connected, streaming, never usable, automation waiting forever -- is invisible
# to it.
#
# Note the predicate is three separate `if`s rather than one `||` chain, and
# these entries are why: a search field is split on '|', so a guard containing
# `||` truncates at the first one and applies an edit nobody wrote. The
# occurrence check cannot see it -- the truncated fragment is still unique, so
# it reports a clean match and the build fails on garbage. Two entries here were
# written that way and both scored BUILD_BROKEN.
"readiness-watchdog-never-fires|components/alpha_hwr/readiness_watchdog.h|  return static_cast<uint32_t>(now_ms - connected_since_ms) > timeout_ms;|  return false;"
"readiness-watchdog-rollover-unsafe|components/alpha_hwr/readiness_watchdog.h|  return static_cast<uint32_t>(now_ms - connected_since_ms) > timeout_ms;|  return now_ms > connected_since_ms + timeout_ms;"
# A ready pump must be exempt, or a healthy link is recycled every five minutes
# forever -- the opposite failure, and a louder one. And the disable has to
# disable: an opt-out that silently still fired would be worse than no option.
"readiness-watchdog-recycles-a-ready-pump|components/alpha_hwr/readiness_watchdog.h|  if (pump_ready)\n    return false;|  if (false)\n    return false;"
"readiness-watchdog-cannot-be-disabled|components/alpha_hwr/readiness_watchdog.h|  if (timeout_ms == 0)\n    return false;|  if (false)\n    return false;"
# The arming rule, which is the whole design: re-arm from anything on the way to
# readiness and the timer chases the state it waits for. Anchored on the
# notification path, because that is what "refresh it on activity" would
# actually look like. (An earlier version of this entry re-armed from millis()
# at the connection-open site, which is a semantic no-op -- link_last_open_ms_
# is assigned from millis() eleven lines above it.)
"readiness-watchdog-rearmed-by-activity|components/alpha_hwr/alpha_hwr.cpp|        this->link_last_inbound_ms_ = inbound_now;|        this->link_last_inbound_ms_ = inbound_now;\n        this->link_ready_since_ms_ = inbound_now;"
"readiness-watchdog-not-checked|components/alpha_hwr/alpha_hwr.cpp|    if (!this->check_link_liveness_())\n      this->check_link_readiness_();|    this->check_link_liveness_();"
# The split (issue #211): naming ships on, recycling is opt-in. A default
# installation that starts tearing its link down is the bond-erase exposure the
# split exists to withhold.
#
# Issue #257 turned the opt-in from a bool into a bound, so this no longer has a
# gate of its own to remove -- "off" is now the limit of 0 falling out of the
# same comparison the bound uses. Hence the off-by-one: it leaves the bound
# working and flips only the default, from never-recycle to recycle-once, which
# is the claim this entry has always been about. `return true` is the other
# entry (ready-recycle-bound-ignored); mutating this to `if (false)` at the call
# site would have been an equivalent mutant of it and proved nothing new.
"readiness-recycles-by-default|components/alpha_hwr/readiness_watchdog.h|  return consecutive < limit;|  return consecutive <= limit;"
# The rank. Both halves of the defect that nearly shipped: taking the default at
# the call site, and ignoring the parameter inside. force_disconnect() used to
# hardcode DATA, so the readiness reason was held at the one rank released by
# inbound data -- in the one failure mode defined by inbound data never stopping.
"readiness-latched-at-data-rank|components/alpha_hwr/alpha_hwr.cpp|  this->ble_manager_.force_disconnect(reason, core::FailureHold::READY);|  this->ble_manager_.force_disconnect(reason);"
"force-disconnect-ignores-the-rank|components/alpha_hwr/ble_connection_manager.cpp|  if (failure_hold_admits(failure_hold_, rank)) {\n    last_failure_ = reason;\n    failure_hold_ = rank;|  if (failure_hold_admits(failure_hold_, FailureHold::DATA)) {\n    last_failure_ = reason;\n    failure_hold_ = FailureHold::DATA;"
# The other half of that defect: readiness read back off an OPTIONAL entity, so
# a hand-written config omitting ready_status recycled a healthy pump forever.
"readiness-reads-the-optional-sensor|components/alpha_hwr/alpha_hwr.cpp|                                      this->link_pump_ready_seen_, millis(),|                                      this->ready_sensor_ != nullptr && this->ready_sensor_->state, millis(),"
"readiness-latch-never-set|components/alpha_hwr/alpha_hwr.cpp|        this->link_pump_ready_seen_ = true;|        (void) 0;"
# The counter the reporter of #211 named as their signal from outside the
# component. It stayed at zero through the failure the change exists for.
"readiness-recycles-not-published|components/alpha_hwr/alpha_hwr.cpp|                           this->link_recycles_without_ready_));|                           0u));"
# Rank and release of the hold itself.
"readiness-hold-released-by-data|components/alpha_hwr/failure_hold.h|    case FailureHold::READY:\n      return false;  // data arriving is the CONDITION|    case FailureHold::READY:\n      return true;  // data arriving is the CONDITION"
"readiness-hold-never-released|components/alpha_hwr/failure_hold.h|    case FailureHold::READY:\n      return true;|    case FailureHold::READY:\n      return false;"
# data_timeout backoff (issue #176). Without it a deaf link is recycled ~1,300
# times a day indefinitely, each pass re-entering the encryption-on-open window
# where a failure can erase the bond (issue #14). The three errors: never
# growing, growing past the ceiling, and clamping a configured budget that was
# deliberately set larger than the ceiling.
"backoff-never-grows|components/alpha_hwr/link_watchdog.h|  const uint32_t doubled = current_ms * 2u;|  const uint32_t doubled = current_ms;"
"backoff-ignores-the-cap|components/alpha_hwr/link_watchdog.h|  return doubled > cap_ms ? cap_ms : doubled;|  return doubled;"
"backoff-shrinks-a-large-budget|components/alpha_hwr/link_watchdog.h|  if (current_ms >= cap_ms)\n    return current_ms;|"
# The gap statistic (issue #176). All three mutations bias it the same way --
# downward, toward "the budget was never close" -- which is the direction that
# argues for keeping a data_timeout default nobody has validated. Dropping the
# recycle sample is the one that shipped: it censors the sample at exactly the
# threshold the number exists to validate, since an interval that ends in a
# recycle is never closed by a notification.
"gap-censored-at-the-budget|components/alpha_hwr/link_watchdog.h|  void on_recycle(uint32_t now_ms) {\n    this->sample_(now_ms, false);|  void on_recycle(uint32_t now_ms) {\n    (void) now_ms;"
# One recycle is one truncated interval. force_disconnect() is asynchronous, so
# the DISCONNECT event arrives a tick later and on_disconnect() runs; left armed
# it samples the re-arm-to-event gap as a second truncated interval and
# link_gaps_truncated reads about twice the recycle count. The trust check on
# the whole histogram then overstates by 2x, which is the direction that makes
# the report refuse a run that was actually fine.
"gap-recycle-leaves-the-link-armed|components/alpha_hwr/link_watchdog.h|    this->sample_(now_ms, false);\n    this->armed_ = false;\n  }\n\n  /// The link dropped|    this->sample_(now_ms, false);\n  }\n\n  /// The link dropped"
"gap-samples-the-time-spent-disconnected|components/alpha_hwr/link_watchdog.h|    this->last_ms_ = now_ms;\n    this->armed_ = true;|    this->armed_ = true;"
"gap-never-closes-an-interval|components/alpha_hwr/link_watchdog.h|    }\n    this->last_ms_ = now_ms;\n  }|    }\n  }"
# The disconnect sample and its arming guard. Dropping the sample loses every
# interval ended by a drop the watchdog did not cause -- the same censoring, at
# a threshold nobody configured. Dropping the guard is the opposite error: a
# connection attempt that fails without opening reports a disconnect, and
# sampling it records the downtime since the previous session as though the link
# had been up and silent for all of it.
"gap-drops-the-disconnect-sample|components/alpha_hwr/link_watchdog.h|  void on_disconnect(uint32_t now_ms) {\n    if (!this->armed_)\n      return;\n    this->sample_(now_ms, false);\n    this->armed_ = false;\n  }|  void on_disconnect(uint32_t now_ms) { (void) now_ms; }"
"gap-samples-an-unarmed-disconnect|components/alpha_hwr/link_watchdog.h|    if (!this->armed_)\n      return;\n    this->sample_(now_ms, false);|    this->sample_(now_ms, false);"
# The tail histogram (issue #176 part 1). The counters are only a decision input
# because "intervals longer than T" is exactly "times a budget of T would have
# fired"; each mutation below answers a slightly different question while
# looking identical in Home Assistant, which is the whole hazard. Three of them
# bias the reading toward "the budget was never close", the direction that
# argues for keeping a default nobody has validated.
"gap-bucket-counts-every-interval|components/alpha_hwr/link_watchdog.h|      if (gap > LINK_GAP_THRESHOLDS_MS[i])\n        this->over_counts_[i]++;|        this->over_counts_[i]++;"
"gap-bucket-boundary-off-by-one|components/alpha_hwr/link_watchdog.h|      if (gap > LINK_GAP_THRESHOLDS_MS[i])|      if (gap >= LINK_GAP_THRESHOLDS_MS[i])"
"gap-truncated-counts-a-notification|components/alpha_hwr/link_watchdog.h|    if (!closed_by_data)\n      this->truncated_++;|    this->truncated_++;"
"gap-watch-time-never-accumulates|components/alpha_hwr/link_watchdog.h|    this->watched_ms_ += gap;|"
"gap-counters-reset-on-reconnect|components/alpha_hwr/link_watchdog.h|  void on_open(uint32_t now_ms) {\n    this->last_ms_ = now_ms;|  void on_open(uint32_t now_ms) {\n    this->over_counts_[0] = 0;\n    this->last_ms_ = now_ms;"
"gap-inbound-samples-across-the-downtime|components/alpha_hwr/link_watchdog.h|    if (!this->armed_) {\n      this->last_ms_ = now_ms;\n      this->armed_ = true;\n      return;\n    }\n    this->sample_(now_ms, true);|    this->sample_(now_ms, true);"
# The publish throttle on watched time. Removing it is the issue #127 load
# shape: a frame per API subscriber every 10 s, forever, for a number nothing
# downstream can resolve faster than hourly. Mutated through the constant
# because the guard itself contains a `||`, which is the field separator here.
"gap-watch-time-published-unthrottled|components/alpha_hwr/link_watchdog.h|static const uint32_t LINK_GAP_WATCH_PUBLISH_MS = 300000u;|static const uint32_t LINK_GAP_WATCH_PUBLISH_MS = 0u;"

# Initial-read re-arm (bench regression: a stalled one-shot read chain left the
# node with device info and the operating statistics unread for as long as the
# BLE link stayed up -- while the self-healing caches brought Pump Ready on, so
# it looked completely healthy).
"initial-read-never-rearms|components/alpha_hwr/initial_read_retry.h|return (now_ms - attempt_started_ms) >= timeout_ms;|return false;"
"initial-read-rearms-forever|components/alpha_hwr/initial_read_retry.h|  if (caches_synchronized && chain_products_complete) {\n    return false;\n  }|"
"initial-read-caches-alone-count-as-success|components/alpha_hwr/initial_read_retry.h|if (caches_synchronized && chain_products_complete) {|if (caches_synchronized) {"
"initial-read-rollover-unsafe|components/alpha_hwr/initial_read_retry.h|return (now_ms - attempt_started_ms) >= timeout_ms;|return now_ms >= attempt_started_ms + timeout_ms;"
"initial-read-backoff-never-grows|components/alpha_hwr/initial_read_retry.h|  const uint32_t doubled = current_ms * 2u;|  const uint32_t doubled = current_ms;"
# Pump-on continuation release (audit finding 10: the tier's only exit was raw
# meter flow above threshold, which cannot go false while the pump runs, so a
# draw that stopped mid-run kept publishing demand until the pump did).
"continuation-never-releases-on-measurement|components/dhw_demand/dhw_demand_logic.h|    if (in.demand_gpm <= in.release_gpm) {\n      const uint32_t seen = (uint32_t) in.measured_stopped_ticks + 1u;\n      return seen >= (uint32_t) in.release_ticks\n                 ? ContinuationVerdict::MEASURED_STOPPED\n                 : ContinuationVerdict::STOPPING;\n    }|"
"continuation-releases-on-any-measurement|components/dhw_demand/dhw_demand_logic.h|    if (in.demand_gpm <= in.release_gpm) {|    if (true) {"
"continuation-release-threshold-off-by-one|components/dhw_demand/dhw_demand_logic.h|    if (in.demand_gpm <= in.release_gpm) {|    if (in.demand_gpm < in.release_gpm) {"
"continuation-falsifies-on-the-firing-threshold|components/dhw_demand/dhw_demand_logic.h|    if (in.demand_gpm <= in.release_gpm) {|    if (in.demand_gpm <= in.demand_flow_threshold) {"
"continuation-releases-on-one-tick|components/dhw_demand/dhw_demand_logic.h|      return seen >= (uint32_t) in.release_ticks|      return seen >= 1u"
"continuation-never-releases-on-a-streak|components/dhw_demand/dhw_demand_logic.h|      return seen >= (uint32_t) in.release_ticks\n                 ? ContinuationVerdict::MEASURED_STOPPED\n                 : ContinuationVerdict::STOPPING;|      return ContinuationVerdict::STOPPING;"
"continuation-stopping-tick-drops-demand|components/dhw_demand/dhw_demand_logic.h|         v == ContinuationVerdict::STOPPING;|         false;"
"continuation-confirmation-never-reported|components/dhw_demand/dhw_demand_logic.h|    if (in.demand_gpm > in.demand_flow_threshold)\n      return ContinuationVerdict::CONFIRMED;|"
"dhw-confirmation-does-not-refresh-expiry|components/dhw_demand/dhw_demand.cpp|    if (result.continuation == ContinuationVerdict::CONFIRMED) {\n      pre_pump_on_flow_since_ms_ = now;\n    }|"
"dhw-stopping-streak-never-resets|components/dhw_demand/dhw_demand.cpp|    } else {\n      continuation_stopping_ticks_ = 0;\n    }|    }"
"meter-provenance-guard-removed|components/dhw_demand/dhw_demand_logic.h|      !reading_predates_pump_start(in.flow_last_update_ms, in.pump_on_since_ms,\n                                   in.now_ms);|      true;"
"continuation-never-expires|components/dhw_demand/dhw_demand_logic.h|  if ((in.now_ms - in.continuation_since_ms) >= in.max_ms)\n    return ContinuationVerdict::EXPIRED;|"
"continuation-expiry-rollover-unsafe|components/dhw_demand/dhw_demand_logic.h|  if ((in.now_ms - in.continuation_since_ms) >= in.max_ms)|  if (in.now_ms >= in.continuation_since_ms + in.max_ms)"
"continuation-unstamped-arm-holds|components/dhw_demand/dhw_demand_logic.h|  if (in.continuation_since_ms == 0)\n    return ContinuationVerdict::EXPIRED;|"
"continuation-dropped-meter-sample-falsifies|components/dhw_demand/dhw_demand_logic.h|    return ContinuationVerdict::METER_QUIET;|    return ContinuationVerdict::MEASURED_STOPPED;"
"continuation-meter-quiet-masks-the-real-exits|components/dhw_demand/dhw_demand_logic.h|  if (!std::isnan(in.demand_gpm)) {|  if (std::isnan(in.flow) || in.flow <= in.flow_threshold)\n    return ContinuationVerdict::METER_QUIET;\n  if (!std::isnan(in.demand_gpm)) {"
"dhw-falsified-capture-not-retired|components/dhw_demand/dhw_demand.cpp|      pre_pump_on_flow_ = NAN;\n      pre_pump_on_flow_since_ms_ = 0;\n      continuation_stopping_ticks_ = 0;\n    } else if (result.continuation == ContinuationVerdict::EXPIRED &&|    } else if (result.continuation == ContinuationVerdict::EXPIRED &&"
"dhw-expired-capture-not-retired|components/dhw_demand/dhw_demand.cpp|               pump_on_continuation_max_seconds_);\n      pre_pump_on_flow_ = NAN;\n      pre_pump_on_flow_since_ms_ = 0;|               pump_on_continuation_max_seconds_);"
"dhw-continuation-arm-never-stamped|components/dhw_demand/dhw_demand.cpp|      pre_pump_on_flow_since_ms_ = now;\n      continuation_stopping_ticks_ = 0;\n|      continuation_stopping_ticks_ = 0;\n"

# The dhw_in_use tier is a recall path for cells where nothing measured the
# loop (issue #173). Dropping the gate restores the state where a flag could
# overrule a measured no-draw -- 937 of 1007 replayed cells, every one of them
# recirculation. The second mutation inverts the gate instead, which silences
# the tier in exactly the cells that are its whole reason for existing.
"dhw-in-use-overrules-a-measured-no-draw|components/dhw_demand/dhw_demand_logic.h|  if (in.dhw_in_use_sustained && std::isnan(r.demand_gpm)) {|  if (in.dhw_in_use_sustained) {"
"dhw-in-use-fires-only-when-measured|components/dhw_demand/dhw_demand_logic.h|  if (in.dhw_in_use_sustained && std::isnan(r.demand_gpm)) {|  if (in.dhw_in_use_sustained && !std::isnan(r.demand_gpm)) {"
# Found by a skeptic pass: this one SURVIVED the original test set, because
# every gated case there sat at 0.00, 0.20 or 0.29 GPM and none was negative --
# while the cited median of the suppressed population is -0.021. It reopens the
# most common suppressed cell (loop reading fractionally above the meter, i.e.
# instrument mismatch rather than water) while looking like a null-safety tweak.
"dhw-in-use-reopens-the-negative-residual|components/dhw_demand/dhw_demand_logic.h|  if (in.dhw_in_use_sustained && std::isnan(r.demand_gpm)) {|  if (in.dhw_in_use_sustained && (std::isnan(r.demand_gpm) || r.demand_gpm < 0.0f)) {"
"frozen-motor-asserts-pump-off|components/dhw_demand/dhw_demand_logic.h|  } else if (out.motor_frozen) {\n    out.pump_on = true;|  } else if (out.motor_frozen) {\n    out.pump_on = false;"
"motor-staleness-mask-removed|components/dhw_demand/dhw_demand_logic.h|  out.speed_used = reading_is_fresh(in.motor_speed_last_update_ms, in.now_ms, in.motor_max_stale_ms)\n                       ? in.motor_speed\n                       : NAN;|  out.speed_used = in.motor_speed;"
"motor-current-staleness-mask-removed|components/dhw_demand/dhw_demand_logic.h|  out.current_used =\n      reading_is_fresh(in.motor_current_last_update_ms, in.now_ms, in.motor_max_stale_ms)\n          ? in.motor_current\n          : NAN;|  out.current_used = in.motor_current;"
"motor-speed-on-threshold|components/dhw_demand/dhw_demand_logic.h|    return motor_speed >= 10.0f;|    return motor_speed > 10.0f;"
"motor-channel-precedence-swapped|components/dhw_demand/dhw_demand_logic.h|  if (!std::isnan(motor_speed))\n    return motor_speed >= 10.0f;\n  if (!std::isnan(motor_current))\n    return motor_current >= pump_off_current_threshold;|  if (!std::isnan(motor_current))\n    return motor_current >= pump_off_current_threshold;\n  if (!std::isnan(motor_speed))\n    return motor_speed >= 10.0f;"
# The upload codec must accept a window that crosses midnight. It rejected them
# for a long time as an "inverted interval", which made the bulk path the only
# one that could not express a window the single-entry service, schedule_entry.h
# and the pump itself all support -- and broke read-then-upload for any grid
# containing one. Only the zero-length case is refused now.
"upload-rejects-midnight-crossing|components/alpha_hwr/schedule_codec.cpp|    if (begin == end)|    if (begin >= end)"
# The temperature-range write must refuse when the pump's own on/off-time
# LIMITS were never read. Those five bytes are echoed back verbatim (issue
# #106) and are only captured when the Sub 430 reply is long enough; a shorter
# reply leaves the cache looking valid and the limits unknown, and writing then
# sends ControlService's historical constants as if they were the pump's.
"temp-write-ignores-unknown-limits|components/alpha_hwr/write_operation_service.cpp|  if (!control_.temp_limits_known()) {|  if (false) {"
# Issue #234. A config write that goes unacknowledged is settled by the pump
# readback, not by the silence: REJECTED asserts the pump did not take the
# write, and nothing at the ACK deadline knows that. Both entries below restore
# the short circuit these replaced, one per config write, and both are killed by
# the case that matters -- the pump stored the values and only the
# acknowledgement was lost.
"temp-range-unacked-short-circuits|components/alpha_hwr/write_operation_service.cpp|          op->config_unacked = true;\n        }|          finish_(seq, WriteStatus::REJECTED, \"config write not acknowledged\");\n          return;\n        }"
"cycle-times-unacked-short-circuits|components/alpha_hwr/write_operation_service.cpp|            op->config_unacked = true;|            finish_(seq, WriteStatus::REJECTED, \"config write not acknowledged\");\n            return;"
# Deferring to the readback is only worth anything because the readback can say
# "the pump kept everything it had" -- without that the not-stored case lands on
# CLAMPED, which claims the pump holds something it chose. Two entries: one for
# the comparison, one for the capture that feeds it, since a capture taken after
# the write would compare the values against themselves and survive the first.
"temp-range-confirm-cannot-say-kept|components/alpha_hwr/write_operation_service.cpp|      status = kept_old ? WriteStatus::REJECTED : WriteStatus::CLAMPED;|      status = WriteStatus::CLAMPED;"
"temp-range-confirm-has-no-pre-values|components/alpha_hwr/write_operation_service.cpp|    op->pre_temp_min = control_.get_cached_temp_min();|    op->pre_temp_min = NAN;"
# The pre-write read has to be a READ. read_obj91_config() runs once per
# connection and is not in the periodic control poll (issue #54), so a baseline
# taken from the cache can be hours old; a GO-app edit in between makes an
# ignored write report CLAMPED against values nobody holds. This mutation keeps
# the callback and skips the wire read, which is precisely the cache-lookup
# version of the code.
"temp-range-baseline-taken-from-a-stale-cache|components/alpha_hwr/write_operation_service.cpp|  op->phase = Phase::RESOLVING;\n  control_.read_obj91_config([this, seq](bool ok) {|  op->phase = Phase::RESOLVING;\n  [](std::function<void(bool)> cb) { cb(true); }([this, seq](bool ok) {"
# The status got coarser, so the detail has to carry what it dropped: a settle
# that does not name the missing acknowledgement loses the only evidence that
# the pump never answered.
# The confirm ladder only runs if the operation is allowed to live long enough
# for it. On the old 10 s default the watchdog fired before CONFIG_MAX_ATTEMPTS
# could send a second readback, so a stored-but-unacknowledged write whose first
# readback was dropped settled `timeout` -- one wrong failure traded for another.
"config-write-budget-cannot-reach-the-retry|components/alpha_hwr/write_operation_service.h|  static constexpr uint32_t WATCHDOG_CONFIG_WRITE_MS = 26000;|  static constexpr uint32_t WATCHDOG_CONFIG_WRITE_MS = 10000;"
"temp-range-settle-drops-the-missing-ack|components/alpha_hwr/write_operation_service.cpp|    finish_(seq, status, unacked_detail(op->config_unacked, detail));|    finish_(seq, status, detail);"
"cycle-times-settle-drops-the-missing-ack|components/alpha_hwr/write_operation_service.cpp|      finish_(seq, WriteStatus::ACCEPTED, unacked_detail(op->config_unacked, \"\"));|      finish_(seq, WriteStatus::ACCEPTED, \"\");"
# Pump Ready must need BOTH caches. Until tests/test_component_wiring.cpp grew a
# case that withholds the schedule overview, every scenario filled both, so the
# gate was only ever observed agreeing and could be reduced to `return true`
# with the whole suite green.
"ready-gate-ignores-caches|components/alpha_hwr/alpha_hwr.cpp|  return control_service_.is_cache_valid() && schedule_service_.is_overview_cache_valid();|  return true;"
# The connect path replaced the opening sequence with a wait (issue #174). Both
# halves of it have to be real: the wait must be waited out, and the timer that
# ends it must not survive the connection it belongs to -- issue #15's defect in
# a new costume. Nothing else pins either; the deleted auth entries pinned the
# equivalents for a sequence that no longer exists.
#
# stabilize-window-is-not-waited-out is killed ONLY by the quiet-window
# assertion in test_the_full_connection_reaches_pump_ready(). With the constant
# at 0 the read chain starts immediately and the run still reaches Pump Ready,
# so a suite checking only the end state would let it survive. That coupling is
# deliberate: this mutation is what makes that assertion non-decorative.
"stabilize-window-is-not-waited-out|components/alpha_hwr/alpha_hwr.h|  static constexpr uint32_t SESSION_STABILIZE_MS = 2000;|  static constexpr uint32_t SESSION_STABILIZE_MS = 0;"
"stabilize-timer-outlives-a-disconnect|components/alpha_hwr/alpha_hwr.cpp|    this->cancel_timeout(SESSION_READY_TIMER);|    (void) 0;"
# Reaching READY is now the only thing the connect path does, so everything
# downstream hangs off one call site. Both were previously covered from the far
# side, by auth-never-reports-completion.
#
# Note the anchors. A bare two-space "  telemetry_service_.start();" is a
# SUBSTRING of the eight-space call in update(), and the applier replaces the
# first occurrence in the file -- so it would mutate update() instead and report
# green for a site nothing tested. That is not the loud exit-1 an unmatched
# search gives. Re-verify both anchors after any reindent of alpha_hwr.cpp.
"ready-never-starts-telemetry|components/alpha_hwr/alpha_hwr.cpp|  telemetry_service_.start();\n\n  // Pump Link Status|  // Pump Link Status"
"ready-never-triggers-the-initial-reads|components/alpha_hwr/alpha_hwr.cpp|  trigger_initial_data_reads();\n}|  (void) 0;\n}"
# The BLE lifecycle wiring, host-testable since issue #174's audit tail. Both
# of these shipped untested: alpha_hwr.cpp and ble_connection_manager.cpp were
# compiled only by `esphome compile`, so nothing could fail when they broke.
"scan-filter-ignores-product-bytes|components/alpha_hwr/ble_connection_manager.cpp|      if (d.size() >= 6 && d[3] == product_family && d[4] == product_type) {|      if (d.size() >= 6) {"
"gap-addr-filter-accepts-anyone|components/alpha_hwr/ble_connection_manager.h|    return client_ != nullptr && core::gap_addr_matches(bda, client_->get_remote_bda());|    return client_ != nullptr;"
# The session FSM had no tests at all until issue #174's audit tail, which is
# how a documented ERROR state with no way into it survived. These pin the
# transitions that do exist, so the removal of the ones that did not stays
# honest.
"session-is-connected-always-true|components/alpha_hwr/session.cpp|bool Session::is_connected() const { return state_ != SessionState::IDLE; }|bool Session::is_connected() const { return true; }"
"session-ready-transition-does-not-reach-ready|components/alpha_hwr/session.cpp|  transition_to(SessionState::READY,|  transition_to(SessionState::STABILIZING,"
"session-disconnect-does-not-reach-idle|components/alpha_hwr/session.cpp|  transition_to(SessionState::IDLE,|  transition_to(SessionState::READY,"
# The APDU length invariant (issue #174). Byte 1 declares the payload byte count
# in bits 5-0, and two frames shipped declaring a count they did not carry: the
# single-event write borrowed the layer write's 0xB3 (51) for a 19-byte payload,
# and the Class 10 setpoint write counted only its float and not the four ID
# bytes before it. This pump accepts either, so neither was a visible failure --
# tests/test_write_operations.cpp now checks every frame any test sends against
# its own declared length.
#
# Only one entry remains: the setpoint write is gone (issue #258), and with it
# `class10-setpoint-opspec-length`. The invariant it covered did not go with it
# -- the check in the harness is unconditional, and the single-event entry below
# still exercises it.
"single-event-opspec-length|components/alpha_hwr/schedule_service.cpp|  // never a visible failure; see the header note.\n  apdu[1] = 0x93;|  // never a visible failure; see the header note.\n  apdu[1] = 0xB3;"
# The APDU acknowledge field (issue #208). transport.cpp read a Class 10 0x81
# reply as a short ACK carrying an error code and called the write successful
# when that byte was zero -- so an Unknown Data Item error whose unknown ID
# happened to be 0x00 was reported accepted, which is exactly the frame captured
# on hardware. These pin the two bits, the length that lets a refusal match at
# all, and the verdict drawn from them.
"apdu-ack-reads-the-wrong-bits|components/alpha_hwr/response_match.h|  return static_cast<ApduAck>((apdu_head >> 6) & 0x03);|  return static_cast<ApduAck>((apdu_head >> 5) & 0x03);"
"apdu-ack-always-ok|components/alpha_hwr/response_match.h|inline bool apdu_ack_is_ok(uint8_t apdu_head) { return apdu_ack(apdu_head) == ApduAck::OK; }|inline bool apdu_ack_is_ok(uint8_t apdu_head) { (void) apdu_head; return true; }"
# The single-APDU assumption in frame_parser's OUTPUT (issue #226). A telegram
# may carry several APDUs -- App C.17 reports errors per-APDU -- and the parser
# used to return everything between the header and the CRC, handing the next
# APDU to callers as part of this one's payload.
"parser-payload-runs-past-the-first-apdu|components/alpha_hwr/frame_parser.cpp|    const size_t bounded = apdu1_end - offset;\n    result.payload = data + offset;\n    result.payload_len = (naive_len < bounded) ? naive_len : bounded;|    result.payload = data + offset;\n    result.payload_len = naive_len;"
"parser-never-flags-a-multi-apdu-telegram|components/alpha_hwr/frame_parser.cpp|  result.multi_apdu = (apdu1_end < body_limit);|  result.multi_apdu = false;"
"apdu-length-includes-the-ack-bits|components/alpha_hwr/response_match.h|inline uint8_t apdu_payload_len(uint8_t apdu_head) { return apdu_head & 0x3F; }|inline uint8_t apdu_payload_len(uint8_t apdu_head) { return apdu_head; }"
# A Class 10 reply carries TWO acknowledges. The head's says whether the APDU was
# understood; the byte after it is Class 10's own status -- OK / BUSY /
# OPERATION_FAILED, named by the GO app's decoder (GeniAPDU.CLASS10_ACK_*, read
# from raw[apdu_offset + 2]) and present in the captures with exactly those three
# values and no others.
# Retargeted when the decode moved into response_match.h so the read-refusal
# branch (issue #283) could share it instead of copying it. Same guarantee, one
# layer down and now covering both callers.
"class10-ack-byte-ignored|components/alpha_hwr/response_match.h|  reply.ok = class10_reply_is_ok(head, reply.has_payload, reply.class10_ack);|  reply.ok = apdu_ack_is_ok(head);"
"class10-ack-only-busy-rejected|components/alpha_hwr/response_match.h|  return first_payload == static_cast<uint8_t>(Class10Ack::OK);|  return first_payload != static_cast<uint8_t>(Class10Ack::BUSY);"
# The payload byte exists only on a 9-byte frame. At `len >= 7` an 8-byte
# CRC-valid frame declaring one payload byte has its CRC HIGH BYTE read as the
# Class 10 status -- and that byte now decides the verdict, not just a log line.
"class10-ack-reads-the-crc-byte|components/alpha_hwr/response_match.h|  reply.has_payload = apdu_payload_len(head) == 1 && frame_len >= 9;|  reply.has_payload = apdu_payload_len(head) == 1 && frame_len >= 7;"
# ---------------------------------------------------------------------------
# A reply's type and version decode at the real byte boundary (issue #281)
#
# The pair form the matcher holds splits the reply's [00][TypeH][TypeL][Version]
# header ONE BYTE OFF that boundary. Correct for matching, and meaningless to
# read: printed as an Object ID it gave `Object 55809 SubID 0` for a type that is
# ClockProgramOverview, which is verbatim the line quoted in #253 while a real
# bug was being chased. These pin the decode the logs now go through.
# ---------------------------------------------------------------------------
"reply-type-low-byte-taken-from-the-wrong-half|components/alpha_hwr/response_match.h|  const uint16_t type_lsb = static_cast<uint16_t>(type_low_ver >> 8);|  const uint16_t type_lsb = static_cast<uint16_t>(type_low_ver & 0xFF);"
"reply-version-is-the-high-byte|components/alpha_hwr/response_match.h|  return static_cast<uint8_t>(type_low_ver & 0xFF);|  return static_cast<uint8_t>(type_low_ver >> 8);"
# ---------------------------------------------------------------------------
# The control readback logs at INFO only when something moved (issue #265)
# ---------------------------------------------------------------------------
# The first reading after a connect. mode_valid_ is false until the pump has been
# read once, so without this the state is gated away against a cache nothing
# populated and never reaches the log at INFO at all.
"readback-gate-ignores-the-first-reading|components/alpha_hwr/control_service.h|  if (!had_prior_mode) return true;|  // mutated: a first reading is not a change"
# NAN is a value here, not a missing one. get_setpoint_for_mode() returns it for
# every mode with no scalar setpoint, and NAN != NAN -- so without this the gate
# reports a change on every poll of exactly those modes and reinstates the defect
# for a pump sitting in AutoAdapt or temperature-range control.
"readback-gate-compares-nan-with-not-equal|components/alpha_hwr/control_service.h|  if (std::isnan(prior_setpoint)) return false;|  // mutated: let NAN != NAN decide"
# Deliberately absent: a mutation on class10_reply_is_ok()'s `!has_payload` early
# return. Its only caller passes 0 for the status byte when there is none, so
# dropping the guard still compares 0 == OK and every outcome is unchanged -- an
# equivalent mutant, confirmed by experiment. The guard stays because the
# function's contract is "the byte may not exist" and a caller that passed the
# raw frame byte instead would read the CRC as a status code. Same reasoning as
# the parse_int_field ERANGE note below.
#
# Deliberately absent for the same reason: the apdu_is_set() term in the
# short-ACK condition. Every command reaching that branch is already a wildcard
# match (expect_type 0/0) AND has declared expect_short_ack, and nothing declares
# that for a GET -- so dropping the term changes no outcome today. It stays as
# the spec-correct way to say what the seven OpSpec constants it replaced had in
# common, and as defence for the writer who adds the first such GET.
#
# Issue #248: the mode write must be AWAITED. Its reply is byte-identical to the
# acknowledgement the config write sent 400 ms later is waiting for, and the
# short-ACK branch can only test the queued command's shape.
"mode-write-fired-and-forgotten|components/alpha_hwr/control_service.cpp|      MODE_ACK_TIMEOUT_MS, false, true, /*quiet_timeout=*/true);|      0, false, true, /*quiet_timeout=*/true);"
# The reply debt is the other half, and each of its three rules is load-bearing.
#
# Arming on EVERY timeout: exempting quiet ones excused the two sends most likely
# to reply late -- the mode write, acknowledged in every captured instance, and
# the schedule layer write, documented as replying after its window closes.
"stale-reply-not-recorded|components/alpha_hwr/transport.cpp|        this->note_reply_owed_(cmd.suppressed_a_frame);|        // mutated: no debt recorded"
# Paying the debt down: a frame that is merely declined leaves the debt standing,
# so the suppressed command times out, records a SECOND debt, and one late reply
# costs every acknowledgement after it. This is the cascade that failed 4 writes
# out of 4 against a healthy pump.
# Retargeted into consume_owed_reply_(), which both short-Class-10 branches now
# call -- a SET acknowledgement and a declined READ (issue #283) are the same
# nine bytes, so the payment rule has one home rather than two copies.
"stale-reply-debt-never-paid|components/alpha_hwr/transport.cpp|  if (this->owed_replies_ > 0) this->owed_replies_--;|  // mutated: leave the debt standing"
"stale-reply-suppressed-command-rearms|components/alpha_hwr/transport.cpp|  if (already_suppressed) return;|  // mutated: count it again"
# ---------------------------------------------------------------------------
# A Class 10 READ the pump declines (issue #283)
#
# The pump answers a read it cannot fulfil with the same nine bytes a write
# acknowledgement uses, so every term below is what keeps the branch from
# claiming a frame that is not its answer. #279 shipped this without them and
# was withdrawn after a skeptic pass reproduced three failures.
# ---------------------------------------------------------------------------
# The caller's declaration. Without it, an orphaned refusal to one of
# TelemetryService's callback-less reads completes an innocent read as a
# failure -- and does so most on the pumps the branch was written for, since a
# pump that refuses reads is a pump that generates these orphans.
"read-refusal-needs-no-declaration|components/alpha_hwr/transport.cpp|        cmd.expect_short_read_refusal && cmd.packet.size() > 5 &&|        cmd.packet.size() > 5 &&"
# The upper length bound. Byte 5 is the FIRST APDU's length on a multi-APDU
# telegram (#226), so without a ceiling any Class 10 telegram of any size whose
# first APDU declared 0 or 1 payload bytes reads as a refusal -- and App C.17
# makes "first APDU errored, second carries the answer" a documented case.
"read-refusal-has-no-upper-length-bound|components/alpha_hwr/transport.cpp|        queued_class == 0x0A && data[4] == 0x0A && len >= 8 && len <= 9 &&|        queued_class == 0x0A && data[4] == 0x0A && len >= 8 &&"
# GET, not "not a SET". There are three operations, and the negation also
# catches INFO -- the log then says "read declined" about one.
"read-refusal-admits-info|components/alpha_hwr/transport.cpp|        protocol::apdu_op(cmd.packet[5]) == protocol::ApduOp::GET;|        !protocol::apdu_is_set(cmd.packet[5]);"
# And the requirement that the reply actually said NOT-OK. A head-only frame
# whose acknowledges are both ok is byte-identical to a legitimate one-byte data
# reply; failing a read on it trades three seconds for a lie.
"read-refusal-claims-an-ok-frame|components/alpha_hwr/transport.cpp|      const bool declined = !reply.ok;|      const bool declined = true;"
# ---------------------------------------------------------------------------
# A bad-CRC frame drop is counted (issue #260)
#
# Without the count the drop leaves one log line and nothing else, which is the
# state that made a link shedding frames indistinguishable from a component that
# occasionally times out for no reason.
# ---------------------------------------------------------------------------
"crc-drops-not-counted|components/alpha_hwr/transport.cpp|       if (this->crc_drops_ < 0xFFFFFFFFu) this->crc_drops_++;|       // mutated: dropped silently, as before"
# And the window, which bounds how long an unpaid debt lingers.
"stale-reply-window-never-expires|components/alpha_hwr/transport.cpp|  if (millis() - this->owed_since_ms_ >= STALE_REPLY_WINDOW_MS) {|  if (false) {"
"stale-reply-window-too-short-for-the-tail|components/alpha_hwr/transport.h|  static constexpr uint32_t STALE_REPLY_WINDOW_MS = 500;|  static constexpr uint32_t STALE_REPLY_WINDOW_MS = 60;"
# Issue #259: what happens to a command nobody will ever answer.
#
# reset() used to clear the queue in silence, so a service with a read in flight
# heard nothing again -- no reply, no failure, and no timeout either, because the
# timeout lived in the queue entry that was just discarded. The suite pinned that
# as a hazard rather than testing against it. These say the repair holds.
"reset-drops-callbacks-silently|components/alpha_hwr/transport.cpp|  abandon_queue_();|  command_queue_.clear();"
"reset-reports-success-to-what-it-abandons|components/alpha_hwr/transport.cpp|    if (cmd.callback) {\n      cmd.callback(false, nullptr, 0);|    if (cmd.callback) {\n      cmd.callback(true, nullptr, 0);"
# A read chain continues past a failed step by sending the next read from inside
# the callback. Those land back in the queue the drain just emptied; without this
# loop the chain stops half-unwound and its caller's on_complete is never
# reached -- the original hang, moved one command along.
"reset-leaves-half-unwound-chains-queued|components/alpha_hwr/transport.cpp|    while (!this->command_queue_.empty()) {|    while (false) {"
# And the command being failed must be off the queue BEFORE its callback runs.
# `cmd` in loop() is a reference into the deque; a callback that reaches reset()
# would otherwise find its own entry still at the head and be invoked a second
# time from inside itself. This mutation restores the old order exactly.
"command-still-on-the-queue-during-its-callback|components/alpha_hwr/transport.cpp|  Command cmd = std::move(this->command_queue_.front());\n  this->command_queue_.pop_front();\n  if (cmd.callback) {\n    cmd.callback(ok, data, len);\n  }|  Command &cmd = this->command_queue_.front();\n  if (cmd.callback) {\n    cmd.callback(ok, data, len);\n  }\n  this->command_queue_.pop_front();"
# The drain is bounded, because a chain that re-sends on every failure would
# otherwise spin until the task watchdog fires. Mutated to a cap of zero rather
# than to no cap at all: removing it entirely makes the suite HANG, which this
# script reports as its own outcome and which would cost every full sweep the
# whole test timeout.
#
# Two entries, because the cap has to be BOTH present and large enough. A cap of
# 8 survived the whole suite when a skeptic tried it, and a cap of 8 would strand
# every read chain longer than eight commands on a disconnect -- the exact
# failure this change exists to fix. The tests now pin the count exactly.
#
# Both mutate the comparison rather than MAX_ABANDON_STEPS itself. Shrinking the
# constant no longer compiles: issue #284 gave EventLogService a static_assert
# that its chain plus headroom fits under the cap, so a cap of 0 or 8 is now a
# build error and the suite never runs -- an entry that proves nothing about
# coverage. The static_assert is real protection, but it guards the chain's size
# against the cap; these entries are about the drain honouring the cap at run
# time, which is a different claim and lives at the comparison.
"abandon-drain-cap-stops-it-dead|components/alpha_hwr/transport.cpp|    if (steps >= MAX_ABANDON_STEPS) {|    if (true) {"
"abandon-drain-cap-too-small-for-a-real-chain|components/alpha_hwr/transport.cpp|    if (steps >= MAX_ABANDON_STEPS) {|    if (steps >= 8) {"
# Issue #278: what the receiver will accept as a frame at all. The length field
# is bounded from both ends and the delimiter from one, and each bound has been
# got wrong at least once -- including while writing this change, where a floor
# taken from the capture corpus (5) rejected the 8-byte Unknown Class refusal
# that only ever appears in traffic the corpus does not contain.
"inbound-frame-accepts-the-request-delimiter|components/alpha_hwr/transport.cpp|  return byte == FRAME_START_RESPONSE;|  return byte == FRAME_START_RESPONSE || byte == FRAME_START_REQUEST;"
"frame-start-length-floor-removed|components/alpha_hwr/transport.cpp|  if (len >= 2) declares_a_possible_frame = data[1] >= protocol::MIN_LENGTH_FIELD;|  // mutated: no floor on the declared length"
# The floor must be exactly 4, and BOTH directions need an entry -- the first cut
# of this shipped only the "too high" one, and a skeptic set MIN_LENGTH_FIELD to
# 3 and to 2 with the whole suite staying green. At 5 the Unknown Class refusal
# stops being a frame; at 3 a fragment declaring 3 arms reassembly and swallows
# the frame behind it.
"frame-start-length-floor-excludes-a-refusal|components/alpha_hwr/frame_builder.h|static const uint8_t MIN_LENGTH_FIELD = 4;|static const uint8_t MIN_LENGTH_FIELD = 5;"
"frame-start-length-floor-too-low|components/alpha_hwr/frame_builder.h|static const uint8_t MIN_LENGTH_FIELD = 4;|static const uint8_t MIN_LENGTH_FIELD = 3;"
# The two reachability defects a skeptic pass found in the same function.
"lone-frame-start-never-learns-its-length|components/alpha_hwr/transport.cpp|    if (expected_packet_length_ == 0 && reassembly_buffer_.size() >= 2) {|    if (false) {"
"complete-frame-discarded-as-an-overflow|components/alpha_hwr/transport.cpp|  if (reassembly_buffer_.size() > MAX_PACKET_SIZE && still_incomplete) {|  if (reassembly_buffer_.size() > MAX_PACKET_SIZE) {"
# Deliberate absence: MAX_PACKET_SIZE's VALUE. It is now inert, and the entry
# that mutated it to 256 has been retired rather than left to survive.
#
# The ceiling is documented as the largest telegram the specification permits --
# LENGTH 255 plus the four bytes outside it -- and at the old 256 the three
# largest legal sizes were discarded as overflows. But what actually protects a
# legal frame is not the ceiling: it is the `still_incomplete` term on the guard,
# because a frame at its declared length has already satisfied the completion
# test and skips the guard whatever the cap says. Set the cap back to 256 with
# that term in place and a 259-byte frame still arrives intact.
#
# So the constant is kept for what it says rather than for what it does -- a
# buffer bound naming 256 when a GENI packet can be 259 is wrong documentation
# even when nothing reads it -- and the coverage lives on
# complete-frame-discarded-as-an-overflow, which removes the term and IS caught.
# Deliberate absence, as of issue #278: the inbound-overflow branch in
# on_notification() is now UNREACHABLE, so its three entries have been removed
# rather than left to survive. CI found all three surviving at once.
#
# Why. expected_packet_length_ is `data[1] + 4`, at most 259, and MAX_PACKET_SIZE
# is now that same 259 -- it was 256, three bytes under a legal frame, and that
# gap is what the branch existed to paper over. A buffer above the cap is
# therefore also at or past the expected length, which is the completion test, so
# the frame leaves through there instead. The only escape is an expected length
# of 0, which holds solely while the buffer has one byte in it.
#
# The branch stays as a backstop -- see the comment at the site -- but nothing
# can prove it, and pretending otherwise is what these entries were doing. The
# properties they asserted (losing frame sync must not touch the command queue,
# the peer-resync hold or the reply debt) are still tested, through the CRC-drop
# path that IS reachable. What is no longer claimed is that an overflow does it.
#
# The entry that remains on this ground is complete-frame-discarded-as-an-overflow,
# which is not about the branch firing: it removes the `still_incomplete` term and
# so makes the guard fire on a COMPLETE frame arriving with trailing bytes,
# destroying it. That is reachable, and caught.
# Reporting the failure is only half of it. The chain now reaches its terminal
# branch on the abandoned exit too, and that branch is where the display cache is
# written -- so both of these services need the readiness gate they already open
# with applied at the far end as well. Without it every dropped link replaces a
# good display with however many entries happened to land first, and it looks
# exactly like a short log rather than a truncated read.
"abandoned-history-read-cached-as-the-answer|components/alpha_hwr/history_service.cpp|      if (!session_.is_ready()) {|      if (false) {"
"abandoned-event-log-read-cached-as-the-answer|components/alpha_hwr/event_log_service.cpp|        if (!session_.is_ready()) {|        if (false) {"
# The re-entrancy guard, which this file previously recorded as a deliberate
# absence and an "equivalent mutant confirmed by experiment". Both halves of that
# were wrong, and the note contradicted itself two sentences later.
#
# The experiment behind it only removed the guard and ran the suite -- and the
# suite's only re-entrant case reset WITHOUT queueing first, so the nested call
# returned at abandon_queue_()'s emptiness check and never reached the guard at
# all. A read chain does the opposite: it sends the next command and could reach
# reset() after, and then the queue is not empty and the guard is the only thing
# standing between this and one recursive drain per chain step. MAX_ABANDON_STEPS
# does not help, because it is counted per call and each nested drain starts at
# zero. A skeptic reproduced the SIGSEGV.
"abandon-drain-re-enters-itself-per-chain-step|components/alpha_hwr/transport.cpp|  if (this->abandoning_) return;|  // mutated: no re-entrancy guard"
# Issue #253: the other four Class 10 sends, and the gate that lets any of them
# be answered.
#
# What admits a short ACK is now the caller's declaration -- expect_short_ack --
# rather than a list of five address shapes the transport recognised. Making the
# term unconditional widens the branch back to every Class 10 SET, including the
# ones that never asked to be answered, so the declaration has to be shown to be
# load-bearing rather than decorative.
"short-ack-taken-without-a-declaration|components/alpha_hwr/transport.cpp|        cmd.expect_short_ack &&|        true &&"
# One per converted send. Two shapes of mistake are represented, because both
# were made in this tree: declaring the wrong thing (the two control writes), and
# waiting for a reply the protocol cannot produce (the other three). The second
# is not hypothetical -- the schedule layer write shipped for a long time asking
# for type 0xDE01, which a SET reply can never carry ("the SET operation never
# returns anything but the APDU Head", App. Prog. Manual fig 3.5 note 1), so it
# burned a full 3 s window on every layer of every schedule write while
# quiet_timeout kept the timeout at DEBUG.
#
# One fewer than there were converted sends: the setpoint register write is gone
# (issue #258), so `setpoint-write-not-declared-as-awaiting-an-ack` went with it.
# Its sibling below covers the same declaration on the write that remains.
"control-request-not-declared-as-awaiting-an-ack|components/alpha_hwr/control_service.cpp|      /*expect_short_ack=*/true, /*quiet_timeout=*/true);\n\n  if (queue_commit && schedule_callback_) {|      /*expect_short_ack=*/false, /*quiet_timeout=*/true);\n\n  if (queue_commit && schedule_callback_) {"
"clock-write-expects-a-type-a-set-cannot-return|components/alpha_hwr/time_service.cpp|      apdu, sizeof(apdu), 0, 0,\n      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {|      apdu, sizeof(apdu), 0x0141, 0,\n      [](bool success, const uint8_t * /*data*/, size_t /*len*/) {"
"commit-write-expects-a-type-a-set-cannot-return|components/alpha_hwr/schedule_service.cpp|      apdu, apdu_len, 0, 0,|      apdu, apdu_len, 0xDA01, 0,"
# Three Object 84 writes made the same mistake, so each gets its own entry: the
# search strings need the lambda capture list because `apdu, sizeof(apdu), 0, 0,`
# now appears three times in this file.
"layer-write-expects-a-type-a-set-cannot-return|components/alpha_hwr/schedule_service.cpp|      apdu, sizeof(apdu), 0, 0,\n      [this, on_complete, layer]|      apdu, sizeof(apdu), 0xDE01, 0,\n      [this, on_complete, layer]"
"schedule-enable-expects-a-type-a-set-cannot-return|components/alpha_hwr/schedule_service.cpp|      apdu, sizeof(apdu), 0, 0,\n      [enable, on_sent]|      apdu, sizeof(apdu), 0xDA01, 0,\n      [enable, on_sent]"
"single-event-write-expects-a-type-a-set-cannot-return|components/alpha_hwr/schedule_service.cpp|      apdu, sizeof(apdu), 0, 0,\n      [this, on_complete, event]|      apdu, sizeof(apdu), 0xDC01, 0,\n      [this, on_complete, event]"
# The wait itself has to be long enough to collect the reply. At zero every
# awaited write times out before the pump can answer, and each timeout records a
# reply debt that spends the NEXT write's acknowledgement -- so the failure is
# not the write that was rushed but the one after it, which is why it needs an
# entry rather than an argument.
"set-ack-timeout-is-zero|components/alpha_hwr/transport.h|  static constexpr uint32_t SET_ACK_TIMEOUT_MS = 400;|  static constexpr uint32_t SET_ACK_TIMEOUT_MS = 0;"
# The telegram size ceilings are the protocol's, and the buffer has to hold the
# largest legal one: MAX_PDU_LEN yields a 257-byte telegram, which did not fit
# the 256-byte buffers callers declared.
"builder-cap-is-the-length-byte-not-the-pdu|components/alpha_hwr/frame_builder.cpp|  if (length > protocol::MAX_PDU_LEN) {|  if (length > 255) {"
# The pre-#208 match condition. A 0xC1 or 0x41 refusal fails this test, falls
# past the len >= 11 floor, and dies by 3 s timeout instead of being reported.
"short-ack-matches-only-the-two-known-heads|components/alpha_hwr/transport.cpp|protocol::apdu_payload_len(data[5]) <= 1 &&|(data[5] == 0x01 || data[5] == 0x81) &&"
# Unknown Class declares a ZERO-length payload, so its head is 0x40 and its
# frame is 8 bytes. `== 1` admits 0x41 -- which App C.17's format table says
# cannot occur -- while rejecting the 0x40 that does, leaving it to die by 3 s
# timeout. The first cut of #208 shipped exactly that; an adversarial review
# caught it, not the suite. This entry is what makes the suite catch it.
"short-ack-misses-the-zero-length-refusal|components/alpha_hwr/transport.cpp|protocol::apdu_payload_len(data[5]) <= 1 &&|protocol::apdu_payload_len(data[5]) == 1 &&"
# A refusal must be reported as ANSWERED to the config-write callers.
#
# It originally guarded their no-readback short-circuit, which would have turned
# a misattributed refusal into a REJECTED verdict for a write that landed. Issue
# #234 removed that short-circuit, so the two no longer differ in status -- and
# this entry survived the sweep for exactly one run, which is how the gap was
# found. What it guards now is the settle DETAIL: reporting a refusal as silence
# marks the operation unacknowledged, so an event about a pump that replied in
# milliseconds would claim nothing came back.
# NOTE the shape of this search string. The obvious one contains `success ||
# data != nullptr`, and this file's format splits fields on `|` -- so a `||` in
# a search silently truncates the field and the entry cannot apply. It reports
# "not applied" and exits 1 rather than scoring Survived, which is how this was
# caught, but the fix is to keep `|` out of the string: hence the named
# `answered` local in control_service.cpp, which is clearer code regardless.
"config-write-treats-a-refusal-as-silence|components/alpha_hwr/control_service.cpp|        if (on_ack) on_ack(answered);\n      },\n      3000, false, true); // 3000ms timeout, no register read, expect short ACK|        if (on_ack) on_ack(success);\n      },\n      3000, false, true); // 3000ms timeout, no register read, expect short ACK"
"sensor-pub-media-temp-range-removed|components/alpha_hwr/sensor_publisher.cpp|    if (temp.media_temperature_c >= -20 && temp.media_temperature_c <= 100) {|    if (true) {"
"sensor-pub-alarm-dedup-removed|components/alpha_hwr/sensor_publisher.cpp|  if (alarms_sensor_->has_state() && alarms_sensor_->state == codes_str) {|  if (false) {"
"sensor-pub-head-rate-gap-reset-removed|components/alpha_hwr/sensor_publisher.cpp|      if (dt_s > 30.0f) {|      if (false) {"
"dhw-motor-freshness-never-stamped|components/dhw_demand/dhw_demand.cpp|    motor_speed_->add_on_state_callback([this](float v) {\n      if (!std::isnan(v))\n        motor_speed_last_update_ms_ = millis();\n    });|    motor_speed_->add_on_state_callback([](float) {});"
"dhw-flow-latch-never-active|components/dhw_demand/dhw_demand.cpp|  for (int i = 0; i < samples; i++) {|  for (int i = 0; i < 0; i++) {"
"dhw-deriv-restarts-across-nan-gap|components/dhw_demand/dhw_demand.cpp|  if (std::isnan(current)) {\n    // Both prev and prev_ms are intentionally left unchanged so the next valid\n    // reading computes dt_s over the true elapsed time (spanning any NaN gap),\n    // not just a single tick.\n    return NAN;\n  }|  if (std::isnan(current)) {\n    prev_ms = now;\n    return NAN;\n  }"
"dhw-confidence-publish-ungated|components/dhw_demand/dhw_demand.cpp|    publish_sensor_if_changed(confidence_sensor_, confidence * 100.0f);|    confidence_sensor_->publish_state(confidence * 100.0f);"
"dhw-release-hold-not-reported|components/dhw_demand/dhw_demand.cpp|    method = \"demand_release_hold\";\n    confidence = 0.5f;|    confidence = 0.5f;"
"dhw-flow-onset-unqualified|components/dhw_demand/dhw_demand.cpp|  bool prev_flow_present_pump_off = prev_tick_confirms_flow_onset(\n      prev_flow_, flow_threshold_, prev_pump_confirmed_off_);|  bool prev_flow_present_pump_off = prev_tick_confirms_flow_onset(\n      prev_flow_, flow_threshold_, true);"
"control-enabled-from-opmode|components/alpha_hwr/control_service.cpp|  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled\n  pump_enabled_ = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));|  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled\n  pump_enabled_ = true;"
# The single-event confirm compared the window and the enabled flag but not the
# ACTION -- and the two actions are opposites: 0x01 Stop holds the pump off
# across the window (a vacation), 0x02 Run turns it on. A pump that stored the
# wrong kind settled ACCEPTED, so a vacation could be confirmed as set while
# the pump was in fact scheduled to run for the whole week.
# The local<->UTC shift at the ends of the uint32 the wire carries (issue #263).
# Both directions used to wrap modulo 2^32, and the two wraps CANCELLED: the
# confirm readback shifted back by the same offset, wrapped symmetrically,
# compared equal, and settled the operation ACCEPTED for an event the pump had
# stored at an instant nobody asked for. Low likelihood -- about a day's worth
# of instants out of 136 years -- and a false success rather than an error,
# which is the combination worth a guard.
#
# The two directions are guarded differently on purpose, so both halves are
# pinned: encoding REFUSES (the caller named an instant this pump cannot store),
# decoding SATURATES (the value is already on the pump; there is nothing to
# refuse, and saturating keeps a pair ordered and its expiry answerable).
"shift-encode-ceiling-wraps|components/alpha_hwr/schedule_service.h|  if (shifted > static_cast<int64_t>(UINT32_MAX))\n    return false;|  if (false)\n    return false;"
"shift-encode-floor-wraps|components/alpha_hwr/schedule_service.h|  if (shifted < 1)\n    return false;|  if (false)\n    return false;"
"shift-decode-floor-not-saturated|components/alpha_hwr/schedule_service.h|  if (shifted < 1)\n    return 1u;|  if (shifted < 1)\n    return 0u;"
"shift-decode-ceiling-not-saturated|components/alpha_hwr/schedule_service.h|  if (shifted > static_cast<int64_t>(UINT32_MAX))\n    return UINT32_MAX;|  if (false)\n    return UINT32_MAX;"
# The argument-level refusal, which settles ahead of the overview read so a
# wrong argument is not reported as a link failure.
"single-event-unrepresentable-window-accepted|components/alpha_hwr/write_operation_service.cpp|    const bool both_ends_fit = begin_fits && end_fits;|    const bool both_ends_fit = true;"
# Deliberately absent: the same guard at the wire, in
# ScheduleService::write_single_event_async(). An equivalent mutant, and by
# construction rather than by experiment -- its sole caller is
# WriteOperationService::write_single_event_(), every path to which runs the
# argument check above on the same two timestamps first, and a CLEAR carries the
# 0/0 sentinel, which always fits. The guard is kept as a choke point on a
# public method; adding the entry only produced a survivor.
#
# This is the fourth time in three changes that tightening one bound made
# another unreachable and turned its entry into an equivalent mutant. Worth
# expecting rather than rediscovering.
# 0 is the wire's disabled/cleared sentinel, never shifted in either direction,
# so an enabled event beginning at 0 confirmed clean while describing a slot
# that says "cleared".
"epoch-field-accepts-the-cleared-sentinel|components/alpha_hwr/api_bridge.cpp|  if (!parse_int_field(s, 1, EPOCH_MAX_TS, &v)) return false;|  if (!parse_int_field(s, 0, EPOCH_MAX_TS, &v)) return false;"
# A window that has already ended (issue #269). It was written, confirmed by
# readback and settled ACCEPTED, after which it was recyclable garbage in one of
# five slots. A write that cannot do anything, reported as a success.
"single-event-past-window-accepted|components/alpha_hwr/write_operation_service.cpp|    const bool refuse_as_past = clock_is_known && window_has_ended;|    const bool refuse_as_past = false;"
# Only the END decides: a window that has BEGUN but not ended is legitimate and
# must still run. Mutating the test to the begin refuses every "start now, stop
# at six" request -- which is what the Lovelace card's Quick Run presets send.
"single-event-past-window-tested-on-the-begin|components/alpha_hwr/write_operation_service.cpp|    const bool window_has_ended = op->end_ts <= now_ts;|    const bool window_has_ended = op->begin_ts <= now_ts;"
#
# Deliberately absent: `clock_is_known` on its own. With no clock now_ts is 0 and
# `end_ts <= 0` is false for every window the parser admits (both epoch fields
# floor at 1, and the ordering rule needs end > begin), so dropping the term
# changes no reachable behaviour. It is written out because the rule -- a node
# that cannot tell the time must not call somebody else's timestamp stale -- is
# the point, not because the arithmetic needs it.

# The read path converts the pump's LOCAL-Unix wire value back to UTC, and the
# cache invariant that it does is load-bearing: the slot picker compares cached
# end_timestamps against the node clock (#262) and the confirm compares a
# readback against the request. The write-op suite pins TZ=UTC, where the
# conversion is the identity, so both of these mutants used to pass it (issue
# #268). They are caught by the two tests that un-pin the zone.
"tz-read-path-leaves-the-cache-in-local-time|components/alpha_hwr/schedule_service.cpp|            ev.begin_timestamp = local_unix_to_utc_resolved(ev.begin_timestamp);\n            ev.end_timestamp = local_unix_to_utc_resolved(ev.end_timestamp);|            // mutated: cache keeps the pump's local-Unix values"
"tz-single-slot-read-leaves-local-time|components/alpha_hwr/schedule_service.cpp|        ev.begin_timestamp = local_unix_to_utc_resolved(ev.begin_timestamp);\n        ev.end_timestamp = local_unix_to_utc_resolved(ev.end_timestamp);|        // mutated: single-slot read keeps the pump's local-Unix values"

# A vacation that has already ended (issue #267). find_vacation_slot() returned
# the first enabled Stop in slot order with no clock, so a FINISHED vacation in
# an early slot shadowed a live one later: clear_vacation cleared the finished
# one and settled `accepted`, telling the user the vacation was over while the
# pump was still holding itself off.
"vacation-slot-ignores-the-window|components/alpha_hwr/schedule_service.cpp|    const bool covers_now =\n        ev.begin_timestamp <= now_ts && now_ts < ev.end_timestamp;|    const bool covers_now = true;"
"vacation-slot-takes-the-furthest-out|components/alpha_hwr/schedule_service.cpp|          : ev.begin_timestamp < soonest_upcoming->begin_timestamp;|          : ev.begin_timestamp > soonest_upcoming->begin_timestamp;"
# The third case, deliberate rather than by omission: an ended vacation is still
# an enabled Stop holding one of five slots, so refusing to clear it would leave
# no way to reclaim it.
"vacation-slot-cannot-reclaim-a-finished-one|components/alpha_hwr/schedule_service.cpp|  if (latest_ended != nullptr) {\n    *when = VacationWhen::ENDED;|  if (false) {\n    *when = VacationWhen::ENDED;"
# The second symptom of the same defect: the Vacation text sensor named a
# finished vacation as THE vacation.
"vacation-display-names-a-finished-one|components/alpha_hwr/schedule_service.cpp|  const bool nothing_to_show = ev == nullptr |  const bool nothing_to_show = ev == nullptr; //"
# ...and that the caller hands it a real clock rather than the unknown sentinel.
# Retargeted when clear_vacation went from picking ONE vacation to clearing every
# one covering now (issue #290). Same guarantee: without the node's clock,
# nothing can be judged to cover now, and the resolver falls back to the
# unknown-clock ranking -- which is #267's defect exactly.
"vacation-slot-caller-passes-no-clock|components/alpha_hwr/write_operation_service.cpp|            schedule_service_.find_vacation_slots(time_service_.now_unix(), &slots);|            schedule_service_.find_vacation_slots(0, &slots);"
"single-event-confirm-ignores-the-action|components/alpha_hwr/write_operation_service.cpp|                                  actual.action == op->single_event_action;|                                  true;"
# ...and the skip for a CLEAR must stay a skip: a cleared slot's window is
# meaningless, so comparing it would reject every successful clear.
"single-event-confirm-checks-a-cleared-window|components/alpha_hwr/write_operation_service.cpp|      const bool content_is_a_verdict = want_enabled ? window_matches : true;|      const bool content_is_a_verdict = window_matches;"
# The auto-slot resolver picks a slot by asking which stored events have
# EXPIRED, and it used to ask that against the new event's own begin timestamp
# rather than against the clock. The two agree for an event a few minutes out --
# which is what the Lovelace card's Quick Run presets produce -- and part
# company completely for one years out: a 2040 event makes everything in the
# next thirteen-odd years look expired, so the picker returns a slot holding a
# live event and the write destroys it, settling ACCEPTED. This restores exactly
# the line issue #262 reported, bench-observed with four slots free.
#
# It needs a fixture whose timestamps mean something. The single-event tests
# anchor their windows to the node clock for that reason: an event stamped in
# 1970 is expired against any real clock, so a fixture calling one "live" is
# live only relative to the new event's begin and stops meaning anything the
# moment the right question is asked.
"single-event-expiry-measured-from-the-new-event|components/alpha_hwr/write_operation_service.cpp|      const uint32_t now_ts = time_service_.now_unix();|      const uint32_t now_ts = op->begin_ts;"
# ...but only when it has to. An EMPTY slot beats a recyclable one, always:
# recycling costs the stored record of an event that ran, and the picker used to
# spend it while four slots sat unused, because it took the first index no LIVE
# event held. On a five-slot pump that meant repeated one-time runs cycled
# through slot 0 forever, each carrying a "this slot was recycled" warning about
# nothing anyone would miss -- which is how a warning stops meaning anything.
"single-event-recycles-while-a-slot-sits-empty|components/alpha_hwr/schedule_service.cpp|    if (cached.find(i) == cached.end())|    if (false)"
# ...and when it does have to recycle, the stalest record is the one to lose.
# Flipping the comparison keeps the oldest event and throws away the most
# recently finished one, which is backwards, and no status assertion notices:
# the write still lands, still confirms, still settles ACCEPTED.
"single-event-recycles-the-freshest-not-the-stalest|components/alpha_hwr/schedule_service.cpp|    const bool ended_earlier = ended < stalest_end;|    const bool ended_earlier = ended > stalest_end;"
# Recycling a slot destroys what was in it. Legitimate -- the event had ended --
# and it used to be silent, which is most of why #262 was expensive to diagnose:
# the operation settles ACCEPTED because the write really did land, and nothing
# said the slot had been occupied. Dropping the note is invisible to every
# status assertion in the suite.
# Retargeted when the ACCEPTED detail gained the vacation-clear wording (issue
# #290). The slot note is now the value `detail` starts from, so blanking it
# there is the same mutation one line up.
"single-event-slot-reuse-is-silent|components/alpha_hwr/write_operation_service.cpp|        std::string detail = op->slot_note;|        std::string detail;"
# ...and the note has to be about the slot actually taken. Reporting the first
# cached event instead names a slot that was never touched and a window that
# still exists -- a worse lie than saying nothing, since it reads as a
# destruction that did not happen.
"single-event-reuse-note-names-any-slot|components/alpha_hwr/write_operation_service.cpp|        const bool recycling_this_slot = ev.enabled && ev.index == slot;|        const bool recycling_this_slot = ev.enabled;"
# A node with no synced clock cannot say what has expired, so the picker treats
# every enabled event as holding its slot and the pool reads full. That is the
# safe direction, but it makes a node that has simply never synced look like a
# pump with five live events -- unless the refusal says which of the two it is.
"single-event-full-pool-hides-a-missing-clock|components/alpha_hwr/write_operation_service.cpp|                now_ts == 0|                false"
# The mirror. Blaming the clock for a pool that is genuinely full of LIVE events
# sends the reader after a problem the node does not have -- the same defect as
# the entry above, pointing the other way.
"single-event-clock-blamed-for-a-full-pool|components/alpha_hwr/write_operation_service.cpp|                now_ts == 0|                true"
# event_type is the only thing distinguishing a vacation from a one-time run in
# the settle event -- they share a command string. Without it the two API
# handlers are interchangeable and nothing notices.
"bridge-single-event-omits-the-event-type|components/alpha_hwr/api_bridge.cpp|      if (result.single_event_action == 0x01) data[\"event_type\"] = \"stop\";|      // mutated: no event_type"
# The Home Assistant surface. api_bridge.cpp had NO mutation target before
# tests/test_api_bridge.cpp, and could not have had one: the mock defines.h
# omitted the USE_API family, so the file compiled out to an object with zero
# symbols even though it sat in test_component_wiring's link line. Its own
# setup() comment named the hazard -- "pairing a handler with the wrong
# enumerator below compiles, passes the whole suite and passes the firmware
# build" -- and the first entry here is that hazard, in its invisible form:
# same signature, same argument list, plausible name. Only calling the service
# and reading the command back off the settle event catches it.
"bridge-handler-paired-with-wrong-command|components/alpha_hwr/api_bridge.cpp|  register_service(&AlphaHwrApiBridge::on_set_schedule_entry,\n                   name(WriteCommand::SET_SCHEDULE_ENTRY), {\"data\", \"op_id\"});|  register_service(&AlphaHwrApiBridge::on_clear_schedule_entry,\n                   name(WriteCommand::SET_SCHEDULE_ENTRY), {\"data\", \"op_id\"});"
# `enabled` on a schedule command is the SCHEDULE flag, not the run state. The
# docs claimed the opposite for a long time and so did the comment beside the
# remote-mode key; sourcing it from result.enabled would make the claim true
# and every schedule event wrong.
"bridge-schedule-enabled-from-run-state|components/alpha_hwr/api_bridge.cpp|      put_bool(\"enabled\", result.sched_enabled);|      put_bool(\"enabled\", result.enabled);"
# Remote mode must not overload `enabled`, which already means two things.
"bridge-remote-mode-overloads-enabled|components/alpha_hwr/api_bridge.cpp|      put_bool(\"remote_enabled\", result.enabled);|      put_bool(\"enabled\", result.enabled);"
# schedule_hash on the event, not only on the sensor: it exists so a client can
# learn what the pump holds without polling the sensor and racing the
# republish. It was emitted and documented nowhere until the step-7 pass.
"bridge-upload-omits-schedule-hash|components/alpha_hwr/api_bridge.cpp|        if (!result.schedule_hash.empty()) data[\"schedule_hash\"] = result.schedule_hash;|        // mutated: no schedule_hash on the event"
# node makes an event self-identifying across a multi-controller install
# (issue #113), where HA's device_id is opaque and re-add-unstable.
"bridge-event-drops-the-node-name|components/alpha_hwr/api_bridge.cpp|  data[\"node\"] = App.get_name();|  // mutated: no node on the event"
# A malformed request settles INVALID at the bridge -- deterministic, never
# worth a retry -- rather than REJECTED, which invites one.
# The bridge's argument parsing, which had never been compiled by a test and
# was wrong three ways for input any HA user can send. Each rule gets its own
# pipe-free line in parse_int_field() so it can be anchored here.
"bridge-parser-accepts-trailing-garbage|components/alpha_hwr/api_bridge.cpp|  if (!consumed_everything) return false;|  // mutated: ignore anything after the number"
# Deliberately absent: a mutation on parse_int_field's ERANGE check. strtoll
# clamps to LLONG_MAX/LLONG_MIN on overflow, and every call site passes a range
# far inside those, so `v > hi` / `v < lo` already reject anything ERANGE could
# flag -- an equivalent mutant no test can kill. The check stays as defence for
# a future caller with a wider range; it is not load-bearing today, and an
# entry claiming otherwise would be a false guarantee. Confirmed by experiment:
# the mutation survived the full suite.
#
# Deliberately absent: mutations narrowing `using ParseInt = long long` back to
# `long`, or `std::strtoll` back to `std::strtol` -- issue #255, both halves.
# Neither can be scored here, because both fail to COMPILE: a static_assert in
# parse_int_field() ties the bound type to the parse, and `long` and `long long`
# are distinct types even where both are 64 bits wide. A mutation that does not
# compile is scored a SURVIVOR by this script on purpose -- the suite never ran,
# so the entry would prove nothing about coverage -- so an entry here would fail
# the sweep while the code is correct.
#
# They are not uncovered. `make test-ilp32` (CI: "Unit tests (32-bit long)")
# rebuilds test_api_bridge with -m32, where `long` is 32 bits as it is on the
# ESP32-C3, and the existing accepted-input cases fail against either narrowing.
# That is the regression net; the static_assert is the early stop.
"bridge-parser-accepts-leading-junk|components/alpha_hwr/api_bridge.cpp|  if (!starts_cleanly) return false;|  // mutated: let strtol skip whitespace and signs"
# Timestamps are compared AFTER narrowing to the wire's 32 bits; comparing the
# wider parse let an ordered pair reach the pump reversed.
"bridge-epoch-range-not-checked|components/alpha_hwr/api_bridge.cpp|  if (!parse_int_field(s, 1, EPOCH_MAX_TS, &v)) return false;|  if (!parse_int_field(s, 1, 999999999999LL, &v)) return false;"
# The ceiling is the wire's uint32, not time_t's int32: ClockProgramSingleEvent
# declares `begin` and `end` as uint32_t, so the pump holds instants up to 2106.
# Stopping at 2147483647 would refuse dates the pump accepts (issue #255).
"bridge-epoch-ceiling-stops-at-2038|components/alpha_hwr/api_bridge.cpp|static constexpr ParseInt EPOCH_MAX_TS = 4294967295;|static constexpr ParseInt EPOCH_MAX_TS = 2147483647;"
"bridge-epoch-order-checked-before-narrowing|components/alpha_hwr/api_bridge.cpp|  return *begin < *end;|  return true;"
# An infinity is not a number a client can parse. The contract is a real value
# or no key -- the same reason NaN was excluded.
"bridge-float-emits-infinity|components/alpha_hwr/api_bridge.cpp|    if (!std::isfinite(value)) return;|    if (std::isnan(value)) return;"
# reject_ echoes its argument into an event map that gets copied into an API
# message on a device with tens of KB of heap.
"bridge-detail-echo-unbounded|components/alpha_hwr/api_bridge.cpp|  constexpr size_t MAX_ECHO = 64;|  constexpr size_t MAX_ECHO = 1000000;"
# A write that never reached the pump must not report a concrete pump state.
# Both caches can be invalid, and -1 is what the event encoding already had.
"bridge-pump-state-invents-a-known-state|components/alpha_hwr/alpha_hwr.h|    auto tri = [](bool known, bool value) -> int8_t { return known ? (value ? 1 : 0) : -1; };|    auto tri = [](bool known, bool value) -> int8_t { (void) known; return value ? 1 : 0; };"
# Issue #302: the entity path's terminal event. Toggling a coupled switch has
# to end in exactly one set_pump_state result, or a client waiting on the
# dashboard write waits forever -- which is what it did before, in the corner
# where the pump already matched the target and nothing was submitted at all.
"entity-toggle-never-settles|components/alpha_hwr/alpha_hwr.h|          write_op_service_.emit_result(result);|          (void) result;"
# The same event reported as a service call. `origin` is the only thing telling
# an automation that a person moved a switch rather than its own script writing.
"entity-toggle-reports-itself-as-a-service|components/alpha_hwr/alpha_hwr.h|          result.origin = origin;|          result.origin = services::WriteOrigin::SERVICE;"
# The no-op arm. Without it a toggle the pump already matches submits nothing
# and aggregates nothing, so the fan-in never fires and the terminal event is
# never built -- issue #302 exactly as filed.
"pump-state-no-op-settles-nothing|components/alpha_hwr/alpha_hwr.h|    if (!need_engaged && !need_scheduled) {|    if (false) {"
# The sub-writes take the caller's sub_op_id, which is empty for a switch and
# the repair's tag for the repair -- never the terminal event's op_id. A leak
# here would put three events under a service caller's op_id where the contract
# promises one.
"pump-state-sub-writes-leak-an-op-id|components/alpha_hwr/alpha_hwr.h|      write_op_service_.submit_set_enabled(target.pump_enabled, sub_op_id, nullptr, origin, step);|      write_op_service_.submit_set_enabled(target.pump_enabled, \"leaked\", nullptr, origin, step);"
# state_name(pump_auto, schedule_on) is asymmetric, so its arguments can be
# swapped without a compiler complaint: a running unscheduled pump would then
# report "off". The test fixtures were symmetric (both flags 1) and could not
# see it; they cover both asymmetric combinations now.
"bridge-state-name-args-swapped|components/alpha_hwr/api_bridge.cpp|        data[\"state\"] = ux::state_name(result.enabled != 0, result.sched_enabled != 0);|        data[\"state\"] = ux::state_name(result.sched_enabled != 0, result.enabled != 0);"
# -1 means "not known". Without the guard, -1 != 0 reads as true and a write
# that never reached the pump reports a fabricated state.
"bridge-state-fabricated-from-unknown-flags|components/alpha_hwr/api_bridge.cpp|      if (result.enabled >= 0 && result.sched_enabled >= 0) {|      if (true) {"
# Issue #243: the diagnostic suspend. Two calls, and the guards are the whole of
# it -- without them the component undoes its own switch, because the
# reconnect-settle path turns auto-connect off on a disconnect and schedules a
# timer that turns it back on. A suspend IS a disconnect, so it trips that path
# with its own teardown.
#
# The timer guard is the load-bearing one. Its sibling in the disconnect handler
# is deliberately not listed: removing it leaves the suspension intact, because
# the timer still declines to act, so it is belt-and-braces rather than the fix
# and no test can distinguish it. Verified by removing each in turn.
"suspend-undone-by-the-reconnect-settle-timer|components/alpha_hwr/alpha_hwr.cpp|    // reading. Raised in review on #285.\n    if (this->suspended_) {|    // reading. Raised in review on #285.\n    if (false) {"
# A link taken down on purpose is not Unreachable and not a fault. Both of these
# are what an operator glancing at Home Assistant sees, and what an automation
# reads -- releasing forgets the node's own 0x16 teardown and nothing else, so a fault
# that was already showing survives the toggle and a genuine failure of the
# reconnect still publishes.
# Releasing a long suspension must restart the Unreachable clock. That rung is
# `now - link_last_open_ms_ > LINK_UNREACHABLE_MS`, and the stamp only advances
# while the session is READY, so it stops the moment the link goes down --
# leaving any suspension past the threshold to resume through a spurious
# Unreachable. Reported by the person who wrote the same bug in their own
# implementation and caught it at 85 seconds.
"suspend-release-leaves-the-unreachable-clock-stopped|components/alpha_hwr/alpha_hwr.cpp|    this->link_last_open_ms_ = millis();\n    // ...but NOT if a reconnect-settle hold is still in force.|    // mutated: clock left stopped\n    // ...but NOT if a reconnect-settle hold is still in force."
"suspended-link-reads-as-a-failed-one|components/alpha_hwr/alpha_hwr.cpp|  if (this->suspended_) {\n    // Ahead of every other rung|  if (false) {\n    // Ahead of every other rung"
"suspend-fault-mask-removed|components/alpha_hwr/alpha_hwr.cpp|    bool show_none = this->suspended_;|    bool show_none = false;"
# Three more from the review on #285, all reproduced before fixing.
#
# Releasing must not spend an active reconnect-settle hold. That hold exists
# because a premature encryption request into a not-ready pump can fail with
# 0x61 and make ESP-IDF erase the bond -- which strands the pump until someone
# re-pairs at the pump itself.
"suspend-release-bypasses-an-active-settle-hold|components/alpha_hwr/alpha_hwr.cpp|    if (this->reconnect_settling_) {|    if (false) {"
# Clearing the ONE expected reason, rather than masking the surface until the
# pump is ready. The mask was the first cut and it hides a failed reconnect, an
# auth error or a readiness fault -- indefinitely, if recovery never succeeds.
"suspend-release-leaves-its-own-teardown-on-the-fault-surface|components/alpha_hwr/alpha_hwr.cpp|    this->ble_manager_.clear_failure_if_local_teardown();|    // mutated: leave the reason latched"
# A suspension is not an outage. on_disconnect() samples the interval up to an
# involuntary drop deliberately; a suspension ends because someone clicked, so
# recording it moves link_gaps_truncated -- the trust check on every other
# number in that histogram -- once per suspend.
# The clear must be CONDITIONAL. Unconditional erases whatever was already
# latched -- and a latched fault is usually why the operator is suspending the
# link. The worst instance is Encryption Start Failed (0x61), whose remedy is to
# walk to the pump with the GO app, so the switch is reached for exactly when
# that string is showing.
"suspend-release-erases-somebody-elses-fault|components/alpha_hwr/ble_connection_manager.h|    if (last_failure_.rfind(\"Local Host Terminated\", 0) != 0) return;|    // mutated: clear whatever is latched"
# Deliberate absence: the `failure_hold_ = NONE` beside that clear. It was
# load-bearing for one round -- verified, one failing assertion -- and stopped
# being so when the clear became CONDITIONAL in the same round.
#
# The reachability argument: the clear now runs only when the latched string is
# the local-host teardown, and that string can only BE latched when no hold
# outranks it, because failure_hold_admits() would otherwise have refused to
# write it. So by the time the reset runs, the hold is already NONE. Kept
# because every other site that writes last_failure_ resets both, and a lone
# exception is how the original defect got in.
# A connection that opens while suspended must be torn down again, not adopted.
# ESPHome reads auto_connect only at advertisement match and cannot close a link
# with no conn_id yet, so an in-flight connect completes and the OPEN reaches the
# component -- which would put the session connected, re-arm the watchdogs and
# resume polling a pump the operator believes they handed over.
"racing-open-adopted-during-a-suspension|components/alpha_hwr/alpha_hwr.cpp|    if (this->suspended_) {\n      ESP_LOGI(TAG, \"Connection opened while suspended|    if (false) {\n      ESP_LOGI(TAG, \"Connection opened while suspended"
# Suspending twice must RETRY the teardown; releasing twice must not repeat its
# destructive side effects.
"suspend-swallows-the-operators-retry|components/alpha_hwr/alpha_hwr.cpp|  if (!suspended && !this->suspended_) return;|  if (suspended == this->suspended_) return;"
# The sampler's ARMING sites, not just the disarm. on_inbound() self-arms by
# design, so one notification in the asynchronous teardown window undoes the
# disarm and the disconnect behind it samples. A notification needs no open, so
# this is not covered by the racing-open guard.
"gap-sampler-re-armed-by-a-notification-during-a-suspension|components/alpha_hwr/alpha_hwr.cpp|        if (!this->suspended_)\n          this->link_gap_.on_inbound(inbound_now);|        this->link_gap_.on_inbound(inbound_now);"
# Deliberate absence: the matching `!suspended_` guard on link_gap_.on_open().
# It became an equivalent mutant when the racing-open guard landed in the same
# round -- that returns from the connection callback before on_open() is
# reached, so no input distinguishes the inner term. Kept as defence in depth
# against the outer guard moving; covered in spirit by
# racing-open-adopted-during-a-suspension, which IS caught.
# A suspension is not a failed connection attempt. Three of them reach
# LINK_FAIL_K and publish "Reconnecting", which an automation reads as a fault.
"suspend-counted-as-a-failed-attempt|components/alpha_hwr/alpha_hwr.cpp|    } else if (!this->suspended_) {|    } else {"
"suspend-recorded-as-a-truncated-gap|components/alpha_hwr/alpha_hwr.cpp|    this->link_gap_.disarm();|    // mutated: sample it as an outage"
"bridge-parse-failure-settles-rejected|components/alpha_hwr/api_bridge.cpp|  result.status = WriteStatus::INVALID;|  result.status = WriteStatus::REJECTED;"
)

if [[ "${1:-}" == "--list" ]]; then
  echo "Mutations:"
  for m in "${MUTATIONS[@]}"; do echo "  - ${m%%|*}"; done
  exit 0
fi

# Does every entry still point at code that exists? An entry whose search string
# stopped matching is scored "(not applied)" and turns the sweep red -- correctly,
# but only after the better part of an hour, and only for the entries a filter
# happened to select. This answers the same question in about seven seconds, for
# all of them, without building anything.
#
# What it does NOT answer, and neither does the sweep: whether an entry has gone
# MISSING. Both check the entries that are here against the code; nothing checks
# the code against the entries. A range delete in this file removed seven entries
# as collateral while retiring three, and every remaining entry still matched, so
# --verify passed at 258 and the sweep would have passed too. The only symptom
# was a filtered run printing "No mutation name contains ...", which is easy to
# read as a typo. If you delete entries, count them.
#
# It exists because retargeting entries after a refactor is easy to half-do:
# issue #259 moved three failure paths behind one helper and left three entries
# anchored on the code it replaced. One was noticed, and two were found by the
# sweep rather than by anyone reading the diff.
#
# Run always, not just under --verify: a filtered run is exactly where a stale
# entry hides, because the filter selects around it.
verify_entries() {
  local rc=0 m name file search rest count
  for m in "${MUTATIONS[@]}"; do
    IFS='|' read -r name file search rest <<< "$m"
    # A '|' anywhere in the search text truncates the entry mid-field, and the
    # truncation is INVISIBLE to the match check below: the shortened string
    # usually still matches exactly once. So test for it first. This is the one
    # entry defect that reaches a full sweep even with --verify in place, which
    # it did -- `len < 2 || data[1] >= ...` scored "malformed" an hour in.
    case "$rest" in
      "|"*)
        echo -e "${RED}✗ $name: the search field contains a '|' and was truncated${NC}" >&2
        echo "    Anchor on a neighbouring line, or hoist the predicate so the" >&2
        echo "    line it targets has no '|' in it." >&2
        rc=1
        continue
        ;;
    esac
    if [ ! -f "$PROJECT_DIR/$file" ]; then
      echo -e "${RED}✗ $name: no such file: $file${NC}" >&2
      rc=1
      continue
    fi
    count=$(SEARCH="$search" python3 - "$PROJECT_DIR/$file" <<'PY'
import os, sys
print(open(sys.argv[1]).read().count(os.environ["SEARCH"].replace("\\n", "\n")))
PY
)
    if [ "$count" != "1" ]; then
      echo -e "${RED}✗ $name: search string matches $count times in $file (need exactly 1)${NC}" >&2
      rc=1
    fi
  done
  return "$rc"
}

# Note this checks EVERY entry, before any filter is applied.
if ! verify_entries; then
  echo "" >&2
  echo "The code these mutations target moved or changed. Retarget them rather" >&2
  echo "than deleting them -- the coverage they prove is real." >&2
  exit 2
fi

if [[ "${1:-}" == "--verify" ]]; then
  echo "All ${#MUTATIONS[@]} mutations still point at code that exists."
  exit 0
fi

# Optional name filter. Applied to the whole array up front rather than skipped
# inside the loop, so the "Caught: n / total" line below counts what was
# actually run instead of reporting most of the suite as missing.
FILTER="${1:-}"
if [[ -n "$FILTER" ]]; then
  SELECTED=()
  for m in "${MUTATIONS[@]}"; do
    [[ "${m%%|*}" == *"$FILTER"* ]] && SELECTED+=("$m")
  done
  if [[ ${#SELECTED[@]} -eq 0 ]]; then
    echo "No mutation name contains '$FILTER'. Try --list." >&2
    exit 2
  fi
  MUTATIONS=("${SELECTED[@]}")
fi

echo "=========================================="
echo "  Mutation check"
echo "=========================================="
echo "Asserting the suite FAILS when production code is broken."
echo ""

# Restore every file any mutation touches. Derived from MUTATIONS rather than
# listed separately: a hardcoded list silently goes stale the moment a mutation
# is added for a new file, which leaves that file mutated in the working tree
# after the run. (That happened -- control_service.cpp stayed broken and the
# next `make test` failed for no visible reason.)
mutated_files() {
  local e
  for e in "${MUTATIONS[@]}"; do
    echo "${e#*|}" | cut -d'|' -f1
  done | sort -u
}

# Returns non-zero if any file could not be restored. A silently-swallowed
# failure here (a locked index, say) would let the run continue -- and even exit
# 0 -- with a production source still mutated, which is the exact state this
# script exists to avoid leaving behind.
RESTORE_FAILED=0
restore_all() {
  local f rc=0
  cd "$PROJECT_DIR" || return 1
  for f in $(mutated_files); do
    # From HEAD, not the index: a mutation that got staged (git add -A while
    # debugging, say) would otherwise be "restored" to the mutated version.
    if ! git checkout HEAD -- "$f" 2>/dev/null; then
      echo -e "${RED}✗ FAILED TO RESTORE $f -- it is still mutated.${NC}" >&2
      echo "  Restore it by hand: git checkout HEAD -- $f" >&2
      rc=1
    fi
  done
  [ "$rc" -eq 0 ] || RESTORE_FAILED=1
  return $rc
}

# Objects are cached across mutations (tests/Makefile builds per translation
# unit), which is what makes a rebuild here cost one or two files instead of a
# whole target. Two reasons to delete the affected ones by hand rather than
# leaving it to make:
#
#   - make decides staleness by mtime at 1 s granularity, and a mutate/build/
#     revert cycle runs well inside a second. That is the same stale-artifact
#     hazard the binary `rm -f` below exists for, one level down, and it has
#     produced false survivors here before.
#   - The compiler already recorded the exact include graph in the .d files, so
#     "every object whose dependencies mention this file" is precise where a
#     guess would be either unsafe or wasteful.
#
# $1 is repo-relative (components/alpha_hwr/link_watchdog.h); the .d files spell
# it ../components/..., so a substring match is what lines the two up.
purge_objects_for() {
  local f="$1" d
  [ -d "$TESTS_DIR/.obj" ] || return 0
  while IFS= read -r d; do
    rm -f "$d" "${d%.d}.o"
  done < <(find "$TESTS_DIR/.obj" -name '*.d' -exec grep -l -- "$f" {} + 2>/dev/null)
}

# Abort the run the moment a restore fails, rather than mutating further on top
# of a source we could not put back.
restore_or_die() {
  if ! restore_all; then
    echo -e "${RED}✗ Aborting: the working tree is left modified.${NC}" >&2
    exit 3
  fi
}

# Refuse to run against a dirty tree: this script edits tracked sources and
# restores them with `git checkout --`, which would discard real work.
#
# This guard runs BEFORE the cleanup trap is installed, and that ordering is
# load-bearing: with the trap already armed, `exit 2` here would fire it and
# `git checkout --` would destroy precisely the uncommitted edits the guard
# exists to protect.
# `|| exit` is not decoration here. Every path below is relative to the repo
# root, and the very next thing is the guard that decides whether it is safe to
# start mutating tracked sources -- run that from the wrong directory and it
# asks git about some other tree, which is the one way this guard could pass
# when it should not.
cd "$PROJECT_DIR" || { echo "cannot cd to $PROJECT_DIR" >&2; exit 2; }

# The files this run may mutate, as an array. Built with a read loop rather than
# `mapfile` for the reason the selection loop below gives -- that is bash 4 and
# macOS ships 3.2 -- and as an array rather than unquoted command substitution
# so the splitting is something this script states rather than something the
# shell does to it on the way past.
MUTATED_FILES=()
while IFS= read -r mf; do
  [ -n "$mf" ] && MUTATED_FILES+=("$mf")
done < <(mutated_files)

# Compare against HEAD so a *staged* edit counts as dirty too -- restore_all
# resets to HEAD and would discard it.
if [ ${#MUTATED_FILES[@]} -gt 0 ] && ! git diff HEAD --quiet -- "${MUTATED_FILES[@]}"; then
  echo -e "${RED}✗ A file this script mutates has uncommitted changes.${NC}"
  echo "  This script mutates tracked sources and reverts them with git checkout,"
  echo "  which would discard those changes. Commit or stash first."
  exit 2
fi

# Safe from here: every tracked file this touches is committed, so restoring is
# lossless. Interrupts restore and then terminate rather than resuming the loop
# with a mutation still applied.
trap restore_all EXIT
trap 'echo; echo -e "${YELLOW}Interrupted — restoring sources.${NC}"; restore_all; exit 130' INT TERM

# Restore, then drop the objects that were compiled from the mutated source.
#
# Purging before the build is not enough. The object built from the mutated file
# is still in the cache afterwards, and the `git checkout` that restores the
# source can land inside the same whole second the object was written -- GNU
# make 3.81 (what macOS ships) compares mtimes at 1 s granularity, so it reports
# the target up to date and keeps the mutated object. Reproduced here: a source
# 44 ms newer than its object, same whole second, `make -n` says "is up to
# date".
#
# The consequence is the one this whole script exists to prevent. A stale
# mutated object survives into later entries, their binaries fail for the
# previous entry's reason, and those entries are scored "caught" while never
# having been tested -- manufactured confidence, and invisible, because the run
# still ends 156/156 and exits 0.
restore_and_purge() {
  restore_or_die
  purge_objects_for "$1"
}

# Establish that the suite passes unmutated. Without this, a pre-existing build
# or test failure — or a missing compiler — makes every mutation "fail the
# suite" and get counted as caught, so the job reports full mutation coverage
# while proving nothing. That is the same false-confidence failure this script
# exists to detect, so it must not be able to produce it.
echo -n "baseline (unmutated suite)  "
# Which test targets compile which file. Built once; the per-mutation lookup is
# a dict hit. SCOPED=0 falls back to rebuilding and running everything, which is
# what this check did before and remains the definition of correct -- use it if
# a scoped result ever looks wrong.
SCOPED="${SCOPED:-1}"
# `mktemp -t NAME` is a BSD form: GNU coreutils requires the XXXXXX template
# and fails, which silently left DEPMAP empty and sent CI down the slow path
# for 21 minutes without failing anything. This form is valid on both.
DEPMAP="$(mktemp "${TMPDIR:-/tmp}/mutation_depmap.XXXXXX")"
if [[ -z "$DEPMAP" ]]; then
  echo -e "${RED}Could not create the dependency-map temp file.${NC}" >&2
  SCOPED=0
fi
# Extend the EXIT trap rather than calling `trap ... EXIT` again: bash replaces
# a trap, it does not add to one, so a second `trap ... EXIT` here would
# silently disarm the restore_all installed above -- and restoring mutated
# sources is this script's entire safety contract. (Learned the hard way: the
# first version of this line did exactly that and left a production header
# mutated in the working tree after an abort.)
trap 'restore_all; rm -f "$DEPMAP"' EXIT
trap 'echo; echo -e "${YELLOW}Interrupted — restoring sources.${NC}"; restore_all; rm -f "$DEPMAP"; exit 130' INT TERM
if [[ "$SCOPED" == "1" ]]; then
  if ! python3 "$SCRIPT_DIR/mutation_targets.py" --build-map "$DEPMAP"; then
    # Falling back is *correct* -- it runs everything, which is the definition
    # of right -- but it costs about 4x, and a silent 4x is exactly the kind of
    # regression nobody notices. Surface it as a CI annotation so a broken map
    # shows up in the run summary rather than only as a longer wall-clock.
    echo -e "${RED}Could not map files to test targets; falling back to full runs.${NC}"
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
      echo "::warning title=Mutation check unscoped::Dependency mapping failed; ran the full suite per mutation (~4x slower). Results are still valid."
    fi
    SCOPED=0
  fi
fi

# -O0 for every build in this run. The suite is compiled hundreds of times here
# and never profiled, so the optimiser is pure wall-clock; behaviour under test
# is identical, and the -O2 build is still what `make test`, the sanitizer job
# and the warning check use. tests/Makefile keys its object cache on the flags,
# so this does not evict a developer's -O2 objects or get linked against them.
export OPT=-O0
# Clear only the object cache this run will use, not every variant. `make clean`
# would take a developer's -O2 objects with it, which this run never touches --
# and would contradict the reason the end of the run keeps the cache at all.
# The Makefile knows where its objects go for the flags in force; ask it.
RUN_OBJDIR="$(cd "$TESTS_DIR" && make -s print-objdir 2>/dev/null)"
# Shape-checked before anything is removed: this feeds `rm -rf`, and a make that
# printed something unexpected must not be able to aim it. An empty or
# unrecognised answer skips the clean, which only costs a stale-cache rebuild.
case "$RUN_OBJDIR" in
  .obj/*) ;;
  *) RUN_OBJDIR="" ;;
esac
baseline_build_and_run() {
  cd "$TESTS_DIR" || return 1
  make clean-bin >/dev/null 2>&1 || return 1
  { [ -z "$RUN_OBJDIR" ] || rm -rf "$RUN_OBJDIR"; } || return 1
  make -j"$JOBS" test >/tmp/mutation_baseline.log 2>&1
}
run_bounded "$BASELINE_TIMEOUT" baseline_build_and_run
BASELINE_RC=$?
if [ "$BASELINE_RC" -eq 0 ]; then
  echo -e "${GREEN}✓ passes${NC}"
elif [ "$BASELINE_RC" -eq "$TIMEOUT_EXIT" ]; then
  echo -e "${RED}✗ HUNG${NC}"
  echo ""
  echo "  The baseline did not finish within ${BASELINE_TIMEOUT}s, before any"
  echo "  mutation was applied. Something in the unmutated tree loops forever or"
  echo "  this machine is far slower than the budget assumes; either way no"
  echo "  mutation result would mean anything. Raise BASELINE_TIMEOUT if it is"
  echo "  the latter. Last 20 lines:"
  echo ""
  tail -20 /tmp/mutation_baseline.log 2>/dev/null | sed 's/^/    /'
  exit 2
else
  echo -e "${RED}✗ FAILS${NC}"
  echo ""
  echo "  The suite does not pass before any mutation is applied, so a mutation"
  echo "  appearing to be 'caught' would prove nothing. Fix the build or the"
  echo "  failing tests first. Last 20 lines:"
  echo ""
  tail -20 /tmp/mutation_baseline.log | sed 's/^/    /'
  exit 2
fi
echo ""

SURVIVORS=()
CAUGHT=0

HUNGS=()

# Run the selected binaries, stopping at the first that fails or hangs.
# 0 = every one passed (the mutation survived), 1 = one failed (caught),
# TIMEOUT_EXIT = one never finished, which is neither.
run_selected() {
  local t rc
  for t in "${SEL[@]}"; do
    run_bounded "$TEST_TIMEOUT" run_one_test "$t"
    rc=$?
    if [ "$rc" -eq "$TIMEOUT_EXIT" ]; then HUNG_TEST="$t"; return "$TIMEOUT_EXIT"; fi
    [ "$rc" -ne 0 ] && return 1
  done
  return 0
}

for entry in "${MUTATIONS[@]}"; do
  SURVIVED=0
  BUILD_BROKEN=0
  HUNG=0
  HUNG_TEST=""
  RUN_RC=0
  IFS='|' read -r name file search replace <<< "$entry"

  # A '|' in the SEARCH field silently truncates it there, and the remainder is
  # swallowed by `replace` -- so the entry applies a mutation nobody wrote.
  #
  # This CANNOT be detected by reassembling the four fields and comparing: the
  # remainder is absorbed verbatim, delimiters included, so `name|file|search|
  # replace` reproduces the original entry exactly even when the split was
  # wrong. (I shipped that check first; it is worthless, and worth recording as
  # such so it is not re-invented.) Nor can it be detected by counting
  # delimiters, because replacements legitimately contain them.
  #
  # It IS detectable a different way, though, and the comment above used to stop
  # one step short of it. When the split truncates, the remainder is absorbed
  # into `replace` -- starting with the very '|' that did the truncating. So a
  # replacement beginning with '|' is the signature, and that is checked below.
  # It catches the common case (a `||` inside the anchored line) at the cost of
  # rejecting a replacement that legitimately starts with '|', which no entry
  # here does and which can be written with a leading space if one ever needs to.
  #
  # Backstop for anything this misses: a mangled entry produces code that does
  # not compile, and the run below reports that as its own outcome instead of
  # scoring it "caught". The rule for authors is unchanged -- the search field
  # must not contain a '|'; anchor on a neighbouring line instead.
  case "$replace" in
    "|"*)
      echo -e "${RED}✗ malformed entry: the search field contains a '|'${NC}"
      echo "    It was truncated at that character and the rest was swallowed by"
      echo "    the replacement, so this entry would apply an edit nobody wrote."
      echo "    Anchor on a neighbouring line that has no '|' in it."
      SURVIVORS+=("$name (malformed: '|' in the search field)")
      continue
      ;;
  esac

  APPLIED=$(SEARCH="$search" REPLACE="$replace" python3 - "$PROJECT_DIR/$file" <<'PY'
import os, sys
path = sys.argv[1]
search = os.environ["SEARCH"].replace("\\n", "\n")
replace = os.environ["REPLACE"].replace("\\n", "\n")
s = open(path).read()
n = s.count(search)
if n != 1:
    print(f"MARKER_COUNT_{n}")
else:
    open(path, "w").write(s.replace(search, replace, 1))
    print("OK")
PY
)
  if [[ "$APPLIED" != "OK" ]]; then
    echo -e "${RED}✗ could not apply ($APPLIED)${NC}"
    echo "    The code this mutation targets moved or changed. Update the"
    echo "    mutation rather than deleting it -- the coverage it proves is real."
    SURVIVORS+=("$name (not applied)")
    restore_and_purge "$file"
    continue
  fi

  # A mutation that does not COMPILE proves nothing about the test suite, which
  # is the only thing this check measures. It used to score as "caught": a
  # failed build short-circuited the && below, leaving SURVIVED at 0. That made
  # a mangled entry -- a search field truncated at a '|', say -- look like it
  # was proving coverage it never tested. Build failures are now their own
  # outcome, so entries have to produce code that compiles and is wrong.

  # Only the targets that actually compile this file. A mutation to
  # dhw_demand_logic.h cannot change what test_session decides, and rebuilding it
  # 54 times was most of this check's wall-clock. See tools/mutation_targets.py
  # for why the map comes from the compiler rather than the Makefile, and for
  # why mis-selection can only cause a false SURVIVED, never a false caught.
  if [[ "$SCOPED" == "1" ]]; then
    # Not mapfile: that is bash 4 and macOS ships 3.2, where the failure mode
    # is an unbound SEL under `set -u` killing the script mid-loop.
    SEL=()
    while IFS= read -r line; do
      [[ -n "$line" ]] && SEL+=("$line")
    done < <(python3 "$SCRIPT_DIR/mutation_targets.py" --lookup "$DEPMAP" "$PROJECT_DIR/$file")
  else
    SEL=()
  fi

  if [[ "$SCOPED" == "1" && ${#SEL[@]} -eq 0 ]]; then
    echo -e "${RED}✗ no test target compiles $file${NC}"
    echo "    Nothing in tests/ builds that file, so no mutation of it can be"
    echo "    caught. That is a coverage hole in itself -- host-compile the file"
    echo "    (AGENTS §4) rather than dropping the mutation."
    SURVIVORS+=("$name (no target builds $file)")
    restore_and_purge "$file"
    continue
  fi

  if [[ "$SCOPED" == "1" ]]; then
    # Remove the selected binaries first. make decides staleness by mtime at
    # 1 s granularity, and a mutate/revert pair inside the same second would
    # otherwise leave a stale binary and report a mutation as caught or
    # survived on the *previous* build. Cheap insurance against the exact
    # stale-binary artifact that has produced false survivors here before.
    ( cd "$TESTS_DIR" && rm -f "${SEL[@]}" )
    # ...and the objects compiled from this file, for the same reason. Every
    # other object in the cache is still valid, which is the point: the rebuild
    # below recompiles the handful of translation units that actually include
    # the mutation instead of the 21 that make up a whole-component target.
    purge_objects_for "$file"
    if ! (cd "$TESTS_DIR" && make -j"$JOBS" "${SEL[@]}" >/dev/null 2>&1); then
      BUILD_BROKEN=1
    else
      run_selected
      RUN_RC=$?
      if [ "$RUN_RC" -eq "$TIMEOUT_EXIT" ]; then
        HUNG=1
      elif [ "$RUN_RC" -eq 0 ]; then
        SURVIVED=1
      else
        SURVIVED=0
      fi
    fi
  else
    # Build and RUN must be separate steps here, exactly as in the scoped arm
    # above. `make test` does both, so folding them together inverts every
    # verdict: a mutation the suite catches makes the target exit non-zero and
    # was reported "did not compile", while one that survives exits zero and was
    # scored "caught". SCOPED=0 therefore reported the opposite of the truth for
    # every entry -- the failure mode this whole script exists to prevent,
    # living in the script. `make` alone builds; `make test` then only runs.
    if ! (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make -j"$JOBS" >/dev/null 2>&1); then
      BUILD_BROKEN=1
    else
      run_bounded "$TEST_TIMEOUT" run_whole_suite
      RUN_RC=$?
      if [ "$RUN_RC" -eq "$TIMEOUT_EXIT" ]; then
        HUNG=1
        HUNG_TEST="the suite"
      elif [ "$RUN_RC" -eq 0 ]; then
        SURVIVED=1
      else
        SURVIVED=0
      fi
    fi
  fi

  if [[ "$HUNG" == "1" ]]; then
    echo -e "${RED}✗ HUNG — ${HUNG_TEST} did not finish within ${TEST_TIMEOUT}s${NC}"
    echo "    Neither caught nor survived: the suite never returned a verdict, so"
    echo "    this entry proves nothing either way. Almost always the mutation"
    echo "    stops something from ever firing and a test waits on it in an"
    echo "    unbounded loop -- bound the loop and assert the property arrived,"
    echo "    rather than looping until it does. If the machine is simply slow,"
    echo "    raise TEST_TIMEOUT."
    HUNGS+=("$name (${HUNG_TEST} exceeded ${TEST_TIMEOUT}s)")
    restore_and_purge "$file"
    continue
  fi

  if [[ "$BUILD_BROKEN" == "1" ]]; then
    echo -e "${RED}✗ did not compile${NC}"
    echo "    The mutated code does not build, so the suite was never run and"
    echo "    this entry proves nothing about coverage. Usually the search"
    echo "    field contains a '|' and was truncated there; sometimes the"
    echo "    replacement is simply not valid C++. Fix the entry."
    SURVIVORS+=("$name (did not compile)")
    restore_and_purge "$file"
    continue
  fi

  if [[ "$SURVIVED" == "1" ]]; then
    echo -e "${RED}✗ SURVIVED — the suite passed with this broken${NC}"
    SURVIVORS+=("$name")
  else
    echo -e "${GREEN}✓ caught${NC}"
    CAUGHT=$((CAUGHT + 1))
  fi
  restore_and_purge "$file"
done

# The last mutation's binaries are still on disk, built from a source that has
# since been restored -- and `git checkout` landing inside the same second would
# not make them stale to make. Remove them, and remove every object compiled
# from a file this run mutated, so nothing built from a mutation can be picked
# up by a later `make test`.
#
# Deliberately not `make clean`: the rest of the object cache was built from
# unmutated sources and is what makes the next run's baseline a few seconds
# rather than a full rebuild, and a full clean would also throw away a
# developer's -O2 objects, which this run never touched.
if cd "$TESTS_DIR"; then
  # Best effort: a failure here leaves stale binaries, and the next run's
  # baseline rebuild clears them anyway. Written as an `if` rather than
  # `cd && make || true`, which reads as if-then-else and is not -- the `|| true`
  # in that form also swallows a failed `cd`, and would then have run `make` in
  # whatever directory the script happened to be in.
  make clean-bin >/dev/null 2>&1 || true
fi
for f in "${MUTATED_FILES[@]}"; do purge_objects_for "$f"; done

echo ""
echo "=========================================="
echo "  Results"
echo "=========================================="
if [[ -n "$FILTER" ]]; then
  echo -e "  ${YELLOW}PARTIAL RUN — only mutations matching '$FILTER'${NC}"
fi
echo "  Caught:   $CAUGHT / ${#MUTATIONS[@]}"
# Listed separately from the survivors, and before them, because they mean
# different things. A survivor is an answer -- the suite is not testing that
# code. A hang is the absence of one, and it is the more urgent of the two to
# fix, because until it is fixed every later entry in the run is delayed by it.
if [[ ${#HUNGS[@]} -gt 0 ]]; then
  echo -e "  ${RED}Hung:     ${#HUNGS[@]}${NC}"
  for h in "${HUNGS[@]}"; do echo "    - $h"; done
fi
if [[ ${#SURVIVORS[@]} -gt 0 ]]; then
  echo -e "  ${RED}Survived: ${#SURVIVORS[@]}${NC}"
  for s in "${SURVIVORS[@]}"; do echo "    - $s"; done
  echo ""
  echo -e "${RED}✗ A surviving mutation means the suite is not testing that code.${NC}"
  echo "  Usually it means a test asserts a replica of the logic rather than"
  echo "  linking the production source. Fix the test, not the mutation."
  exit 1
fi
if [[ ${#HUNGS[@]} -gt 0 ]]; then
  echo ""
  echo -e "${RED}✗ A mutation that hangs the suite has no result at all.${NC}"
  echo "  It is not evidence of coverage and must not be read as any. Bound the"
  echo "  loop that waits on the mutated behaviour and assert what it waited for."
  exit 1
fi
echo ""
if [ "$RESTORE_FAILED" -ne 0 ]; then
  echo -e "${RED}✗ A restore failed during the run; the tree may be modified.${NC}"
  exit 3
fi
echo -e "${GREEN}✓ Every mutation was caught${NC}"
