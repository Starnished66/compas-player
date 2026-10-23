#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/hiby-albumart-priority-spec.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir -p "$out/work"

set -- -std=gnu11 -O0 -g -Wall -Wextra -DHOST_BUILD=1 -ffunction-sections -fdata-sections \
  "-I$root" "-I$root/src" "-I$root/src/library" "-I$root/src/core"
cc "$@" -c "$root/src/library/albumart.c" -o "$out/albumart.o"
cc "$@" "$root/tests/albumart_priority_spec.c" "$out/albumart.o" \
  -Wl,--gc-sections -lpthread -o "$out/albumart-priority-spec"
(cd "$out/work" && "$out/albumart-priority-spec")
printf '%s\n' 'albumart priority and cache version regression: PASS'
