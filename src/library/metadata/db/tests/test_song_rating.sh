#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_song_rating"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -pthread -I"$repo" -DHOST_BUILD=1 "$repo/src/library/metadata/db/tests/test_song_rating.c" \
    "$repo/src/library/metadata/db/metadata_db_library_lists.c" \
    "$repo/src/library/metadata/db/metadata_db_browse.c" -Wl,--wrap=pthread_mutex_unlock,--wrap=pthread_mutex_lock,--wrap=pthread_mutex_trylock -Wl,--gc-sections -o "$out"
"$out"
