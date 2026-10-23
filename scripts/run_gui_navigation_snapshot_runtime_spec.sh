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
mbedtls_objects=(
    "$repo_root/build_host/mbedtls/sha256.o"
    "$repo_root/build_host/mbedtls/platform_util.o"
)
for object in "${mbedtls_objects[@]}"; do
    if [[ ! -f "$object" ]]; then
        echo "Host mbedTLS object is missing: $object" >&2
        exit 1
    fi
done

gcc -std=gnu11 -Wall -Wextra -Werror -pthread -DHOST_BUILD=1 -DBOARD_R1 \
    -DLV_CONF_INCLUDE_SIMPLE=1 -ffunction-sections -fdata-sections \
    -I"$repo_root" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
    -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio" \
    -I"$repo_root/src/network" -I"$repo_root/src/library" -I"$repo_root/src/hardware" \
    -I"$repo_root/src/plugins" -I"$repo_root/mbedtls/include" \
    "$repo_root/src/ui/gui_navigation_snapshot_runtime_spec.c" \
    "$repo_root/src/ui/ui_fonts.c" \
    "$repo_root/src/ui/ui_snapshot_prefetch.c" \
    "${lvgl_objects[@]}" \
    "${mbedtls_objects[@]}" \
    -Wl,--gc-sections -Wl,--wrap=lv_snapshot_take -Wl,--wrap=lv_timer_create \
    -Wl,--wrap=lv_anim_start -lpthread -lm \
    -o "$build_dir/gui_navigation_snapshot_runtime_spec"

COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$build_dir/gui_navigation_snapshot_runtime_spec"
