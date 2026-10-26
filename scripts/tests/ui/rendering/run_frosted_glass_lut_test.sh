#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
BUILD="$ROOT/build/manual-selection-optimization"
mkdir -p "$BUILD"
cc -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$ROOT" -I"$ROOT/src/ui" -I"$ROOT/lvgl" \
  -c "$ROOT/src/ui/effects/frosted_glass.c" -o "$BUILD/optimized.o"
cc -O2 -Wall -Wextra -Werror -I"$ROOT" -I"$ROOT/src/ui" -I"$ROOT/lvgl" \
  -c "$ROOT/scripts/tests/ui/rendering/frosted_glass_lut_test.c" -o "$BUILD/test.o"
cc -Wl,--gc-sections "$BUILD/test.o" "$BUILD/optimized.o" -o "$BUILD/frosted_glass_lut_test"
"$BUILD/frosted_glass_lut_test"
echo "frosted glass LUT equivalence cases passed"
