#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$ROOT_DIR/build_test/ape_parity"
FIXTURES="$OUT_DIR/fixtures"
mkdir -p "$FIXTURES"
cd "$ROOT_DIR"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2}"
"$CC" -std=gnu11 $CFLAGS -Isrc/audio scripts/ape_parity_decode.c \
    src/audio/ape_decoder.c src/audio/ape_demux.c src/audio/ape_ffmpeg_core.c \
    -o "$OUT_DIR/ape_parity_decode"

fetch() {
    local name="$1"
    local path="$FIXTURES/$name"
    if [[ ! -s "$path" ]]; then
        curl -fsSL "https://fate-suite.ffmpeg.org/lossless-audio/$name" -o "$path"
    fi
    printf '%s' "$path"
}

compare_s16() {
    local name="$1" frames="$2"
    local input reference actual
    input="$(fetch "$name")"
    reference="$OUT_DIR/reference.s16le"
    actual="$OUT_DIR/actual.s16le"
    ffmpeg -v error -i "$input" -af "atrim=end_sample=$frames" \
        -f s16le -acodec pcm_s16le "$reference" -y
    "$OUT_DIR/ape_parity_decode" "$input" "$actual" 0 "$frames" s16
    cmp "$reference" "$actual"
    printf 'APE parity OK: %s (%s samples)\n' "$name" "$frames"
}

# These are the exact files and first-sample ranges used by FFmpeg's
# tests/fate/monkeysaudio.mak. The old files are intentionally cut after their
# early frames; FFmpeg's reference filter stops at sample 73,728.
for sample in \
    luckynight-mac380-c2000.ape \
    luckynight-mac388-c2000.ape \
    luckynight-mac389b1-c2000.ape \
    luckynight-mac391b1-c2000.ape \
    luckynight-mac392b2-c2000.ape \
    luckynight-mac394b1-c2000.ape; do
    compare_s16 "$sample" 73728
done

# Modern 3.99 streaming fixture: compare a real seek after the first 69,999
# decoded samples with the same offset from FFmpeg's PCM reference.
modern="$(fetch luckynight-partial.ape)"
ffmpeg -v error -i "$modern" -f s16le -acodec pcm_s16le "$OUT_DIR/modern.s16le" -y
"$OUT_DIR/ape_parity_decode" "$modern" "$OUT_DIR/seek.s16le" 70000 2048 s16
dd if="$OUT_DIR/modern.s16le" of="$OUT_DIR/seek.ref.s16le" bs=1 skip=280000 count=8192 status=none
cmp "$OUT_DIR/seek.ref.s16le" "$OUT_DIR/seek.s16le"
printf 'APE seek parity OK: luckynight-partial.ape (frame 70000)\n'

# FFmpeg's legacy test includes a cut 24-bit file. Its signed S32 output is
# left-justified, while Compas deliberately returns right-justified samples.
legacy24="$(fetch NoLegacy-cut.ape)"
ffmpeg -v error -i "$legacy24" -f s32le -acodec pcm_s32le "$OUT_DIR/legacy24.s32le" -y 2>/dev/null || true
"$OUT_DIR/ape_parity_decode" "$legacy24" "$OUT_DIR/legacy24.ours.s32le" 0 4096 s32
python3 - "$OUT_DIR/legacy24.s32le" "$OUT_DIR/legacy24.ours.s32le" <<'PY'
import array
import sys

raw = open(sys.argv[1], 'rb').read()
ours = open(sys.argv[2], 'rb').read()
if len(raw) < len(ours):
    raise SystemExit('FFmpeg produced too little 24-bit reference PCM')
ref = array.array('i')
ref.frombytes(raw[:len(ours)])
got = array.array('i')
got.frombytes(ours)
if sys.byteorder != 'little':
    ref.byteswap(); got.byteswap()
if [v >> 8 for v in ref] != list(got):
    raise SystemExit('24-bit PCM differs after restoring right-justified samples')
PY
printf 'APE right-justified 24-bit parity OK: NoLegacy-cut.ape\n'

# A damaged tail stops with a sticky fatal status after the frame-error budget,
# while the read that also returned valid leading PCM remains usable.
set +e
"$OUT_DIR/ape_parity_decode" "$legacy24" "$OUT_DIR/truncated.s16le" 0 1000000 s16 \
    >"$OUT_DIR/truncated.log" 2>&1
status=$?
set -e
if [[ $status -ne 7 ]] || ! grep -q 'status 3' "$OUT_DIR/truncated.log"; then
    cat "$OUT_DIR/truncated.log" >&2
    printf 'expected a fatal decoder status after the truncated tail\n' >&2
    exit 1
fi
printf 'APE truncation status OK: valid prefix followed by fatal tail\n'
