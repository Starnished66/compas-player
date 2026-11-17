#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_plugin_http_bindings"
mkdir -p "$(dirname "$out")"
cd "$repo"
lua_sources=(lua/src/lapi.c lua/src/lcode.c lua/src/lctype.c lua/src/ldebug.c lua/src/ldo.c \
  lua/src/ldump.c lua/src/lfunc.c lua/src/lgc.c lua/src/llex.c lua/src/lmem.c lua/src/lobject.c \
  lua/src/lopcodes.c lua/src/lparser.c lua/src/lstate.c lua/src/lstring.c lua/src/ltable.c \
  lua/src/ltm.c lua/src/lundump.c lua/src/lvm.c lua/src/lzio.c lua/src/lauxlib.c lua/src/lbaselib.c \
  lua/src/lcorolib.c lua/src/ldblib.c lua/src/liolib.c lua/src/lmathlib.c lua/src/loadlib.c \
  lua/src/loslib.c lua/src/lstrlib.c lua/src/ltablib.c lua/src/lutf8lib.c lua/src/linit.c)
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -Wl,--gc-sections -I. -Isrc -Isrc/network -Ilvgl -Ilua/src -IcJSON -I/usr/include/SDL2 \
  -DLV_CONF_INCLUDE_SIMPLE=1 -DHOST_BUILD=1 -DBOARD_R1 \
  src/plugins/bindings/io/tests/test_plugin_http_bindings.c "${lua_sources[@]}" -pthread -lm -ldl -o "$out"
"$out"
