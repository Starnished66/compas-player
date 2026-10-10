#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$repo_root/build_test/audio_http_range_seek"
mkdir -p "$build_dir"

make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path '*/libsndfile/*' ! -name 'ogg_demux.o' ! -name 'main.o' -print)
gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/library" \
    $(sdl2-config --cflags) -c "$repo_root/src/audio/audio_http_range_seek_spec.c" \
    -o "$build_dir/audio_http_range_seek_spec.o"
g++ -pthread "$build_dir/audio_http_range_seek_spec.o" "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" "$repo_root/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lm -o "$build_dir/audio_http_range_seek_spec"

ffmpeg -v error -f lavfi -i 'anoisesrc=sample_rate=48000:duration=120:amplitude=0.3' \
    -c:a flac -y "$build_dir/range-seek.flac"
ffmpeg -v error -f lavfi -i 'anoisesrc=sample_rate=48000:duration=120:amplitude=0.3' \
    -c:a libmp3lame -write_xing 1 -id3v2_version 0 -y "$build_dir/range-seek.mp3"
ffmpeg -v error -f lavfi -i 'anoisesrc=sample_rate=48000:duration=120:amplitude=0.3' \
    -c:a aac -movflags +faststart -y "$build_dir/range-seek.m4a"
ffmpeg -v error -f lavfi -i 'anoisesrc=sample_rate=48000:duration=120:amplitude=0.3' \
    -c:a aac -y "$build_dir/range-seek-moov-end.m4a"
SDL_AUDIODRIVER=dummy "$build_dir/audio_http_range_seek_spec" \
    "$build_dir/range-seek.flac" "$build_dir/range-seek.mp3" "$build_dir/range-seek.m4a" \
    "$build_dir/range-seek-moov-end.m4a"
