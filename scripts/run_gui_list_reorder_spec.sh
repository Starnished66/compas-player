#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GUI_LIST_REORDER_SPEC_BUILD_DIR:-$repo_root/build_ui_test/gui_list_reorder}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -w -fwrapv -ffunction-sections -fdata-sections \
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" \
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
    "$repo_root/src/ui/gui_list_reorder_spec.c" \
    $(find "$repo_root/build_host/lvgl" -name '*.o' -print) \
    -Wl,--gc-sections -lpthread -lm -o "$build_dir/gui_list_reorder_test"
"$build_dir/gui_list_reorder_test"
