#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/opus-stability"
mkdir -p "$build_dir"

# Compile only the vendored codec objects needed by the standalone decoder.
mapfile -t sources < <(find opus/src opus/celt opus/silk opus/silk/float \
    -maxdepth 1 -name '*.c' ! -name '*demo.c' ! -name 'opus_compare.c' | sort)
objects=()
for source in "${sources[@]}"; do objects+=("build_host/${source%.c}.o"); done
objects+=(build_host/libogg/bitwise.o build_host/libogg/framing.o
    build_host/opusfile/info.o build_host/opusfile/internal.o
    build_host/opusfile/opusfile.o build_host/opusfile/stream.o)
make -s BOARD=r1 "${objects[@]}"

ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' \
    -ac 2 -c:a libopus -frame_duration 120 -metadata title='Opus test' \
    -metadata artist='Compas' -metadata album='Pipeline' -metadata lyrics='Test lyrics' \
    -metadata replaygain_track_gain='-3.0 dB' -y "$build_dir/stereo120.opus"
ffmpeg -v error -f lavfi -i 'sine=frequency=660:sample_rate=48000:duration=2' \
    -ac 1 -c:a libopus -b:a 12k -application voip -y "$build_dir/mono-silk.opus"
ffmpeg -v error -f lavfi -i 'sine=frequency=880:sample_rate=48000:duration=2' \
    -ac 2 -c:a libopus -b:a 32k -application voip -y "$build_dir/hybrid.opus"
ffmpeg -v error -f lavfi -i 'anullsrc=r=48000:cl=5.1' -t 2 \
    -c:a libopus -y "$build_dir/multichannel.opus"
cat "$build_dir/mono-silk.opus" "$build_dir/stereo120.opus" > "$build_dir/chained.ogg"
ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' \
    -c:a libvorbis -y "$build_dir/vorbis.ogg"

python3 - "$build_dir" <<'PY'
import shutil
import sys
from pathlib import Path
from mutagen.oggopus import OggOpus

root = Path(sys.argv[1])
output = root / "valid-large-tags.opus"
shutil.copyfile(root / "stereo120.opus", output)
tags = OggOpus(output)
tags["x"] = ["x" * (1024 * 1024)]
tags.save()

def header_pages(data):
    offset = 0
    packet = bytearray()
    saw_head = False
    while offset < len(data):
        assert data[offset:offset + 4] == b"OggS"
        segments = data[offset + 26]
        lacing_start = offset + 27
        lacing = data[lacing_start:lacing_start + segments]
        page_end = lacing_start + segments + sum(lacing)
        payload = data[lacing_start + segments:page_end]
        payload_offset = 0
        tags_complete = False
        for length in lacing:
            packet.extend(payload[payload_offset:payload_offset + length])
            payload_offset += length
            if length < 255:
                if packet.startswith(b"OpusHead"):
                    saw_head = True
                elif saw_head and packet.startswith(b"OpusTags"):
                    tags_complete = True
                packet.clear()
        if tags_complete:
            return data[:page_end]
        offset = page_end
    raise AssertionError("OpusTags packet not found")

# Keep the first link complete, then only the second link's OpusHead and large
# OpusTags packets. Its missing audio makes initial-PCM discovery fail after
# tags have been allocated.
first = (root / "mono-silk.opus").read_bytes()
second_data = output.read_bytes()
second_headers = header_pages(second_data)
assert first[14:18] != second_data[14:18], "fixture links need distinct serial numbers"
(root / "broken-later-link.ogg").write_bytes(first + second_headers)
PY

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isrc/audio -Iopus/include -Iopusfile/include -Ilibogg_vendor_config -Ilibogg/include \
    src/audio/opus_decoder_test.c src/audio/opus_decoder.c src/audio/opusfile_alloc.c \
    src/audio/ogg_probe.c "${objects[@]}" \
    -lm -pthread -o "$build_dir/opus_decoder_test"
"$build_dir/opus_decoder_test" "$build_dir/stereo120.opus" \
    "$build_dir/mono-silk.opus" "$build_dir/hybrid.opus" \
    "$build_dir/multichannel.opus" "$build_dir/chained.ogg" "$build_dir/vorbis.ogg" \
    "$build_dir/valid-large-tags.opus"

make -s BOARD=r1 build_host/mbedtls/base64.o
cc -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -ffunction-sections -fdata-sections -Isrc/audio -Isrc/library -Isrc/core \
    -Isrc/network -Isrc/ui -I. -Ilvgl -Idr_libs -Istb_vorbis -Imbedtls/include \
    -Iopus/include -Iopusfile/include -Ilibogg_vendor_config -Ilibogg/include \
    src/library/opus_metadata_test.c src/audio/opusfile_alloc.c src/core/utf8_util.c \
    "${objects[@]}" build_host/mbedtls/base64.o \
    -Wl,--gc-sections -lm -pthread -o "$build_dir/opus_metadata_test"
"$build_dir/opus_metadata_test" "$build_dir/stereo120.opus"

# Mutagen writes valid tag continuation pages, exercising the real library's
# allocations rather than replacing its opener with a test stub.
python3 - "$build_dir" <<'PY'
import shutil
import sys
from pathlib import Path
from mutagen.oggopus import OggOpus

root = Path(sys.argv[1])
for name, values in (("oversized", ["x" * (9 * 1024 * 1024)]),
                     ("manycomments", [""] * 1000000)):
    output = root / (name + ".opus")
    shutil.copyfile(root / "stereo120.opus", output)
    tags = OggOpus(output)
    tags["x"] = values
    tags.save()
PY

cc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Isrc/audio -Iopus/include -Iopusfile/include -Ilibogg_vendor_config -Ilibogg/include \
    src/audio/opus_resource_test.c src/audio/opus_decoder.c src/audio/opusfile_alloc.c \
    "${objects[@]}" -lm -pthread -o "$build_dir/opus_resource_test"
"$build_dir/opus_resource_test" "$build_dir/oversized.opus" \
    "$build_dir/manycomments.opus" "$build_dir/valid-large-tags.opus" \
    "$build_dir/broken-later-link.ogg"
