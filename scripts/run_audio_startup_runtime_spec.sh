#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${AUDIO_STARTUP_SPEC_BUILD_DIR:-$repo_root/build_ui_test/audio_startup}"
mkdir -p "$build_dir"

# Build the real HOST_BUILD audio implementation. The remaining application
# objects provide its decoder/output dependencies; main.o is excluded so the
# spec supplies its own entry point.
make -s -C "$repo_root" host BOARD=r1
mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/library" $(sdl2-config --cflags) \
    "$repo_root/src/audio/audio_startup_runtime_spec.c" \
    -c -o "$build_dir/audio_startup_runtime_spec.o"

g++ -pthread \
    "$build_dir/audio_startup_runtime_spec.o" \
    "${objects[@]}" \
    -Wl,--wrap=metadata_read_without_artwork \
    -Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=SDL_CloseAudioDevice \
    $(sdl2-config --libs) -lpthread -lm \
    -o "$build_dir/audio_startup_runtime_spec"

SDL_AUDIODRIVER=dummy "$build_dir/audio_startup_runtime_spec" "$build_dir/long_silence.wav"
