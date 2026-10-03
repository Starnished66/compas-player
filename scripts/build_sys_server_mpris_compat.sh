#!/usr/bin/env bash
# Build a narrow LD_PRELOAD fix for sys_server's malformed MPRIS metadata.
# The daemon itself remains the stock firmware binary.
set -euo pipefail

readonly SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
readonly TARGET_FLAGS='-EL -mips32r2 -mabi=32 -mhard-float -mfp32'
stock_root=${BASE_STOCK_ROOT:-/home/josegarita/Desktop/Test2/squashfs-root}
glib_stage=${BASE_GLIB_STAGE:-$REPO_ROOT/scratch/base-upgrade/glib/stage}
cross_prefix=${BASE_CROSS_PREFIX:-$REPO_ROOT/scratch/ingenic-toolchain-v5.2/toolchain/bin/mips-linux-gnu-}
work=${BASE_SYS_SERVER_MPRIS_COMPAT_BUILD_DIR:-$REPO_ROOT/scratch/base-upgrade/sys-server-mpris-compat}

usage() {
    echo "Usage: $0 [--stockroot DIR] [--build-dir DIR]" >&2
}
die() { echo "build_sys_server_mpris_compat.sh: $*" >&2; exit 1; }

while (($#)); do
    case $1 in
        --stockroot) (($# >= 2)) || { usage; exit 2; }; stock_root=$2; shift 2 ;;
        --build-dir) (($# >= 2)) || { usage; exit 2; }; work=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; die "unknown option: $1" ;;
    esac
done

readonly DAEMON=$stock_root/usr/bin/sys_server
readonly GLIB_INCLUDE=$glib_stage/usr/include/glib-2.0
readonly GLIB_CONFIG_INCLUDE=$glib_stage/usr/lib/glib-2.0/include
readonly GLIB_LIB=$glib_stage/usr/lib
readonly CC=${cross_prefix}gcc
readonly READELF=${cross_prefix}readelf
readonly CRTI=$($CC -print-file-name=crti.o)
readonly CRTBEGIN=$($CC -print-file-name=crtbeginS.o)
readonly CRTEND=$($CC -print-file-name=crtendS.o)
readonly CRTN=$($CC -print-file-name=crtn.o)

for tool in "$CC" "$READELF"; do
    [[ -x $tool ]] || die "missing cross tool: $tool"
done
for object in "$CRTI" "$CRTBEGIN" "$CRTEND" "$CRTN"; do
    [[ -r $object ]] || die "missing compiler startup object: $object"
done
[[ -x $DAEMON ]] || die "missing stock sys_server: $DAEMON"
[[ -f $GLIB_INCLUDE/glib.h && -f $GLIB_CONFIG_INCLUDE/glibconfig.h ]] ||
    die "GLib headers are missing from $glib_stage"
[[ -f $GLIB_LIB/libglib-2.0.so ]] || die "GLib library is missing from $glib_stage"
[[ ! -L $work ]] || die "build directory must not be a symlink: $work"

"$READELF" -h "$DAEMON" | rg -q 'Class:[[:space:]]+ELF32' || die "stock sys_server is not ELF32"
"$READELF" -h "$DAEMON" | rg -q 'Data:[[:space:]]+2.s complement, little endian' ||
    die "stock sys_server is not little endian"
"$READELF" -h "$DAEMON" | rg -q 'Machine:[[:space:]]+MIPS' || die "stock sys_server is not MIPS"
symbols=$("$READELF" --wide --dyn-syms "$DAEMON")
rg -q 'UND[[:space:]]+g_variant_builder_add(@|[[:space:]]|$)' <<< "$symbols" ||
    die "stock sys_server does not import interposable g_variant_builder_add"

mkdir -p -- "$work"
work=$(cd -- "$work" && pwd)
readonly RUN=$(mktemp -d "$work/run.XXXXXX")
readonly STAGE=$RUN/stage
mkdir -p -- "$STAGE/usr/lib"

"$CC" $TARGET_FLAGS -Os -fPIC -fno-stack-protector -D_DEFAULT_SOURCE \
    -nostdlib -shared -Wl,-soname,libsys_server_mpris_compat.so -Wl,--no-undefined \
    -I"$GLIB_INCLUDE" -I"$GLIB_CONFIG_INCLUDE" \
    "$CRTI" "$CRTBEGIN" \
    "$SCRIPT_DIR/base_image/sys_server_mpris_compat.c" \
    -L"$GLIB_LIB" -Wl,-rpath-link,"$GLIB_LIB" -lglib-2.0 \
    "$CRTEND" "$CRTN" \
    -o "$STAGE/usr/lib/libsys_server_mpris_compat.so"

"$READELF" -h "$STAGE/usr/lib/libsys_server_mpris_compat.so" | sed -n '1,22p'
"$READELF" -d "$STAGE/usr/lib/libsys_server_mpris_compat.so" | rg 'NEEDED|SONAME'
"$READELF" -d "$STAGE/usr/lib/libsys_server_mpris_compat.so" | rg -q 'INIT|INIT_ARRAY' ||
    die "constructor init entry missing from shared object"
"$READELF" -Ws "$STAGE/usr/lib/libsys_server_mpris_compat.so" | rg \
    'g_variant_(builder_add|builder_add_value|new_va)|g_unsetenv'

[[ ! -e $work/stage || -L $work/stage ]] || die "stage must be a symlink: $work/stage"
ln -sfnT "$STAGE" "$work/stage"
printf 'sys_server MPRIS compatibility stage (stock daemon unchanged): %s\n' "$STAGE"
