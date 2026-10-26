#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$repo_root"
mkdir -p build_test
make -s -j"${JOBS:-4}" BOARD=r1 host
bash scripts/tests/audio/decoders/test_mp4_timing.sh
bash scripts/tests/audio/ape/ape_parity.sh
bash scripts/tests/audio/decoders/test_wavpack_caf_decoders.sh
cc -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -ffunction-sections -fdata-sections \
    -Isrc/audio -Isrc/library -Isrc/core -Isrc/network -Isrc/ui -I. -Ilvgl \
    -Idr_libs -Istb_vorbis -Imbedtls/include -Iopus/include -Iopusfile/include \
    -Ilibogg_vendor_config -Ilibogg/include -Ithird_party/libwavpack/include \
    -Ithird_party/libsndfile/include src/library/metadata/parsing/extra_formats_metadata_test.c \
    src/audio/decoders/wavpack/wavpack_decoder.c src/audio/decoders/caf/caf_decoder.c src/core/text/utf8_util.c \
    build_host/libwavpack/libwavpack.a build_host/libsndfile/libsndfile.a \
    -Wl,--gc-sections -lm -pthread -o build_test/extra_formats_metadata_test
build_test/extra_formats_metadata_test \
    build_host/wavpack_caf_test/integer.wv build_host/wavpack_caf_test/integer.caf
