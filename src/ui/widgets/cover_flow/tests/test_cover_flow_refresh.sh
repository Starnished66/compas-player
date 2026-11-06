#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
# Run make host first: use exactly the LVGL configuration built for the player.
mapfile -t objects < <(find build_host/lvgl -name '*.o' -print)
if ((${#objects[@]} == 0)); then echo 'Run make host first' >&2; exit 1; fi
out=build/host-tests/test_cover_flow_refresh
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=gnu11 -Wall -Wextra -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1 -I. -Ilvgl \
    src/ui/widgets/cover_flow/tests/test_cover_flow_refresh.c \
    src/ui/widgets/cover_flow/cover_flow_layout.c src/ui/widgets/cover_flow/cover_flow_span.c \
    src/ui/widgets/cover_flow/cover_flow_tilt.c src/ui/widgets/cover_flow/cover_flow_warp.c \
    "${objects[@]}" $(sdl2-config --libs) -lm -lpthread -o "$out"
"$out"
