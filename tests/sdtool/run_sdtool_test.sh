#!/bin/sh
# End-to-end test of compas-sdtool on a generated card: info, scan, an
# unchanged rescan that writes nothing, art, incremental changes, and the
# time-zone rule for file times (see docs/SDTOOL.md, "File times").
# Usage: run_sdtool_test.sh [path/to/compas-sdtool]
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
tool=${1:-$root/build_sdtool/compas-sdtool}
work=${TMPDIR:-/tmp}/compas-sdtool-test.$$
trap 'rm -rf "$work"' EXIT INT TERM
card=$work/card
mkdir -p "$work"
python3 "$here/make_fixture.py" "$card"

fail() {
  echo "FAIL: $*" >&2
  tail -n 5 "$work/out" "$work/err" >&2 2>/dev/null || true
  exit 1
}

# Runs the tool, keeps stdout in $work/out, checks every line is a JSON object
# and returns the last line.
run() {
  "$tool" "$@" > "$work/out" 2> "$work/err" || return 1
  python3 -I - "$work/out" <<'PY'
import json, sys
lines = open(sys.argv[1]).read().splitlines()
assert lines, "no output"
for line in lines:
    assert isinstance(json.loads(line), dict), line
print(lines[-1])
PY
}

field() { python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$1" "$2"; }

# Every file under .compas with its size, modification time and inode: a
# rewrite, even of identical bytes, shows as a change.
snapshot() {
  python3 -I - "$card/.compas" <<'PY'
import os, sys
for base, dirs, files in os.walk(sys.argv[1]):
    for name in sorted(files):
        path = os.path.join(base, name)
        st = os.stat(path)
        print(os.path.relpath(path, sys.argv[1]), st.st_size, st.st_mtime_ns, st.st_ino)
PY
}

shift_times() {
  python3 -I - "$card" "$1" <<'PY'
import os, sys
root, delta = sys.argv[1], int(sys.argv[2])
for base, dirs, files in os.walk(root):
    dirs[:] = [d for d in dirs if d != ".compas"]
    for name in files:
        path = os.path.join(base, name)
        st = os.stat(path)
        os.utime(path, (st.st_atime + delta, st.st_mtime + delta))
PY
}

# info on a card with no database: zero songs, and nothing is created.
r=$(run info "$card") || fail "info on empty card"
[ "$(field "$r" songs)" = 0 ] || fail "empty card songs"
[ "$(field "$r" generation)" = 0 ] || fail "empty card generation"
[ -e "$card/.compas" ] && fail "info created .compas"
[ "$(field "$r" database_format)" = 5 ] || fail "database_format"

# scan builds the database: 5 songs (one with a non-ASCII path), the Audiobooks folder is excluded.
r=$(run scan "$card") || fail "first scan"
[ "$(field "$r" songs)" = 5 ] || fail "songs after scan: $r"
[ "$(field "$r" written)" = True ] || fail "first scan wrote nothing"
grep -rqa "/data/mnt/sd_0/Artist One/Album One/01 First.mp3" "$card/.compas" || fail "paths are not device paths"
grep -rqa "/data/mnt/sd_0/Artist .*n./.*lbum ./01 .*\.mp3" "$card/.compas" || fail "non-ASCII path not stored"
grep -rqa "$card" "$card/.compas" && fail "host path leaked into the database"

# an unchanged scan, plain or --full, writes nothing.
snapshot > "$work/before"
r=$(run scan "$card") || fail "second scan"
[ "$(field "$r" written)" = False ] || fail "second scan wrote: $r"
[ "$(field "$r" parsed)" = 0 ] || fail "second scan parsed files: $r"
r=$(run scan "$card" --full) || fail "full scan"
[ "$(field "$r" written)" = False ] || fail "full rescan wrote: $r"
snapshot > "$work/after"
cmp -s "$work/before" "$work/after" || fail "unchanged scans modified the database"

# art: two albums have a cover, two have none.
r=$(run art "$card") || fail "art"
[ "$(field "$r" written)" = 2 ] || fail "art written: $r"
[ "$(field "$r" no_art)" = 2 ] || fail "art no_art: $r"
[ "$(ls "$card/.compas/albumart" | wc -l)" = 4 ] || fail "art file count"
snapshot > "$work/before"
r=$(run art "$card") || fail "second art"
[ "$(field "$r" written)" = 0 ] || fail "second art wrote: $r"
[ "$(field "$r" unchanged)" = 2 ] || fail "second art unchanged: $r"
snapshot > "$work/after"
cmp -s "$work/before" "$work/after" || fail "unchanged art run modified the cache"

# --board is validated; device.json supplies the default.
run art "$card" --board nonsense && fail "bad board accepted"
grep -q '"code":"bad_board"' "$work/out" || fail "bad board error line"
r=$(run art "$card" --board r3proii) || fail "art --board"
[ "$(field "$r" board)" = r3proii ] || fail "board echo"
printf '{"schema":1,"model":"r3ii_2025"}\n' > "$card/.compas/device.json"
r=$(run art "$card") || fail "art with device.json"
[ "$(field "$r" board)" = r3ii_2025 ] || fail "board from device.json"

# file times written under another time zone must not trigger a rescan:
# FAT keeps local time, so the same file reads differently per computer.
for delta in 32400 -18000 20700 3600 -93600 93600; do
  shift_times "$delta"
  snapshot > "$work/before"
  r=$(run scan "$card") || fail "scan after $delta s shift"
  [ "$(field "$r" parsed)" = 0 ] && [ "$(field "$r" written)" = False ] || fail "zone shift $delta rescanned: $r"
  r=$(run art "$card") || fail "art after $delta s shift"
  [ "$(field "$r" written)" = 0 ] || fail "zone shift $delta rebuilt covers: $r"
  snapshot > "$work/after"
  cmp -s "$work/before" "$work/after" || fail "zone shift $delta wrote to the card"
done
# ... but a real edit still is noticed, including one off the 15-minute grid.
shift_times 1000
r=$(run scan "$card") || fail "scan after off-grid shift"
[ "$(field "$r" parsed)" = 5 ] || fail "off-grid change not noticed: $r"
shift_times 108000
r=$(run scan "$card") || fail "scan after 30 h shift"
[ "$(field "$r" parsed)" = 5 ] || fail "30 h change not noticed: $r"

# a changed file is re-read, a removed one dropped.
python3 "$here/make_fixture.py" "$work/other" >/dev/null
cp "$work/other/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/02 Extra.mp3"
r=$(run scan "$card") || fail "scan with new file"
[ "$(field "$r" songs)" = 6 ] || fail "new file not added: $r"
[ "$(field "$r" parsed)" = 1 ] || fail "only the new file should be read: $r"
rm "$card/Artist Three/Album Three/02 Extra.mp3"
r=$(run scan "$card") || fail "scan after removal"
[ "$(field "$r" songs)" = 5 ] || fail "removed file kept: $r"

# failures are one error line and a nonzero exit.
"$tool" scan "$work/missing" > "$work/out" 2>/dev/null && fail "missing card accepted"
grep -q '"event":"error","code":"card_not_found"' "$work/out" || fail "missing card error line"
"$tool" bogus "$card" > "$work/out" 2>/dev/null && fail "bogus command accepted"
echo "sdtool: PASS"
