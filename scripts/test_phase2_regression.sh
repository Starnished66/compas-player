#!/bin/bash
set -euo pipefail

# Directory of this script and repository root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Portable default persistent local build directory on host disk, or caller override
DEFAULT_SCRATCH="${REPO_ROOT}/build_ui_test/phase2-regression"
SCRATCH_DIR="${SCRATCH_DIR:-$DEFAULT_SCRATCH}"
LOG_DIR="${LOG_DIR:-$SCRATCH_DIR}"

mkdir -p "${SCRATCH_DIR}"
mkdir -p "${LOG_DIR}"

TEST_BIN="${SCRATCH_DIR}/phase2_regression_test"
TEST_LOG="${LOG_DIR}/phase2_regression_test.log"

echo "=== Ensuring fresh host prerequisites ==="
make -C "${REPO_ROOT}" -j2 host BOARD=r1

echo "=== Compiling Phase 2 Regression Test ==="
gcc -DHOST_BUILD=1 \
    -I"${REPO_ROOT}" \
    -I"${REPO_ROOT}/src/audio" \
    -I"${REPO_ROOT}/src/network" \
    -I"${REPO_ROOT}/src/library" \
    -I"${REPO_ROOT}/src/hardware" \
    -I"${REPO_ROOT}/src/ui" \
    -I"${REPO_ROOT}/src/core" \
    -I"${REPO_ROOT}/src/plugins" \
    -I"${REPO_ROOT}/lvgl" \
    -I"${REPO_ROOT}/lvgl/src" \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections \
    "${REPO_ROOT}/src/ui/phase2_regression_test.c" \
    "${REPO_ROOT}/build_host/ui/screen_builders.o" \
    "${REPO_ROOT}/build_host/ui/gui_theme.o" \
    "${REPO_ROOT}/build_host/ui/i18n.o" \
    "${REPO_ROOT}/build_host/ui/i18n_catalog.o" \
    $(find "${REPO_ROOT}/build_host/lvgl" -name "*.o") \
    -Wl,--gc-sections \
    -lpthread -lm \
    -o "${TEST_BIN}"

echo "Compilation successful: ${TEST_BIN}"
echo "=== Running Phase 2 Regression Test ==="
"${TEST_BIN}" 2>&1 | tee "${TEST_LOG}"

EXIT_CODE="${PIPESTATUS[0]}"
if [ "${EXIT_CODE}" -eq 0 ]; then
    echo "=== Phase 2 Regression Tests PASSED (exit code 0) ==="
else
    echo "=== Phase 2 Regression Tests FAILED (exit code ${EXIT_CODE}) ==="
    exit "${EXIT_CODE}"
fi
