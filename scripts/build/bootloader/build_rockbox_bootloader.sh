#!/usr/bin/env bash
# Build the combined Compás/Rockbox bootloader for R1 or R3 Pro II.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
board=${1:-r1}
case "$board" in
    r1) target=hibyr1; define=BOARD_R1; binary=bootloader.r1 ;;
    r3proii) target=hibyr3proii; define=BOARD_R3PROII; binary=bootloader.r3proii ;;
    *) echo "Rockbox selector has no verified target for board: $board" >&2; exit 2 ;;
esac

revision=2b12d568ff32089a9efcbc2db95e9a18c7d9c68f
upstream=https://github.com/Rockbox/rockbox.git
work="$repo/build/rockbox-bootloader/$board"
source_dir="$work/source"
objects="$work/native"
build_dir="$work/rockbox"
patch_file="$repo/firmware/bootloader/rockbox-compas.patch"
cross_cc=$(command -v "${CROSS_CC:-mipsel-linux-musl-gcc}")
cross_prefix=${cross_cc%gcc}
jobs=${JOBS:-2}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be a positive integer" >&2; exit 2; }
[[ -s $patch_file ]] || { echo "Missing Rockbox integration patch: $patch_file" >&2; exit 1; }
mkdir -p "$work" "$objects" "$build_dir"
# A dedicated build checkout is disposable; never reset a supplied source tree.
exec 9>"$work/build.lock"
flock 9
if [[ ! -d $source_dir/.git ]]; then
    if [[ -n ${ROCKBOX_SOURCE:-} ]]; then
        git clone --no-checkout --shared "$ROCKBOX_SOURCE" "$source_dir"
    else
        git init "$source_dir"
        git -C "$source_dir" remote add origin "$upstream"
    fi
fi
if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    # Retry an interrupted first fetch, including checkouts whose .git
    # directory exists but never received the pinned commit.
    git -C "$source_dir" fetch --depth=1 "$upstream" "$revision"
fi
git -C "$source_dir" checkout --detach --force "$revision"
git -C "$source_dir" apply --check "$patch_file"
git -C "$source_dir" apply "$patch_file"

cpu=(-march=mips32r2 -mabi=32 -mfp32 -mno-odd-spreg -ffp-contract=off)
flags=(-O2 -Wall -ffunction-sections -fdata-sections "-D$define"
       -DCOMPAS_LOADER_BUILD -DCOMPAS_LOADER_EMBEDDED
       "-I$repo" "-I$repo/src/bootloader" "-I$repo/src/core"
       "-I$repo/mbedtls/include")
native_sources=(
    src/bootloader/app/main.c src/bootloader/rockbox/rockbox_bridge.c src/bootloader/display/fb_draw.c src/bootloader/startup/boot_tuning.c
    src/core/ui/display/startup_splash.c src/core/ui/web/glyph5x7.c
    src/bootloader/scan/scanner.c src/bootloader/install/installer.c src/bootloader/install/internal_storage.c
    src/bootloader/selector/ui/controller/selector_controller.c src/bootloader/selector/ui/controller/selector_input.c
    src/bootloader/selector/ui/render/selector_ui.c src/bootloader/selector/ui/render/selector_paint.c src/bootloader/selector/ui/controller/selector_request.c
    src/bootloader/selector/assets/logos/selector_logos.c src/bootloader/selector/assets/logos/selector_logos_wordmark_480.c
    src/bootloader/selector/assets/logos/selector_logos_rockbox_480.c
    src/bootloader/selector/assets/icons/selector_icons.c src/bootloader/selector/assets/icons/selector_icons_utilities_480.c
    src/bootloader/selector/assets/icons/selector_icons_nav.c src/bootloader/selector/assets/icons/selector_icons_tools.c
    src/core/storage/sd_fsck.c src/core/storage/sd_fsck_run.c
    src/core/updates/player_install.c src/core/system/lifecycle/reboot_handoff.c
    mbedtls/library/sha256.c mbedtls/library/platform_util.c
    src/bootloader/storage/sd_ready.c src/bootloader/storage/sd_ready_real.c
    src/core/process/subprocess.c src/core/system/logging/boot_trace.c
    lvgl/src/libs/tjpgd/tjpgd.c libexecinfo/execinfo.c libexecinfo/stacktraverse.c
)
native_objects=()
for file in "${native_sources[@]}"; do
    object="$objects/${file//\//_}.o"
    vendor_flags=()
    # Match the existing target rule for this generated backtrace dependency.
    [[ $file != libexecinfo/stacktraverse.c ]] || vendor_flags+=(-Wno-frame-address)
    "$cross_cc" "${cpu[@]}" "${flags[@]}" "${vendor_flags[@]}" "-I$repo/libexecinfo" -c "$repo/$file" -o "$object"
    native_objects+=("$object")
