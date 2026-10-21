#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"
build_dir="$repo_root/build/player_layout_xml_cache_test"
mkdir -p "$build_dir"

cc=${CC:-cc}
common_flags='-std=gnu11 -O0 -g -ffunction-sections -fdata-sections -DPLAYER_LAYOUTS_CACHE_TEST -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/core -Isrc/plugins -Ilvgl -Ilvgl/src/others/translation'
$cc $common_flags -c src/ui/player_layouts.c -o "$build_dir/player_layouts.o"
$cc $common_flags -c src/ui/player_layout_xml_cache_test.c -o "$build_dir/player_layout_xml_cache_test.o"
$cc -Wl,--gc-sections -o "$build_dir/player_layout_xml_cache_test" \
    "$build_dir/player_layouts.o" "$build_dir/player_layout_xml_cache_test.o"
"$build_dir/player_layout_xml_cache_test"
