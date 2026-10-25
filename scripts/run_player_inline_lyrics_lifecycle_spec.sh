#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

build_dir="$repo_root/build/player_inline_lyrics_lifecycle"
mkdir -p "$build_dir"

if [[ ! -d build_host/lvgl || ! -d build_host/lv_xml || ! -f build_host/ui/ui_fonts.o || \
      ! -f build_host/ui/player/player_seekbar.o || \
      ! -f build_host/ui/player/player_lyrics_backdrop.o || \
      ! -f build_host/ui/player/player_timeline_labels.o ]]; then
    make host
fi

cc=${CC:-cc}
common_flags=(
    -std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui
    -Isrc/core -Isrc/plugins -Isrc/ui/player -Itests/player -Ilvgl -Ilvgl/src/others/translation
    -Ithird_party/libwavpack/include
)
sanitize_flags=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitize_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    common_flags+=("${sanitize_flags[@]}")
fi

if [[ -n "${PLAYER_INLINE_LYRICS_BASELINE_ROOT:-}" ]]; then
    fixture="$PLAYER_INLINE_LYRICS_BASELINE_ROOT/tests/player/player_layout_bind_test.c"
    if [[ ! -f "$fixture" ]]; then fixture="$PLAYER_INLINE_LYRICS_BASELINE_ROOT/src/ui/player_layout_bind_test.c"; fi
    [[ -f "$fixture" ]] || { echo "missing baseline binder fixture: $fixture" >&2; exit 2; }
    common_flags+=("-DPLAYER_INLINE_LYRICS_BIND_TEST_SOURCE=\"$fixture\"")
    common_flags+=(-DPLAYER_INLINE_LYRICS_BASELINE_MODE=1)
fi

albumart_info_objects=()
if [[ -z "${PLAYER_INLINE_LYRICS_BASELINE_ROOT:-}" ]]; then
    "$cc" "${common_flags[@]}" -c src/library/albumart_info.c \
        -o "$build_dir/albumart_info.o"
    albumart_info_objects=("$build_dir/albumart_info.o")
fi

"$cc" "${common_flags[@]}" -c src/ui/player/player_layouts.c -o "$build_dir/player_layouts.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_layout_builtin.c -o "$build_dir/player_layout_builtin.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_volume_popup.c -o "$build_dir/player_volume_popup.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_transport_images.c -o "$build_dir/player_transport_images.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_artwork_pixels.c -o "$build_dir/player_artwork_pixels.o"
"$cc" "${common_flags[@]}" -c src/library/cue_parser.c -o "$build_dir/cue_parser.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_seek_input.c -o "$build_dir/player_seek_input.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_queue_model.c -o "$build_dir/player_queue_model.o"
"$cc" "${common_flags[@]}" -c src/ui/player/player_queue_storage.c -o "$build_dir/player_queue_storage.o"
"$cc" "${common_flags[@]}" -c src/library/queue_resume.c -o "$build_dir/queue_resume.o"
"$cc" "${common_flags[@]}" -c src/library/playlist_files.c -o "$build_dir/playlist_files.o"
"$cc" "${common_flags[@]}" -c tests/player/player_inline_lyrics_lifecycle_spec.c \
    -o "$build_dir/player_inline_lyrics_lifecycle_spec.o"
# Preserve the existing binder harness's ignore-all policy for unrelated GUI
# symbols; the queue model and its concrete storage dependencies are linked above.
"$cc" "${sanitize_flags[@]}" -no-pie -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all \
    -o "$build_dir/player_inline_lyrics_lifecycle_spec" \
    "$build_dir/player_layouts.o" "$build_dir/player_layout_builtin.o" \
    "$build_dir/player_volume_popup.o" "$build_dir/player_transport_images.o" \
    "$build_dir/player_artwork_pixels.o" \
    "$build_dir/player_inline_lyrics_lifecycle_spec.o" \
    "${albumart_info_objects[@]}" "$build_dir/cue_parser.o" "$build_dir/player_seek_input.o" \
    "$build_dir/player_queue_model.o" "$build_dir/player_queue_storage.o" \
    "$build_dir/queue_resume.o" "$build_dir/playlist_files.o" \
    build_host/library/sacd_iso.o build_host/libwavpack/libwavpack.a \
    build_host/ui/ui_fonts.o build_host/ui/player/player_seekbar.o \
    build_host/ui/player/player_lyrics_backdrop.o build_host/ui/player/player_timeline_labels.o \
    build_host/ui/transition_compositor.o build_host/ui/frosted_glass.o \
    $(find build_host/lvgl build_host/lv_xml build_host/mbedtls -name '*.o' -print) -lm

"$build_dir/player_inline_lyrics_lifecycle_spec" --interactions-only
