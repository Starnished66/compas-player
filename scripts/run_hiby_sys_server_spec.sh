#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${HIBY_SYS_SERVER_SPEC_BUILD_DIR:-build_ui_test/hiby_sys_server}"
mkdir -p "$BUILD_DIR"
SOCKET_PATH="$(pwd)/$BUILD_DIR/sys_server.sock"

gcc -std=gnu11 -Wall -Wextra -Werror -pthread \
    -DSYS_SERVER_SOCKET_PATH=\"$SOCKET_PATH\" \
    -Isrc/network -Isrc/core \
    src/network/hiby_sys_server.c \
    src/network/hiby_sys_server_spec.c \
    src/core/utf8_util.c \
    -o "$BUILD_DIR/hiby_sys_server_spec"

"$BUILD_DIR/hiby_sys_server_spec"
