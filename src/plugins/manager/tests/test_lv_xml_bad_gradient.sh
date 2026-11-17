#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
build="$repo/build_host"
[[ -f "$build/app/main.o" ]] || { echo 'run make host first' >&2; exit 1; }
runtime=$(mktemp -d "${TMPDIR:-/tmp}/lvxml-gradient.XXXXXX")
trap 'rm -rf "$runtime"' EXIT
cd "$repo"
objcopy --redefine-sym main=compas_original_main "$build/app/main.o" "$runtime/main.o"
mapfile -t objs < <(find "$build" -name '*.o' ! -path '*/app/main.o' ! -path '*/CMakeFiles/*' \
  ! -path '*/lv_xml/src/others/xml/lv_xml_component.o' -print | sort)
cc -std=gnu11 -O0 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DHOST_BUILD=1 -DBOARD_R1 \
  -I. -Isrc -Isrc/audio -Isrc/network -Isrc/library -Isrc/hardware -Isrc/ui -Isrc/ui/player \
  -Isrc/ui/library -Isrc/core -Isrc/plugins -Isrc/plugins/bindings -Ilvgl -Ilvgl/src \
  -Ilvgl/src/others/translation -Ithird_party/lv_xml/src/others/xml -IcJSON \
  -Ilua/src -DLV_CONF_INCLUDE_SIMPLE=1 $(sdl2-config --cflags) \
  -c third_party/lv_xml/src/others/xml/lv_xml_component.c -o "$runtime/lv_xml_component.o"
cc -std=gnu11 -O0 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -DHOST_BUILD=1 -DBOARD_R1 \
  -I. -Ilvgl -Ilvgl/src -Ithird_party/lv_xml/src/others/xml -Ilvgl/src/others/translation -DLV_CONF_INCLUDE_SIMPLE=1 \
  -c src/plugins/manager/tests/test_lv_xml_bad_gradient.c -o "$runtime/test.o"
g++ "$runtime/test.o" "$runtime/main.o" "$runtime/lv_xml_component.o" "${objs[@]}" \
  "$build/libwavpack/libwavpack.a" "$build/libsndfile/libsndfile.a" \
  $(sdl2-config --libs) -lpthread -lm -ldl -o "$runtime/test"
"$runtime/test"
