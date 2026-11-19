#!/usr/bin/env bash
# Power actions that hand the USB DAC screen's policy back. Built as target
# code (no HOST_BUILD) so the car mode and power menu paths exist; each test
# links only what it reaches, with typed stubs for the rest.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests"
mkdir -p "$out"
for name in car idle menu; do
    "${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
        -I"$repo" -I"$repo/lvgl" -I"$repo/lvgl/src" -I"$repo/cJSON" \
        "$repo/src/ui/application/tests/test_usb_dac_power_$name.c" \
        -Wl,--gc-sections -pthread -o "$out/test_usb_dac_power_$name"
    "$out/test_usb_dac_power_$name"
done
