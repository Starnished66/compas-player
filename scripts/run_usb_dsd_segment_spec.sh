#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${USB_DSD_SEGMENT_SPEC_BUILD_DIR:-$repo_root/build_ui_test/usb_dsd_segment}"
mkdir -p "$build_dir"

make -s -C "$repo_root" host BOARD=r1
fixture="$build_dir/dsd128_segment.dsf"
python3 - "$fixture" <<'PY'
import struct, sys
path = sys.argv[1]
sample_count = 1_048_576  # DSD128 bits per channel
block_size = 4096
channel_bytes = sample_count // 8
payload_size = channel_bytes * 2
file_size = 28 + 52 + 12 + payload_size
header = (b"DSD " + struct.pack("<QQQ", 28, file_size, 0) + b"fmt " +
          struct.pack("<QIIIIIIQII", 52, 1, 0, 2, 2, 5_644_800, 1,
                       sample_count, block_size, 0) +
          b"data" + struct.pack("<Q", 12 + payload_size))
with open(path, "wb") as f:
    f.write(header)
    for i in range(payload_size):
        f.write(bytes(((i * 73 + (i // block_size) * 29 + 17) & 0xff,)))
PY

mapfile -t objects < <(find "$repo_root/build_host" -mindepth 2 -name '*.o' \
    ! -path "$repo_root/build_host/libwavpack/*" \
    ! -path "$repo_root/build_host/libsndfile/*" \
    ! -path "$repo_root/build_host/audio/audio.o" \
    ! -name ogg_demux.o ! -name main.o -print)
if ((${#objects[@]} == 0)); then
    echo "No host object files found under build_host" >&2
    exit 1
fi

gcc -std=gnu11 -O3 -g -Wall -fwrapv -pthread -ffunction-sections -fdata-sections \
    -D_GNU_SOURCE=1 -D_REENTRANT -DHOST_BUILD=1 -DBOARD_R1 -DAUDIO_DSD_RUNTIME_SPEC \
    -DLV_CONF_INCLUDE_SIMPLE=1 -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ARCHIVE_APIS \
    -I"$repo_root" -I"$repo_root/src/audio" -I"$repo_root/src/network" \
    -I"$repo_root/src/library" -I"$repo_root/src/hardware" -I"$repo_root/src/ui" \
    -I"$repo_root/src/core" -I"$repo_root/src/plugins" -I"$repo_root/lvgl" \
    -I"$repo_root/dr_libs" -I"$repo_root/faad2/include" -I"$repo_root/alac/codec" \
    -I"$repo_root/mbedtls/include" -I"$repo_root/cJSON" -I"$repo_root/opus/include" \
    -I"$repo_root/opusfile/include" -I"$repo_root/libogg_vendor_config" \
    -I"$repo_root/libogg/include" -I"$repo_root/lua/src" -I"$repo_root/stb_vorbis" \
    -I"$repo_root/jpeg_vendor_config" -I"$repo_root/jpeg" \
    -I"$repo_root/third_party/libwavpack/include" \
    -I"$repo_root/third_party/libsndfile/include" -I"$repo_root/sonic" -I"$repo_root/tinfl" \
    $(sdl2-config --cflags) "$repo_root/src/audio/usb_dsd_segment_spec.c" \
    -Wl,--gc-sections -c -o "$build_dir/usb_dsd_segment_spec.o"

g++ -pthread -Wl,--gc-sections "$build_dir/usb_dsd_segment_spec.o" "${objects[@]}" \
    "$repo_root/build_host/libwavpack/libwavpack.a" \
    "$repo_root/build_host/libsndfile/libsndfile.a" \
    $(sdl2-config --libs) -lpthread -lm -o "$build_dir/usb_dsd_segment_spec"
"$build_dir/usb_dsd_segment_spec" "$fixture"
