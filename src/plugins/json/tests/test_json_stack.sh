#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
out="$repo/build/host-tests/test_json_stack"
mkdir -p "$(dirname "$out")"
cd "$repo"
lua_sources=(lua/src/lapi.c lua/src/lcode.c lua/src/lctype.c lua/src/ldebug.c lua/src/ldo.c \
  lua/src/ldump.c lua/src/lfunc.c lua/src/lgc.c lua/src/llex.c lua/src/lmem.c lua/src/lobject.c \
  lua/src/lopcodes.c lua/src/lparser.c lua/src/lstate.c lua/src/lstring.c lua/src/ltable.c \
  lua/src/ltm.c lua/src/lundump.c lua/src/lvm.c lua/src/lzio.c lua/src/lauxlib.c lua/src/lbaselib.c \
  lua/src/lcorolib.c lua/src/ldblib.c lua/src/liolib.c lua/src/lmathlib.c lua/src/loadlib.c \
  lua/src/loslib.c lua/src/lstrlib.c lua/src/ltablib.c lua/src/lutf8lib.c lua/src/linit.c)
"${CC:-cc}" -std=gnu11 -O1 -g -Wall -Wextra -Werror -I. -Isrc -Ilua/src -IcJSON \
  src/plugins/json/tests/test_json_stack.c src/plugins/json/plugin_json.c \
  src/core/text/utf8_util.c cJSON/cJSON.c "${lua_sources[@]}" -lm -ldl -o "$out"
"$out"
