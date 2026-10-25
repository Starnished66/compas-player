#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "$ROOT/.remote-import-order-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
SOURCE=${REMOTE_IMPORT_FILES_SOURCE:-$ROOT/src/network/remote_import_files.c}
"${CC:-cc}" -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -DHOST_BUILD=1 ${CFLAGS:-} \
  -I"$ROOT/src/network" -I"$ROOT/src/core" -I"$ROOT/src/library" \
  -I"$ROOT" -I"$ROOT/src/ui" -I"$ROOT/src/hardware" \
  -x c "$SOURCE" -x none \
  "$ROOT/src/network/remote_import_files_order_spec.c" \
  "$ROOT/src/core/utf8_util.c" -o "$WORK/test"
(cd "$WORK" && ./test)
