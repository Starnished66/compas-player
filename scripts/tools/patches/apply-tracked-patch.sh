#!/bin/sh
# Apply one tracked unified diff idempotently.
# Usage: apply-tracked-patch.sh <directory> <patch>
# A reverse dry-run means the patch is already present. A forward dry-run
# applies it. Anything else is a mismatch and fails the build.
set -eu
dir=$1
patchfile=$2
if patch -p1 -d "$dir" --force --fuzz=0 --dry-run --reverse < "$patchfile" >/dev/null 2>&1; then
  exit 0
fi
if patch -p1 -d "$dir" --force --fuzz=0 --dry-run < "$patchfile" >/dev/null 2>&1; then
  patch -p1 -d "$dir" --force --fuzz=0 < "$patchfile"
  exit 0
fi
echo "patch $patchfile does not match $dir" >&2
exit 1
