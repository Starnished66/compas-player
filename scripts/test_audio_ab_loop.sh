#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
cd "$repo_root"
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Isrc/audio \
    "$repo_root/src/audio/audio_ab_loop_test.c" -o "$test_dir/audio_ab_loop_test"
"$test_dir/audio_ab_loop_test"
