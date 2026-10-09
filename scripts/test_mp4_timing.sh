#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/mp4-timing"
mkdir -p "$build_dir"

ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=44100:duration=3.1' \
    -ac 2 -c:a alac -y "$build_dir/lossless.m4a"
ffmpeg -v error -f lavfi -i 'sine=frequency=660:sample_rate=44100:duration=3.1' \
    -ac 2 -c:a aac -b:a 128k -y "$build_dir/aac.m4a"
ffmpeg -v error -f lavfi -i 'sine=frequency=660:sample_rate=44100:duration=3.1' \
    -ac 2 -c:a aac -b:a 128k -f adts -y "$build_dir/aac.aac"
ffmpeg -v error -f lavfi -i 'sine=frequency=440:sample_rate=96000:duration=0.1' \
    -ac 2 -c:a alac -y "$build_dir/lossless-96k.m4a"

python3 - "$build_dir/lossless-96k.m4a" <<'PY'
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
def walk(start, end):
    pos = start
    while pos + 8 <= end:
        size = struct.unpack_from(">I", data, pos)[0]
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        if size == 0:
            size = end - pos
        if size < header or pos + size > end:
            return
        kind = data[pos + 4:pos + 8]
        payload = pos + header
        if kind == b"stsd":
            struct.pack_into(">I", data, payload + 8 + 32, 48000 << 16)
        elif kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl"):
            walk(payload, pos + size)
        pos += size
pos = 0
while pos + 8 <= len(data):
    size = struct.unpack_from(">I", data, pos)[0]
    header = 8
    if size == 1:
        size = struct.unpack_from(">Q", data, pos + 8)[0]
        header = 16
    if data[pos + 4:pos + 8] == b"moov":
        walk(pos + header, pos + size)
        break
    pos += size
path.write_bytes(data)
PY

python3 - "$build_dir/lossless.m4a" "$build_dir/lossless-nonnative-timescale.m4a" <<'PY'
import struct
import sys
from pathlib import Path

src, dst = map(Path, sys.argv[1:])
data = bytearray(src.read_bytes())
old_scale, new_scale = 44100, 88200

def boxes(start, end):
    pos = start
    while pos + 8 <= end:
        size = struct.unpack_from(">I", data, pos)[0]
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        if size == 0:
            size = end - pos
        if size < header or pos + size > end:
            raise ValueError("invalid box while building non-native timescale fixture")
        yield pos, size, header, data[pos + 4:pos + 8]
        pos += size

def walk(start, end):
    for pos, size, header, kind in list(boxes(start, end)):
        payload = pos + header
        box_end = pos + size
        if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl", b"edts"):
            walk(payload, box_end)
        elif kind == b"mdhd":
            version = data[payload]
            at = payload + (20 if version == 1 else 12)
            struct.pack_into(">I", data, at, new_scale)
        elif kind == b"stts":
            count = struct.unpack_from(">I", data, payload + 4)[0]
            for i in range(count):
                at = payload + 8 + i * 8 + 4
                value = struct.unpack_from(">I", data, at)[0]
                struct.pack_into(">I", data, at, max(1, round(value * new_scale / old_scale)))
        elif kind == b"elst":
            version = data[payload]
            count = struct.unpack_from(">I", data, payload + 4)[0]
            stride = 20 if version == 1 else 12
            for i in range(count):
                at = payload + 8 + i * stride + (8 if version == 1 else 4)
                fmt = ">q" if version == 1 else ">i"
                value = struct.unpack_from(fmt, data, at)[0]
                if value >= 0:
                    struct.pack_into(fmt, data, at, round(value * new_scale / old_scale))

for p, size, header, kind in boxes(0, len(data)):
    if kind == b"moov":
        walk(p + header, p + size)
dst.write_bytes(data)
PY

python3 - "$build_dir/aac.m4a" "$build_dir/aac-itun.m4a" \
    "$build_dir/aac-invalid-itun.m4a" "$build_dir/lossless.m4a" \
    "$build_dir/lossless-large-priming.m4a" \
    "$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts \
        -of default=noprint_wrappers=1:nokey=1 "$build_dir/lossless.m4a")" <<'PY'
import struct
import sys
from pathlib import Path

source, valid_path, invalid_path, alac_source, alac_priming_path = map(Path, sys.argv[1:6])
alac_duration = int(sys.argv[6])
original = source.read_bytes()

def box(kind, payload):
    return struct.pack(">I4s", len(payload) + 8, kind) + payload

def parts(data):
    result = []
    pos = 0
    while pos + 8 <= len(data):
        size = struct.unpack_from(">I", data, pos)[0]
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        if size == 0:
            size = len(data) - pos
        if size < header or pos + size > len(data):
            raise ValueError("malformed MP4 fixture")
        result.append((data[pos + 4:pos + 8], data[pos + header:pos + size]))
        pos += size
    if pos != len(data):
        raise ValueError("trailing MP4 fixture data")
    return result

def rebuild(kind, payload):
    return box(kind, payload)

