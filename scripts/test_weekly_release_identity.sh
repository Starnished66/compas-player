#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
identity_script="$script_dir/weekly_release_identity.sh"

expect_identity() {
  local date=$1 expected_tag=$2 expected_title=$3 actual expected
  actual=$("$identity_script" "$date")
  expected=$(printf 'tag=%s\ntitle=%s' "$expected_tag" "$expected_title")
  if [[ "$actual" != "$expected" ]]; then
    printf 'unexpected identity for %s:\n%s\n' "$date" "$actual" >&2
    exit 1
  fi
}

# v1.1 ships in the week starting October 5; the next Monday is v1.2.
expect_identity 2026-10-05 v1.1 'Compas v1.1'
expect_identity 2026-10-10 v1.1 'Compas v1.1'
expect_identity 2026-10-12 v1.2 'Compas v1.2'
expect_identity 2026-10-19 v1.3 'Compas v1.3'

if "$identity_script" not-a-date >/dev/null 2>&1; then
  echo 'release identity unexpectedly accepted an invalid date' >&2
  exit 1
fi

echo 'Weekly release identity checks passed.'
