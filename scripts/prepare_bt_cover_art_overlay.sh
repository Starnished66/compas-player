#!/usr/bin/env bash
# Add the staged AVRCP cover-art responder to an existing firmware overlay.
set -euo pipefail

usage() {
    echo "Usage: $0 BLUEZ_STAGE STOCK_ROOT OVERLAY_ROOT" >&2
}
die() { echo "prepare_bt_cover_art_overlay.sh: $*" >&2; exit 1; }

[[ $# == 3 ]] || { usage; exit 2; }
stage=$1
stock=$2
overlay=$3

[[ -x $stage/usr/libexec/bluetooth/obexd ]] || die "missing staged obexd: $stage/usr/libexec/bluetooth/obexd"
[[ -x $stage/usr/libexec/bluetooth/bluetoothd ]] || die "missing staged bluetoothd: $stage/usr/libexec/bluetooth/bluetoothd"
[[ -f $stage/etc/dbus-1/system.d/org.bluez.obex.conf ]] || die "missing staged system-bus policy: $stage/etc/dbus-1/system.d/org.bluez.obex.conf"
[[ -x $stage/etc/init.d/S81compas_obex ]] || die "missing staged init hook: $stage/etc/init.d/S81compas_obex"
stock=$(realpath -- "$stock")
[[ -d $stock/usr/lib ]] || die "stock root is incomplete: $stock"
[[ -d $overlay ]] || die "overlay root does not exist: $overlay"
overlay=$(realpath -- "$overlay")
[[ $overlay != "$stock" && $overlay != "$stock/"* ]] ||
    die 'the overlay must be separate from the stock tree'

mkdir -p "$overlay/usr/libexec/bluetooth" "$overlay/etc/dbus-1/system.d" "$overlay/etc/init.d"
cp -a "$stage/usr/libexec/bluetooth/bluetoothd" "$overlay/usr/libexec/bluetooth/"
cp -a "$stage/usr/libexec/bluetooth/obexd" "$overlay/usr/libexec/bluetooth/"
cp -a "$stage/etc/dbus-1/system.d/org.bluez.obex.conf" "$overlay/etc/dbus-1/system.d/"
cp -a "$stage/etc/init.d/S81compas_obex" "$overlay/etc/init.d/"
sh -n "$overlay/etc/init.d/S81compas_obex"

printf 'Added OBEX cover-art responder from %s\n' "$stage"
