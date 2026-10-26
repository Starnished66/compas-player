#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${GUI_PLAYLISTS_SPEC_BUILD_DIR:-$repo_root/build_ui_test/gui_playlists}"
mkdir -p "$build_dir"

if [[ ! -f "$repo_root/build_host/app/main.o" || ! -d "$repo_root/build_host/lvgl" ]]; then
    echo "GUI playlists spec requires host build objects under build_host" >&2
    exit 2
fi

cc="${CC:-cc}"
common_flags=(-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec and included playlist modules; cached host objects are uninstrumented."
fi
"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/library/playlists/gui_playlists_spec.c" \
    -o "$build_dir/gui_playlists_spec.o"

python3 - "$repo_root" "$build_dir" <<'PY'
import os, pathlib, shlex, subprocess, sys
root, build = map(pathlib.Path, sys.argv[1:])
sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if os.environ.get('SANITIZE') == '1' else []
link = subprocess.run(['make', '--no-print-directory', '-s', 'print-host-link', 'BOARD=r1'],
                      cwd=root, text=True, capture_output=True, check=True).stdout
args = shlex.split(link)
main_obj = build/'main_without_entry.o'
subprocess.run(['objcopy', '--redefine-sym', 'main=compas_player_main',
                str(root/'build_host/app/main.o'), str(main_obj)], check=True)
objects, flags = [str(main_obj)], []
skip = {'build_host/app/main.o', 'build_host/ui/library/playlists/gui_playlists.o',
        'build_host/ui/library/playlists/gui_playlist_target.o'}
for arg in args:
    if arg in skip:
        continue
    if arg.endswith('.o'):
        objects.append(str(root / arg))
    else:
        flags.append(arg)
missing = [path for path in objects if not pathlib.Path(path).is_file()]
if missing:
    print('GUI playlists spec requires a complete host build; missing ' + ', '.join(pathlib.Path(p).name for p in missing[:5]), file=sys.stderr)
    sys.exit(2)
cmd = ['g++', *sanitize, '-Wl,--gc-sections', '-o', str(build/'gui_playlists_spec'),
       str(build/'gui_playlists_spec.o'), *objects, *flags, '-pthread']
subprocess.run(cmd, cwd=root, check=True)
PY

run_dir="$build_dir/run"
mkdir -p "$run_dir"
(
    cd "$run_dir"
    "$build_dir/gui_playlists_spec"
)
