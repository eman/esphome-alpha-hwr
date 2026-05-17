#!/usr/bin/env bash
# ==============================================================================
# Static Analysis for ESPHome ALPHA HWR Component
# ==============================================================================
#
# Runs cppcheck on the component source files to detect bugs, performance
# issues, and portability concerns without requiring ESP32 cross-compilation.
#
# Usage:
#   ./tools/lint.sh              # Run analysis
#   ./tools/lint.sh --strict     # Treat warnings as errors
#
# Requirements:
#   - cppcheck (brew install cppcheck)
#
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
COMPONENT_DIR="$PROJECT_DIR/components/alpha_hwr"
DHW_DIR="$PROJECT_DIR/components/dhw_demand"
TESTS_DIR="$PROJECT_DIR/tests"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

STRICT=0
if [[ "${1:-}" == "--strict" ]]; then
  STRICT=1
fi

echo "=========================================="
echo "  Static Analysis: ALPHA HWR Component"
echo "=========================================="

# Check for cppcheck
if ! command -v cppcheck &>/dev/null; then
  echo -e "${RED}Error: cppcheck not found. Install with: brew install cppcheck${NC}"
  exit 1
fi

echo ""
echo "Tool: cppcheck $(cppcheck --version 2>&1 | head -1)"
echo "Targets: $COMPONENT_DIR, $DHW_DIR, $TESTS_DIR"
echo ""

# Run cppcheck
# - Suppress missingInclude: ESP-IDF/ESPHome headers aren't available on host
# - Suppress unusedFunction: Functions called from ESPHome YAML lambdas
# - Suppress syntaxError: Some ESP32-specific syntax not parseable on host
CPPCHECK_ARGS=(
  --enable=warning,performance,portability
  --suppress=missingInclude
  --suppress=unusedFunction
  --suppress=unmatchedSuppression
  --suppress=syntaxError
  --suppress=unusedStructMember
  --suppress=knownConditionTrueFalse
  --suppress=useStlAlgorithm
  --std=c++11
  --language=c++
  --inline-suppr
  --template='{file}:{line}: {severity}: {message} [{id}]'
)

if [[ $STRICT -eq 1 ]]; then
  CPPCHECK_ARGS+=(--error-exitcode=1)
  echo -e "${YELLOW}Running in strict mode (warnings are errors)${NC}"
else
  CPPCHECK_ARGS+=(--error-exitcode=0)
fi

echo "Running cppcheck..."
echo ""

OUTPUT=$(cppcheck "${CPPCHECK_ARGS[@]}" \
  "$COMPONENT_DIR"/*.cpp "$COMPONENT_DIR"/*.h \
  "$DHW_DIR"/*.cpp "$DHW_DIR"/*.h \
  "$TESTS_DIR"/*.cpp "$TESTS_DIR"/*.h 2>&1)

WARNINGS=$(echo "$OUTPUT" | grep -c "warning:" || true)
ERRORS=$(echo "$OUTPUT" | grep -c "error:" || true)
PERF=$(echo "$OUTPUT" | grep -c "performance:" || true)
STYLE=$(echo "$OUTPUT" | grep -c "style:" || true)

if [[ -n "$OUTPUT" ]]; then
  echo "$OUTPUT"
  echo ""
fi

echo "=========================================="
echo "  Results"
echo "=========================================="
echo -e "  Errors:      ${ERRORS}"
echo -e "  Warnings:    ${WARNINGS}"
echo -e "  Performance: ${PERF}"
echo -e "  Style:       ${STYLE}"
echo ""

if [[ $ERRORS -gt 0 ]]; then
  echo -e "${RED}✗ Analysis found errors${NC}"
  exit 1
elif [[ $WARNINGS -gt 0 && $STRICT -eq 1 ]]; then
  echo -e "${RED}✗ Analysis found warnings (strict mode)${NC}"
  exit 1
else
  echo -e "${GREEN}✓ Analysis passed${NC}"
  exit 0
fi
