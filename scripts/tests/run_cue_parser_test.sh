#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${BUILD_HOST_DIR:-$repo/build_host}
bin=$(mktemp "${TMPDIR:-/tmp}/cue-parser-test.XXXXXX")
fixture=$(mktemp "${TMPDIR:-/tmp}/cue-parser-fixture.XXXXXX.flac")
truncated=$(mktemp "${TMPDIR:-/tmp}/cue-parser-truncated.XXXXXX.flac")
comment_fixture=$(mktemp "${TMPDIR:-/tmp}/cue-parser-comment.XXXXXX.flac")
wav_source=$(mktemp "${TMPDIR:-/tmp}/cue-parser-wavpack-source.XXXXXX.wav")
wavpack_cue=$(mktemp "${TMPDIR:-/tmp}/cue-parser-wavpack.XXXXXX.cue")
wavpack_fixture=$(mktemp "${TMPDIR:-/tmp}/cue-parser-wavpack.XXXXXX.wv")
large_fixture=$(mktemp "${TMPDIR:-/tmp}/cue-parser-large.XXXXXX.flac")
truncated_cue=$(mktemp "${TMPDIR:-/tmp}/cue-parser-truncated-cue.XXXXXX.flac")
trap 'rm -f "$bin" "$fixture" "$truncated" "$comment_fixture" "$wav_source" "$wavpack_cue" "$wavpack_fixture" "$large_fixture" "$truncated_cue"' EXIT
python3 - "$fixture" "$truncated" "$comment_fixture" "$wav_source" "$wavpack_cue" "$large_fixture" "$truncated_cue" <<'PY'
import struct, sys, wave
path, truncated, comment_path, wav_path, wv_cue_path, large_path, truncated_cue = sys.argv[1:]
streaminfo = bytearray(34)
packed = (48000 << 44) | (1 << 36)  # rate 48000, mono, 16 bit
streaminfo[10:18] = packed.to_bytes(8, "big")
def block(kind, data, last=False):
    return bytes([kind | (0x80 if last else 0)]) + len(data).to_bytes(3, "big") + data
def track(offset, number, index_offset=None):
    indices = b"" if index_offset is None else (index_offset.to_bytes(8, "big") + bytes([1]) + bytes(3))
    return (offset.to_bytes(8, "big") + bytes([number]) + bytes(12) + bytes([0]) + bytes(13)
            + bytes([0 if index_offset is None else 1]) + indices)
cuesheet = bytes(128) + (0).to_bytes(8, "big") + bytes([0]) + bytes(258) + bytes([3])
cuesheet += track(0, 1, 12345) + track(90000, 2, 10000) + track(150000, 255)
picture = bytes(2 * 1024 * 1024)
with open(path, "wb") as f:
    f.write(b"fLaC" + block(0, streaminfo) + block(6, picture) + block(5, cuesheet) + block(6, picture, True))
with open(truncated, "wb") as f:
    f.write(b"fLaC" + block(0, streaminfo) + bytes([0x86]) + (32).to_bytes(3, "big") + bytes(4))
text_cue = ('PERFORMER "Comment Performer"\nTITLE "Comment Album"\n'
            'FILE "external.wav" WAVE\nTRACK 01 AUDIO\nTITLE "One"\n'
            'INDEX 01 00:00:00\nTRACK 02 AUDIO\nTITLE "Two"\nINDEX 01 00:00:10\n')
vendor = b"generated-cue-test"
comment = struct.pack("<I", len(vendor)) + vendor + struct.pack("<I", 1)
value = b"CUESHEET=" + text_cue.encode()
comment += struct.pack("<I", len(value)) + value
with open(comment_path, "wb") as f:
    f.write(b"fLaC" + block(0, streaminfo) + block(4, comment, True))
with open(large_path, "wb") as f:
    f.write(b"fLaC" + block(0, streaminfo) + block(5, cuesheet, True))
    f.seek((1 << 31) + 8192)
    f.truncate()
with open(truncated_cue, "wb") as f:
    f.write(b"fLaC" + block(0, streaminfo) + bytes([0x85]) + (512).to_bytes(3, "big") + bytes(32))
with wave.open(wav_path, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000); w.writeframes(bytes(16000))
with open(wv_cue_path, "w", encoding="utf-8") as f:
    f.write(text_cue)
PY
wavpack -y --no-threads -w "CUESHEET=@$wavpack_cue" "$wav_source" -o "$wavpack_fixture" >/dev/null 2>&1
set -- -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64 -I"$repo/src/library" -I"$repo/third_party/libwavpack/include"
if [ "${SANITIZE:-0}" = 1 ]; then
    set -- "$@" -fsanitize=address,undefined -fno-omit-frame-pointer -g
fi
cc "$@" "$repo/src/library/cue_parser.c" "$repo/src/library/cue_parser_test.c" \
  "$build_dir/libwavpack/libwavpack.a" -lm -o "$bin"
"$bin" "$fixture" "$truncated" "$comment_fixture" "$wavpack_fixture" "$large_fixture" "$truncated_cue"
