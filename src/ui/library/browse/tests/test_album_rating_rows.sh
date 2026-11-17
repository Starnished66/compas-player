#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../../../.." && pwd)
cd "$root"
out=build/host-tests/test_album_rating_rows
mkdir -p "$(dirname "$out")"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    src/ui/library/browse/tests/test_album_rating_rows.c -o "$out"
"$out"
