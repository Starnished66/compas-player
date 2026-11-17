#!/bin/bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../../.." && pwd)"
cd "$repo"
out=$(mktemp "${TMPDIR:-/tmp}/shutdown-test.XXXXXX")
trap 'rm -f "$out"' EXIT
# Host compiler only; mocks intercept every terminal action in the device path.
cc -std=gnu11 -D_GNU_SOURCE -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
  -Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread \
  -I. -Ilvgl -Imbedtls/include -Idr_libs -Ifaad2/include -Ialac/codec -IcJSON -Ilua/src \
  -Isrc/core -Isrc/audio -Isrc/library -Isrc/network -Isrc/ui \
  src/core/system/lifecycle/tests/test_shutdown_persistence.c \
  -Wl,--gc-sections,--wrap=sync,--wrap=execl,--wrap=reboot,--wrap=readlink,--wrap=_exit -o "$out"
"$out"
