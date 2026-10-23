#!/usr/bin/env bash
set -euo pipefail
# Official FFmpeg DST sample: https://ffmpeg.org/pipermail/ffmpeg-devel/attachments/20181222/75daabd7/attachment.obj
# SHA-256: 29d305bd19731ce079324ed14ac17254f254115d7787b5646a943e367b238ed0
# Set DST_FRAME to one extracted DSTF payload to exercise compressed decoding.
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${repo_dir}/build_host"
sanitizers=()
if [[ "${SANITIZE:-0}" == "1" ]]; then
    sanitizers=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
cc -std=c11 -O2 -Wall -Wextra -Werror "${sanitizers[@]}" \
    -I"${repo_dir}/src/audio" \
    "${repo_dir}/src/audio/dst_decoder_spec.c" \
    "${repo_dir}/src/audio/dst_decoder.c" \
    -o "${repo_dir}/build_host/dst_decoder_spec"
if [[ -n "${DST_FRAME:-}" ]]; then
    "${repo_dir}/build_host/dst_decoder_spec" "${DST_FRAME}"
else
    "${repo_dir}/build_host/dst_decoder_spec"
fi
