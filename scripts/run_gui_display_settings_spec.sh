#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GUI_DISPLAY_SETTINGS_SPEC_BUILD_DIR:-${TMPDIR:-/tmp}/compas-gui-display-settings}"
mkdir -p "$build_dir"
if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/i18n.o" ||
      ! -f "$repo_root/build_host/ui/i18n_catalog.o" ]]; then
    echo "cached host LVGL and i18n objects are required; this runner will not rebuild the host graph" >&2
    exit 2
fi

mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
cc="${CC:-cc}"
# The included full source has one existing usb_dsd_mode_t/int signedness
# warning in an unrelated list builder; keep all other warnings fatal.
flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-error=sign-compare
    -Wno-unused-parameter -Wno-unused-function
    -Wno-missing-field-initializers -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core"
    -I"$repo_root/src/audio" -I"$repo_root/src/network" -I"$repo_root/src/library" -I"$repo_root/src/hardware"
    -I"$repo_root/src/plugins" -I"$repo_root/src/ui/player" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src"
    -I"$repo_root/lvgl/src/others/translation")
if [[ "${SANITIZE:-0}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the callback spec and included gui_settings.c; cached LVGL/i18n objects are uninstrumented."
fi

exe="$build_dir/gui_display_settings_spec"
"$cc" "${flags[@]}" "$repo_root/tests/gui_display_settings_spec.c" \
    "${lvgl_objects[@]}" "$repo_root/build_host/ui/i18n.o" \
    "$repo_root/build_host/ui/i18n_catalog.o" \
    -Wl,--gc-sections -lpthread -lm -o "$exe"
run_dir="$build_dir/run"
mkdir -p "$run_dir"
(cd "$run_dir" && ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" timeout 30s "$exe")
