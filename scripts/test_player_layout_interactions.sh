#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

board_define=${PLAYER_LAYOUT_BOARD_DEFINE:-BOARD_R1}
build_dir="${PLAYER_LAYOUT_TEST_BUILD_DIR:-${TMPDIR:-/tmp}/player_layout_interactions_${board_define}}"
matrix_mode=${PLAYER_LAYOUT_RENDER_MATRIX:-0}
mkdir -p "$build_dir"
if [ "$matrix_mode" = 1 ]; then
    plugin_root=${COMPAS_PLUGINS_ROOT:-"$repo_root/../compas-plugins/plugins"}
    if [ ! -d "$plugin_root" ]; then
        echo "layout render matrix requires the plugin layout directory: $plugin_root" >&2
        exit 2
    fi
    export COMPAS_PLUGINS_ROOT="$plugin_root"
    cover_image=${PLAYER_LAYOUT_COVER_IMAGE:-"$repo_root/assets/theme2/playing_plane/default_cover_565.png"}
    if [ ! -f "$cover_image" ]; then
        echo "layout render matrix cover image not found: $cover_image" >&2
        exit 2
    fi
    if ! command -v magick >/dev/null 2>&1; then
        echo "layout render matrix needs ImageMagick (magick) to prepare RGB565 artwork data" >&2
        exit 2
    fi
    export PLAYER_LAYOUT_COVER_IMAGE="$cover_image"
    magick "$cover_image" -strip -depth 8 "$build_dir/aurora-cover.ppm"
    export PLAYER_LAYOUT_COVER_PPM="$build_dir/aurora-cover.ppm"
    mkdir -p "$build_dir/contact"
    export PLAYER_LAYOUT_SNAPSHOT_DIR="$build_dir/contact"
fi

if [ ! -d build_host/lvgl ] || [ ! -d build_host/lv_xml ] ||
   [ ! -f build_host/ui/ui_fonts.o ] || [ ! -f build_host/mbedtls/sha256.o ] ||
   [ ! -f build_host/mbedtls/platform_util.o ]; then
    echo "cached host LVGL/XML/font graph is incomplete; building host with one job" >&2
    make host -j1
    if [ ! -d build_host/lvgl ] || [ ! -d build_host/lv_xml ] ||
       [ ! -f build_host/ui/ui_fonts.o ] || [ ! -f build_host/mbedtls/sha256.o ] ||
       [ ! -f build_host/mbedtls/platform_util.o ]; then
        echo "host build completed without the LVGL/XML/font objects required by this test" >&2
        exit 2
    fi
fi

cc=${CC:-cc}
common_flags="-std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections -DHOST_BUILD=1 -D${board_define} -DLV_CONF_INCLUDE_SIMPLE=1 -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/ui/player -Itests/player -Isrc/core -Isrc/plugins -Ilvgl -Ilvgl/src/others/translation -Ithird_party/libwavpack/include"
vendor_objects=$(make -s print-host-link | tr ' ' '\n' | awk '$0 ~ /^build_host\/(lvgl|lv_xml)\/.*\.o$/')

$cc $common_flags -c src/ui/player/player_layouts.c -o "$build_dir/player_layouts.o"
$cc $common_flags -c src/ui/player/player_layout_builtin.c -o "$build_dir/player_layout_builtin.o"
$cc $common_flags -c tests/player/player_layouts_test.c -o "$build_dir/player_layouts_test.o"
$cc -no-pie -Wl,--gc-sections -o "$build_dir/player_layouts_test" \
    "$build_dir/player_layouts.o" "$build_dir/player_layouts_test.o" \
    build_host/ui/ui_fonts.o \
    build_host/mbedtls/sha256.o build_host/mbedtls/platform_util.o \
    $vendor_objects -lm

$cc $common_flags -c src/ui/player/player_volume_popup.c -o "$build_dir/player_volume_popup.o"
$cc $common_flags -c src/ui/player/player_transport_images.c -o "$build_dir/player_transport_images.o"
$cc $common_flags -c src/ui/player/player_artwork_pixels.c -o "$build_dir/player_artwork_pixels.o"
$cc $common_flags -c src/ui/player/player_seek_input.c -o "$build_dir/player_seek_input.o"
$cc $common_flags -c src/ui/player/player_queue_model.c -o "$build_dir/player_queue_model.o"
$cc $common_flags -c src/ui/player/player_queue_storage.c -o "$build_dir/player_queue_storage.o"
$cc $common_flags -c src/library/queue_resume.c -o "$build_dir/queue_resume.o"
$cc $common_flags -c src/library/playlist_files.c -o "$build_dir/playlist_files.o"
$cc $common_flags -c src/ui/player/player_seekbar.c -o "$build_dir/player_seekbar.o"
$cc $common_flags -c src/ui/player/player_cover_fade.c -o "$build_dir/player_cover_fade.o"
$cc $common_flags -c src/ui/player/player_lyrics_backdrop.c -o "$build_dir/player_lyrics_backdrop.o"
$cc $common_flags -c src/library/albumart_info.c -o "$build_dir/albumart_info.o"
$cc $common_flags -c src/library/cue_parser.c -o "$build_dir/cue_parser.o"
$cc $common_flags -c tests/player/player_layout_bind_test.c -o "$build_dir/player_layout_bind_test.o"
# Keep this binder harness's pre-existing ignore-all policy for unrelated GUI
# symbols; the queue model and its concrete storage dependencies are linked above.
$cc -no-pie -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
    -o "$build_dir/player_layout_bind_test" \
    "$build_dir/player_layouts.o" "$build_dir/player_layout_builtin.o" \
    "$build_dir/player_volume_popup.o" "$build_dir/player_transport_images.o" \
    "$build_dir/player_artwork_pixels.o" "$build_dir/albumart_info.o" \
    "$build_dir/player_layout_bind_test.o" "$build_dir/player_seek_input.o" \
    "$build_dir/player_queue_model.o" "$build_dir/player_queue_storage.o" \
    "$build_dir/queue_resume.o" "$build_dir/playlist_files.o" "$build_dir/cue_parser.o" \
    build_host/library/sacd_iso.o build_host/libwavpack/libwavpack.a \
    build_host/ui/ui_fonts.o "$build_dir/player_seekbar.o" \
    "$build_dir/player_cover_fade.o" "$build_dir/player_lyrics_backdrop.o" build_host/ui/frosted_glass.o \
    $vendor_objects build_host/mbedtls/sha256.o build_host/mbedtls/platform_util.o -lm

"$build_dir/player_layouts_test" --interactions-only
"$build_dir/player_layouts_test" --plugin-owner-only
"$build_dir/player_layout_bind_test" --interactions-only
if [ "$matrix_mode" = 1 ]; then
    "$build_dir/player_layouts_test" --plugin-render-matrix
    "$build_dir/player_layout_bind_test" --plugin-render-matrix
    "$build_dir/player_layout_bind_test" --builtin-render-only
fi
