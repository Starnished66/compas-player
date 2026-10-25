#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sanitize="${SANITIZE:-0}"
if [[ "$sanitize" == "1" ]]; then
    run_dir="$repo_root/build_ui_test/library_search-sanitize"
else
    run_dir="$repo_root/build_ui_test/library_search"
fi
mkdir -p "$run_dir"

python3 - "$repo_root" "$run_dir" "$sanitize" <<'PY'
import json, pathlib, shlex, subprocess, sys

root, run = map(pathlib.Path, sys.argv[1:3])
sanitize = sys.argv[3] == '1'
compile_db = root/'compile_commands.json'
if not compile_db.is_file():
    raise SystemExit('compile_commands.json is missing; run the normal host build first')
commands = json.loads(compile_db.read_text())
entry = next((item for item in commands if item['file'] == 'src/ui/gui_library.c'), None)
if entry is None:
    raise SystemExit('compile_commands.json has no src/ui/gui_library.c entry')

# Reuse the normal host translation-unit flags. Trace flags can be left over
# from another generated compile database, while the cached backend graph is
# built in normal mode.
args = [arg for arg in entry['arguments'] if not arg.startswith('-DUI_PERF_TRACE')]
if not any(arg == '-DBOARD_R1' or arg.startswith('-DBOARD_R1=') for arg in args):
    raise SystemExit('Search spec requires an R1 host build: make host BOARD=r1')
args[args.index('src/ui/gui_library.c')] = str(root/'tests/library/library_search_spec.c')
output = args[args.index('-o') + 1]
args[args.index(output)] = str(run/'library_search_spec.o')
args.extend(['-ffunction-sections', '-fdata-sections'])
if sanitize:
    args.extend(['-fsanitize=address,undefined', '-fno-omit-frame-pointer'])
args[args.index('-c') + 1:args.index('-c') + 1] = ['-Isrc/ui/library', '-I'+str(run)]
subprocess.run(args, cwd=root, check=True)

link = subprocess.run(['make', '-s', '--no-print-directory', 'print-host-link', 'BOARD=r1'],
                      cwd=root, text=True, capture_output=True, check=True).stdout
link_args = shlex.split(link)
skip = {'build_host/main.o', 'build_host/ui/gui_library.o',
        'build_host/ui/library/library_search.o',
        'build_host/ui/library/library_az_index.o'}
if not skip.issubset(set(link_args)):
    raise SystemExit('host link graph does not contain the expected main/GUI/search objects')

main_input = root/'build_host/main.o'
if not main_input.is_file():
    raise SystemExit('cached host backend is incomplete: build_host/main.o is missing')
main_obj = run/'main_without_entry.o'
subprocess.run(['objcopy', '--redefine-sym', 'main=compas_player_main',
                str(main_input), str(main_obj)], check=True)

objects = [str(main_obj)]
flags = []
missing = []
for arg in link_args:
    if arg in skip:
        continue
    if arg.endswith(('.o', '.a')):
        path = root/arg
        if not path.is_file():
            missing.append(arg)
        else:
            objects.append(str(path))
    else:
        flags.append(arg)
if missing:
    raise SystemExit('cached host backend dependencies are missing: '+', '.join(missing[:12]))

wraps = [
    'free', 'pthread_create', 'pthread_join', 'compact_list_set_items',
    'compact_list_set_paged_provider', 'metadata_db_search_names',
    'metadata_db_get_songs_filtered_page', 'metadata_db_get_song_count',
    'metadata_db_get_group_counts', 'metadata_db_get_song_recency_offset',
    'song_album_thumbnail_key', 'refresh_now_playing_indicators', 't9_keypad_release',
    'library_thumbnails_begin_view', 'library_thumbnails_end_view',
]
ldwrap = ['-Wl,--wrap='+name for name in wraps]
san_flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else []
binary = run/'library_search_spec'
subprocess.run(['g++', *san_flags, '-Wl,--gc-sections', *ldwrap, '-o', str(binary),
                str(run/'library_search_spec.o'), *objects, *flags, '-pthread'],
               cwd=root, check=True)
PY
if [[ "$sanitize" == "1" ]]; then
    (cd "$run_dir" && ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
        COMPAS_FONT_ROOT="$repo_root/assets/fonts" ./library_search_spec)
else
    (cd "$run_dir" && COMPAS_FONT_ROOT="$repo_root/assets/fonts" ./library_search_spec)
fi
