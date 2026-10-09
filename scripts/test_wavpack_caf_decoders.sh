#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_dir}/build_host"
fixture_dir="${build_dir}/wavpack_caf_test"
test_bin="${build_dir}/wavpack_caf_decoder_test"

command -v ffmpeg >/dev/null || { echo "ffmpeg is required" >&2; exit 2; }
command -v wavpack >/dev/null || { echo "wavpack CLI is required for the hybrid and DSD fixtures" >&2; exit 2; }
mkdir -p "${fixture_dir}"

if [[ ! -f "${build_dir}/libwavpack/libwavpack.a" || ! -f "${build_dir}/libsndfile/libsndfile.a" ]]; then
    echo "Static host archives missing; build build_host/libwavpack/libwavpack.a and build_host/libsndfile/libsndfile.a first." >&2
    exit 2
fi

python3 - "${fixture_dir}" <<'PY'
import os, struct, sys
d = sys.argv[1]
frames = 2048
with open(os.path.join(d, 'float_source.f32le'), 'wb') as f:
    values = (0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, float('nan'), float('inf'), float('-inf'))
    for i in range(frames):
        f.write(struct.pack('<ff', values[i % len(values)], values[(i + 3) % len(values)]))
# Minimal stereo DSF: two 4096-byte blocks per channel at DSD64.
sample_count = 65536
block_size = 4096
payload = b'\x69' * (2 * sample_count // 8)
file_size = 28 + 52 + 12 + len(payload)
dsf = (b'DSD ' + struct.pack('<QQQ', 28, file_size, 0) + b'fmt ' +
       struct.pack('<QIIIIIIQII', 52, 1, 0, 2, 2, 2822400, 1, sample_count, block_size, 0) +
       b'data' + struct.pack('<Q', 12 + len(payload)) + payload)
with open(os.path.join(d, 'dsd.dsf'), 'wb') as f:
    f.write(dsf)
PY

ffmpeg -v error -y -f lavfi -i 'sine=frequency=997:sample_rate=48000:duration=0.125' \
    -ac 2 -c:a pcm_s24le "${fixture_dir}/source.wav"
ffmpeg -v error -y -i "${fixture_dir}/source.wav" -c:a wavpack -compression_level 3 \
    -metadata title='Format test' -metadata artist='Compas' -metadata album='Pipeline' \
    -metadata track='3' -metadata replaygain_track_gain='-3.0 dB' "${fixture_dir}/integer.wv"
wavpack -q -y -b3 -c "${fixture_dir}/source.wav" -o "${fixture_dir}/hybrid.wv"
wavpack -q -y "${fixture_dir}/dsd.dsf" -o "${fixture_dir}/dsd.wv"
ffmpeg -v error -y -i "${fixture_dir}/source.wav" -c:a pcm_s24le -f caf \
    -metadata title='Format test' -metadata artist='Compas' -metadata album='Pipeline' \
    -metadata track='3' -metadata tracknumber='3' "${fixture_dir}/integer.caf"
ffmpeg -v error -y -i "${fixture_dir}/source.wav" -c:a alac -f caf \
    -metadata title='Format test' -metadata artist='Compas' -metadata album='Pipeline' \
    -metadata track='3' -metadata tracknumber='3' "${fixture_dir}/alac.caf"
# FFmpeg's CAF muxer writes fmt_flags=0 for ALAC; libsndfile follows the CAF
# definition where this field carries the ALAC bit depth (3 means 24 bit).
python3 - "${fixture_dir}/alac.caf" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, 'rb').read())
pos = data.find(b'desc')
if pos < 0 or data[pos + 20:pos + 24] != b'alac':
    raise SystemExit('FFmpeg did not create a CAF ALAC description chunk')
data[pos + 24:pos + 28] = (3).to_bytes(4, 'big')
open(path, 'wb').write(data)
PY
ffmpeg -v error -y -f f32le -ar 48000 -ac 2 -i "${fixture_dir}/float_source.f32le" \
    -c:a pcm_f32le -metadata title='Format test' -metadata artist='Compas' \
    -metadata album='Pipeline' -metadata track='3' -metadata tracknumber='3' "${fixture_dir}/float.caf"
ffmpeg -v error -y -i "${fixture_dir}/float.caf" -c:a wavpack -sample_fmt fltp \
    -metadata title='Format test' -metadata artist='Compas' -metadata album='Pipeline' \
    -metadata track='3' -metadata replaygain_track_gain='-3.0 dB' "${fixture_dir}/float.wv"
python3 - "${fixture_dir}" <<'PY'
import os, sys
d = sys.argv[1]
for source, target in (('integer.wv', 'truncated.wv'), ('integer.caf', 'truncated.caf')):
    with open(os.path.join(d, source), 'rb') as f:
        data = f.read()
    with open(os.path.join(d, target), 'wb') as f:
        f.write(data[:len(data) // 2])
PY

sanitizers=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
cc -std=c11 -O2 -Wall -Wextra -Werror "${sanitizers[@]}" \
    -I"${repo_dir}/src/audio" -I"${repo_dir}/third_party/libwavpack/include" \
    -I"${repo_dir}/third_party/libsndfile/include" \
    "${repo_dir}/src/audio/wavpack_caf_decoder_test.c" \
    "${repo_dir}/src/audio/wavpack_decoder.c" "${repo_dir}/src/audio/caf_decoder.c" \
    "${build_dir}/libsndfile/libsndfile.a" "${build_dir}/libwavpack/libwavpack.a" \
    -lm -lpthread -o "${test_bin}"

"${test_bin}" "${fixture_dir}/integer.wv" "${fixture_dir}/float.wv" \
    "${fixture_dir}/integer.caf" "${fixture_dir}/float.caf" \
    "${fixture_dir}/hybrid.wv" "${fixture_dir}/alac.caf" "${fixture_dir}/dsd.wv" \
    "${fixture_dir}/truncated.wv" "${fixture_dir}/truncated.caf"
