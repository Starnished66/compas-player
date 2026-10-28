#!/usr/bin/env bash
# Host tests for the external apps backend (manifest, board filter, launch file
# rules, environment, approval keying, reserved program folders).
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out="$repo/build/host-tests/test_external_apps"
mkdir -p "$(dirname "$out")"
cc=${CC:-cc}

# A static ELF program for the manifest checks (never run): the target's own
# toolchain when it is on PATH, else the host compiler if it can link static.
printf 'int main(void){return 0;}\n' > "$out.fixture.c"
if command -v mipsel-linux-musl-gcc > /dev/null; then
    mipsel-linux-musl-gcc -static -o "$out.fixture" "$out.fixture.c"
else
    "$cc" -static -o "$out.fixture" "$out.fixture.c"
fi

# store_common.c is the real validation code; its network helpers are stubbed.
cat > "$out.stubs.c" <<'STUBS'
#include "src/network/store/plugins/store_common.h"
bool http_get_to_file_redirects(const char * url, bool verify_tls, const char * dest_path, size_t max_body_size,
                                http_progress_cb_t progress, void * context, uint32_t connect_ms, uint32_t read_ms,
                                http_cancel_token_t * cancel, int max_redirects, int * status) {
    (void) url; (void) verify_tls; (void) dest_path; (void) max_body_size; (void) progress; (void) context;
    (void) connect_ms; (void) read_ms; (void) cancel; (void) max_redirects; (void) status;
    return false;
}
bool http_request_ex(const http_request_t * req, http_cancel_token_t * cancel, http_response_t * response) {
    (void) req; (void) cancel; (void) response;
    return false;
}
void http_response_free(http_response_t * response) { (void) response; }
STUBS

mbed="$repo/mbedtls/library"
"$cc" -std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-format-truncation -Wno-unused-parameter -DHOST_BUILD=1 -DBOARD_R1 -pthread \
    -I"$repo" -I"$repo/src/core" -I"$repo/cJSON" -I"$repo/mbedtls/include" -I"$repo/src/network" -I"$repo/lvgl" \
    -DLV_CONF_INCLUDE_SIMPLE=1 \
    "$repo/scripts/tests/test_external_apps.c" "$out.stubs.c" \
    "$repo/src/core/device/app_registry.c" "$repo/src/core/device/app_approval.c" \
    "$repo/src/core/process/app_session.c" "$repo/src/plugins/storage/plugin_reserved_dirs.c" \
    "$repo/src/network/store/plugins/store_common.c" "$repo/src/ui/text/i18n/i18n.c" \
    "$repo/src/ui/text/i18n/i18n_catalog.c" "$repo/cJSON/cJSON.c" \
    "$mbed/sha256.c" "$mbed/platform_util.c" \
    -o "$out"
"$out" "$out.fixture"
