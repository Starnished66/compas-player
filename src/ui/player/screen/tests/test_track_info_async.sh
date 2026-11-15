#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../../.." && pwd)"
assets="${COMPAS_HOST_ASSETS:?set COMPAS_HOST_ASSETS to a directory with theme1, theme2 and fonts}"
assets="$(cd "$assets" && pwd)"
runtime="$(mktemp -d "${TMPDIR:-/tmp}/dac_screens.XXXXXX")"
trap 'rm -rf "$runtime"' EXIT
[[ -f "$repo/build_host/app/main.o" ]] || { echo 'run make host first' >&2; exit 1; }
mkdir -p "$runtime/.compas" "$runtime/music/.plugins" "$runtime/scripts/plugins"
printf 'setup_complete=1\n' > "$runtime/.compas/settings.txt"
ln -s "$assets" "$runtime/assets"
objcopy --redefine-sym main=compas_original_main "$repo/build_host/app/main.o" "$runtime/main.o"
mapfile -t objs < <(find "$repo/build_host" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' -print | sort)
gcc -std=c11 -D_DEFAULT_SOURCE -DHOST_BUILD=1 -I"$repo" -I"$repo/lvgl/src" -I"$repo/cJSON" \
    -c "$repo/src/ui/player/screen/tests/test_track_info_async.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/build_host/libwavpack/libwavpack.a" "$repo/build_host/libsndfile/libsndfile.a" \
    -Wl,--wrap=audio_probe_file_format $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
python3 - <<'PYDATA'
import wave
with wave.open('music/selected.wav', 'wb') as f:
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(48000)
    f.writeframes(b'\0' * (48000 * 2 * 2 * 2))
with open('music/selected.cue', 'w') as f:
    f.write('FILE "selected.wav" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:01:00\n')
PYDATA
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./test
