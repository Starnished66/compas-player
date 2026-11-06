#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../.."
bin="${TMPDIR:-/tmp}/compas-test-subsonic-artist-$$"
trap 'rm -f "$bin"' EXIT
cc -std=c11 -DHOST_BUILD -D_GNU_SOURCE -I. -Imbedtls/include -Wall -Wextra -Werror -pthread \
    src/ui/network/subsonic/tests/test_artist_summary.c src/ui/network/subsonic/gui_subsonic_artist_summary.c -o "$bin"
"$bin"
