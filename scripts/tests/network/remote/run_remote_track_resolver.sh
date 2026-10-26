#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
BIN=$(mktemp)
trap 'rm -f "$BIN"' EXIT
${CC:-cc} -std=c11 -D_POSIX_C_SOURCE=200809L -DREMOTE_TRACK_TESTING -Wall -Wextra -Werror -pthread \
  -I"$ROOT/src/library" "$ROOT/src/library/remote/remote_track.c" \
  "$ROOT/src/library/remote/remote_track_resolver_spec.c" -o "$BIN"
"$BIN"
