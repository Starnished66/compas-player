#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build_test
make tinyalsa/.compas_native_dsd
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wconversion -Wshadow -fsanitize=address,undefined \
  -I tinyalsa/include src/audio/usb_dsd_output.c src/audio/usb_dsd_output_checks.c \
  -o build_test/usb_dsd_output_spec
ASAN_OPTIONS=detect_leaks=1 build_test/usb_dsd_output_spec
