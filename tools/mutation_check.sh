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
MUTATIONS=(
"crc-initial-value|components/alpha_hwr/codec.cpp|uint16_t crc = init;|uint16_t crc = init ^ 0x0001;"
"crc-byte-order|components/alpha_hwr/frame_builder.cpp|packet_out[9] = (crc >> 8) & 0xFF;|packet_out[9] = crc & 0xFF;"
"class-match-equality|components/alpha_hwr/response_match.h|incoming_class == queued_class &&|true &&"
"frame-length-guard|components/alpha_hwr/frame_parser.cpp|if (len < expected_total) {|if (false) {"
"schedule-day-bound|components/alpha_hwr/schedule_codec.cpp|if (v[1] > 6) return fail|if (v[1] > 99) return fail"
"ignore-unrelated-gate|components/alpha_hwr/response_match.h|return is_class3_or_7(queued_class) && !is_class3_or_7(incoming_class);|return false;"
"remote-mode-confirm-fresh|components/alpha_hwr/write_operation_service.cpp|bool confirmed = success && fresh && control_.remote_state_valid_ &&|bool confirmed = success && control_.remote_state_valid_ &&"
# Restores the exact naming defect issue #159 reported: the service was called
# `pump_set_state`, its settle event answered `set_pump_state`. Since api_bridge
# now registers each service *by* its command string, this one mutation renames
# a Home Assistant service and the event field together -- a silent break of two
# public surfaces. It went unnoticed for as long as it did precisely because no
# test asserted either spelling.
"command-string-service-name-drift|components/alpha_hwr/write_operation_service.cpp|    case WriteCommand::SET_PUMP_STATE:        return \"set_pump_state\";|    case WriteCommand::SET_PUMP_STATE:        return \"pump_set_state\";"
"response-crc-enforcement|components/alpha_hwr/transport.cpp|if (!protocol::frame_crc_valid(reassembly_buffer_.data(), frame_len)) {|if (false) {"
"response-crc-trim|components/alpha_hwr/transport.cpp|if (expected_packet_length_ >= 4 && frame_len > expected_packet_length_) {|if (false) {"
"register-read-vetoes-type-match|components/alpha_hwr/transport.cpp|bool wildcard_command = (cmd.expect_type_low_ver == 0x0000 && cmd.expect_type_high == 0x0000);|bool wildcard_command = true;"
"register-read-guard-removed|components/alpha_hwr/transport.cpp|if (is_register_read && wildcard_command && !cmd.allow_register_read) {|if (false) {"
"link-watchdog-never-fires|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return false;"
"link-watchdog-rollover-unsafe|components/alpha_hwr/link_watchdog.h|return static_cast<uint32_t>(now_ms - last_inbound_ms) > timeout_ms;|return now_ms > last_inbound_ms + timeout_ms;"

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
"continuation-stopping-tick-drops-demand|components/dhw_demand/dhw_demand_logic.h|  return v == ContinuationVerdict::ACTIVE ||\n         v == ContinuationVerdict::CONFIRMED ||\n         v == ContinuationVerdict::STOPPING;|  return v == ContinuationVerdict::ACTIVE ||\n         v == ContinuationVerdict::CONFIRMED;"
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
"frozen-motor-asserts-pump-off|components/dhw_demand/dhw_demand_logic.h|  } else if (out.motor_frozen) {\n    out.pump_on = true;|  } else if (out.motor_frozen) {\n    out.pump_on = false;"
"motor-staleness-mask-removed|components/dhw_demand/dhw_demand_logic.h|  out.speed_used = reading_is_fresh(in.motor_speed_last_update_ms, in.now_ms, in.motor_max_stale_ms)\n                       ? in.motor_speed\n                       : NAN;|  out.speed_used = in.motor_speed;"
"motor-current-staleness-mask-removed|components/dhw_demand/dhw_demand_logic.h|  out.current_used =\n      reading_is_fresh(in.motor_current_last_update_ms, in.now_ms, in.motor_max_stale_ms)\n          ? in.motor_current\n          : NAN;|  out.current_used = in.motor_current;"
"motor-speed-on-threshold|components/dhw_demand/dhw_demand_logic.h|    return motor_speed >= 10.0f;|    return motor_speed > 10.0f;"
"motor-channel-precedence-swapped|components/dhw_demand/dhw_demand_logic.h|  if (!std::isnan(motor_speed))\n    return motor_speed >= 10.0f;\n  if (!std::isnan(motor_current))\n    return motor_current >= pump_off_current_threshold;|  if (!std::isnan(motor_current))\n    return motor_current >= pump_off_current_threshold;\n  if (!std::isnan(motor_speed))\n    return motor_speed >= 10.0f;"
"auth-stage2-repeat-count|components/alpha_hwr/auth.cpp|  if (repeat_count < 5) {|  if (repeat_count < 4) {"
"auth-extension-packet-order|components/alpha_hwr/auth.cpp|  send_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));\n  send_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));|  send_packet(AUTH_EXT_2, sizeof(AUTH_EXT_2));\n  send_packet(AUTH_EXT_1, sizeof(AUTH_EXT_1));"
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

if (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make -j"$JOBS" test >/tmp/mutation_baseline.log 2>&1); then
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
  IFS='|' read -r name file search replace <<< "$entry"
  printf "%-24s " "$name"

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
    restore_or_die
    continue
  fi

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
    restore_or_die
    continue
  fi

  if [[ "$SCOPED" == "1" ]]; then
    # Remove the selected binaries first. make decides staleness by mtime at
    # 1 s granularity, and a mutate/revert pair inside the same second would
    # otherwise leave a stale binary and report a mutation as caught or
    # survived on the *previous* build. Cheap insurance against the exact
    # stale-binary artifact that has produced false survivors here before.
    ( cd "$TESTS_DIR" && rm -f "${SEL[@]}" )
    if (cd "$TESTS_DIR" && make -j"$JOBS" "${SEL[@]}" >/dev/null 2>&1) && \
       (cd "$TESTS_DIR" && for t in "${SEL[@]}"; do ./"$t" >/dev/null 2>&1 || exit 1; done); then
      SURVIVED=1
    else
      SURVIVED=0
    fi
  else
    if (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make -j"$JOBS" test >/dev/null 2>&1); then
      SURVIVED=1
    else
      SURVIVED=0
    fi
  fi

  if [[ "$SURVIVED" == "1" ]]; then
    echo -e "${RED}✗ SURVIVED — the suite passed with this broken${NC}"
    SURVIVORS+=("$name")
  else
    echo -e "${GREEN}✓ caught${NC}"
    CAUGHT=$((CAUGHT + 1))
  fi
  restore_or_die
done

cd "$TESTS_DIR" && make clean >/dev/null 2>&1 || true

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
