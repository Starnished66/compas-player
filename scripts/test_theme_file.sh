#!/usr/bin/env bash
set -euo pipefail

# Persistent host build directory or caller scratch override
BUILD_DIR="${THEME_TEST_BUILD_DIR:-build_ui_test/theme-parser}"
mkdir -p "$BUILD_DIR"

echo "Compiling theme_file standalone test runner in $BUILD_DIR..."

gcc -std=c99 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
    -I. \
    src/core/theme_file.c \
    src/core/theme_file_test.c \
    -o "$BUILD_DIR/theme_file_test"

echo "Running theme_file unit tests..."
"$BUILD_DIR/theme_file_test"

echo "=== theme_file tests completed successfully ==="
