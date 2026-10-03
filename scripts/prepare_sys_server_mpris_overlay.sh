#!/usr/bin/env bash
# Add the metadata compatibility library to a firmware overlay, never the
# input tree. Older daemons without a MPRIS bridge need no interposition.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# == 2 ]] || { echo "Usage: $0 STOCK_ROOT OVERLAY_ROOT" >&2; exit 2; }
stock=$(realpath -- "$1")
mkdir -p -- "$2"
overlay=$(realpath -- "$2")
[[ $overlay != "$stock" && $overlay != "$stock/"* ]] || {
    echo 'The overlay must be separate from the stock tree' >&2; exit 1;
}
daemon="$stock/usr/bin/sys_server"
[[ -f $daemon ]] || { echo "Missing daemon: $daemon" >&2; exit 1; }
symbols=$(readelf --wide --dyn-syms "$daemon")
if ! rg -q 'UND[[:space:]]+g_variant_builder_add(@|[[:space:]]|$)' <<< "$symbols"; then
    echo 'sys_server has no GLib metadata builder; no compatibility library needed.'
    exit 0
fi
init="$stock/etc/init.d/S50sys_server"
[[ -f $init ]] || { echo "Missing init script: $init" >&2; exit 1; }
pattern='^[[:space:]]*/usr/bin/sys_server[[:space:]]+&[[:space:]]*$'
[[ $(rg -c "$pattern" "$init") == 1 ]] || {
    echo 'Unrecognized sys_server startup; refusing to change it' >&2; exit 1;
}
"$repo/scripts/build_sys_server_mpris_compat.sh" --stockroot "$stock"
stage=${BASE_SYS_SERVER_MPRIS_COMPAT_BUILD_DIR:-$repo/scratch/base-upgrade/sys-server-mpris-compat}/stage
mkdir -p "$overlay/usr/lib" "$overlay/etc/init.d"
cp "$stage/usr/lib/libsys_server_mpris_compat.so" "$overlay/usr/lib/"
sed -E 's|^([[:space:]]*)/usr/bin/sys_server[[:space:]]+&[[:space:]]*$|\1LD_PRELOAD=/usr/lib/libsys_server_mpris_compat.so /usr/bin/sys_server \&|' \
    "$init" > "$overlay/etc/init.d/S50sys_server"
chmod --reference="$init" "$overlay/etc/init.d/S50sys_server"
sh -n "$overlay/etc/init.d/S50sys_server"
echo 'Added sys_server MPRIS metadata compatibility to the overlay.'
