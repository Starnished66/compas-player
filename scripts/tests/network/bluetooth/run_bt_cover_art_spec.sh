#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$ROOT"
BOARD_NAME="${BOARD:-r1}"
BUILD_DIR="${BT_COVER_ART_SPEC_BUILD_DIR:-build_ui_test/bt_cover_art_runner}"
mkdir -p "$BUILD_DIR"
DATA_DIR="build_ui_test/bt_art_data"
mkdir -p "$DATA_DIR"
HOST_OBJ_DIR=build_host
if [[ "$BOARD_NAME" != r1 ]]; then HOST_OBJ_DIR="build_host_$BOARD_NAME"; fi

jpeg_objects=(
    jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit jcmainct
    jcmarker jcmaster jcparam jcprepct jcsample jdapimin jdapistd jdatasrc
    jdcoefct jdcolor jddctmgr jdhuff jdinput jdmainct jdmarker jdmaster jdmerge
    jdpostct jdsample jdtrans jdarith jaricom jcomapi jerror jmemmgr jmemnobs
    jutils jidctint jidctfst jidctflt jfdctint jfdctfst jfdctflt jquant1 jquant2
)
make_args=()
for object in "${jpeg_objects[@]}"; do
    make_args+=("$HOST_OBJ_DIR/jpeg/$object.o")
done
make -s "BOARD=$BOARD_NAME" "${make_args[@]}"

objects=()
for object in "${jpeg_objects[@]}"; do
    objects+=("$HOST_OBJ_DIR/jpeg/$object.o")
done
gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -DBT_COVER_ART_TESTING -DBT_COVER_ART_DIR=\"$DATA_DIR\" \
    -Isrc/network -Ijpeg_vendor_config -Ijpeg \
    src/network/bluetooth/media/bt_cover_art.c src/network/bluetooth/media/bt_cover_art_spec.c \
    "${objects[@]}" -o "$BUILD_DIR/bt_cover_art_spec"

"$BUILD_DIR/bt_cover_art_spec"
