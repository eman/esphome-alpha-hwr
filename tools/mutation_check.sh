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

# Each mutation: name | file | python-repr search | python-repr replace
# The search string must appear exactly once; the script fails loudly if not,
# so a refactor that moves the code is reported rather than silently skipped.
#
# Deliberately absent: a mutation on Authentication's `auth_sequence_++`.
# BOTH start() and cancel() increment it, and either alone is enough to
# invalidate a stale scheduler lambda -- so removing one is an equivalent
# mutant that no test can kill, and adding it here would be an uncatchable
# entry rather than a coverage gap. Removing *both* is catchable, and
# test_stale_timers_cannot_re_enter_a_restarted_handshake() does catch it, but
# no single search/replace spans two functions. Verified by experiment after
# this check flagged the single-point version as surviving; the redundancy is
# defence in depth and was left in production rather than trimmed to suit the
# tooling.
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
# Both found by a skeptic pass, both survived the original tests: the only
# TZ-driven test used US Pacific, whose offsets are whole hours and whose sample
# instants share a calendar year with UTC. So a whole-hour-only implementation
# and a broken year-rollover branch were each indistinguishable from the real
# thing. Pre-existing code, but this change is the first to test the function at
# all, which makes it the place to close them.
"dst-offset-ignores-sub-hour-zones|components/alpha_hwr/schedule_service.h|(lt.tm_min - gt.tm_min) * 60|0 * (lt.tm_min - gt.tm_min) * 60"
"dst-offset-year-rollover-branch|components/alpha_hwr/schedule_service.h|    day_delta = (lt.tm_year > gt.tm_year) ? 1 : -1;|    day_delta = 0;"
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
"transport-send-failure-silent|components/alpha_hwr/transport.cpp|          this->peer_resync_started_ms_ = now;\n        }\n        if (cmd.callback) {\n          cmd.callback(false, nullptr, 0);\n        }|          this->peer_resync_started_ms_ = now;\n        }"
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
"transport-failed-command-stays-queued|components/alpha_hwr/transport.cpp|          this->peer_resync_started_ms_ = now;\n        }\n        if (cmd.callback) {\n          cmd.callback(false, nullptr, 0);\n        }\n        this->command_queue_.pop_front();|          this->peer_resync_started_ms_ = now;\n        }\n        if (cmd.callback) {\n          cmd.callback(false, nullptr, 0);\n        }"
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
"transport-missing-writer-silent|components/alpha_hwr/transport.cpp|ESP_LOGW(TAG, \"Write callback not set, dropping command\");\n        if (cmd.callback) {\n          cmd.callback(false, nullptr, 0);\n        }|ESP_LOGW(TAG, \"Write callback not set, dropping command\");"
# Restores issue #179's off-by-one: a seven-byte Class 7 header instead of six,
# which cost every device-info string its first character. It survived for as
# long as it did because the only test fixture was generated from the same wrong
# assumption -- so the parser and its test agreed, and the pump was blamed. The
# fixtures are transcribed from a capture now, which is what makes this mutation
# fail rather than merely shift both sides together.
"class7-header-length|components/alpha_hwr/device_info_service.cpp|      static const size_t HEADER_LEN = 6;|      static const size_t HEADER_LEN = 7;"
# The length guard is the only thing standing between a runt frame and an
# unsigned underflow: string_len is size_t, so a frame under 8 bytes wraps it to
# ~1.8e19 and the copy loop reads ~127 bytes past the frame. transport.cpp
# dispatches Class 3/7 on len >= 5, so 5-, 6- and 7-byte frames do reach the
# callback. A skeptic pass found this relaxation left the whole suite green.
"class7-runt-guard-relaxed|components/alpha_hwr/device_info_service.cpp|      static const size_t MIN_FRAME_LEN = HEADER_LEN + CRC_LEN;|      static const size_t MIN_FRAME_LEN = 5;"
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
"link-watchdog-never-fires|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return false;"
"link-watchdog-rollover-unsafe|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return now_ms > last_inbound_ms + timeout_ms;"
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
"auth-stage2-repeat-count|components/alpha_hwr/auth.cpp|  if (repeat_count < 5) {|  if (repeat_count < 4) {"
# Stage 3 stopped being two blind sends when issue #174 made it two matched
# reads, so this entry's old search string (two consecutive send_packet calls)
# matched nothing. mutation_check.sh reports that loudly as "could not apply"
# and exits 1 rather than scoring it Survived -- so a stale entry is a red
# build, not a silent hole. Retargeted at the same property: EXT_1 first.
# Killed by tests/test_auth.cpp as well as by the end-to-end wiring test, so it
# is not new coverage -- the comment here previously credited it to the wiring
# test alone, which overstated what that test added. Kept because it is a cheap
# check that the two agree.
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
# Pump Ready must need BOTH caches. Until tests/test_component_wiring.cpp grew a
# case that withholds the schedule overview, every scenario filled both, so the
# gate was only ever observed agreeing and could be reduced to `return true`
# with the whole suite green.
"ready-gate-ignores-caches|components/alpha_hwr/alpha_hwr.cpp|  return control_service_.is_cache_valid() && schedule_service_.is_overview_cache_valid();|  return true;"
"auth-never-reports-completion|components/alpha_hwr/auth.cpp|  if (completion_callback_) {|  if (false) {"
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
"session-authenticated-does-not-reach-ready|components/alpha_hwr/session.cpp|  transition_to(SessionState::READY,|  transition_to(SessionState::AUTHENTICATING,"
"session-disconnect-does-not-reach-idle|components/alpha_hwr/session.cpp|  transition_to(SessionState::IDLE,|  transition_to(SessionState::READY,"
# Both halves of the APDU length invariant (issue #174). Byte 1 declares the
# payload byte count in bits 5-0, and two frames shipped declaring a count they
# did not carry: the single-event write borrowed the layer write's 0xB3 (51)
# for a 19-byte payload, and the Class 10 setpoint write counted only its float
# and not the four ID bytes before it. This pump accepts either, so neither was
# a visible failure -- tests/test_write_operations.cpp now checks every frame
# any test sends against its own declared length.
"single-event-opspec-length|components/alpha_hwr/schedule_service.cpp|  // never a visible failure; see the header note.\n  apdu[1] = 0x93;|  // never a visible failure; see the header note.\n  apdu[1] = 0xB3;"
"class10-setpoint-opspec-length|components/alpha_hwr/control_service.cpp|  apdu[1] = 0x88;  // OpSpec: SET + 8 payload bytes (2 sub + 2 obj + 4 float)|  apdu[1] = 0x84;"
# The backstop must actually complete a stalled sequence. Stages 1 and 3 are
# continued only by the transport's command callback, and Transport::reset()
# drops it without invoking it -- so an inert backstop restores a node that sits
# in AUTHENTICATING forever, connected and never ready. Nothing tested it until
# tests/test_auth.cpp began resetting the transport with a read pending.
"auth-backstop-is-inert|components/alpha_hwr/auth.cpp|      if (!this->running_) return;              // Finished normally; nothing to do.|      if (true) return;"
# Stage 2 must stay a blind send. Matching its Class 10 reply consumes the
# frame, and that frame is the operation-status notification TelemetryService
# publishes control mode, operation mode and setpoint from on every connect --
# so this mutation silently stops that publish. It survived the whole suite
# until tests/test_auth.cpp began counting frames reaching the packet callback.
"auth-stage2-must-stay-blind|components/alpha_hwr/auth.cpp|    send_packet(AUTH_CLASS10, sizeof(AUTH_CLASS10));|    send_read(AUTH_CLASS10, sizeof(AUTH_CLASS10), [](bool, const uint8_t *, size_t) {});"
"auth-extension-packet-order|components/alpha_hwr/auth.cpp|  send_read(AUTH_EXT_1, sizeof(AUTH_EXT_1), [this](bool, const uint8_t *, size_t) {|  send_read(AUTH_EXT_2, sizeof(AUTH_EXT_2), [this](bool, const uint8_t *, size_t) {"
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
"single-event-confirm-ignores-the-action|components/alpha_hwr/write_operation_service.cpp|                                  actual.action == op->single_event_action;|                                  true;"
# ...and the skip for a CLEAR must stay a skip: a cleared slot's window is
# meaningless, so comparing it would reject every successful clear.
"single-event-confirm-checks-a-cleared-window|components/alpha_hwr/write_operation_service.cpp|      const bool content_is_a_verdict = want_enabled ? window_matches : true;|      const bool content_is_a_verdict = window_matches;"
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
# Deliberately absent: a mutation on parse_int_field's ERANGE check. strtol
# clamps to LONG_MAX/LONG_MIN on overflow, and every call site passes a range
# far inside those, so `v > hi` / `v < lo` already reject anything ERANGE could
# flag -- an equivalent mutant no test can kill. The check stays as defence for
# a future caller with a wider range; it is not load-bearing today, and an
# entry claiming otherwise would be a false guarantee. Confirmed by experiment:
# the mutation survived the full suite.
"bridge-parser-accepts-leading-junk|components/alpha_hwr/api_bridge.cpp|  if (!starts_cleanly) return false;|  // mutated: let strtol skip whitespace and signs"
# Timestamps are compared AFTER narrowing to the wire's 32 bits; comparing the
# wider parse let an ordered pair reach the pump reversed.
"bridge-epoch-range-not-checked|components/alpha_hwr/api_bridge.cpp|  if (!parse_int_field(s, 0, 4294967295L, &v)) return false;|  if (!parse_int_field(s, 0, 999999999999L, &v)) return false;"
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
# state_name(pump_auto, schedule_on) is asymmetric, so its arguments can be
# swapped without a compiler complaint: a running unscheduled pump would then
# report "off". The test fixtures were symmetric (both flags 1) and could not
# see it; they cover both asymmetric combinations now.
"bridge-state-name-args-swapped|components/alpha_hwr/api_bridge.cpp|        data[\"state\"] = ux::state_name(result.enabled != 0, result.sched_enabled != 0);|        data[\"state\"] = ux::state_name(result.sched_enabled != 0, result.enabled != 0);"
# -1 means "not known". Without the guard, -1 != 0 reads as true and a write
# that never reached the pump reports a fabricated state.
"bridge-state-fabricated-from-unknown-flags|components/alpha_hwr/api_bridge.cpp|      if (result.enabled >= 0 && result.sched_enabled >= 0) {|      if (true) {"
"bridge-parse-failure-settles-rejected|components/alpha_hwr/api_bridge.cpp|  result.status = WriteStatus::INVALID;|  result.status = WriteStatus::REJECTED;"
)

