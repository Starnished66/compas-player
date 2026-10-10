#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
build_dir="$repo_root/build_test/http_stream_range"
mkdir -p "$build_dir"
mapfile -t tls_sources < <(find mbedtls/library -maxdepth 1 -name '*.c' -print | sort)
gcc -O0 -g -pthread -DHTTP_STREAM_TESTING -I. -Isrc/network -Isrc/core -Imbedtls/include \
    src/network/http_stream_range_test.c src/network/http_stream.c \
    src/network/http_conn.c src/network/ca_bundle.c "${tls_sources[@]}" \
    -o "$build_dir/test"
"$build_dir/test"

openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$build_dir/tls.key" -out "$build_dir/tls.crt" \
    -subj /CN=localhost -addext subjectAltName=IP:127.0.0.1 >/dev/null 2>&1
port_file="$build_dir/tls.port"
rm -f "$port_file"
python3 scripts/http_range_tls_test_server.py "$port_file" \
    "$build_dir/tls.crt" "$build_dir/tls.key" &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
for _ in $(seq 1 100); do
    [[ -s "$port_file" ]] && break
    sleep 0.05
done
[[ -s "$port_file" ]]
"$build_dir/test" "$(cat "$port_file")"
