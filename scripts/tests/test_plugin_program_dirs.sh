#!/usr/bin/env bash
# Runs the host player with a probe plugin that tries to write into .apps,
# .apps-data and .emulators through every plugin API, and fails unless every
# attempt is refused (and the same APIs still work elsewhere).
#
#   make host BOARD=r1
#   COMPAS_HOST_ASSETS=<dir with theme1, theme2 and fonts> scripts/tests/test_plugin_program_dirs.sh
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
assets=${COMPAS_HOST_ASSETS:?set COMPAS_HOST_ASSETS to a directory with theme1, theme2 and fonts}
assets=$(cd "$assets" && pwd)
bin=${COMPAS_HOST_BIN:-$repo/compas_player_host}
run=$(mktemp -d "${TMPDIR:-/tmp}/plugin_program_dirs.XXXXXX")
trap 'rm -rf "$run"' EXIT

mkdir -p "$run/assets" "$run/.compas" "$run/music/.plugins" "$run/music/.emulators/e1" "$run/music/.apps-data" \
    "$run/music/.apps/hello"
for d in theme1 theme2 fonts; do ln -s "$assets/$d" "$run/assets/$d"; done
printf 'setup_complete=1\n' > "$run/.compas/settings.txt"
cp "$repo/scripts/tests/plugins/ProtectTest.lua" "$run/music/.plugins/"
echo a > "$run/music/a.txt"
mkdir -p "$run/music/stage/.apps" "$run/music/stage/.emulators" "$run/music/stage/.apps-data"
: > "$run/apps_approved.txt"
cp "$repo/scripts/tests/apps/app.json" "$run/music/.apps/hello/"
: > "$run/music/.apps/hello/hello"; chmod +x "$run/music/.apps/hello/hello"
sleep 1; touch "$run/marker"

# The plugin writes its result at load; the player does not exit on its own.
(cd "$run" && SDL_VIDEODRIVER=offscreen timeout 15 "$bin" > "$run/log.txt" 2>&1 || true)

results="$run/music/protect_results.txt"
[ -s "$results" ] || { echo "no results (did the plugin load?)"; tail -20 "$run/log.txt"; exit 1; }
grep '^FAIL' "$results" && { echo "plugin protection test failed"; exit 1; }
echo "test_plugin_program_dirs: $(grep -c '^PASS' "$results") checks passed"
# No spelling of a reserved name may have been created at the card root or in the stage folder.
alias=$(cd "$run/music" && ls -A | grep -iE '^\.(apps|emulators)' | grep -vE '^\.(emulators|apps-data|apps)$' || true)
[ -z "$alias" ] || { echo "created: $alias"; exit 1; }
[ ! -s "$run/apps_approved.txt" ] || { echo "approval file was written"; exit 1; }
# Nothing may have been created inside the program folders.
extra=$(cd "$run/music" && find .apps .apps-data .emulators -newer ../marker \( -type f -o -type d \) || true)
[ -z "$extra" ] || { echo "unexpected files:"; echo "$extra"; exit 1; }
