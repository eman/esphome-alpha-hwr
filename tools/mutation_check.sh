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

restore_all() {
  cd "$PROJECT_DIR" && git checkout -- \
    components/alpha_hwr/codec.cpp \
    components/alpha_hwr/frame_builder.cpp \
    components/alpha_hwr/response_match.h \
    components/alpha_hwr/frame_parser.cpp \
    components/alpha_hwr/schedule_codec.cpp 2>/dev/null || true
}

# Refuse to run against a dirty tree: this script edits tracked sources and
# restores them with `git checkout --`, which would discard real work.
#
# This guard runs BEFORE the cleanup trap is installed, and that ordering is
# load-bearing: with the trap already armed, `exit 2` here would fire it and
# `git checkout --` would destroy precisely the uncommitted edits the guard
# exists to protect.
cd "$PROJECT_DIR"
if ! git diff --quiet -- components/alpha_hwr/; then
  echo -e "${RED}✗ components/alpha_hwr has uncommitted changes.${NC}"
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
search, replace = os.environ["SEARCH"], os.environ["REPLACE"]
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
    restore_all
    continue
  fi

  if (cd "$TESTS_DIR" && make clean >/dev/null 2>&1 && make test >/dev/null 2>&1); then
    echo -e "${RED}✗ SURVIVED — the suite passed with this broken${NC}"
    SURVIVORS+=("$name")
  else
    echo -e "${GREEN}✓ caught${NC}"
    CAUGHT=$((CAUGHT + 1))
  fi
  restore_all
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
echo -e "${GREEN}✓ Every mutation was caught${NC}"
