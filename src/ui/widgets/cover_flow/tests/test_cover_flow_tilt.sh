#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
dir="$here/../../../../../build/host-tests"
mkdir -p "$dir"
cc=${CC:-cc}
tests="tilt span azmap warp"
build() { # name, extra flags
    "$cc" -std=gnu11 -Wall -Wextra "${@:2}" "$here/test_cover_flow_$1.c" "$here/../cover_flow_tilt.c" \
        "$here/../cover_flow_span.c" "$here/../cover_flow_warp.c" "$here/../cover_flow_azmap.c" "$here/../../../library/library_az_table.c" -o "$dir/test_cover_flow_$1$SUFFIX"
    "$dir/test_cover_flow_$1$SUFFIX"
}
SUFFIX=""
for t in $tests; do build "$t" -O0 -g; done
# Again under the sanitizers, when the compiler has them.
if echo 'int main(void){return 0;}' | "$cc" -x c -fsanitize=address,undefined - -o /dev/null 2>/dev/null; then
    SUFFIX=".san"
    for t in $tests; do build "$t" -O1 -g -fsanitize=address,undefined; done
fi