if [[ "${1:-}" == "--list" ]]; then
  echo "Mutations:"
  for m in "${MUTATIONS[@]}"; do echo "  - ${m%%|*}"; done
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
cd "$PROJECT_DIR"
# Compare against HEAD so a *staged* edit counts as dirty too -- restore_all
# resets to HEAD and would discard it.
if ! git diff HEAD --quiet -- $(mutated_files); then
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
if (cd "$TESTS_DIR" && make clean-bin >/dev/null 2>&1 \
      && { [ -z "$RUN_OBJDIR" ] || rm -rf "$RUN_OBJDIR"; } \
      && make -j"$JOBS" test >/tmp/mutation_baseline.log 2>&1); then
  echo -e "${GREEN}✓ passes${NC}"
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

for entry in "${MUTATIONS[@]}"; do
  SURVIVED=0
  BUILD_BROKEN=0
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
  # What catches it is downstream: a mangled entry produces code that does not
  # compile, and the run below now reports that as its own outcome instead of
  # scoring it "caught". See the note there. The rule for authors is simply:
  # the search field must not contain a '|' -- if the line you want to anchor on
  # has '||' in it, anchor on a neighbouring line. A '|' in the replacement is
  # fine.

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
  # dhw_demand_logic.h cannot change what test_auth decides, and rebuilding it
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
    elif (cd "$TESTS_DIR" && for t in "${SEL[@]}"; do ./"$t" >/dev/null 2>&1 || exit 1; done); then
      SURVIVED=1
    else
      SURVIVED=0
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
    elif (cd "$TESTS_DIR" && make test >/dev/null 2>&1); then
      SURVIVED=1
    else
      SURVIVED=0
    fi
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
cd "$TESTS_DIR" && make clean-bin >/dev/null 2>&1 || true
for f in $(mutated_files); do purge_objects_for "$f"; done

echo ""
echo "=========================================="
echo "  Results"
echo "=========================================="
if [[ -n "$FILTER" ]]; then
  echo -e "  ${YELLOW}PARTIAL RUN — only mutations matching '$FILTER'${NC}"
fi
echo "  Caught:   $CAUGHT / ${#MUTATIONS[@]}"
if [[ ${#SURVIVORS[@]} -gt 0 ]]; then
  echo -e "  ${RED}Survived: ${#SURVIVORS[@]}${NC}"
  for s in "${SURVIVORS[@]}"; do echo "    - $s"; done
  echo ""
  echo -e "${RED}✗ A surviving mutation means the suite is not testing that code.${NC}"
  echo "  Usually it means a test asserts a replica of the logic rather than"
  echo "  linking the production source. Fix the test, not the mutation."
  exit 1
fi
echo ""
if [ "$RESTORE_FAILED" -ne 0 ]; then
  echo -e "${RED}✗ A restore failed during the run; the tree may be modified.${NC}"
  exit 3
fi
echo -e "${GREEN}✓ Every mutation was caught${NC}"
