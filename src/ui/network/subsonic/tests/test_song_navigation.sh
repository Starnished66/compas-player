#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../../.." && pwd)"
assets="${COMPAS_HOST_ASSETS:?set COMPAS_HOST_ASSETS to a directory with theme1, theme2 and fonts}"
assets="$(cd "$assets" && pwd)"
runtime="$(mktemp -d "${TMPDIR:-/tmp}/dac_screens.XXXXXX")"
trap 'rm -rf "$runtime"' EXIT
build="build_host${BOARD:+_$BOARD}"
board_flags=()
[[ -z "${BOARD:-}" ]] || board_flags+=("-DBOARD_${BOARD^^}=1")
[[ -f "$repo/$build/app/main.o" ]] || { echo 'run make host first' >&2; exit 1; }
mkdir -p "$runtime/.compas" "$runtime/music/.plugins" "$runtime/scripts/plugins"
printf 'setup_complete=1\n' > "$runtime/.compas/settings.txt"
ln -s "$assets" "$runtime/assets"
objcopy --redefine-sym main=compas_original_main "$repo/$build/app/main.o" "$runtime/main.o"
mapfile -t objs < <(find "$repo/$build" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' -print | sort)
gcc -std=c11 -D_DEFAULT_SOURCE -DHOST_BUILD=1 "${board_flags[@]}" -I"$repo" -I"$repo/lvgl/src" -I"$repo/cJSON" \
    -c "$repo/src/ui/network/subsonic/tests/test_song_navigation.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/$build/libwavpack/libwavpack.a" "$repo/$build/libsndfile/libsndfile.a" \
    -Wl,--wrap=gui_navigation_transition_in_progress,--wrap=show_error_toast,--wrap=wifi_get_status,--wrap=gui_subsonic_global_open_artist,--wrap=gui_subsonic_global_open_album,--wrap=start_subsonic_browse,--wrap=subsonic_get_song,--wrap=subsonic_get_artist_albums,--wrap=metadata_db_get_song_by_path,--wrap=subsonic_get_album_songs,--wrap=subsonic_cache_songs_save $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
