#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${PLAYER_TRACK_TRANSITION_SPEC_BUILD_DIR:-$repo_root/build_ui_test/player_track_transition}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

cc=${CC:-cc}
common_flags=(
    -std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network"
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui"
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/lvgl"
    -I"$repo_root/lvgl/src/others/translation"
)

"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player_track_transition_runtime_spec.c" \
    -o "$build_dir/player_track_transition_runtime_spec.o"
"$cc" -no-pie -Wl,--gc-sections -o "$build_dir/player_track_transition_runtime_spec" \
    "$build_dir/player_track_transition_runtime_spec.o" \
    "$repo_root/build_host/ui/ui_fonts.o" \
    "$repo_root/build_host/mbedtls/sha256.o" \
    "$repo_root/build_host/mbedtls/platform_util.o" \
    $(find "$repo_root/build_host/lvgl" "$repo_root/build_host/lv_xml" -name '*.o' -print) \
    -lpthread -lm

"$build_dir/player_track_transition_runtime_spec"
