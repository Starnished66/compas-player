#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GUI_NAVIGATION_SNAPSHOT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/gui_navigation_snapshot}"
mkdir -p "$build_dir"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print)
if ((${#lvgl_objects[@]} == 0)); then
    echo "Host LVGL objects are missing; build the host configuration first" >&2
    exit 1
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread -DHOST_BUILD=1 -DBOARD_R1 \
    -DLV_CONF_INCLUDE_SIMPLE=1 -ffunction-sections -fdata-sections \
    -I"$repo_root" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
    -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio" \
    -I"$repo_root/src/network" -I"$repo_root/src/library" -I"$repo_root/src/hardware" \
    -I"$repo_root/src/plugins" \
    "$repo_root/src/ui/gui_navigation_snapshot_runtime_spec.c" \
    "${lvgl_objects[@]}" \
    -Wl,--gc-sections -Wl,--wrap=lv_snapshot_take -Wl,--wrap=lv_timer_create \
    -Wl,--wrap=lv_anim_start -lpthread -lm \
    -o "$build_dir/gui_navigation_snapshot_runtime_spec"

"$build_dir/gui_navigation_snapshot_runtime_spec"
