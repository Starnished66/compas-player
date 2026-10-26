#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/http_url_spec"
mkdir -p "$build_dir"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Isrc/network \
    src/network/http/url/http_url.c src/network/http/url/http_url_spec.c -o "$build_dir/test"
"$build_dir/test"
