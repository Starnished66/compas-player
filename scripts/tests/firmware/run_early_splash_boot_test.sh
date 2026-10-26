#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

overlay="$repo/firmware/overlay"
s11="$overlay/etc/init.d/S11jpeg_display_shell"
s22="$overlay/etc/init.d/S22compas_startup_image"
sh -n "$s11"
sh -n "$s22"
[[ $(basename "$s11") < $(basename "$s22") ]]
grep -q 'COMPAS_STOCK_SPLASH_DEFERRED' "$s11"
! grep -Eq 'nanddump|cmd_jpeg_display|mount|sleep' "$s11"
! grep -Eq 'mount|ubimkvol|flash_erase|ubiattach|ubidetach|mmcblk|/dev/mmc' "$s22"
[[ S21mount_ubifs < $(basename "$s22") ]]

# The standardizer rejects unknown vendor init scripts before touching them.
mkdir -p "$tmp/root/usr/bin" "$tmp/root/etc/init.d"
printf '#!/bin/sh\n' > "$tmp/root/usr/bin/hiby_player.sh"
printf '#!/bin/sh\ncustom vendor behavior\n' > "$tmp/root/etc/init.d/S11jpeg_display_shell"
if "$repo/scripts/firmware/boot_pipeline/standardize_boot_pipeline.sh" --board r3proii "$tmp/root" >"$tmp/standardize.out" 2>&1; then
    echo "standardizer unexpectedly accepted an unknown S11 layout" >&2
    exit 1
fi
grep -q 'Unsupported S11jpeg_display_shell layout' "$tmp/standardize.out"
grep -q 'custom vendor behavior' "$tmp/root/etc/init.d/S11jpeg_display_shell"

# A known stock layout passes the S11 validator, then fails at the deliberately
# absent Wi-Fi vendor script without partially rewriting boot files.
cp "$repo/scripts/fixtures/S11jpeg_display_shell.vendor" \
    "$tmp/root/etc/init.d/S11jpeg_display_shell"
if "$repo/scripts/firmware/boot_pipeline/standardize_boot_pipeline.sh" --board r3proii "$tmp/root" >"$tmp/standardize.out" 2>&1; then
    echo "standardizer unexpectedly accepted an incomplete rootfs" >&2
    exit 1
fi
grep -q 'Missing Wi-Fi vendor implementation' "$tmp/standardize.out"
grep -q 'jpeg_display()' "$tmp/root/etc/init.d/S11jpeg_display_shell"
[[ ! -e $tmp/root/etc/init.d/S22compas_startup_image ]]

mkdir -p "$tmp/bin" "$tmp/etc" "$tmp/sys/backlight"
: > "$tmp/sys/backlight/brightness"
cat > "$tmp/bin/helper" <<'EOF'
#!/bin/sh
echo helper >> "$EVENTS"
exit "${HELPER_STATUS:-0}"
EOF
cat > "$tmp/bin/nanddump" <<'EOF'
#!/bin/sh
echo nanddump >> "$EVENTS"
printf '%s\n' "${THEME:-theme:1}"
EOF
cat > "$tmp/bin/cmd_jpeg_display" <<'EOF'
#!/bin/sh
echo "jpeg:$1" >> "$EVENTS"
EOF
cat > "$tmp/bin/sleep" <<'EOF'
#!/bin/sh
echo "sleep:$1" >> "$EVENTS"
EOF
chmod +x "$tmp/bin/"*

# Relocate only fixed system paths so the init script runs safely with host mocks.
sed -e "s|/usr/bin/compas-early-splash|$tmp/bin/helper|g" \
    -e "s|/sys/class/backlight/backlight_pwm0/brightness|$tmp/sys/backlight/brightness|g" \
    "$s22" > "$tmp/S22"
chmod +x "$tmp/S22"
export PATH="$tmp/bin:$PATH" EVENTS="$tmp/events"

: > "$EVENTS"
"$tmp/S22" start
[[ $(cat "$EVENTS") == $'helper\nsleep:0.1' ]]
[[ $(cat "$tmp/sys/backlight/brightness") == 60 ]]

: > "$EVENTS"
HELPER_STATUS=1 THEME=theme:2 "$tmp/S22" start
[[ $(cat "$EVENTS") == $'helper\nnanddump\njpeg:/etc/logo2.jpeg\nsleep:0.1' ]]

: > "$EVENTS"
HELPER_STATUS=1 THEME=theme:unrecognized "$tmp/S22" start
[[ $(tail -n 2 "$EVENTS") == $'jpeg:/etc/logo.jpeg\nsleep:0.1' ]]

: > "$EVENTS"
HELPER_STATUS=0 THEME=theme:1 "$tmp/S22" start
[[ $(cat "$EVENTS") == $'helper\nsleep:0.1' ]]

: > "$EVENTS"
echo 0 > "$tmp/sys/backlight/brightness"
"$tmp/S22" stop
[[ ! -s "$EVENTS" ]]
"$tmp/S22" start
[[ $(cat "$tmp/sys/backlight/brightness") == 60 ]]
[[ $(tail -n 2 "$EVENTS") == $'helper\nsleep:0.1' ]]

echo "early splash boot tests passed"
