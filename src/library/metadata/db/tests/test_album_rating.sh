#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
out="$repo/build/host-tests/test_album_rating"
mkdir -p "$(dirname "$out")"
"${CC:-cc}" -std=gnu11 -O0 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -pthread -I"$repo" -DHOST_BUILD=1 "$repo/src/library/metadata/db/tests/test_album_rating.c" \
    "$repo/src/library/metadata/db/album_rating.c" \
    "$repo/src/library/metadata/db/metadata_db_library_lists.c" \
    "$repo/src/library/remote/remote_state.c" "$repo/src/library/playlists/rating_writer.c" \
    -Wl,--gc-sections -o "$out"
"$out"
