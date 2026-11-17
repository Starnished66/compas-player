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
    -c "$repo/src/ui/player/screen/tests/test_track_info_async.c" -o "$runtime/test.o"
g++ \
    "$runtime/test.o" "$runtime/main.o" "${objs[@]}" \
    "$repo/$build/libwavpack/libwavpack.a" "$repo/$build/libsndfile/libsndfile.a" \
    -Wl,--wrap=register_swipe_dead_zone,--wrap=gui_player_transport_button,--wrap=build_compact_list_screen,--wrap=gui_library_open_album,--wrap=gui_library_open_album_menu \
    -Wl,--wrap=compact_list_set_now_playing,--wrap=compact_list_scroll_to_index \
    -Wl,--wrap=metadata_db_get_songs_filtered_page,--wrap=open_song_context_menu \
    -Wl,--wrap=metadata_db_snapshot_open,--wrap=metadata_db_snapshot_count,--wrap=metadata_db_snapshot_retain,--wrap=metadata_db_snapshot_close \
    -Wl,--wrap=metadata_db_build_az_table,--wrap=metadata_db_load_rating_order,--wrap=metadata_db_get_songs_at_title_ranks,--wrap=metadata_db_get_song_count,--wrap=metadata_db_try_get_generation \
    -Wl,--wrap=metadata_db_load_top_played_songs,--wrap=metadata_db_load_favorite_songs,--wrap=compact_list_set_items \
    -Wl,--wrap=on_file_selected,--wrap=set_player_source_group_songs_direct \
    -Wl,--wrap=metadata_db_load_rated_albums,--wrap=album_group_resolve \
    -Wl,--wrap=audio_probe_file_format,--wrap=rating_writer_stars \
    -Wl,--wrap=gui_player_has_active_track,--wrap=gui_player_get_current_track_path $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
cd "$runtime"
python3 - <<'PYDATA'
import wave
with wave.open('music/selected.wav', 'wb') as f:
    f.setnchannels(2)
    f.setsampwidth(2)
    f.setframerate(48000)
    f.writeframes(b'\0' * (48000 * 2 * 2 * 2))
import shutil
for i in range(6):
    shutil.copyfile('music/selected.wav', f'music/rating-{i}.wav')
with open('music/selected.cue', 'w') as f:
    f.write('FILE "selected.wav" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 00:01:00\n')
PYDATA
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
export COMPAS_TEST_MINIMAL_XML="$repo/src/ui/player/screen/tests/minimal_rating.xml"
./test
if [[ -z "${COMPAS_TEST_PLAYER_XML:-}" ]]; then
    case "${BOARD:-}" in
        r3proii) suffix=_480x720 ;;
        r3ii_2025) suffix=_320x480 ;;
        tempotec_v1) suffix=_240x320 ;;
        *) suffix= ;;
    esac
    plugins="${COMPAS_TEST_PLUGINS:-$repo/../compas-plugins/plugins}"
    orbit="$plugins/OrbitPlayer/player_layouts/orbit_player$suffix.xml"
    [[ -f "$orbit" ]] || { echo "Missing layout fixture: $orbit" >&2; exit 1; }
    mapfile -t layouts < <(python3 - "$plugins" "$suffix" <<'PYLAYOUTS'
from pathlib import Path
import sys
import xml.etree.ElementTree as ET
for path in sorted(Path(sys.argv[1]).glob("*/player_layouts/*.xml")):
    suffix = sys.argv[2]
    matches = path.stem.endswith(suffix) if suffix else not any(
        path.stem.endswith(s) for s in ("_480x720", "_320x480", "_240x320"))
    if matches and ET.parse(path).find('.//*[@name="favorite_circle"]') is not None:
        print(path)
PYLAYOUTS
    )
    for xml in "${layouts[@]}"; do
        echo "Checking rating layout: $xml"
        anchor="$(python3 - "$xml" <<'PYANCHOR'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
parents = {c: p for p in root.iter() for c in p}
node = root.find('.//*[@name="favorite_circle"]')
vals = [node.get(k, "") for k in ("x", "y", "width", "height")]
if all(v.lstrip("-").isdigit() for v in vals):
    parent = parents.get(node)
    name = parent.get("name") if parent is not None else None
    print(" ".join(vals + ([name] if name else [])))
PYANCHOR
        )"
        COMPAS_TEST_ANCHOR="$anchor" COMPAS_TEST_PLAYER_XML="$xml" ./test
    done
fi