done

export COMPAS_ROCKBOX_CC="$cross_cc" COMPAS_ROCKBOX_ROOT="$repo"
wrapper="$work/rockbox-cc"
cat > "$wrapper" <<'EOF'
#!/bin/sh
set -- "$@" \
    -I"$COMPAS_ROCKBOX_ROOT/libexecinfo" \
    -I"$COMPAS_ROCKBOX_ROOT" \
    '-Dsigevent_t=struct sigevent' \
    -march=mips32r2 -mabi=32 -mfp32 -mno-odd-spreg -ffp-contract=off
for arg do
    if [ "$arg" = -c ]; then
        set -- "$@" '-Dsnprintf=compas_rb_snprintf' '-Dvsnprintf=compas_rb_vsnprintf' \
            '-Dsleep=compas_rb_sleep'
        break
    fi
done
exec "$COMPAS_ROCKBOX_CC" "$@"
EOF
chmod 0755 "$wrapper"
(
    cd "$build_dir"
    "$source_dir/tools/configure" --no-ccache --target="$target" --type=b \
        --compiler-prefix="$cross_prefix"
    # The compiler wrapper's symbol renaming must apply to every Rockbox
    # object, even when this build directory already contains older objects.
    env -u MAKEFLAGS -u MFLAGS make clean "CC=$wrapper"
    printf '\n%s: %s\n' "$build_dir/bootloader.elf" "${native_objects[*]}" >> Makefile
    # Keep Rockbox's nonstandard formatter namespaced so native objects use
    # the target libc's snprintf/vsnprintf semantics (notably %lld handling).
    # The hosted bootloader never initializes audio, so ALSA is not linked.
    # Retain the ordinary hosted libraries and link everything statically.
    env -u MAKEFLAGS -u MFLAGS make -j"$jobs" "CC=$wrapper" \
        "LDOPTS=${native_objects[*]} -static -no-pie -lpthread -lm -ldl -lrt"
)
python3 - "${cross_prefix}nm" "$build_dir" "${native_objects[@]}" <<'PY'
import os
import subprocess
import sys

nm, rockbox_dir, *native_objects = sys.argv[1:]
rockbox_objects = []
for root, _, files in os.walk(rockbox_dir):
    rockbox_objects.extend(
        os.path.join(root, name)
        for name in files
        if name.endswith((".o", ".a"))
    )

def symbols(args, paths):
    if not paths:
        return set()
    result = subprocess.run(
        [nm, *args, *paths], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True,
    )
    return result.stdout.splitlines()

native_undefined = {
    line.split()[-1]
    for line in symbols(["-u"], native_objects)
    if line.split()
}
strong_types = set("TDBRSGC")
rockbox_strong = set()
for line in symbols(["-g", "--defined-only"], rockbox_objects):
    fields = line.split()
    if len(fields) >= 2 and fields[-2] in strong_types:
        rockbox_strong.add(fields[-1])

collisions = sorted(native_undefined & rockbox_strong)
if collisions:
    print(
        "Refusing selector with native/Rockbox strong-symbol collisions: "
        + ", ".join(collisions),
        file=sys.stderr,
    )
    raise SystemExit(1)
PY
output="$work/compas_rockbox_bootloader"
temporary_output="$output.new"
trap 'rm -f "$temporary_output"' EXIT
"${cross_prefix}strip" -s -o "$temporary_output" "$build_dir/$binary"
"${cross_prefix}readelf" -h -l -A "$temporary_output" > "$work/elf-info.txt"
python3 - "$temporary_output" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as image:
    header = image.read(52)
if len(header) != 52 or header[:7] != b"\x7fELF\x01\x01\x01":
    raise SystemExit("Expected ELF32 little-endian selector")
kind, machine = struct.unpack_from("<HH", header, 16)
flags, = struct.unpack_from("<I", header, 36)
if kind != 2 or machine != 8 or flags & 0xf0000000 != 0x70000000 or flags & 0xf000 != 0x1000:
    raise SystemExit("Expected a MIPS32r2 O32 executable selector")
PY
if grep -q INTERP "$work/elf-info.txt"; then
    echo "Refusing a selector that requires a dynamic interpreter" >&2
    exit 1
fi
mv -f "$temporary_output" "$output"
echo "Built combined Compás/Rockbox bootloader: $output"
