#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:---current}"
build_dir="${METADATA_ISOLATION_TEST_BUILD_DIR:-$root/build_ui_test/metadata_isolation}"
mkdir -p "$build_dir"

if [[ "$mode" == "--baseline" ]]; then
    baseline_source="${METADATA_ISOLATION_BASELINE_SOURCE:-}"
    if [[ -z "$baseline_source" || ! -f "$baseline_source" ]]; then
        echo "--baseline requires METADATA_ISOLATION_BASELINE_SOURCE to name a frozen metadata.c" >&2
        exit 2
    fi
    isolation_source="$build_dir/metadata_isolation_baseline.c"
    python3 - "$baseline_source" "$isolation_source" <<'PYFIXTURE'
import sys
from pathlib import Path
source=Path(sys.argv[1])
output=Path(sys.argv[2])
text=source.read_text()
marker='/* Isolates decoder-based metadata parsing in a short-lived child process'
start=text.find(marker)
if start < 0:
    raise SystemExit(f'isolation tail marker not found in {source}')
body=text[start:]
for old,new in (
    ('metadata_read_internal(path, out, false);','metadata_read_without_artwork(path, out);'),
    ('artwork_only = true;','metadata_parser_enable_artwork_only();'),
):
    if body.count(old) != 1:
        raise SystemExit(f'expected one approved seam {old!r}, found {body.count(old)}')
    body=body.replace(old,new,1)
includes='''#include "metadata.h"
#include "metadata_parser_internal.h"
#include "artwork_memory.h"
#include "subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

'''
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(includes+body)
PYFIXTURE
    if [[ ! -s "$isolation_source" ]]; then
        echo "failed to extract isolation tail from $baseline_source" >&2
        exit 1
    fi
elif [[ "$mode" == "--current" ]]; then
    isolation_source="$root/src/library/metadata_isolation.c"
    if [[ ! -f "$isolation_source" ]]; then
        echo "metadata_isolation.c is not present yet; use --baseline for frozen-source characterization" >&2
        exit 2
    fi
else
    echo "usage: $0 [--baseline|--current]" >&2
    exit 2
fi

cc -std=gnu11 -D_GNU_SOURCE -O0 -g -Wall -Wextra -Werror \
    -I"$root/scripts/metadata_isolation_support" \
    -I"$root/src/library" -I"$root/src/core" -I"$root/src/ui" -I"$root" \
    "-DMETADATA_ISOLATION_SOURCE=\"$isolation_source\"" \
    "$root/scripts/metadata_isolation_spec.c" \
    -pthread -o "$build_dir/metadata_isolation_spec"
ulimit -c 0
"$build_dir/metadata_isolation_spec"
