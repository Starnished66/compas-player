#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
build_dir="${repo_dir}/build_host"
fixture_dir="${build_dir}/dsd_wavpack_test"
test_bin="${build_dir}/dsd_wavpack_decoder_test"

command -v wavpack >/dev/null || { echo "wavpack CLI is required" >&2; exit 2; }
mkdir -p "${fixture_dir}"
make -C "${repo_dir}" build_host/libwavpack/libwavpack.a

python3 - "${fixture_dir}" <<'PY'
import os, struct, subprocess, sys

root = sys.argv[1]
profiles = ((2822400, 64), (5644800, 128), (11289600, 256),
            (3072000, 64), (6144000, 128), (12288000, 256))

def chunk(ident, payload):
    return ident + struct.pack('>Q', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')

for rate, multiple in profiles:
    # Enough material to exercise long filter warmups and non-zero seek offsets.
    samples = rate // 10
    samples -= samples % 16
    per_channel = samples // 8
    # Deterministic, independent, non-silent channel patterns. Canonical raw
    # bytes are MSB first; DSF stores the bit-reversed bytes (LSB first).
    canonical = [bytes(((i * 73 + ch * 101 + (i >> 3) * 19) ^ (i >> 5)) & 255
                       for i in range(per_channel)) for ch in range(2)]
    reverse = lambda b: int(f'{b:08b}'[::-1], 2)
    block = 4096
    reversed_channels = [bytes(reverse(x) for x in c) for c in canonical]
    payload_parts = []
    for start in range(0, per_channel, block):
        for channel in reversed_channels:
            part = channel[start:start + block]
            payload_parts.append(part + bytes(block - len(part)))
    payload = b''.join(payload_parts)
    data_size = 12 + len(payload)
    file_size = 28 + 52 + data_size
    dsf = (b'DSD ' + struct.pack('<QQQ', 28, file_size, 0) + b'fmt ' +
           struct.pack('<QIIIIIIQII', 52, 1, 0, 2, 2, rate, 1, samples, block, 0) +
           b'data' + struct.pack('<Q', data_size) + payload)
    dsf_path = os.path.join(root, f'dsd{multiple}_{rate}.dsf')
    open(dsf_path, 'wb').write(dsf)

    prop = b'SND ' + chunk(b'FS  ', struct.pack('>I', rate)) + \
           chunk(b'CHNL', struct.pack('>H', 2) + b'SLFTSRGT') + \
           chunk(b'CMPR', b'DSD ' + bytes([15]) + b'not compressed ')
    dff_data = b''.join(bytes((canonical[0][i], canonical[1][i])) for i in range(per_channel))
    body = b'DSD ' + chunk(b'FVER', struct.pack('>I', 0x01050000)) + \
           chunk(b'PROP', prop) + chunk(b'DSD ', dff_data)
    dff = b'FRM8' + struct.pack('>Q', len(body)) + body
    dff_path = os.path.join(root, f'dsd{multiple}_{rate}.dff')
    open(dff_path, 'wb').write(dff)

    for source in (dsf_path, dff_path):
        # -q quiet, -y overwrite existing outputs. Explicit output avoids
        # dependence on wavpack's default extension/path behavior.
        subprocess.run(['wavpack', '-q', '-y', '--blocksize=4096', source,
                        '-o', source + '.wv'], check=True)

# Preserve the declared sample count while damaging the encoded payload so
# open/read must distinguish an actual WavPack failure from ordinary EOF.
reference = os.path.join(root, 'dsd64_2822400.dsf.wv')
data = open(reference, 'rb').read()
blocks, offset = [], 0
while offset + 32 <= len(data) and data[offset:offset + 4] == b'wvpk':
    block_size = int.from_bytes(data[offset + 4:offset + 8], 'little') + 8
    if block_size < 32 or offset + block_size > len(data):
        raise SystemExit(f'invalid WavPack block at {offset}: {block_size}')
    blocks.append((offset, block_size))
    offset += block_size
if len(blocks) < 3 or offset != len(data):
    raise SystemExit(f'expected at least three complete WavPack blocks, got {len(blocks)}')
second, second_size = blocks[1]
third, _ = blocks[2]
# Preserve block one and its full header; truncate inside block two after its
# header, and corrupt compressed payload bytes in that same block.
open(os.path.join(root, 'truncated.wv'), 'wb').write(data[:second + max(40, second_size // 2)])
damaged = bytearray(data)
damaged[second + min(96, second_size - 9)] ^= 0x40
open(os.path.join(root, 'corrupt.wv'), 'wb').write(damaged)

# A normal PCM WavPack stream must remain outside the native DSD decoder.
with open(os.path.join(root, 'pcm.wav'), 'wb') as f:
    f.write(b'RIFF' + struct.pack('<I', 36 + 4800 * 4) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16) + b'data' +
            struct.pack('<I', 4800 * 4))
    for i in range(4800):
        f.write(struct.pack('<hh', ((i * 313) & 65535) - 32768,
                            ((i * 197 + 91) & 65535) - 32768))
PY

wavpack -q -y "${fixture_dir}/pcm.wav" -o "${fixture_dir}/pcm.wv"
sanitizers=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
cc -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror "${sanitizers[@]}" \
    -I"${repo_dir}/src/audio" -I"${repo_dir}/third_party/libwavpack/include" \
    "${repo_dir}/src/audio/decoders/dsd/dsd_wavpack_test.c" \
    "${repo_dir}/src/audio/decoders/dsd/dsd_decoder.c" "${repo_dir}/src/audio/decoders/dsd/dsd_filter.c" \
    "${repo_dir}/src/audio/readers/sacd/sacd_reader.c" "${repo_dir}/src/audio/decoders/dst/dst_decoder.c" \
    "${repo_dir}/src/library/sacd/sacd_iso.c" \
    "${build_dir}/libwavpack/libwavpack.a" -lm -lpthread -o "${test_bin}"

args=("${fixture_dir}/pcm.wv" "${fixture_dir}/truncated.wv" "${fixture_dir}/corrupt.wv")
for source in "${fixture_dir}"/*.dsf "${fixture_dir}"/*.dff; do
    args+=("${source}" "${source}.wv")
done
"${test_bin}" "${args[@]}"
