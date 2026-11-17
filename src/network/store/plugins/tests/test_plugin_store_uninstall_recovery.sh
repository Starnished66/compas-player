#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../../../.." && pwd)
cd "$ROOT"
OUT=$(mktemp)
WORK=$(mktemp -d)
trap 'rm -f "$OUT"; rm -rf "$WORK"' EXIT
cc -DHOST_BUILD=1 -DLV_CONF_INCLUDE_SIMPLE=1 -ffunction-sections -fdata-sections \
  -I. -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/ui/player \
  -Isrc/ui/library -Isrc/core -Isrc/plugins -Isrc/plugins/bindings -Ilvgl -Idr_libs \
  -Ifaad2/include -Ialac/codec -Imbedtls/include -IcJSON -Ilua/src -Istb_vorbis \
  -Ijpeg_vendor_config -Ijpeg -Ithird_party/libwavpack/include -Ithird_party/libsndfile/include \
  src/network/store/plugins/tests/test_plugin_store_uninstall_recovery.c \
  src/network/store/plugins/plugin_store_card.c \
  src/network/store/plugins/plugin_store_catalog.c \
  src/network/store/plugins/store_common.c \
  -Wl,--gc-sections -Wl,--wrap=ps_card_still -Wl,--wrap=ps_storage_path_writable -lpthread -lm -o "$OUT"
"$OUT" "$WORK"
