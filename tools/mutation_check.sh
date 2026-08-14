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
#   ./tools/mutation_check.sh          # run every mutation
#   ./tools/mutation_check.sh --list   # just show them
#
# ==============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
TESTS_DIR="$PROJECT_DIR/tests"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

# Each mutation: name | file | python-repr search | python-repr replace
# The search string must appear exactly once; the script fails loudly if not,
# so a refactor that moves the code is reported rather than silently skipped.
MUTATIONS=(
"crc-initial-value|components/alpha_hwr/codec.cpp|uint16_t crc = init;|uint16_t crc = init ^ 0x0001;"
"crc-byte-order|components/alpha_hwr/frame_builder.cpp|packet_out[9] = (crc >> 8) & 0xFF;|packet_out[9] = crc & 0xFF;"
"class-match-equality|components/alpha_hwr/response_match.h|incoming_class == queued_class &&|true &&"
"frame-length-guard|components/alpha_hwr/frame_parser.cpp|if (len < expected_total) {|if (false) {"
"schedule-day-bound|components/alpha_hwr/schedule_codec.cpp|if (v[1] > 6) return fail|if (v[1] > 99) return fail"
"ignore-unrelated-gate|components/alpha_hwr/response_match.h|return is_class3_or_7(queued_class) && !is_class3_or_7(incoming_class);|return false;"
"remote-mode-confirm-fresh|components/alpha_hwr/write_operation_service.cpp|bool confirmed = success && fresh && control_.remote_state_valid_ &&|bool confirmed = success && control_.remote_state_valid_ &&"
"response-crc-enforcement|components/alpha_hwr/transport.cpp|if (!protocol::frame_crc_valid(reassembly_buffer_.data(), frame_len)) {|if (false) {"
"response-crc-trim|components/alpha_hwr/transport.cpp|if (expected_packet_length_ >= 4 && frame_len > expected_packet_length_) {|if (false) {"
"register-read-vetoes-type-match|components/alpha_hwr/transport.cpp|bool wildcard_command = (cmd.expect_type_low_ver == 0x0000 \&\& cmd.expect_type_high == 0x0000);|bool wildcard_command = true;"
"register-read-guard-removed|components/alpha_hwr/transport.cpp|if (is_register_read \&\& wildcard_command \&\& !cmd.allow_register_read) {|if (false) {"
"control-enabled-from-opmode|components/alpha_hwr/control_service.cpp|  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled\n  pump_enabled_ = (operation_mode != static_cast<uint8_t>(OperationMode::STOP));|  // AUTO (0) or USER_DEFINED (4) = enabled, STOP (1) = disabled\n  pump_enabled_ = true;"
)

if [[ "${1:-}" == "--list" ]]; then
  echo "Mutations:"
  for m in "${MUTATIONS[@]}"; do echo "  - ${m%%|*}"; done
  exit 0
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
if (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make test >/tmp/mutation_baseline.log 2>&1); then
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

  if (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make test >/dev/null 2>&1); then
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
