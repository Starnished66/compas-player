#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../../.." && pwd)"
assets="${COMPAS_HOST_ASSETS:?set COMPAS_HOST_ASSETS to a directory with theme1, theme2 and fonts}"
assets="$(cd "$assets" && pwd)"
runtime="$(mktemp -d "${TMPDIR:-/tmp}/remote_dac_state.XXXXXX")"
trap 'rm -rf "$runtime"' EXIT
[[ -f "$repo/build_host/app/main.o" ]] || { echo 'run make host first' >&2; exit 1; }
mkdir -p "$runtime/.compas" "$runtime/music/.plugins" "$runtime/scripts/plugins"
printf 'setup_complete=1\n' > "$runtime/.compas/settings.txt"
ln -s "$assets" "$runtime/assets"
objcopy --redefine-sym main=compas_original_main "$repo/build_host/app/main.o" "$runtime/main.o"
mapfile -t objs < <(find "$repo/build_host" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' -print | sort)
gcc -std=c11 -DHOST_BUILD=1 -I"$repo" -I"$repo/lvgl/src" -I"$repo/cJSON" \
    -c "$repo/src/network/remote/control/tests/test_remote_dac_state.c" -o "$runtime/test.o"
g++ -Wl,--wrap=gui_shell_bt_dac_owns_output "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/build_host/libwavpack/libwavpack.a" "$repo/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
