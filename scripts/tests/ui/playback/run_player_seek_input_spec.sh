#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${PLAYER_SEEK_INPUT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/player_seek_input}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/text/fonts/ui_fonts.o" ]]; then
    echo "Player seek input spec requires cached build_host/lvgl and build_host/ui/text/fonts/ui_fonts.o" >&2
    echo "Run: make host BOARD=r1" >&2
    exit 2
fi

cc=${CC:-cc}
sanitizer_flags=()
if [[ -n "${SANITIZERS:-}" ]]; then
    sanitizer_flags=(-fsanitize="$SANITIZERS" -fno-omit-frame-pointer)
fi
common_flags=(
    -std=gnu11 -O0 -g -Wall -Wextra -Werror -fwrapv -ffunction-sections -fdata-sections
    "${sanitizer_flags[@]}"
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network"
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui"
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/src/ui/player" -I"$repo_root/tests/player" -I"$repo_root/lvgl"
    -I"$repo_root/lvgl/src/others/translation" -I"$repo_root/third_party/libwavpack/include"
)

"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player/transport/player_seek_input.c" \
    -o "$build_dir/player_seek_input.o"
"$cc" "${common_flags[@]}" -c "$repo_root/src/ui/player/transport/player_seekbar.c" \
    -o "$build_dir/player_seekbar.o"
"$cc" "${common_flags[@]}" -c "$repo_root/tests/player/player_seek_input_spec.c" \
    -o "$build_dir/player_seek_input_spec.o"
"$cc" "${sanitizer_flags[@]}" -no-pie -Wl,--gc-sections -o "$build_dir/player_seek_input_spec" \
    "$build_dir/player_seek_input.o" "$build_dir/player_seekbar.o" \
    "$build_dir/player_seek_input_spec.o" \
    "$repo_root/build_host/ui/text/fonts/ui_fonts.o" \
    $(find "$repo_root/build_host/lvgl" -name '*.o' -print) \
    -lpthread -lm

"$build_dir/player_seek_input_spec"
