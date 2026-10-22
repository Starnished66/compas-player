#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${CUE_AUDIO_RUNTIME_SPEC_BUILD_DIR:-$repo_root/build_ui_test/cue_audio_runtime}"
mkdir -p "$build_dir"

# Link the production HOST_BUILD audio, metadata, and CUE parser objects. The
# spec provides its own entry point, so omit the application's main.o.
make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path "$repo_root/build_host/libwavpack/*" \
    ! -path "$repo_root/build_host/libsndfile/*" \
    ! -name ogg_demux.o \
    ! -name main.o -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/library" \
    $(sdl2-config --cflags) \
    "$repo_root/src/audio/cue_audio_runtime_spec.c" \
    -c -o "$build_dir/cue_audio_runtime_spec.o"

g++ -pthread \
    "$build_dir/cue_audio_runtime_spec.o" \
    "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" \
    "$repo_root/build_host/libsndfile/libsndfile.a" \
    -Wl,--wrap=SDL_QueueAudio -Wl,--wrap=SDL_GetQueuedAudioSize \
    $(sdl2-config --libs) -lpthread -lm \
    -o "$build_dir/cue_audio_runtime_spec"

SDL_AUDIODRIVER=dummy "$build_dir/cue_audio_runtime_spec" \
    "$build_dir/source.wav" "$build_dir/runtime.cue" "$build_dir/second.wav"
