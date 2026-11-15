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
    -c "$repo/src/ui/player/screen/tests/test_subsonic_lyrics_async.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/$build/libwavpack/libwavpack.a" "$repo/$build/libsndfile/libsndfile.a" \
    -Wl,--wrap=subsonic_get_lyrics $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
