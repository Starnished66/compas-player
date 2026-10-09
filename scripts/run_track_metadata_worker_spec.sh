#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${TRACK_METADATA_WORKER_SPEC_BUILD_DIR:-build_ui_test/track_metadata_worker}"
mkdir -p "$BUILD_DIR"

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -Isrc/library -Isrc/audio -Isrc/core \
    src/library/track_metadata_worker.c \
    src/library/track_metadata_worker_spec.c \
    -lm \
    -o "$BUILD_DIR/track_metadata_worker_spec"

"$BUILD_DIR/track_metadata_worker_spec"