def add_tag(data, text):
    top = parts(data)
    output = []
    for kind, payload in top:
        if kind != b"moov":
            output.append(rebuild(kind, payload))
            continue
        children = []
        for child_kind, child_payload in parts(payload):
            if child_kind == b"trak":
                trak = []
                for trak_kind, trak_payload in parts(child_payload):
                    if trak_kind == b"edts":
                        edits = [(k, p) for k, p in parts(trak_payload) if k != b"elst"]
                        if edits:
                            trak.append(box(b"edts", b"".join(rebuild(k, p) for k, p in edits)))
                    else:
                        trak.append(rebuild(trak_kind, trak_payload))
                children.append(box(b"trak", b"".join(trak)))
            elif child_kind == b"udta":
                udta = [rebuild(k, p) for k, p in parts(child_payload)]
                children.append(box(b"udta", b"".join(udta)))
            else:
                children.append(rebuild(child_kind, child_payload))

        mean = box(b"mean", b"\0\0\0\0com.apple.iTunes")
        name = box(b"name", b"\0\0\0\0iTunSMPB")
        data_box = box(b"data", b"\0\0\0\1\0\0\0\0" + text.encode("ascii"))
        item = box(b"----", mean + name + data_box)
        ilst = box(b"ilst", item)
        metadata_added = False
        for i, child in enumerate(children):
            child_kind, child_payload = parts(child)[0]
            if child_kind != b"udta":
                continue
            udta_parts = parts(child_payload)
            for j, (udta_kind, udta_payload) in enumerate(udta_parts):
                if udta_kind != b"meta":
                    continue
                meta_children = parts(udta_payload[4:])
                for m, (meta_kind, meta_payload) in enumerate(meta_children):
                    if meta_kind == b"ilst":
                        meta_children[m] = (b"ilst", meta_payload + item)
                        break
                else:
                    meta_children.append((b"ilst", item))
                udta_parts[j] = (b"meta", udta_payload[:4] + b"".join(rebuild(k, p) for k, p in meta_children))
                metadata_added = True
                break
            if not metadata_added:
                udta_parts.append((b"meta", b"\0\0\0\0" + ilst))
                metadata_added = True
            children[i] = box(b"udta", b"".join(rebuild(k, p) for k, p in udta_parts))
            break
        if not metadata_added:
            children.append(box(b"udta", b"\0\0\0\0" + ilst))
        output.append(box(b"moov", b"".join(children)))
    return b"".join(output)

# Use the independently measured playable duration; this fixture has no edit
# list, so iTunSMPB is the sole trimming source. The invalid text must be ignored.
duration = 136710
valid = "00000000 00000400 00000000 %016X" % duration
invalid = "00000000 -00000001 00000000 %016X" % duration
valid_path.write_bytes(add_tag(original, valid))
invalid_path.write_bytes(add_tag(original, invalid))
alac_priming_path.write_bytes(add_tag(alac_source.read_bytes(),
    "00000000 00001388 00000000 %016X" % (alac_duration - 5000)))
PY

python3 - "$build_dir/lossless.m4a" "$build_dir/malformed-stts.m4a" <<'PY'
import struct
import sys
from pathlib import Path

src, dst = map(Path, sys.argv[1:])
data = bytearray(src.read_bytes())
def walk(start, end):
    pos = start
    while pos + 8 <= end:
        size = struct.unpack_from(">I", data, pos)[0]
        header = 8
        if size == 1:
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        if not size:
            size = end - pos
        if size < header or pos + size > end:
            return False
        kind = data[pos + 4:pos + 8]
        payload = pos + header
        if kind == b"stts":
            struct.pack_into(">I", data, payload + 4, 0xFFFFFFFF)
            return True
        if kind in (b"moov", b"trak", b"mdia", b"minf", b"stbl") and walk(payload, pos + size):
            return True
        pos += size
    return False
pos = 0
while pos + 8 <= len(data):
    size = struct.unpack_from(">I", data, pos)[0]
    if size == 1:
        size = struct.unpack_from(">Q", data, pos + 8)[0]
        header = 16
    else:
        header = 8
    if data[pos + 4:pos + 8] == b"moov":
        assert walk(pos + header, pos + size)
        break
    pos += size
dst.write_bytes(data)
PY

cc -std=gnu11 -O2 -Wall -Wextra -Isrc/audio -Ifaad2/include \
    -c src/audio/mp4_timing_test.c -o "$build_dir/test.o"
cc -std=gnu11 -O2 -Wall -Wextra -Isrc/audio -Ifaad2/include \
    -c src/audio/mp4_demux.c -o "$build_dir/mp4_demux.o"
cc -std=gnu11 -O2 -Wall -Wextra -Isrc/audio -Ifaad2/include \
    -c src/audio/aac_decoder.c -o "$build_dir/aac_decoder.o"
c++ -std=gnu++11 -O2 -Wall -Wextra -Isrc/audio -Ialac/codec -Ialac/util \
    -c src/audio/alac_decoder.cpp -o "$build_dir/alac_decoder.o"
c++ "$build_dir/test.o" "$build_dir/mp4_demux.o" "$build_dir/aac_decoder.o" \
    "$build_dir/alac_decoder.o" build_host/faad2/*.o build_host/alac/*.o \
    -o "$build_dir/mp4_timing_test"

"$build_dir/mp4_timing_test" "$build_dir/lossless.m4a" \
    "$build_dir/lossless-nonnative-timescale.m4a" "$build_dir/lossless-96k.m4a" \
    "$build_dir/lossless-large-priming.m4a" "$build_dir/aac.m4a" \
        "$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts \
            -of default=noprint_wrappers=1:nokey=1 "$build_dir/aac.m4a")" \
    "$(ffprobe -v error -select_streams a:0 -show_entries stream=duration_ts \
        -of default=noprint_wrappers=1:nokey=1 "$build_dir/aac.m4a")" \
    "$build_dir/aac.aac" "$build_dir/aac-itun.m4a" "$build_dir/aac-invalid-itun.m4a" \
    "$build_dir/malformed-stts.m4a"
