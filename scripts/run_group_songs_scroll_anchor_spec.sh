#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GROUP_SONGS_SCROLL_ANCHOR_SPEC_BUILD_DIR:-$repo_root/build_ui_test/group_songs_scroll_anchor}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections \
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network" \
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/lvgl" \
    -I"$repo_root/lvgl/src/others/translation" \
    -c "$repo_root/src/ui/group_songs_scroll_anchor_spec.c" \
    -o "$build_dir/group_songs_scroll_anchor_spec.o"
"$cc" -no-pie -Wl,--gc-sections -o "$build_dir/group_songs_scroll_anchor_spec" \
    "$build_dir/group_songs_scroll_anchor_spec.o" -lpthread -lm
"$build_dir/group_songs_scroll_anchor_spec"
