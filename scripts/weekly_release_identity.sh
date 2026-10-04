#!/usr/bin/env bash
set -euo pipefail

if (( $# > 1 )); then
  echo "usage: $0 [YYYY-MM-DD]" >&2
  exit 2
fi

release_date=${1:-$(TZ=America/Costa_Rica date +%F)}
normalized_date=$(date -u -d "$release_date" +%F) || {
  echo "invalid release date: $release_date" >&2
  exit 2
}
if [[ "$normalized_date" != "$release_date" ]]; then
  echo "release date must use YYYY-MM-DD: $release_date" >&2
  exit 2
fi
weekday=$(date -u -d "$release_date" +%u) || {
  echo "invalid release date: $release_date" >&2
  exit 2
}
week_monday=$(date -u -d "$release_date -$((weekday - 1)) days" +%F)
anchor_epoch=$(date -u -d '2026-09-28' +%s)
week_epoch=$(date -u -d "$week_monday" +%s)
week_delta=$((week_epoch - anchor_epoch))
if (( week_delta < 0 || week_delta % 604800 != 0 )); then
  echo "release date precedes version anchor: $release_date" >&2
  exit 2
fi

week_number=$((week_delta / 604800))
printf 'tag=weekly-beta-%s\n' "$week_monday"
printf 'title=Compas v1.%s\n' "$week_number"
