#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${LV_EVDEV_EXTERNAL_HANDOFF_BUILD_DIR:-${TMPDIR:-$repo_root/build_ui_test}/lv_evdev_external_handoff}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "cached host LVGL objects missing; run the approved host build first" >&2
    exit 2
fi
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
if ((${#lvgl_objects[@]} == 0)); then
    echo "cached host LVGL object graph is empty" >&2
    exit 2
fi
rm -f "$build_dir/liblvgl_host.a"
ar rcs "$build_dir/liblvgl_host.a" "${lvgl_objects[@]}"

# The test compiles the real target evdev driver against the ABI-compatible
# cached LVGL core graph, then drives actual LVGL pointer/object processing.
gcc -std=gnu11 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE=1 \
    -I"$repo_root" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
    "$repo_root/tests/lv_evdev_external_handoff_spec.c" \
    "$build_dir/liblvgl_host.a" \
    -Wl,--wrap=ioctl -lm -pthread -o "$build_dir/lv_evdev_external_handoff_spec"
"$build_dir/lv_evdev_external_handoff_spec"
