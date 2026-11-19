#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_usb_dac_lifecycle"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -DHOST_BUILD -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$repo" -I"$repo/lvgl" "$repo/src/ui/player/transport/tests/test_usb_dac_lifecycle.c" \
    -Wl,--gc-sections -pthread -o "$out"
"$out"
# The USB DAC screen note names the device by its short name on every board.
for pair in ":R1" "-DBOARD_R3PROII:R3 Pro II" "-DBOARD_R3II_2025:R3 II 2025" "-DBOARD_TEMPOTEC_V1:V1"; do
    printf '#include "src/core/system/runtime/app_version.c"\n#include <string.h>\nint main(void) { return strcmp(app_board_short_name(), "%s") != 0; }\n' "${pair#*:}" |
        "${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror ${pair%%:*} -I"$repo" -x c - -o "$out.name"
    "$out.name"
done
echo "Board short name tests passed"
