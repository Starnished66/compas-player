#!/bin/sh
# Host crossfeed DSP checks. Compiles peq.c with the focused self-test.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
out=${TMPDIR:-/tmp}/peq-crossfeed.$$
trap 'rm -rf "$out"' EXIT INT TERM
mkdir "$out"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -DHOST_BUILD=1 \
  -I"$root/src/audio" -I"$root/src/library" -I"$root/src/core" \
  "$root/src/audio/effects/peq_crossfeed_selftest.c" "$root/src/audio/effects/peq.c" "$root/src/audio/effects/mseb.c" \
  -lm -lpthread -o "$out/test"
"$out/test"
printf '%s\n' 'peq crossfeed: PASS'
