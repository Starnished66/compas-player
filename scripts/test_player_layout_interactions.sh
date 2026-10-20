#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

build_dir="$repo_root/build/player_layout_interactions"
mkdir -p "$build_dir"

if [ ! -d build_host/lvgl ] || [ ! -d build_host/lv_xml ] ||
   [ ! -f build_host/ui/ui_fonts.o ] || [ ! -f build_host/ui/player_seekbar.o ] ||
   [ ! -f build_host/ui/player_lyrics_backdrop.o ]; then
    make host
fi

cc=${CC:-cc}
common_flags='-std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/core -Isrc/plugins -Ilvgl -Ilvgl/src/others/translation'

$cc $common_flags -c src/ui/player_layouts.c -o "$build_dir/player_layouts.o"
$cc $common_flags -c src/ui/player_layouts_test.c -o "$build_dir/player_layouts_test.o"
$cc -no-pie -Wl,--gc-sections -o "$build_dir/player_layouts_test" \
    "$build_dir/player_layouts.o" "$build_dir/player_layouts_test.o" \
    build_host/ui/ui_fonts.o \
    $(find build_host/lvgl build_host/lv_xml -name '*.o' -print) -lm

$cc $common_flags -c src/ui/player_layout_bind_test.c -o "$build_dir/player_layout_bind_test.o"
$cc -no-pie -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
    -o "$build_dir/player_layout_bind_test" \
    "$build_dir/player_layouts.o" "$build_dir/player_layout_bind_test.o" \
    build_host/ui/ui_fonts.o build_host/ui/player_seekbar.o \
    build_host/ui/player_lyrics_backdrop.o \
    $(find build_host/lvgl build_host/lv_xml build_host/mbedtls -name '*.o' -print) -lm

"$build_dir/player_layouts_test" --interactions-only
"$build_dir/player_layouts_test" --plugin-owner-only
"$build_dir/player_layout_bind_test" --interactions-only
