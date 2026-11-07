#!/usr/bin/env bash
# Original release year precedence.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
make -C "$repo" host > /dev/null
out="$repo/build/host-tests/test_original_year"
mkdir -p "$(dirname "$out")"
mapfile -t objs < <(find "$repo/build_host" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' | sort)
cc=${CC:-cc}
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -I"$repo" -DHOST_BUILD=1 -c "$repo/src/library/metadata/parsing/tests/test_original_year.c" -o "$out.o"
g++ "$out.o" "${objs[@]}" "$repo/build_host/libwavpack/libwavpack.a" "$repo/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lpthread -lm -ldl -o "$out"
"$out"
