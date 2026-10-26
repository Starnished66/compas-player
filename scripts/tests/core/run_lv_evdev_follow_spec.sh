#!/usr/bin/env bash
# Build and run the patched LVGL evdev driver test (single finger following)
# with gesture recognition off (shipped). With it on, the driver reports the
# first touch slot instead of the followed contact (unchanged upstream
# behavior, which does not suit type A panels), so that configuration is only
# compiled to keep the gesture paths building. The test includes
# lvgl/src/drivers/evdev/lv_evdev.c itself, so it needs the patched LVGL clone
# (make host applies the patches) and nothing else.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${LV_EVDEV_FOLLOW_SPEC_BUILD_DIR:-$repo_root/build_ui_test/lv_evdev_follow}"
mkdir -p "$build_dir"

for gesture in 0 1; do
    # Not HOST_BUILD: lv_conf.h enables LV_USE_EVDEV only for the target.
    gcc -std=gnu11 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE=1 \
        -DLV_USE_GESTURE_RECOGNITION=$gesture -DLV_USE_FLOAT=$gesture \
        -I"$repo_root" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
        "$repo_root/tests/ui/lv_evdev_follow_spec.c" \
        -o "$build_dir/lv_evdev_follow_spec_$gesture"
    echo "gesture recognition = $gesture"
    if [[ $gesture == 0 ]]; then
        "$build_dir/lv_evdev_follow_spec_$gesture"
    else
        echo "compiled only"
    fi
done
