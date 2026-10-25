#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

board_define="${THEME_RENDER_BOARD_DEFINE:-BOARD_R1}"
DEFAULT_SCRATCH="${REPO_ROOT}/build_ui_test/theme-service-${board_define}"
SCRATCH_DIR="${SCRATCH_DIR:-$DEFAULT_SCRATCH}"
mkdir -p "${SCRATCH_DIR}"
SCRATCH_DIR="$(cd "${SCRATCH_DIR}" && pwd)"
mkdir -p "${SCRATCH_DIR}/contact"
cd "${REPO_ROOT}"
vendor_objects=$(make -s print-host-link | tr ' ' '\n' | awk '$0 ~ /^build_host\/(lvgl|lv_xml)\/.*\.o$/')

matrix_mode="${THEME_RENDER_MATRIX:-0}"
plugin_root="${COMPAS_PLUGINS_ROOT:-${REPO_ROOT}/../compas-plugins/plugins}"
if [[ "${matrix_mode}" == "1" && ! -d "${plugin_root}" ]]; then
    echo "theme render matrix requires the plugin theme directory: ${plugin_root}" >&2
    exit 2
fi

TEST_BIN="${SCRATCH_DIR}/gui_themes_test"

echo "=== Compiling gui_themes regression test runner ==="
gcc -DHOST_BUILD=1 -D"${board_define}" \
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
    -I"${REPO_ROOT}/lvgl/src/others/translation" \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    -g \
    -ffunction-sections -fdata-sections \
    "${REPO_ROOT}/src/ui/gui_themes.c" \
    "${REPO_ROOT}/src/core/theme_file.c" \
    "${REPO_ROOT}/src/core/theme_file_values.c" \
    "${REPO_ROOT}/src/ui/gui_themes_test.c" \
    ${vendor_objects} \
    "${REPO_ROOT}/build_host/mbedtls/sha256.o" \
    "${REPO_ROOT}/build_host/mbedtls/platform_util.o" \
    "${REPO_ROOT}/build_host/ui/ui_fonts.o" \
    "${REPO_ROOT}/build_host/ui/i18n.o" \
    "${REPO_ROOT}/build_host/ui/i18n_catalog.o" \
    -Wl,--gc-sections \
    -lpthread -lm \
    -o "${TEST_BIN}"

echo "=== Running gui_themes test runner ==="
THEME_RENDER_TMPDIR="${SCRATCH_DIR}" THEME_RENDER_SNAPSHOT_DIR="${SCRATCH_DIR}/contact" \
    THEME_RENDER_MATRIX="${matrix_mode}" COMPAS_PLUGINS_ROOT="${plugin_root}" "${TEST_BIN}"
echo "=== gui_themes tests PASSED ==="
