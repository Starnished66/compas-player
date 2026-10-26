#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${PLAYER_TRACK_TRANSITION_SPEC_BUILD_DIR:-$repo_root/build_ui_test/player_track_transition}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/text/fonts/ui_fonts.o" ||
      ! -f "$repo_root/build_host/library/artwork/albumart/albumart_info.o" ||
      ! -f "$repo_root/build_host/library/sacd/sacd_iso.o" ||
      ! -f "$repo_root/build_host/ui/text/i18n/i18n.o" ||
      ! -f "$repo_root/build_host/ui/text/i18n/i18n_catalog.o" ||
      ! -f "$repo_root/build_host/libwavpack/libwavpack.a" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

cc=${CC:-cc}
common_flags=(
    -std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network"
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui"
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/src/ui/player" -I"$repo_root/tests/player" -I"$repo_root/lvgl"
    -I"$repo_root/lvgl/src/others/translation" -I"$repo_root/third_party/libwavpack/include"
)

"$cc" "${common_flags[@]}" -c "$repo_root/src/library/parsers/cue/cue_parser.c" \
    -o "$build_dir/cue_parser.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player/transport/player_seek_input.c" \
    -o "$build_dir/player_seek_input.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player/queue/player_queue_storage.c" \
    -o "$build_dir/player_queue_storage.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player/queue/player_queue_model.c" \
    -o "$build_dir/player_queue_model.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/library/playlists/queue_resume.c" \
    -o "$build_dir/queue_resume.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/library/playlists/playlist_files.c" \
    -o "$build_dir/playlist_files.o"
"$cc" "${common_flags[@]}" -c "$repo_root/tests/player/player_track_transition_runtime_spec.c" \
    -o "$build_dir/player_track_transition_runtime_spec.o"
"$cc" -no-pie -Wl,--gc-sections -Wl,--wrap=pthread_create -Wl,--wrap=free -o "$build_dir/player_track_transition_runtime_spec" \
    "$build_dir/player_track_transition_runtime_spec.o" \
    "$repo_root/build_host/library/artwork/albumart/albumart_info.o" \
    "$build_dir/cue_parser.o" \
    "$repo_root/build_host/library/sacd/sacd_iso.o" \
    "$repo_root/build_host/libwavpack/libwavpack.a" \
    "$repo_root/build_host/ui/text/fonts/ui_fonts.o" \
    "$repo_root/build_host/ui/text/i18n/i18n.o" \
    "$repo_root/build_host/ui/text/i18n/i18n_catalog.o" \
    "$build_dir/player_seek_input.o" \
    "$build_dir/player_queue_storage.o" \
    "$build_dir/player_queue_model.o" \
    "$build_dir/queue_resume.o" \
    "$build_dir/playlist_files.o" \
    "$repo_root/build_host/mbedtls/sha256.o" \
    "$repo_root/build_host/mbedtls/platform_util.o" \
    $(find "$repo_root/build_host/lvgl" "$repo_root/build_host/lv_xml" -name '*.o' -print) \
    -lpthread -lm

"$build_dir/player_track_transition_runtime_spec"
