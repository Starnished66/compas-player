#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GUI_POWER_HUB_SPEC_BUILD_DIR:-$repo_root/build_ui_test/power_hub}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    echo "GUI Power Hub spec requires cached build_host/lvgl and build_host/ui/ui_fonts.o" >&2
    exit 2
fi

cc="${CC:-cc}"
mapfile -t lvgl_objects < <(find "$repo_root/build_host/lvgl" -name '*.o' -print | sort)
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and Power Hub module; cached LVGL/font objects are uninstrumented."
fi

exe="$build_dir/gui_power_hub_spec"
"$cc" "${common_flags[@]}" "$repo_root/src/ui/gui_power_hub_spec.c" \
    "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" \
    "$repo_root/build_host/mbedtls/sha256.o" "$repo_root/build_host/mbedtls/platform_util.o" \
    -Wl,--gc-sections -Wl,--wrap=lv_snapshot_take -lpthread -lm -o "$exe"
run_dir="$build_dir/run"
mkdir -p "$run_dir"
(
    cd "$run_dir"
    COMPAS_FONT_ROOT="$repo_root/assets/fonts" "$exe"
) > "$build_dir/gui_power_hub_spec.trace"
echo "GUI Power Hub characterization passed"
cat "$build_dir/gui_power_hub_spec.trace"
