#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cc -std=gnu11 -DHOST_BUILD -I. -Wall -Wextra -Werror -ffunction-sections -fdata-sections -pthread \
  src/library/metadata/db/tests/test_genre_cache.c src/core/text/utf8_util.c -Wl,--gc-sections -o "$work/test"
"$work/test"
