#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${UI_EXPERIMENT_TEST_DIR:-$repo_root/build_ui_test/ui-experiments}"
mkdir -p "$build_dir"
for pair in 'core ui_wake' 'library cover_raster_cache'; do
    read -r area module <<< "$pair"
    gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread \
        "$repo_root/src/$area/$module.c" "$repo_root/src/$area/${module}_test.c" \
        -o "$build_dir/$module-test"
    "$build_dir/$module-test"
done
if [[ "${1:-}" == "--lvgl" ]]; then
    make -s -C "$repo_root" host BOARD=r1
    gcc -O2 -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
        -I"$repo_root" -I"$repo_root/lvgl" -ffunction-sections -fdata-sections \
        "$repo_root/src/core/ui_event_input_test.c" \
        $(find "$repo_root/build_host/lvgl" -name '*.o') \
        -Wl,--gc-sections -lpthread -lm $(sdl2-config --libs) \
        -o "$build_dir/event-input-test"
    "$build_dir/event-input-test"
fi
