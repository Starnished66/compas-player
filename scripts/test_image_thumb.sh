#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${IMAGE_THUMB_TEST_BUILD_DIR:-$repo_root/build_ui_test/image-thumb}"
host_build="$repo_root/build_host"
mkdir -p "$build_dir"

# Populate the normal host decoder dependencies in persistent build storage.
make -s -C "$repo_root" host BOARD=r1

gcc -std=c99 -D_GNU_SOURCE -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1 \
    -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS -ffunction-sections -fdata-sections \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network" \
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/lvgl" \
    -I"$repo_root/jpeg_vendor_config" -I"$repo_root/jpeg" -I"$repo_root/tinfl" \
    "$repo_root/src/library/image_thumb_test.c" "$repo_root/src/library/image_thumb.c" \
    "$repo_root/src/library/cover_decode.c" \
    "$host_build/library/artwork_coordinator.o" \
    "$host_build/lvgl/src/libs/tjpgd/tjpgd.o" "$host_build/tinfl/miniz_tinfl.o" \
    "$host_build"/jpeg/*.o \
    -Wl,--gc-sections -lpthread -lm -o "$build_dir/image_thumb_test"

"$build_dir/image_thumb_test"
