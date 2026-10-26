#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
out="$root/build/button-lock-grok"
mkdir -p "$out"
cd "$root"
set -- -std=gnu11 -Wall -Wextra -Werror -O0 -g -ffunction-sections -fdata-sections
if [ "${SANITIZE:-0}" = 1 ]; then
    set -- "$@" -fsanitize=address,undefined -fno-omit-frame-pointer
fi
for board in R1 R3II_2025; do
    cc "$@" -D"BOARD_$board" -Isrc/core -Isrc/hardware -Isrc/network -Isrc/ui \
        src/hardware/input/hw_buttons_lock_test.c -Wl,--gc-sections -pthread \
        -o "$out/hw_buttons_lock_test_$board"
    "$out/hw_buttons_lock_test_$board"
done
