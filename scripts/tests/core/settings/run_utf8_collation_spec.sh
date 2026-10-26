#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../"
mkdir -p build_host
cc -std=gnu11 -Wall -Wextra -Werror -Isrc/core src/core/text/utf8_util.c src/core/text/utf8_collation_spec.c -o build_host/utf8_collation_spec
build_host/utf8_collation_spec
