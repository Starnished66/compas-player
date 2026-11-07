#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
mapfile -t objects < <(find build_host/lvgl -name '*.o' -print)
if ((${#objects[@]} == 0)); then echo 'Run make host first' >&2; exit 1; fi
out=build/host-tests/test_artist_mosaic
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -Wall -Wextra -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1 -I. -Ilvgl \
    src/ui/library/browse/tests/test_artist_mosaic.c src/ui/assets/gui_image_dsc.c \
    "${objects[@]}" $(sdl2-config --libs) -lm -lpthread -o "$out"
"$out" "$@"
