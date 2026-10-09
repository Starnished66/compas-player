#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/opus-stability"
mkdir -p "$build_dir"

# Compile only the vendored codec objects needed by the standalone decoder.
mapfile -t sources < <(find opus/src opus/celt opus/silk opus/silk/float \
    -maxdepth 1 -name '*.c' ! -name '*demo.c' ! -name 'opus_compare.c' | sort)
objects=()
for source in "${sources[@]}"; do objects+=("build_host/${source%.c}.o"); done
make -s BOARD=r1 "${objects[@]}"

ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' \
    -ac 2 -c:a libopus -frame_duration 120 -y "$build_dir/stereo120.opus"
ffmpeg -v error -f lavfi -i 'sine=frequency=660:sample_rate=48000:duration=2' \
    -ac 1 -c:a libopus -b:a 12k -application voip -y "$build_dir/mono-silk.opus"
ffmpeg -v error -f lavfi -i 'sine=frequency=880:sample_rate=48000:duration=2' \
    -ac 2 -c:a libopus -b:a 32k -application voip -y "$build_dir/hybrid.opus"

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isrc/audio -Iopus/include src/audio/opus_decoder_test.c \
    src/audio/opus_decoder.c src/audio/ogg_demux.c "${objects[@]}" \
    -lm -pthread -o "$build_dir/opus_decoder_test"
"$build_dir/opus_decoder_test" "$build_dir/stereo120.opus" \
    "$build_dir/mono-silk.opus" "$build_dir/hybrid.opus"

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isrc/audio src/audio/ogg_demux.c src/audio/ogg_demux_test.c \
    -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc -o "$build_dir/ogg_demux_test"
(cd "$build_dir" && ./ogg_demux_test)
