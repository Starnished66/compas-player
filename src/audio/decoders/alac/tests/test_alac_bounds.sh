#!/bin/sh
set -eu
cd "$(dirname "$0")/../../../../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
flags="-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Ialac/codec"
for source in ALACBitUtilities EndianPortable ag_dec dp_dec matrix_dec; do
    cc $flags -c "alac/codec/$source.c" -o "$work/$source.o"
done
c++ $flags src/audio/decoders/alac/tests/test_alac_bounds.cpp alac/codec/ALACDecoder.cpp "$work"/*.o -o "$work/test"
"$work/test"
