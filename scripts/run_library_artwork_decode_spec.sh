#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${LIBRARY_ART_BASELINE_SOURCE:-}"
build_dir="${LIBRARY_ART_SPEC_BUILD_DIR:-$repo_root/build_ui_test/library_artwork_decode}"
mkdir -p "$build_dir"

final_available=0
if [[ -f "$repo_root/src/library/library_album_art_decode.c" && -f "$repo_root/src/library/library_artist_art.c" ]]; then
    final_available=1
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested frozen library artwork baseline does not exist: $baseline_source" >&2
    exit 2
fi
if [[ -z "$baseline_source" && "$final_available" == 0 ]]; then
    echo "set LIBRARY_ART_BASELINE_SOURCE to characterize the pre-move implementation" >&2
    exit 2
fi

if [[ -n "$baseline_source" ]]; then
python3 - "$baseline_source" "$build_dir/library_artwork_decode_fixture.inc" <<'PY'
from pathlib import Path
import re
import sys

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")


def matching_brace(text, opening):
    depth, state, escaped, i = 0, "code", False, opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif ch == "/" and nxt == "/": state = "line"; i += 1
        elif ch == "/" and nxt == "*": state = "block"; i += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced C body in frozen gui_library.c")


def function(name):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", source):
        opening = source.find("(", match.start())
        depth, i = 1, opening + 1
        while i < len(source) and depth:
            if source[i] == "(": depth += 1
            elif source[i] == ")": depth -= 1
            i += 1
        while i < len(source) and source[i].isspace(): i += 1
        if i < len(source) and source[i] == "{":
            start = source.rfind("\n", 0, match.start()) + 1
            return source[start:matching_brace(source, i) + 1] + "\n"
    raise RuntimeError(f"frozen artwork function not found: {name}")


def exact_between(start, end):
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]

music_root = exact_between("#ifdef HOST_BUILD\n  #define MUSIC_ROOT_DIR", "#define PLAYLISTS_DIR")
sibling = exact_between("typedef enum {\n    SIBLING_ART_FOUND", "} sibling_art_result_t;") + "} sibling_art_result_t;\n"
macro_names = {"ALBUM_ART_METADATA_TIMEOUT_MS", "ALBUM_ART_SIBLING_FALLBACK_MAX"}
macros = "\n".join(line for line in source.splitlines()
                   if line.startswith("#define ") and line.split()[1] in macro_names) + "\n"
functions = [
    "song_album_thumbnail_key", "album_thumbnail_sized_cache_hit", "album_player_cache_hit",
    "gui_library_album_cover_cached", "album_source_mtime_with_art_mtime", "album_source_mtime",
    "album_thumbnail_maybe_store_player_cache", "album_thumbnail_pixels_from_source",
    "generate_player_cover_from_data", "gui_library_generate_player_cover",
    "album_thumbnail_try_embedded_art_from_path", "album_thumbnail_load_or_decode_with_mtime",
    "album_thumbnail_load_or_decode_ex", "thumbnail_directory_of", "thumbnail_parent_directory",
    "thumbnail_directory_name", "artist_directory_signature", "artist_thumbnail_load_persistent",
    "artist_thumbnail_set_alias", "artist_thumbnail_from_album_group", "artist_thumbnail_folder_for_song",
    "artist_thumbnail_load_or_decode", "album_thumbnail_reload_persisted",
]
fixture = (
    "/* Generated exact function slices from frozen gui_library.c. */\n"
    + music_root + "\n" + sibling + macros
    + "\nstatic __thread bool album_art_materialize_player;\n"
    + "static __thread bool album_art_thumb_from_user_file;\n\n"
    + "\n".join(function(name) for name in functions)
)
for name, old, new in [
    ("song_album_thumbnail_key", "static uint64_t", "uint64_t"),
    ("album_thumbnail_sized_cache_hit", "static bool", "bool"),
    ("album_player_cache_hit", "static bool", "bool"),
    ("album_source_mtime_with_art_mtime", "static time_t", "time_t"),
    ("album_source_mtime", "static time_t", "time_t"),
    ("album_thumbnail_load_or_decode_with_mtime", "static bool", "bool"),
    ("album_thumbnail_load_or_decode_ex", "static bool", "bool"),
    ("album_thumbnail_reload_persisted", "static bool", "bool"),
    ("artist_thumbnail_load_or_decode", "static artist_thumbnail_result_t", "artist_thumbnail_result_t"),
]:
    code = function(name)
    if not code.startswith(old):
        raise RuntimeError(f"unexpected baseline linkage for {name}")
    fixture = fixture.replace(code, code.replace(old, new, 1))
output_path.write_text(fixture, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function
    -ffunction-sections -fdata-sections -D_GNU_SOURCE -DHOST_BUILD=1 -DBOARD_R1
    -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src" -I"$repo_root/src/library"
    -I"$repo_root/src/core" -I"$repo_root/src/audio" -I"$repo_root/src/ui"
    -I"$repo_root/src/hardware" -I"$repo_root/src/plugins" -I"$repo_root/lvgl"
    -I"$repo_root/lvgl/src" -I"$repo_root/jpeg_vendor_config" -I"$repo_root/jpeg"
    -I"$repo_root/tinfl" -I"$build_dir")

if [[ "$final_available" == 1 && -z "$baseline_source" ]]; then
    mode=final
else
    mode=baseline
fi
[[ "$mode" == final ]] && common_flags+=(-DLIBRARY_ART_FINAL=1)
[[ "$mode" == baseline ]] && common_flags+=(-DLIBRARY_ART_BASELINE=1)
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included artwork functions; cached decoder dependencies are uninstrumented."
fi

required_objects=("$repo_root/build_host/tinfl/miniz_tinfl.o")
for object in "${required_objects[@]}"; do
    if [[ ! -f "$object" ]]; then
        echo "missing cached decoder dependency: $object (run the host build first)" >&2
        exit 2
    fi
done
shopt -s nullglob globstar
jpeg_objects=("$repo_root"/build_host/jpeg/*.o)
if [[ ${#jpeg_objects[@]} -eq 0 ]]; then
    echo "missing cached JPEG objects under build_host/jpeg (run the host build first)" >&2
    exit 2
fi
lvgl_objects=("$repo_root"/build_host/lvgl/**/*.o)

exe="$build_dir/library_artwork_${mode}_spec"
"$cc" "${common_flags[@]}" "$repo_root/src/library/library_artwork_decode_spec.c" \
    "$repo_root/src/library/albumart.c" "$repo_root/src/library/cover_decode.c" \
    "$repo_root/src/library/artwork_coordinator.c" "$repo_root/src/library/artwork_memory.c" \
    "$repo_root/src/library/artwork_failure_cache.c" \
    "$repo_root/src/library/albumart_info.c" \
    "${required_objects[@]}" "${jpeg_objects[@]}" "${lvgl_objects[@]}" \
    -Wl,--gc-sections -lpthread -lm -o "$exe"

run_dir="$build_dir/run_$mode"
rm -rf "$run_dir"
mkdir -p "$run_dir"
(cd "$run_dir" && "$exe") > "$build_dir/library_artwork_${mode}_spec.trace"
echo "Library artwork $mode characterization passed"
cat "$build_dir/library_artwork_${mode}_spec.trace"
