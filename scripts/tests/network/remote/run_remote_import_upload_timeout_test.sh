#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
BUILD="$ROOT/build_ui_test/remote-import-upload-timeout"
mkdir -p "$BUILD"
WORK_BASE=$(mktemp -d "$ROOT/.remote-import-upload-baseline.XXXXXX")
WORK_REGRESSION=$(mktemp -d "$ROOT/.remote-import-upload-regression.XXXXXX")
WORK_FIXED=$(mktemp -d "$ROOT/.remote-import-upload-fixed.XXXXXX")
trap 'rm -rf "$WORK_BASE" "$WORK_REGRESSION" "$WORK_FIXED"' EXIT

python3 - "$ROOT" "$BUILD" <<'PY'
from pathlib import Path
import os
import subprocess
import sys

root, build = map(Path, sys.argv[1:])
source = (root / "src/network/remote/import/remote_import_files.c").read_text()
old = "#define IMPORT_IDLE_SECONDS 12"
if source.count(old) != 1:
    raise SystemExit("cannot reconstruct 60-second upload-timeout baseline")
(build / "remote_import_files_changed.c").write_text(source)

# Keep a reproducible pre-follow-up 12-second implementation to prove the
# large-write and sync-latency cases detect stale receive-only activity time.
regression = source
helper = '''static bool elapsed_at_least(const struct timespec *now, const struct timespec *then, time_t seconds) {
    time_t elapsed = now->tv_sec - then->tv_sec;
    return elapsed > seconds || (elapsed == seconds && now->tv_nsec >= then->tv_nsec);
}

'''
if regression.count(helper) != 1:
    raise SystemExit("cannot reconstruct previous elapsed-time helper")
regression = regression.replace(helper, "", 1)
fixed_check = '''        if (elapsed_at_least(&now, last, IMPORT_IDLE_SECONDS) ||
            elapsed_at_least(&now, start, IMPORT_TOTAL_SECONDS)) { errno = ETIMEDOUT; return false; }
'''
old_check = '''        long elapsed_idle = now.tv_sec - last->tv_sec;
        long elapsed_total = now.tv_sec - start->tv_sec;
        if (elapsed_idle >= IMPORT_IDLE_SECONDS || elapsed_total >= IMPORT_TOTAL_SECONDS) { errno = ETIMEDOUT; return false; }
'''
if regression.count(fixed_check) != 1:
    raise SystemExit("cannot reconstruct previous timeout check")
regression = regression.replace(fixed_check, old_check, 1)
activity_update = "            clock_gettime(CLOCK_MONOTONIC, &last_activity);\n"
if regression.count(activity_update) != 2:
    raise SystemExit("cannot reconstruct previous activity tracking")
regression = regression.replace(activity_update, "", 2)
(build / "remote_import_files_regression.c").write_text(regression)
(build / "remote_import_files_baseline.c").write_text(regression.replace(old, "#define IMPORT_IDLE_SECONDS 60", 1))

cc = os.environ.get("CC", "cc")
common = [cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
          "-DHOST_BUILD=1", "-ffunction-sections", "-fdata-sections",
          "-I" + str(root), "-I" + str(root / "src/network"),
          "-I" + str(root / "src/core"), "-I" + str(root / "src/library"),
          "-I" + str(root / "src/ui"), "-I" + str(root / "src/hardware"),
          "-Wl,--gc-sections", "-Wl,--wrap=clock_gettime", "-Wl,--wrap=poll",
          "-Wl,--wrap=write", "-Wl,--wrap=fdatasync"]
test = root / "src/network/remote/import/remote_import_upload_timeout_spec.c"
utf8 = root / "src/core/text/utf8_util.c"
for mode, implementation, seconds in (
    ("baseline", build / "remote_import_files_baseline.c", "60"),
    ("regression", build / "remote_import_files_regression.c", "12"),
    ("changed", build / "remote_import_files_changed.c", "12"),
):
    flags = ["-DEXPECT_TIMEOUT_SECONDS=" + seconds]
    if mode != "changed": flags.append("-DSKIP_WALLCLOCK=1")
    if mode == "regression": flags.append("-DEXPECT_STALE_ACTIVITY=1")
    subprocess.run([*common, *flags, str(implementation), str(test), str(utf8),
                    "-o", str(build / ("upload_timeout_" + mode))], check=True)
PY

(cd "$WORK_BASE" && "$BUILD/upload_timeout_baseline")
(cd "$WORK_REGRESSION" && "$BUILD/upload_timeout_regression")
(cd "$WORK_FIXED" && timeout 25s "$BUILD/upload_timeout_changed")
