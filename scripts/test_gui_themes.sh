#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DEFAULT_SCRATCH="${REPO_ROOT}/build_ui_test/theme-service-runner"
SCRATCH_DIR="${SCRATCH_DIR:-$DEFAULT_SCRATCH}"
mkdir -p "${SCRATCH_DIR}"

TEST_BIN="${SCRATCH_DIR}/gui_themes_test"

echo "=== Compiling gui_themes regression test runner ==="
gcc -DHOST_BUILD=1 \
    -DGUI_THEMES_TEST=1 \
    -I"${REPO_ROOT}" \
    -I"${REPO_ROOT}/src/audio" \
    -I"${REPO_ROOT}/src/network" \
    -I"${REPO_ROOT}/src/library" \
    -I"${REPO_ROOT}/src/hardware" \
    -I"${REPO_ROOT}/src/ui" \
    -I"${REPO_ROOT}/src/core" \
    -I"${REPO_ROOT}/src/plugins" \
    -I"${REPO_ROOT}/mbedtls/include" \
    -I"${REPO_ROOT}/lvgl" \
    -I"${REPO_ROOT}/lvgl/src" \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    -ffunction-sections -fdata-sections \
    "${REPO_ROOT}/src/ui/gui_themes.c" \
    "${REPO_ROOT}/src/core/theme_file.c" \
    "${REPO_ROOT}/src/ui/gui_themes_test.c" \
    $(find "${REPO_ROOT}/build_host/lvgl" -name "*.o") \
    $(find "${REPO_ROOT}/build_host/mbedtls" -name "*.o") \
    "${REPO_ROOT}/build_host/ui/i18n.o" \
    "${REPO_ROOT}/build_host/ui/i18n_catalog.o" \
    -Wl,--gc-sections \
    -lpthread -lm \
    -o "${TEST_BIN}"

echo "=== Running gui_themes test runner ==="
"${TEST_BIN}"
echo "=== gui_themes tests PASSED ==="
