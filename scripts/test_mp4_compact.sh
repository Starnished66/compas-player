#!/usr/bin/env bash
# Compact-mode MP4 sample tables (long audiobooks) must read the same bytes
# as fully expanded tables, including across interleaved tracks' chunks and
# table-window refills.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/mp4-compact"
mkdir -p "$build_dir"

ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=60' \
    -ac 2 -c:a aac -b:a 96k -y "$build_dir/plain.m4a"
# A second audio track and a chapter track interleave their chunks with the
# first track's, so consecutive chunks of the played track are not adjacent.
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=20000\ntitle=One\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=20000\nEND=60000\ntitle=Two\n' \
    > "$build_dir/chapters.txt"
ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=60' \
    -i "$build_dir/chapters.txt" -map_metadata 1 -map_chapters 1 \
    -ac 2 -c:a aac -b:a 96k -f ipod -y "$build_dir/chapters.m4b"
ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=60' \
    -f lavfi -i 'sine=frequency=880:sample_rate=44100:duration=60' -map 0 -map 1 \
    -c:a aac -b:a 64k -y "$build_dir/two-tracks.m4a"

cc_demux() {
    local out="$1"
    shift
    cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc/audio "$@" \
        src/audio/mp4_compact_spec.c src/audio/mp4_demux.c -o "$out"
}
cc_demux "$build_dir/expanded" -DMP4_EXPAND_MAX_SAMPLES=0xFFFFFFFFu
cc_demux "$build_dir/compact" -DMP4_EXPAND_MAX_SAMPLES=16 -DMP4_STSZ_WINDOW=100 -DMP4_STCO_WINDOW=7

for name in plain.m4a chapters.m4b two-tracks.m4a; do
    "$build_dir/expanded" "$build_dir/$name" > "$build_dir/$name.expanded"
    "$build_dir/compact" "$build_dir/$name" > "$build_dir/$name.compact"
    cmp "$build_dir/$name.expanded" "$build_dir/$name.compact"
    echo "ok $name ($(head -1 "$build_dir/$name.expanded"))"
done
