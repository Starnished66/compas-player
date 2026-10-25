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

# File times written under another time zone must not trigger a rescan: FAT
# keeps local time, so one file reads differently per computer. sdtool corrects
# for it (a database the player wrote calibrates the offset; --mtime-offset
# states it), so shifting every file as another zone would changes nothing.
# The correction must be one this computer's own zone could explain, so each
# shift runs with the zone it simulates.
for step in 32400:JST-9 -18000:EST5 20700:NPT-5:45 3600:CET-1; do
  delta=${step%%:*}
  zone=${step#*:}
  shift_times "$delta"
  snapshot > "$work/before"
  r=$(TZ=$zone; export TZ; run scan "$card") || fail "scan after $delta s shift"
  [ "$(field "$r" parsed)" = 0 ] && [ "$(field "$r" written)" = False ] || fail "zone shift $delta rescanned: $r"
  [ "$(field "$r" mtime_offset)" = "$((-delta))" ] || fail "applied offset not reported: $r"
  r=$(TZ=$zone; export TZ; run art "$card") || fail "art after $delta s shift"
  [ "$(field "$r" written)" = 0 ] || fail "zone shift $delta rebuilt covers: $r"
  snapshot > "$work/after"
  cmp -s "$work/before" "$work/after" || fail "zone shift $delta wrote to the card"
  shift_times "$((-delta))"
done
shift_times 7200
r=$(run scan "$card" --mtime-offset -7200) || fail "scan with --mtime-offset"
[ "$(field "$r" parsed)" = 0 ] || fail "--mtime-offset not applied: $r"
# a shift this computer's zone cannot explain (here UTC) is not corrected:
# everything is read again, and the card is left consistent.
shift_times 32400
r=$(TZ=UTC0; export TZ; run scan "$card") || fail "scan after implausible shift"
[ "$(field "$r" parsed)" = 5 ] && [ "$(field "$r" mtime_offset)" = 0 ] || fail "implausible shift corrected: $r"
# a shift that is no zone offset is not corrected either.
shift_times 1000
r=$(run scan "$card") || fail "scan after off-grid shift"
[ "$(field "$r" parsed)" = 5 ] || fail "off-grid change not noticed: $r"

# a same-size edit is always noticed, whatever step its time moved by.
python3 -I - "$card" <<'PY'
import os, sys
path = os.path.join(sys.argv[1], "Artist Three", "Album Three", "01 Fourth.mp3")
data = bytearray(open(path, "rb").read())
data[-1] ^= 0xFF
st = os.stat(path)
open(path, "wb").write(data)
os.utime(path, (st.st_atime + 1800, st.st_mtime + 1800))
PY
r=$(run scan "$card") || fail "scan after same-size edit"
[ "$(field "$r" parsed)" = 1 ] || fail "same-size edit not noticed: $r"

# file names may hold a newline (not on Windows, which rejects it); results
# stay with the right files.
if [ -z "${SDTOOL_TEST_NO_NEWLINE_NAMES:-}" ]; then
python3 -I - "$card" <<'PY'
import os, shutil, sys
d = os.path.join(sys.argv[1], "Artist Three", "Album Three")
shutil.copy(os.path.join(d, "01 Fourth.mp3"), os.path.join(d, "05 New\nline.mp3"))
PY
r=$(run scan "$card") || fail "scan with newline in a name"
[ "$(field "$r" songs)" = 6 ] && [ "$(field "$r" parsed)" = 1 ] || fail "newline name: $r"
rm "$card/Artist Three/Album Three/"05*
run scan "$card" >/dev/null || fail "scan after removing the newline name"
fi

# a tag parser that crashes or hangs fails that file only: no placeholder row,
# even when it is the only file to read; the scan still commits.
cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/03 Crash.mp3"
for fault in crash:03 hang:03; do
  r=$(SDTOOL_TEST_FAULT=$fault; export SDTOOL_TEST_FAULT; run scan "$card") || fail "lone bad file ($fault) blocked the scan"
  [ "$(field "$r" failed)" = 1 ] && [ "$(field "$r" songs)" = 5 ] && [ "$(field "$r" written)" = True ] ||
    fail "lone bad file ($fault): $r"
done
cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/04 Hang.mp3"
r=$(SDTOOL_TEST_FAULT=crash:03; export SDTOOL_TEST_FAULT; run scan "$card") || fail "scan with crashing parser"
[ "$(field "$r" failed)" = 1 ] && [ "$(field "$r" songs)" = 6 ] || fail "crash handling: $r"
cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/07 Fine.mp3"
r=$(SDTOOL_TEST_FAULT=hang:03; export SDTOOL_TEST_FAULT; run scan "$card") || fail "scan with hanging parser"
[ "$(field "$r" failed)" = 1 ] && [ "$(field "$r" songs)" = 7 ] || fail "hang handling: $r"
rm "$card/Artist Three/Album Three/03 Crash.mp3" "$card/Artist Three/Album Three/04 Hang.mp3" "$card/Artist Three/Album Three/07 Fine.mp3"
r=$(run scan "$card") || fail "scan after removing test files"
[ "$(field "$r" songs)" = 5 ] || fail "test files kept: $r"

# a worker that exits cleanly mid-batch is retried once, in a fresh worker.
for n in a b c; do cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/08$n.mp3"; done
r=$(SDTOOL_TEST_FAULT=exit:08; export SDTOOL_TEST_FAULT; run scan "$card") || fail "scan with an exiting worker"
[ "$(field "$r" failed)" = 0 ] && [ "$(field "$r" songs)" = 8 ] || fail "retry after a clean exit: $r"
rm "$card/Artist Three/Album Three/"08?.mp3
run scan "$card" >/dev/null || fail "scan after removing the retry files"

# three corrupt files are three failed files, not a broken worker: it commits.
for n in a b c; do cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/09$n.mp3"; done
r=$(SDTOOL_TEST_FAULT=crash:09; export SDTOOL_TEST_FAULT; run scan "$card") || fail "scan with three corrupt files"
[ "$(field "$r" failed)" = 3 ] && [ "$(field "$r" songs)" = 5 ] && [ "$(field "$r" written)" = True ] || fail "three corrupt files: $r"
rm "$card/Artist Three/Album Three/"09?.mp3
run scan "$card" >/dev/null || fail "scan after removing the corrupt files"

# a parser process that cannot work is an error, and nothing is committed.
cp "$card/Artist Three/Album Three/01 Fourth.mp3" "$card/Artist Three/Album Three/06 Start.mp3"
snapshot > "$work/before"
r=$(SDTOOL_TEST_FAULT=start; export SDTOOL_TEST_FAULT; run scan "$card") && fail "broken worker accepted: $r"
grep -q '"code":"worker_failed"' "$work/out" || fail "worker_failed error line"
snapshot > "$work/after"
cmp -s "$work/before" "$work/after" || fail "broken worker wrote to the card"
rm "$card/Artist Three/Album Three/06 Start.mp3"
run scan "$card" >/dev/null || fail "scan after removing the start test file"

# info reads without writing, even when the identity marker is missing.
rm -f "$card/.compas/catalog.incarnation"
snapshot > "$work/before"
run info "$card" >/dev/null || fail "info"
snapshot > "$work/after"
cmp -s "$work/before" "$work/after" || fail "info wrote to the card"
run scan "$card" >/dev/null || fail "scan after info"

# a database that cannot be read is rebuilt, once: later scans leave the card alone.
printf 'not a generation pointer' > "$card/.compas/tagcache.gen"
r=$(run scan "$card") || fail "scan of damaged database"
[ "$(field "$r" written)" = True ] && [ "$(field "$r" songs)" = 5 ] || fail "damaged database not rewritten: $r"
r=$(run scan "$card") || fail "scan after repair"
[ "$(field "$r" written)" = False ] || fail "repaired database rewritten again: $r"
cp "$card/.compas/catalog.incarnation" "$work/incarnation"
run scan "$card" >/dev/null || fail "third scan"
cmp -s "$work/incarnation" "$card/.compas/catalog.incarnation" || fail "catalog identity rotated on an unchanged scan"

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
