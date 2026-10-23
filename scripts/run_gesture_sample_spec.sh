#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${GESTURE_SAMPLE_SPEC_BUILD_DIR:-$repo_root/build_ui_test/gesture_sample}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" ]]; then
    make -s -C "$repo_root" host BOARD=r1
fi

python3 - "$repo_root" "$build_dir" <<'PY'
from pathlib import Path
import sys

repo = Path(sys.argv[1])
out = Path(sys.argv[2]) / "gesture_sample_helper.inc"
source = (repo / "src/ui/gui_shell.c").read_text()
marker = "static void gesture_note_pointer_sample(const lv_indev_data_t * data) {"
start = source.index(marker)
brace = source.index("{", start)
depth = 0
end = None
for i in range(brace, len(source)):
    if source[i] == "{":
        depth += 1
    elif source[i] == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
if end is None:
    raise SystemExit("could not find end of gesture_note_pointer_sample")
out.write_text(source[start:end] + "\n")
PY

cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -fwrapv -ffunction-sections -fdata-sections \
    -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
    -I"$repo_root" -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" \
    -I"$build_dir" "$repo_root/tests/gesture_sample_spec.c" \
    $(find "$repo_root/build_host/lvgl" -name '*.o' -print) \
    -Wl,--gc-sections -lpthread -lm -o "$build_dir/gesture_sample_spec"
"$build_dir/gesture_sample_spec"

# The source compiles with both the ready-path disabled (baseline behavior)
# and UI_PERF_TRACE enabled. These are compile-only: runtime behavior is
# exercised by the default build above.
for variant in UI_SWIPE_SAMPLE_READY_DISABLE UI_PERF_TRACE; do
    "$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -fwrapv \
        -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 \
        -DLV_CONF_INCLUDE_SIMPLE=1 -D"$variant" \
        -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/lvgl" \
        -I"$repo_root/lvgl/src" -I"$build_dir" \
        -c "$repo_root/tests/gesture_sample_spec.c" \
        -o "$build_dir/gesture_sample_spec_$variant.o"
done
