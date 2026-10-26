#!/usr/bin/env bash
# Repack a supported HiBy firmware image with freshly built player binaries.
# The base remains external because it contains stock HiBy firmware assets.

set -euo pipefail

usage() {
    echo "Usage: $0 [--board r1|r3proii|r3ii_2025] [--vendor-drivers VENDOR_UPT] [--early-splash EARLY_SPLASH_BINARY] BASE_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_UPT" >&2
    echo "       BOARD=r3proii $0 BASE_UPT PLAYER_BINARY BOOTLOADER_BINARY OUTPUT_UPT" >&2
    exit 2
}

patch_bootloader_wrapper() {
    local wrapper=$1
    if grep -Eq '^[[:space:]]*(exec[[:space:]]+)?/usr/bin/(open_hiby_bootloader|compas_bootloader|compas_loader)[[:space:]]*$' \
        "$wrapper"; then
        sed -i -E 's#^([[:space:]]*)(exec[[:space:]]+)?/usr/bin/(open_hiby_bootloader|compas_bootloader)([[:space:]]*)$#\1\2/usr/bin/compas_loader\4#' \
            "$wrapper"
    else
        grep -Eq '^[[:space:]]*(exec[[:space:]]+)?/usr/bin/hiby_player[[:space:]]*$' \
            "$wrapper" || return 1
        sed -i -E 's#^([[:space:]]*)(exec[[:space:]]+)?/usr/bin/hiby_player([[:space:]]*)$#\1\2/usr/bin/compas_loader\3#' \
            "$wrapper"
    fi
    grep -Eq '^[[:space:]]*(exec[[:space:]]+)?/usr/bin/compas_loader[[:space:]]*$' "$wrapper" &&
        ! grep -Eq '^[[:space:]]*(exec[[:space:]]+)?/usr/bin/(open_hiby_bootloader|compas_bootloader)[[:space:]]*$' "$wrapper"
}

# Raise the touch controller to two simultaneous contacts (R1 only). The stock
# loader passes a limit of one, so the kernel never reports a second finger.
# Fail closed if the loader no longer has the expected shape. Already-2 is
# accepted so the edit is idempotent.
raise_touch_contact_limit() {
    local driver_dir=$1
    python3 - "$driver_dir/cst8xx_touch.sh" cst8xx_touch.ko cst_max_touch_number <<'PY'
import pathlib, re, sys

path, module, key = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
if path.is_symlink() or not path.is_file():
    raise SystemExit(f"Touch loader is missing or not a regular file: {path}")
text = path.read_text()
if len(re.findall(r"(?:^|\s)insmod\s+" + re.escape(module) + r"(?=\s|$)", text)) != 1:
    raise SystemExit(f"Unsupported touch loader (expected one insmod {module}): {path}")
pattern = r"((?:^|\s)" + key + r"=)([0-9]+)(?=\s|$)"
if len(re.findall(pattern, text)) != 1:
    raise SystemExit(f"Missing or duplicate {key} in {path}")
text, count = re.subn(pattern, r"\g<1>2", text)
if count != 1:
    raise SystemExit(f"Could not set {key}=2 in {path}")
path.write_text(text)
PY
}

# Production R1 packages must retain HiBy's closed cywdhd Wi-Fi stack. A
# candidate built with the experimental brcmfmac backend is packaged separately
# and must never pass through this general production repacker.
require_closed_r1_wifi() {
    local root=$1
    python3 - "$root" <<'PY'
import os, pathlib, re, sys

root = pathlib.Path(sys.argv[1])
module_driver = root / "module_driver"
if module_driver.is_symlink() or not module_driver.is_dir():
    raise SystemExit("Closed R1 Wi-Fi gate: /module_driver is missing or unsafe")

def regular(rel, label):
    path = root / rel
    if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
        raise SystemExit(f"Closed R1 Wi-Fi gate: {label} is missing or not a regular non-empty file: /{rel}")
    return path

regular("module_driver/cywdhd.ko", "vendor cywdhd module")
loader = regular("module_driver/cywdhd.sh", "vendor cywdhd loader")
init = regular("module_driver/driver_default_init_script.sh", "vendor module startup script")

def active_text(path):
    # Ignore full-line shell comments so documentation cannot satisfy a load check.
    return "\n".join(line for line in path.read_text(errors="replace").splitlines()
                     if not re.match(r"^\s*#", line))

loader_text = active_text(loader)
init_text = active_text(init)
if len(re.findall(r"(?:^|\s)insmod\s+cywdhd\.ko(?=\s|$)", loader_text)) != 1:
    raise SystemExit("Closed R1 Wi-Fi gate: cywdhd.sh must load cywdhd.ko exactly once")
if len(re.findall(r"(?:^|\s)sh\s+cywdhd\.sh(?=\s|$)", init_text)) != 1:
    raise SystemExit("Closed R1 Wi-Fi gate: driver startup must invoke cywdhd.sh exactly once")
if re.search(r"(?:^|\s)sh\s+bcm_wlbt_power\.sh(?=\s|$)", init_text):
    raise SystemExit("Closed R1 Wi-Fi gate: driver startup invokes the experimental bcm_wlbt_power helper")

for rel in (
    "usr/bin/wifi_on.sh",
    "usr/bin/wifi_off.sh",
    "usr/bin/wifi_up.sh",
    "usr/bin/wifi_down.sh",
    "usr/bin/bt_init",
    "usr/bin/bt_resume",
    "usr/bin/bt_suspend",
    "usr/bin/bt_enable_bsa.sh",
):
    path = root / rel
    if path.is_file() or path.is_symlink():
        target_is_compas_radio = path.is_symlink() and path.resolve().name == "compas-radio"
        if target_is_compas_radio or (path.is_file() and re.search(
            r"(?<![A-Za-z0-9_.-])compas-radio(?![A-Za-z0-9_.-])", active_text(path)
        )):
            raise SystemExit(f"Closed R1 Wi-Fi gate: active compas-radio reference is present: /{rel}")

for rel in (
    "usr/bin/compas-radio",
    "usr/lib/compas-radio",
    "usr/libexec/compas/radio-bt",
    "usr/libexec/compas/wifi_on.brcmfmac.vendor.sh",
    "usr/libexec/compas/wifi_off.brcmfmac.vendor.sh",
):
    path = root / rel
    if path.exists() or path.is_symlink():
        raise SystemExit(f"Closed R1 Wi-Fi gate: experimental radio helper is present: /{rel}")

for parent, dirs, files in os.walk(root, followlinks=False):
    for name in dirs + files:
        if name in {"brcmfmac.ko", "brcmutil.ko", "bcm_wlbt_power.ko", "bcm_wlbt_power.sh"}:
            path = pathlib.Path(parent) / name
            raise SystemExit(f"Closed R1 Wi-Fi gate: experimental module/helper is present: /{path.relative_to(root)}")
PY
}

# R1 runtime replacements are opt-in and must not replace binaries selected
# explicitly by the repack command. The ordinary release path still consumes
# only the tracked firmware/overlay tree.
validate_r1_runtime_overlay() {
    r1_runtime_overlay=
    [[ -n ${R1_RUNTIME_OVERLAY:-} ]] || return 0
    [[ $board == r1 ]] || {
        echo 'R1_RUNTIME_OVERLAY is only supported with --board r1' >&2
        return 1
    }
    [[ -d $R1_RUNTIME_OVERLAY && ! -L $R1_RUNTIME_OVERLAY ]] || {
        echo "R1_RUNTIME_OVERLAY must be a real directory: $R1_RUNTIME_OVERLAY" >&2
        return 1
    }
    r1_runtime_overlay=$(realpath "$R1_RUNTIME_OVERLAY")
    for rel in usr usr/bin; do
        if [[ -L $r1_runtime_overlay/$rel ]]; then
            echo "R1_RUNTIME_OVERLAY may not symlink the selected binary parent /$rel" >&2
            return 1
        fi
    done
    for rel in \
        usr/bin/compas_player \
        usr/bin/open_hiby_player \
        usr/bin/compas_loader \
        usr/bin/open_hiby_bootloader \
        usr/bin/compas_bootloader; do
        if [[ -e $r1_runtime_overlay/$rel || -L $r1_runtime_overlay/$rel ]]; then
            echo "R1_RUNTIME_OVERLAY may not provide the selected binary /$rel" >&2
            return 1
        fi
    done
}

apply_r1_runtime_overlay() {
    local root=$1
    [[ $board == r1 && -n ${r1_runtime_overlay:-} ]] || return 0
    cp -a "$r1_runtime_overlay"/. "$root/"
}

# R3 Pro II: install the open gt9xx_touch.ko (five contacts) in place of the
# OEM one, which drops multi-contact frames, and load it with
# gtp_max_touch_number=5. Everything is pinned in
# firmware/kernel/open-modules/r3proii.json: the module, the OEM module and
# loader being replaced, and the kernel the module was built against. The
# module has no modversions, so only the kernel build can guarantee it matches;
# an unlisted kernel is refused. Also writes the record (it names the GPL
# source commit) into the image. Mode "install" edits the tree; mode "verify"
# only re-checks the installed result, for use right before packaging.
open_touch_module() {
    local mode=$1 root=$2 kernel=$3
    python3 - "$root" "$kernel" "$repo/firmware/kernel/open-modules/r3proii.json" "$repo" "$mode" <<'PY'
import hashlib, json, pathlib, re, shutil, sys

root, kernel, record_path, repo = map(pathlib.Path, sys.argv[1:5])
mode = sys.argv[5]

def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def regular(path, label):
    if path.is_symlink() or not path.is_file():
        raise SystemExit(f"{label} is missing or not a regular file: {path}")

regular(record_path, "Open module record")
record = json.loads(record_path.read_text())
entry = record["modules"]["gt9xx_touch.ko"]
built = record["built_against"]
source = repo / entry["path"]
regular(source, "Open gt9xx_touch.ko")
if sha(source) != entry["sha256"]:
    raise SystemExit("Open gt9xx_touch.ko does not match its recorded SHA-256")
regular(kernel, "Kernel image")
if sha(kernel) not in built["kernel_xImage_sha256_allowed"]:
    raise SystemExit("Base kernel is not one the open gt9xx_touch.ko was built against; rebuild it for this kernel and update the record")

drivers = root / "module_driver"
if drivers.is_symlink() or not drivers.is_dir():
    raise SystemExit("Base OTA has an unsafe or missing /module_driver directory")
module, loader, utils = drivers / "gt9xx_touch.ko", drivers / "gt9xx_touch.sh", drivers / "utils.ko"
for path in (module, loader, utils):
    regular(path, "Touch driver file")

# The module has no modversions, so utils.ko must be exactly the build whose
# exports were inspected. The symbol and vermagic checks below repeat what the
# pin already implies, as documentation of what the open module needs.
pinned_utils = built.get("utils_ko_sha256_inspected")
if not pinned_utils:
    raise SystemExit("Open module record has no utils_ko_sha256_inspected")
if sha(utils) != pinned_utils:
    raise SystemExit("utils.ko is not the build the open gt9xx_touch.ko was checked against")
utils_bytes = utils.read_bytes()
for symbol in entry["imports_from_utils"]:
    if b"\0__ksymtab_" + symbol.encode() + b"\0" not in utils_bytes:
        raise SystemExit(f"utils.ko does not export {symbol} needed by the open gt9xx_touch.ko")
def vermagic_of(data):
    found = re.findall(rb"(?:^|\0)vermagic=([^\0]*)\0", data)
    return found[0].decode().rstrip() if len(found) == 1 else None  # the kernel pads it with a space
if vermagic_of(source.read_bytes()) != entry["vermagic"] or vermagic_of(utils_bytes) != entry["vermagic"]:
    raise SystemExit("utils.ko and the open gt9xx_touch.ko do not share the recorded vermagic")

loader_text = loader.read_text()
loader_sha = sha(loader)
if mode == "verify":
    if sha(module) != entry["sha256"] or loader_sha != entry["installed_loader_sha256"]:
        raise SystemExit("Touch driver files changed after the open module was installed")
    installed_record = root / "usr/share/compas/open-modules.json"
    regular(installed_record, "Installed open module record")
    if installed_record.read_bytes() != record_path.read_bytes():
        raise SystemExit("Installed open module record differs from firmware/kernel/open-modules/r3proii.json")
    raise SystemExit(0)
if sha(module) == entry["sha256"] and loader_sha == entry["installed_loader_sha256"]:
    pass  # already installed
elif sha(module) == entry["replaces_oem_sha256"] and loader_sha == entry["oem_loader_sha256"]:
    pattern = r"((?:^|\s)gtp_max_touch_number=)1(?=\s|$)"
    loader_text, count = re.subn(pattern, r"\g<1>5", loader_text)
    if count != 1:
        raise SystemExit(f"Could not set gtp_max_touch_number=5 in {loader}")
    loader.write_text(loader_text)
    if sha(loader) != entry["installed_loader_sha256"]:
        raise SystemExit("Edited gt9xx_touch.sh does not match the recorded loader")
    shutil.copyfile(source, module)
else:
    raise SystemExit("Base gt9xx_touch.ko or gt9xx_touch.sh is not the expected OEM pair")
if sha(module) != entry["sha256"]:
    raise SystemExit("Installed gt9xx_touch.ko hash mismatch")

target = root / "usr/share/compas"
for parent in (root / "usr", root / "usr/share", target):
    if parent.exists() and (parent.is_symlink() or not parent.is_dir()):
        raise SystemExit(f"Unsafe directory in base OTA: {parent.relative_to(root)}")
target.mkdir(parents=True, exist_ok=True)
out = target / "open-modules.json"
if out.is_symlink():
    raise SystemExit("Open module record target is an unexpected symlink")
shutil.copyfile(record_path, out)
PY
}

board=${BOARD:-r1}
vendor_drivers=""
early_splash=""
board_seen=0
while [[ ${1:-} == --board || ${1:-} == --vendor-drivers || ${1:-} == --early-splash ]]; do
    case $1 in
        --board)
            [[ $# -ge 2 && -n $2 && $board_seen == 0 ]] || usage
            board_seen=1
            board=$2
            shift 2
            ;;
        --vendor-drivers)
            [[ $# -ge 2 && -n $2 ]] || usage
            [[ -z $vendor_drivers ]] || { echo "--vendor-drivers may be specified only once" >&2; exit 2; }
            vendor_drivers=$2
            shift 2
            ;;
        --early-splash)
            [[ $# -ge 2 && -n $2 && -z $early_splash ]] || usage
            early_splash=$2
            shift 2
            ;;
    esac
done
case $board in
    r1) ;;
    r3proii) board_name="R3 Pro II"; board_device=R3PROII ;;
    r3ii_2025) board_name="R3II 2025"; board_device=R3II_2025 ;;
    *)
        echo "Unsupported board '$board' (expected r1, r3proii or r3ii_2025)" >&2
        exit 2
        ;;
esac
[[ $# -eq 4 ]] || usage

validate_r1_runtime_overlay || exit 1

repo="$(cd "$(dirname "$0")/../../.." && pwd)"

case $board in
    r1) [[ -n $early_splash ]] || early_splash="$repo/compas_early_splash" ;;
    r3proii) [[ -n $early_splash ]] || early_splash="$repo/compas_early_splash_r3proii" ;;
    r3ii_2025) [[ -n $early_splash ]] || early_splash="$repo/compas_early_splash_r3ii_2025" ;;
esac

base_upt=$(realpath "$1")
player=$(realpath "$2")
bootloader=$(realpath "$3")
output=$(realpath -m "$4")
if [[ -n $vendor_drivers ]]; then
    vendor_drivers=$(realpath "$vendor_drivers")
fi

for file in "$base_upt" "$player" "$bootloader" "$early_splash"; do
    [[ -s "$file" ]] || { echo "Missing or empty input: $file" >&2; exit 1; }
done

# The helper executes directly from init. Require a little-endian, 32-bit,
# statically linked MIPS executable; never package a host or dynamic binary.
python3 - "$early_splash" <<'PY'
import pathlib, struct, sys
data = pathlib.Path(sys.argv[1]).read_bytes()
if len(data) < 52 or data[:4] != b"\x7fELF":
    raise SystemExit("Early splash helper is not an ELF executable")
if data[4:7] != bytes((1, 1, 1)):
    raise SystemExit("Early splash helper must be ELF32, little-endian, current version")
etype, machine = struct.unpack_from("<HH", data, 16)
version = struct.unpack_from("<I", data, 20)[0]
flags = struct.unpack_from("<I", data, 36)[0]
phoff = struct.unpack_from("<I", data, 28)[0]
phentsize, phnum = struct.unpack_from("<HH", data, 42)
abi = flags & 0x0000F000
arch = flags & 0xF0000000
if (etype != 2 or machine != 8 or version != 1 or
        (flags & 0x00000020) != 0 or (abi not in (0, 0x00001000)) or
        (arch not in (0, 0x10000000, 0x50000000, 0x70000000)) or
        (flags & 0x0F000000) != 0):
    raise SystemExit("Early splash helper must use the supported MIPS O32 ABI and ISA")
if phnum == 0 or phentsize != 32 or phoff + phentsize * phnum > len(data):
    raise SystemExit("Early splash helper must be a valid MIPS ET_EXEC executable")
has_load = False
for index in range(phnum):
    start = phoff + index * phentsize
    p_type, p_offset, _, _, p_filesz, p_memsz = struct.unpack_from("<6I", data, start)
    if p_offset + p_filesz > len(data):
        raise SystemExit("Early splash helper has an out-of-bounds program segment")
    if p_type == 1:
        has_load = True
        if p_filesz > p_memsz:
            raise SystemExit("Early splash helper has an invalid PT_LOAD size")
    if p_type in (2, 3):  # PT_DYNAMIC or PT_INTERP
        raise SystemExit("Early splash helper must be statically linked")
if not has_load:
    raise SystemExit("Early splash helper has no PT_LOAD segment")
PY

for command in 7z unsquashfs mksquashfs genisoimage md5sum sha256sum split file; do
    command -v "$command" >/dev/null || {
        echo "Required command is unavailable: $command" >&2
        exit 1
    }
done

vendor_version=""
vendor_sha256=""
vendor_release_date=""
vendor_source_url=""
vendor_archive_url=""
if [[ -n $vendor_drivers ]]; then
    command -v python3 >/dev/null || {
        echo "Required command is unavailable: python3 (needed for --vendor-drivers)" >&2
        exit 1
    }
    manifest="$repo/firmware/vendor_drivers.json"
    [[ -s $manifest ]] || { echo "Missing vendor-driver pin manifest: $manifest" >&2; exit 1; }
    vendor_meta=$(python3 - "$manifest" "$board" "$vendor_drivers" <<'PY'
import hashlib, json, pathlib, sys
manifest_path, board, archive = sys.argv[1:]
try:
    data = json.loads(pathlib.Path(manifest_path).read_text())
    entry = data["boards"][board]
    version = entry["version"]
    expected = entry["sha256"].lower()
    release_date = entry["release_date"]
    source_url = entry["source_url"]
    archive_url = entry["archive_url"]
except (OSError, KeyError, TypeError, ValueError) as exc:
    raise SystemExit(f"No approved vendor-driver UPT pin for {board}: {exc}")
if not isinstance(version, str) or not version or len(expected) != 64 or any(c not in "0123456789abcdef" for c in expected):
    raise SystemExit(f"Invalid vendor-driver pin for {board}")
archive_sha = entry.get("archive_sha256")
if archive_sha is not None and (not isinstance(archive_sha, str) or len(archive_sha) != 64 or any(c not in "0123456789abcdef" for c in archive_sha)):
    raise SystemExit(f"Invalid outer archive SHA-256 record for {board}")
h = hashlib.sha256()
with pathlib.Path(archive).open("rb") as f:
    for block in iter(lambda: f.read(1024 * 1024), b""):
        h.update(block)
actual = h.hexdigest()
if actual != expected:
    raise SystemExit(f"Vendor-driver UPT SHA-256 mismatch for {board}: expected {expected}, got {actual}")
print("\t".join((version, expected, release_date, source_url, archive_url)))
PY
    ) || exit 1
    IFS=$'\t' read -r vendor_version vendor_sha256 vendor_release_date vendor_source_url vendor_archive_url <<< "$vendor_meta"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/iso" "$work/new-iso/ota_v0" "$(dirname "$output")"

7z x -y "$base_upt" -o"$work/iso" >/dev/null
ota_dir="$work/iso/ota_v0"
[[ -d "$ota_dir" ]] || { echo "Base OTA has no ota_v0 directory" >&2; exit 1; }

mapfile -t root_chunks < <(find "$ota_dir" -maxdepth 1 -type f \
    -name 'rootfs.squashfs.[0-9]*' -print | LC_ALL=C sort)
mapfile -t kernel_chunks < <(find "$ota_dir" -maxdepth 1 -type f \
    -name 'xImage.[0-9]*' -print | LC_ALL=C sort)
(( ${#root_chunks[@]} > 0 )) || { echo "Base OTA has no rootfs chunks" >&2; exit 1; }
(( ${#kernel_chunks[@]} > 0 )) || { echo "Base OTA has no xImage chunks" >&2; exit 1; }

cat "${root_chunks[@]}" > "$work/rootfs.squashfs"
cat "${kernel_chunks[@]}" > "$work/xImage"
unsquashfs -no-xattrs -d "$work/root" "$work/rootfs.squashfs" >/dev/null

# Board identity comes from the firmware's device config ("device":"R1",
# "R3PROII" or "R3II_2025"), not the stock player binary, which base images no
# longer ship.
if [[ $board != r1 ]]; then
    board_config="$work/root/usr/resource/config.json"
    if [[ ! -s "$board_config" ]] ||
        ! grep -Eq "\"device\"[[:space:]]*:[[:space:]]*\"$board_device\"" "$board_config"; then
        echo "Base OTA is not identified as an $board_name; refusing a cross-board image" >&2
        exit 1
    fi
fi

vendor_root=""
vendor_provenance=""
vendor_kernel_sha256=""
vendor_driver_manifest=""
if [[ -n $vendor_drivers ]]; then
    # This optional path is deliberately tied to an approved board/version
    # hash in firmware/vendor_drivers.json. The ordinary four-argument flow
    # above remains unchanged when --vendor-drivers is omitted.
    vendor_root="$work/vendor-root"
    vendor_iso="$work/vendor-iso"
    vendor_images="$work/vendor-images"
    mkdir -p "$vendor_iso" "$vendor_images"

    7z l -slt "$vendor_drivers" > "$work/vendor-list.slt"
    python3 - "$work/vendor-list.slt" <<'PY'
import pathlib, re, sys

text = pathlib.Path(sys.argv[1]).read_text(errors="replace")
# 7-Zip prints one dashed separator before the first entry, then separates
# subsequent entries with blank lines.
blocks = re.split(r"\n\s*\n", text)
allowed = re.compile(
    r"(?:ota_config\.in|ota_v0|ota_v0/(?:ota_update\.in|ota_v0\.ok|"
    r"ota_md5_(?:rootfs\.squashfs|xImage)\.[0-9a-f]{32}|"
    r"(?:rootfs\.squashfs|xImage)\.[0-9]{4}\.[0-9a-f]{32}))$"
)
seen = set()
for block in blocks:
    fields = {}
    for line in block.splitlines():
        if " = " in line:
            key, value = line.split(" = ", 1)
            fields[key] = value
    path = fields.get("Path")
    if not path or path.endswith(".upt") or fields.get("Type") == "Iso":
        continue
    candidate = pathlib.PurePosixPath(path)
    if candidate.is_absolute() or ".." in candidate.parts or not allowed.fullmatch(path):
        raise SystemExit(f"Unsafe or unsupported vendor UPT entry: {path}")
    if path in seen:
        raise SystemExit(f"Duplicate vendor UPT entry: {path}")
    seen.add(path)
    if fields.get("Symbolic Link") or fields.get("Hard Link"):
        raise SystemExit(f"Linked vendor UPT entry is not allowed: {path}")
required = {"ota_config.in", "ota_v0", "ota_v0/ota_update.in", "ota_v0/ota_v0.ok"}
if not required.issubset(seen):
    raise SystemExit("Vendor UPT is missing required OTA metadata")
PY
    7z x -y "$vendor_drivers" -o"$vendor_iso" >/dev/null

    python3 - "$vendor_iso/ota_v0" "$vendor_images" <<'PY'
import hashlib, pathlib, re, sys

ota = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
lines = (ota / "ota_update.in").read_text().splitlines()
images = {}
entry = {}
for line in lines + [""]:
    if line.startswith("img_type=") and entry.get("img_type"):
        image_type = entry["img_type"]
        if image_type in images:
            raise SystemExit(f"Duplicate image type: {image_type}")
        images[image_type] = entry
        entry = {}
    if not line.strip():
        if entry:
            if entry.get("img_type") in images:
                raise SystemExit(f"Duplicate image type: {entry['img_type']}")
            images[entry.get("img_type", "")] = entry
            entry = {}
        continue
    if "=" not in line:
        continue
    key, value = line.split("=", 1)
    entry[key.strip()] = value.strip()
for image_type, expected_name in (("kernel", "xImage"), ("rootfs", "rootfs.squashfs")):
    info = images.get(image_type)
    if not info or info.get("img_name") != expected_name:
        raise SystemExit(f"Vendor UPT has no supported {image_type} image")
    try:
        size = int(info["img_size"])
        image_md5 = info["img_md5"].lower()
    except (KeyError, ValueError):
        raise SystemExit(f"Invalid {image_type} size or MD5 metadata")
    if size <= 0 or not re.fullmatch(r"[0-9a-f]{32}", image_md5):
        raise SystemExit(f"Invalid {image_type} size or MD5 metadata")
    chains = list(ota.glob(f"ota_md5_{expected_name}.{image_md5}"))
    if len(chains) != 1:
        raise SystemExit(f"Missing or duplicate chunk chain for {expected_name}")
    chain = chains[0].read_text().splitlines()
    if not chain or any(not re.fullmatch(r"[0-9a-f]{32}", part) for part in chain):
        raise SystemExit(f"Invalid chunk hash list for {expected_name}")
    pattern = re.compile(re.escape(expected_name) + r"\.([0-9]{4})\.([0-9a-f]{32})$")
    chunks = []
    for path in ota.iterdir():
        match = pattern.fullmatch(path.name)
        if match:
            chunks.append((int(match.group(1)), match.group(2), path))
    chunks.sort()
    if len(chunks) != len(chain) or [x[0] for x in chunks] != list(range(len(chain))):
        raise SystemExit(f"Chunk count or numbering mismatch for {expected_name}")
    previous = image_md5
    digest = hashlib.md5()
    written = 0
    destination = out / expected_name
    with destination.open("wb") as target:
        for index, (number, link_md5, path) in enumerate(chunks):
            if link_md5 != previous:
                raise SystemExit(f"Broken previous-hash link in {path.name}")
            part_digest = hashlib.md5()
            part_size = 0
            with path.open("rb") as src:
                while True:
                    block = src.read(1024 * 1024)
                    if not block:
                        break
                    part_digest.update(block)
                    digest.update(block)
                    target.write(block)
                    part_size += len(block)
            actual_part = part_digest.hexdigest()
            if actual_part != chain[index]:
                raise SystemExit(f"Chunk MD5 mismatch for {path.name}")
            if index < len(chunks) - 1 and part_size != 512 * 1024:
                raise SystemExit(f"Unexpected non-final chunk size: {path.name}")
            if part_size <= 0 or part_size > 512 * 1024:
                raise SystemExit(f"Invalid chunk size: {path.name}")
            previous = actual_part
            written += part_size
    if written != size or digest.hexdigest() != image_md5:
        raise SystemExit(f"Reassembled {expected_name} size or MD5 mismatch")
PY

    vendor_rootfs="$vendor_images/rootfs.squashfs"
    vendor_kernel="$vendor_images/xImage"
    [[ -s $vendor_rootfs && -s $vendor_kernel ]] || {
        echo "Vendor UPT image payloads are missing" >&2
        exit 1
    }
    python3 - "$vendor_kernel" <<'PY'
import pathlib, struct, sys, zlib

data = pathlib.Path(sys.argv[1]).read_bytes()
if len(data) < 64:
    raise SystemExit("Vendor xImage is shorter than a uImage header")
magic, header_crc, timestamp, payload_size, load, entry, data_crc = struct.unpack(">7I", data[:28])
if magic != 0x27051956 or payload_size != len(data) - 64:
    raise SystemExit("Vendor xImage has invalid uImage magic or payload size")
header = bytearray(data[:64])
header[4:8] = b"\0\0\0\0"
if zlib.crc32(header) & 0xffffffff != header_crc:
    raise SystemExit("Vendor xImage header CRC mismatch")
if zlib.crc32(data[64:]) & 0xffffffff != data_crc:
    raise SystemExit("Vendor xImage data CRC mismatch")
if data[28:31] != bytes((5, 5, 2)):
    raise SystemExit("Vendor xImage is not a Linux/MIPS kernel image")
PY
    unsquashfs -no-xattrs -d "$vendor_root" "$vendor_rootfs" >/dev/null

    # With explicit vendor selection, both the source package and the base
    # must match the selected board. The regular R1-only path keeps its prior
    # behavior when this option is absent.
    python3 - "$work/root/usr/resource/config.json" "$vendor_root/usr/resource/config.json" "$board" "$vendor_version" <<'PY'
import json, pathlib, sys

base_path, vendor_path, board, version = sys.argv[1:]
expected_device = {"r1": "R1", "r3proii": "R3PROII", "r3ii_2025": "R3II_2025"}[board]
def product(path):
    data = json.loads(pathlib.Path(path).read_text())
    return next((item for item in data if item.get("type") == "product"), {})
for label, path in (("base", base_path), ("vendor", vendor_path)):
    try:
        item = product(path)
    except (OSError, ValueError) as exc:
        raise SystemExit(f"Cannot read {label} board identity: {exc}")
    if item.get("device") != expected_device:
        raise SystemExit(f"{label.capitalize()} OTA is not identified as {expected_device}")
    if label == "vendor" and item.get("version") != version:
        raise SystemExit(f"Vendor OTA version {item.get('version')} does not match approved {version}")
PY

    python3 - "$vendor_root" <<'PY'
import pathlib, sys

root = pathlib.Path(sys.argv[1]).resolve()
for rel in ("module_driver", "lib/firmware"):
    directory = root / rel
    parents = [root / "module_driver"] if rel == "module_driver" else [root / "lib", root / "lib/firmware"]
    if any(not item.is_dir() or item.is_symlink() for item in parents):
        raise SystemExit(f"Vendor OTA is missing required directory: /{rel}")
    count = 0
    for parent, dirs, files in __import__("os").walk(directory, followlinks=False):
        for name in dirs + files:
            path = pathlib.Path(parent) / name
            if not path.is_symlink():
                if path.is_file():
                    count += 1
                continue
            target = pathlib.Path(path.readlink())
            resolved = (root / target.relative_to("/")) if target.is_absolute() else (path.parent / target)
            try:
                resolved.resolve(strict=False).relative_to(root)
            except ValueError:
                raise SystemExit(f"Vendor symlink escapes root: {path.relative_to(root)} -> {target}")
    if count == 0:
        raise SystemExit(f"Vendor OTA directory is empty: /{rel}")
PY

    vendor_source_manifest="$work/vendor-source-files.json"
    python3 - "$vendor_root" "$vendor_source_manifest" <<'PY'
import hashlib, json, os, pathlib, sys

root, output = map(pathlib.Path, sys.argv[1:])
files = {}
for rel in ("module_driver", "lib/firmware"):
    directory = root / rel
    for parent, dirs, names in os.walk(directory, followlinks=False):
        dirs.sort(); names.sort()
        for name in dirs + names:
            path = pathlib.Path(parent) / name
            key = "/" + str(path.relative_to(root))
            if path.is_symlink():
                files[key] = {"symlink": os.readlink(path)}
            elif path.is_file():
                h = hashlib.sha256()
                with path.open("rb") as f:
                    for block in iter(lambda: f.read(1024 * 1024), b""):
                        h.update(block)
                files[key] = {"sha256": h.hexdigest(), "size": path.stat().st_size}
pathlib.Path(output).write_text(json.dumps(files, sort_keys=True, separators=(",", ":")) + "\n")
PY

    # Preserve the two supported R1 charging limits while bringing in the
    # newer kernel and its matching module tree. Fail closed if the staging
    # base or new vendor driver no longer exposes the expected parameters.
    preserved_charge_voltage=""
    preserved_charge_current=""
    if [[ $board == r1 ]]; then
        python3 - "$work/root/module_driver/axp2101.sh" "$vendor_root/module_driver/axp2101.sh" <<'PY'
import pathlib, re, sys

base, vendor = map(pathlib.Path, sys.argv[1:])
def values(path):
    text = path.read_text()
    if text.count("insmod axp2101.ko") != 1:
        raise SystemExit(f"Unsupported AXP2101 loader: {path}")
    found = {}
    for key in ("charge_voltage_limit", "charge_term_current"):
        matches = re.findall(r"(?:^|\s)" + key + r"=([0-9]+)(?=\s|$)", text)
        if len(matches) != 1:
            raise SystemExit(f"Missing or duplicate {key} in {path}")
        found[key] = matches[0]
    return text, found
base_text, base_values = values(base)
_, vendor_values = values(vendor)
if base_values != {"charge_voltage_limit": "4350", "charge_term_current": "70"}:
    raise SystemExit("R1 base no longer has the approved 4350mV/70mA charging limits")
vendor_text, _ = values(vendor)
for key, value in base_values.items():
    vendor_text, count = re.subn(r"((?:^|\s)" + key + r"=)[0-9]+(?=\s|$)", r"\g<1>" + value, vendor_text)
    if count != 1:
        raise SystemExit(f"Could not preserve supported {key} parameter")
vendor.write_text(vendor_text)
PY
        preserved_charge_voltage=4350
        preserved_charge_current=70
    fi

    # Allow two simultaneous fingers on the R1. Done after the stock source
    # manifest was taken and before the installed manifest, so the manifests
    # record the stock and the edited hash and the final integrity check still
    # matches. Only the R1 has vendor driver pins; the R3 touch policy below
    # is not wired into this path.
    [[ $board == r1 ]] || { echo "Vendor driver refresh is only supported for the R1" >&2; exit 1; }
    raise_touch_contact_limit "$vendor_root/module_driver"

    # Never write through existing destination links. These two directories
    # are the complete board-specific driver payload being replaced.
    for rel in module_driver lib/firmware; do
        destination="$work/root/$rel"
        if [[ $rel == lib/firmware ]]; then
            [[ -d "$work/root/lib" && ! -L "$work/root/lib" ]] || {
                echo "Base OTA has an unsafe or missing /lib directory" >&2
                exit 1
            }
        fi
        [[ -d $destination && ! -L $destination ]] || {
            echo "Base OTA has an unsafe or missing /$rel directory" >&2
            exit 1
        }
    done
    [[ -f "$work/root/module_driver/driver_default_init_script.sh" && ! -L "$work/root/module_driver/driver_default_init_script.sh" ]] || {
        echo "Base OTA is missing a regular vendor driver initialization script" >&2
        exit 1
    }
    previous_driver_init_sha256=$(sha256sum "$work/root/module_driver/driver_default_init_script.sh" | awk '{print $1}')
    install -m 0644 "$vendor_kernel" "$work/xImage"
    rm -rf "$work/root/module_driver" "$work/root/lib/firmware"
    cp -a "$vendor_root/module_driver" "$work/root/module_driver"
    cp -a "$vendor_root/lib/firmware" "$work/root/lib/firmware"
    vendor_kernel_sha256=$(sha256sum "$work/xImage" | awk '{print $1}')
    vendor_driver_manifest="$work/vendor-driver-files.json"
    python3 - "$work/root" "$vendor_source_manifest" "$vendor_driver_manifest" "$previous_driver_init_sha256" <<'PY'
import hashlib, json, os, pathlib, sys

root, source_manifest, output, previous_driver_init_sha256 = sys.argv[1:]
root = pathlib.Path(root)
files = {}
for rel in ("module_driver", "lib/firmware"):
    directory = root / rel
    for parent, dirs, names in os.walk(directory, followlinks=False):
        dirs.sort(); names.sort()
        for name in dirs + names:
            path = pathlib.Path(parent) / name
            key = "/" + str(path.relative_to(root))
            if path.is_symlink():
                files[key] = {"symlink": os.readlink(path)}
            elif path.is_file():
                h = hashlib.sha256()
                with path.open("rb") as f:
                    for block in iter(lambda: f.read(1024 * 1024), b""):
                        h.update(block)
                files[key] = {"sha256": h.hexdigest(), "size": path.stat().st_size}
manifest = {"source_files": json.loads(pathlib.Path(source_manifest).read_text()), "installed_files": files, "previous_driver_init_sha256": previous_driver_init_sha256}
pathlib.Path(output).write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
PY
    vendor_provenance="$work/root/usr/share/compas/vendor-driver-provenance.json"
fi

wrapper="$work/root/usr/bin/hiby_player.sh"
if [[ ! -f "$wrapper" ]]; then
    echo "Base OTA is missing /usr/bin/hiby_player.sh" >&2
    exit 1
fi

if [[ $board == r1 ]]; then
    # An approved R1 Staging Image must already contain the bootloader handoff.
    # Refuse an older public beta instead of quietly producing a firmware that
    # bypasses the boot menu after the new bootloader binary is copied in.
    grep -Eq '^[[:space:]]*(exec[[:space:]]+)?/usr/bin/(open_hiby_bootloader|compas_bootloader|compas_loader)[[:space:]]*$' "$wrapper" || {
        echo "Base OTA is not an approved R1 Staging Image (bootloader wrapper missing)" >&2
        exit 1
    }
fi
if ! patch_bootloader_wrapper "$wrapper"; then
    echo "Base OTA has no supported standalone launcher in /usr/bin/hiby_player.sh" >&2
    exit 1
fi

# A previous Compás image may have left the old standalone name behind.
# Remove it before installing the renamed player so the update has one
# unambiguous standalone binary and does not waste space in the rootfs.
rm -f "$work/root/usr/bin/open_hiby_player"
install -m 0755 "$player" "$work/root/usr/bin/compas_player"

# The R1 stock boot scripts start the A2DP source daemon without the encoder
# arguments the player's default "auto" codec preference expects. Keep this
# established R1 fix; R3 stock uses a different bluealsa command line and its
# Bluetooth scripts are deliberately left untouched.
if [[ $board == r1 ]]; then
for bt_script in bt_init bt_resume; do
    bt_script_path="$work/root/usr/bin/$bt_script"
    [[ -f "$bt_script_path" ]] || {
        echo "Base OTA is missing /usr/bin/$bt_script" >&2
        exit 1
    }
    # The custom R1 radio backend serializes these entry points and invokes
    # the preserved Bluetooth startup script. Check and tune that script.
    bt_action=${bt_script#bt_}
    if grep -Fxq "exec /usr/bin/compas-radio run-bt $bt_action \"\$@\"" "$bt_script_path"; then
        cmp -s "$work/root/usr/bin/compas-radio" \
            "$repo/firmware/kernel/wifi-experimental/rootfs/usr/bin/compas-radio" || {
            echo "Base OTA has an unrecognized Compas radio backend" >&2
            exit 1
        }
        sh -n "$bt_script_path"
        bt_script_path="$work/root/usr/libexec/compas/radio-bt/$bt_script.vendor.sh"
        [[ -f "$bt_script_path" ]] || {
            echo "Base OTA is missing the delegated $bt_script startup script" >&2
            exit 1
        }
    fi
    grep -q 'bluealsad -p a2dp-source' "$bt_script_path" || {
        echo "Base OTA /usr/bin/$bt_script does not start bluealsad as an A2DP source" >&2
        exit 1
    }
    sed -i '/--all-codecs/!s|bluealsad -p a2dp-source|bluealsad -p a2dp-source --all-codecs|g' \
        "$bt_script_path"
    # Negotiate 44.1 kHz rather than BlueALSA's default pick of the highest
    # rate up to 48 kHz: resampling only happens when a track's rate differs
    # from the transport's, and CD-derived 44.1 kHz material is the bulk of a
    # typical music library.
    sed -i '/--a2dp-force-audio-cd/!s|bluealsad -p a2dp-source|bluealsad -p a2dp-source --a2dp-force-audio-cd|g' \
        "$bt_script_path"
    for bt_arg in --all-codecs --a2dp-force-audio-cd; do
        grep -q -- "$bt_arg" "$bt_script_path" || {
            echo "Failed to add $bt_arg to /usr/bin/$bt_script" >&2
            exit 1
        }
    done
    sh -n "$bt_script_path"
done
fi

repo="$(cd "$(dirname "$0")/../../.." && pwd)"

# These boot images are part of each board's identity. Require the exact
# tracked files and panel dimensions before building a firmware package.
require_boot_image() {
    local path=$1 type=$2 width=$3 height=$4 description
    git -C "$repo" ls-files --error-unmatch "$path" >/dev/null 2>&1 || {
        echo "Boot image is not tracked in git: $path" >&2
        exit 1
    }
    [[ -s "$repo/$path" ]] || { echo "Missing boot image: $path" >&2; exit 1; }
    description=$(file -b "$repo/$path")
    [[ $description == *"$type image data"* &&
       $description =~ (^|[^0-9])${width}[[:space:]]*x[[:space:]]*${height}([^0-9]|$) ]] || {
        echo "Wrong boot image format or size ($type ${width}x${height} required): $path" >&2
        exit 1
    }
}
if [[ $board == r1 ]]; then
    require_boot_image assets/theme2/boot_animation/en/0.jpg JPEG 480 800
    require_boot_image assets/theme2/boot_animation/en/0.png PNG 480 800
    require_boot_image assets/r1/etc/logo1.jpeg JPEG 480 800
elif [[ $board == r3ii_2025 ]]; then
    require_boot_image assets/r3ii_2025/theme2/boot_animation/en/0.jpg JPEG 320 480
    require_boot_image assets/r3ii_2025/theme2/boot_animation/en/0.png PNG 320 480
    require_boot_image assets/r3ii_2025/etc/logo1.jpeg JPEG 320 480
else
    require_boot_image assets/r3proii/theme2/boot_animation/en/0.jpg JPEG 480 720
    require_boot_image assets/r3proii/theme2/boot_animation/en/0.png PNG 480 720
    require_boot_image assets/r3proii/etc/logo1.jpeg JPEG 480 720
fi

# UI assets and fonts we own, kept under assets/ in the tree the device itself
# uses: assets/theme2/<dir>/<file> lands at /usr/resource/litegui/theme2/, and
# assets/fonts/ at /usr/resource/fonts/. Copied after unpack so ours win over
# whatever the Staging Image already has -- or doesn't. Anything not copied
# here reaches the device only if the base image already carried it.
# Only tracked files are copied, so "tracked in git" and "shipped on the
# device" mean the same thing. That also keeps a contributor's own local
# stock-firmware dump (ignored, see .gitignore) out of the image -- stock
# assets already come from the base image.
copy_tracked_assets() {
    local src="$1" dest="$2" f rel
    [[ -d "$repo/$src" ]] || return 0
    while IFS= read -r f; do
        rel="${f#"$src"/}"
        mkdir -p "$dest/$(dirname "$rel")"
        cp -a "$repo/$f" "$dest/$rel"
    done < <(git -C "$repo" ls-files "$src")
}
copy_tracked_assets assets/theme1 "$work/root/usr/resource/litegui/theme1"
copy_tracked_assets assets/theme2 "$work/root/usr/resource/litegui/theme2"
# These designs are offered by the downloadable layout repository. Drop
# native copies after all board assets have been copied so older bases or
# board-specific assets cannot expose a second, firmware-bundled copy.
remove_downloadable_layout_assets() {
    local layout name
    for layout in vinyl Hibys HibysGraph; do
        for name in \
            "$layout.xml" "$layout.png" \
            "${layout}@320x480.xml" "${layout}_320x480.xml" \
            "${layout}@320x480.png" "${layout}_320x480.png" \
            "${layout}@480x720.xml" "${layout}_480x720.xml" \
            "${layout}@480x720.png" "${layout}_480x720.png"; do
            rm -f "$work/root/usr/resource/litegui/theme2/player_layouts/$name"
        done
    done
}
# Shared icons ship on every board. Panel-specific artwork and icon variants
# must win over the shared files, particularly the full-screen boot images.
if [[ $board == r1 ]]; then
    copy_tracked_assets assets/r1/etc "$work/root/etc"
elif [[ $board == r3ii_2025 ]]; then
    copy_tracked_assets assets/r3ii_2025/theme2 "$work/root/usr/resource/litegui/theme2"
    copy_tracked_assets assets/r3ii_2025/etc "$work/root/etc"
else
    copy_tracked_assets assets/r3proii/theme2 "$work/root/usr/resource/litegui/theme2"
    copy_tracked_assets assets/r3proii/etc "$work/root/etc"
fi
python3 "$repo/scripts/generators/fonts/generate_ui_fonts.py" --verify
copy_tracked_assets assets/fonts  "$work/root/usr/resource/fonts"
python3 "$repo/scripts/generators/fonts/generate_ui_fonts.py" --verify --font-directory "$work/root/usr/resource/fonts"

# Non-asset files we own that are not in stock, laid out as squashfs-root-
# relative paths under firmware/overlay/ (e.g. usr/share/udhcpc/
# default.script.d/ntpdate). cp -a keeps mode bits and relative symlinks
# (sync_ntp.sh).
# Preserve the vendor splash for validation before the shared overlay lands.
stock_splash="$work/root/etc/init.d/S11jpeg_display_shell"
[[ -f "$stock_splash" && ! -L "$stock_splash" ]] || {
    echo "Base OTA is missing a regular /etc/init.d/S11jpeg_display_shell" >&2
    exit 1
}
stock_splash_snapshot="$work/original-S11jpeg_display_shell"
cp -p "$stock_splash" "$stock_splash_snapshot"

# Preserve the reviewed radio startup before the shared vendor overlay lands.
radio_startup=
if [[ -e "$work/root/usr/bin/compas-radio" ]]; then
    [[ $board == r1 ]] || { echo "Compas radio backend is R1-only" >&2; exit 1; }
    python3 "$repo/scripts/kernel/radio/verification/verify_radio_boot.py" "$work/root"
    radio_startup="$work/radio-S43"
    cp -p "$work/root/etc/init.d/S43wifi_bcm_init_config" "$radio_startup"
fi
overlay="$repo/firmware/overlay"
if [[ -d "$overlay" ]]; then
    cp -a "$overlay"/. "$work/root/"
fi

if [[ -n $radio_startup ]]; then
    cp -p "$radio_startup" "$work/root/etc/init.d/S43wifi_bcm_init_config"
fi

# A local R3 runtime overlay is opt-in. CI supplies an already-upgraded base
# image and leaves this unset; never discover or source ignored scratch output
# implicitly. The overlay is copied before the release gates so local builds
# can be checked against the same BlueALSA/BlueZ contract as CI.
if [[ $board != r1 && -n ${R3_RUNTIME_OVERLAY:-} ]]; then
    [[ -d "$R3_RUNTIME_OVERLAY" ]] || {
        echo "R3_RUNTIME_OVERLAY is not a directory: $R3_RUNTIME_OVERLAY" >&2
        exit 1
    }
    runtime_overlay=$(realpath "$R3_RUNTIME_OVERLAY")
    cp -a "$runtime_overlay"/. "$work/root/"
fi

apply_r1_runtime_overlay "$work/root"

# Restore the actual vendor launcher before standardization. This prevents an
# overlay copy from becoming the input to the stock-script fail-closed check.
cp -p "$stock_splash_snapshot" "$stock_splash"

# Apply the same portable startup changes to every board after optional local
# runtime overlays, so those overlays cannot restore an unstandardized wrapper.
"$repo/scripts/firmware/boot_pipeline/standardize_boot_pipeline.sh" --board "$board" "$work/root"

if [[ $board == r1 ]]; then
    require_closed_r1_wifi "$work/root"
fi

# Touch policy for a base image that keeps its own driver tree. This is the
# last writer of the touched files: both overlays are already in place, and an
# overlay that carries one of them is refused instead of being overwritten or
# silently overwriting the edit. (--vendor-drivers applies the R1 edit inside
# the vendor refresh and is covered by its whole-tree check above.)
touch_files=()
touch_hash_r1=""
if [[ -z $vendor_drivers ]]; then
    [[ -d "$work/root/module_driver" && ! -L "$work/root/module_driver" ]] || {
        echo "Base OTA has an unsafe or missing /module_driver directory" >&2
        exit 1
    }
    case $board in
        r1) touch_files=(cst8xx_touch.sh) ;;
        r3proii) touch_files=(gt9xx_touch.ko gt9xx_touch.sh utils.ko) ;;
        # R3 II 2025 keeps its stock touch loader unchanged
        # (gtp_max_touch_number=1). Its OEM gt9xx_touch.ko drops multi-contact
        # frames, so raising the parameter would not give multitouch, and no
        # open module has been built or analyzed for that board.
        r3ii_2025) ;;
        *) echo "No touch policy for board $board" >&2; exit 1 ;;
    esac
    for overlay_dir in "$overlay" "${runtime_overlay:-}"; do
        [[ -n $overlay_dir ]] || continue
        for name in "${touch_files[@]}"; do
            if [[ -e "$overlay_dir/module_driver/$name" || -L "$overlay_dir/module_driver/$name" ]]; then
                echo "Overlay $overlay_dir carries module_driver/$name, which the touch policy owns" >&2
                exit 1
            fi
        done
    done
    case $board in
        r1)
            raise_touch_contact_limit "$work/root/module_driver"
            touch_hash_r1=$(sha256sum "$work/root/module_driver/cst8xx_touch.sh" | awk '{print $1}')
            ;;
        r3proii) open_touch_module install "$work/root" "$work/xImage" ;;
    esac
fi

if [[ -n $vendor_drivers ]]; then
    # Check the final tree after both overlays and boot standardization, then
    # write provenance from the exact bytes that will enter the squashfs.
    python3 - "$work/root" "$work/xImage" "$vendor_driver_manifest" \
        "$vendor_provenance" "$board" "$vendor_version" "$vendor_release_date" \
        "$vendor_sha256" "$vendor_kernel_sha256" "$vendor_source_url" "$vendor_archive_url" \
        "$preserved_charge_voltage" "$preserved_charge_current" "$repo/firmware/vendor_drivers.json" \
        "$vendor_root" "$repo/scripts/firmware/boot_pipeline/install_startup_trace.py" <<'PY'
import hashlib, json, os, pathlib, sys, tempfile
import runpy

(root, kernel, manifest_path, provenance_path, board, version, release_date,
 upt_sha, kernel_sha, source_url, archive_url, charge_voltage, charge_current, pin_path,
 vendor_root, trace_installer) = sys.argv[1:]
root, kernel = pathlib.Path(root), pathlib.Path(kernel)
vendor_root, trace_installer = pathlib.Path(vendor_root), pathlib.Path(trace_installer)
manifest = json.loads(pathlib.Path(manifest_path).read_text())

def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest(), path.stat().st_size

actual = {}
for rel in ("module_driver", "lib/firmware"):
    directory = root / rel
    parents = [root / "module_driver"] if rel == "module_driver" else [root / "lib", root / "lib/firmware"]
    if any(not item.is_dir() or item.is_symlink() for item in parents):
        raise SystemExit(f"Final OTA has an unsafe or missing /{rel}")
    for parent, dirs, names in os.walk(directory, followlinks=False):
        dirs.sort(); names.sort()
        for name in dirs + names:
            path = pathlib.Path(parent) / name
            key = "/" + str(path.relative_to(root))
            if path.is_symlink():
                actual[key] = {"symlink": os.readlink(path)}
            elif path.is_file():
                sha, size = digest(path)
                actual[key] = {"sha256": sha, "size": size}
expected = manifest["installed_files"].copy()
loader_rel = "/module_driver/driver_default_init_script.sh"
source_loader = vendor_root / loader_rel.lstrip("/")
installed_loader = root / loader_rel.lstrip("/")
source_sha, source_size = digest(source_loader)
installed_sha, installed_size = digest(installed_loader)
pinned_loader = expected.get(loader_rel)
if pinned_loader != {"sha256": source_sha, "size": source_size}:
    raise SystemExit("Pinned vendor module loader differs from its installed-files manifest")
trace_module = runpy.run_path(str(trace_installer))["instrument_modules"]
instrumented_loader = trace_module(source_loader.read_text()).encode()
derived_sha = hashlib.sha256(instrumented_loader).hexdigest()
derived_size = len(instrumented_loader)
if (installed_sha, installed_size) != (derived_sha, derived_size):
    raise SystemExit("Final module loader differs from validated startup-trace transformation")
expected[loader_rel] = {"sha256": derived_sha, "size": derived_size}
if actual != expected:
    raise SystemExit("Driver or firmware files changed after vendor refresh (overlay integrity check failed)")
actual_kernel, _ = digest(kernel)
if actual_kernel != kernel_sha:
    raise SystemExit("Kernel xImage changed after vendor refresh (overlay integrity check failed)")

provenance = {
    "schema_version": 1,
    "board": board,
    "version": version,
    "release_date": release_date,
    "source_url": source_url,
    "archive_url": archive_url,
    "upt_sha256": upt_sha,
    "kernel_xImage_sha256": kernel_sha,
    "source_files": manifest["source_files"],
    "installed_files": actual,
    "driver_init_policy": "vendor initialization sequence replaces base sequence and diagnostic wrappers",
    "previous_driver_init_sha256": manifest["previous_driver_init_sha256"],
    "startup_trace": {
        "module_loader_source_sha256": source_sha,
        "module_loader_source_size": source_size,
        "module_loader_installed_sha256": derived_sha,
        "module_loader_installed_size": derived_size,
    },
}
pin = json.loads(pathlib.Path(pin_path).read_text())["boards"][board]
if pin["sha256"] != upt_sha or pin["version"] != version:
    raise SystemExit("Vendor pin changed during packaging")
provenance.update({key: pin[key] for key in ("source_note", "release_announcement_url", "archive_sha256") if key in pin})
if charge_voltage or charge_current:
    provenance["preserved_parameters"] = {
        "module": "axp2101",
        "charge_voltage_limit": int(charge_voltage),
        "charge_term_current": int(charge_current),
    }
target = pathlib.Path(provenance_path)
if target.is_symlink():
    raise SystemExit("Final OTA provenance target is an unexpected symlink")
for parent in (root / "usr", root / "usr/share", root / "usr/share/compas"):
    if parent.exists() and (not parent.is_dir() or parent.is_symlink()):
        raise SystemExit(f"Unsafe provenance directory in final OTA: {parent.relative_to(root)}")
target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.NamedTemporaryFile("w", dir=target.parent, delete=False) as f:
    json.dump(provenance, f, sort_keys=True, indent=2)
    f.write("\n")
    temp = pathlib.Path(f.name)
temp.replace(target)
PY
fi

remove_downloadable_layout_assets

# Apply the handoff after overlays so they cannot restore an old launcher.
wrapper="$work/root/usr/bin/hiby_player.sh"
if ! patch_bootloader_wrapper "$wrapper"; then
    echo "Final OTA has no supported standalone launcher in /usr/bin/hiby_player.sh" >&2
    exit 1
fi

# Remove a stale base or overlay copy so the wrapper has one bootloader path.
rm -f "$work/root/usr/bin/open_hiby_bootloader" "$work/root/usr/bin/compas_bootloader"
install -m 0755 "$bootloader" "$work/root/usr/bin/compas_loader"
install -m 0755 "$early_splash" "$work/root/usr/bin/compas-early-splash"

# The player promises the Speex rate converter, which is built from the
# pinned, redistributable SpeexDSP/alsa-plugins sources by the base-image
# process. Do not silently publish a base image that falls back to the
# lower-quality alsa-lib converter: the approved R1 and R3 base images must
# already contain the plugin and its runtime library.
speex_plugin="$work/root/usr/lib/alsa-lib/libasound_module_rate_speexrate.so"
[[ -e "$speex_plugin" ]] || {
    echo "${board} base OTA is missing the Speex ALSA rate plugin required by the player" >&2
    exit 1
}
[[ -e "$work/root/usr/lib/libspeexdsp.so.1" ]] || {
    echo "${board} base OTA is missing the SpeexDSP runtime library required by the ALSA plugin" >&2
    exit 1
}

if [[ $board != r1 ]]; then
    # R3 releases must use the updated BlueALSA 5/BlueZ runtime. Stock R3
    # images contain only the legacy bluealsa daemon, so fail before packaging
    # unless CI's upgraded base or an explicitly selected local overlay has
    # supplied every player-facing runtime component.
    r3_runtime_paths=(
        /usr/bin/bluealsad
        /usr/bin/bluealsactl
        /usr/lib/alsa-lib/libasound_module_pcm_bluealsa.so
        /usr/lib/alsa-lib/libasound_module_ctl_bluealsa.so
        /usr/libexec/bluetooth/bluetoothd
    )
    for runtime_path in "${r3_runtime_paths[@]}"; do
        [[ -e "$work/root$runtime_path" ]] || {
            echo "$board_name runtime gate failed: missing $runtime_path (use an updated staging base or R3_RUNTIME_OVERLAY)" >&2
            exit 1
        }
    done
    for bt_script in bt_init bt_resume; do
        bt_script_path="$work/root/usr/bin/$bt_script"
        [[ -f "$bt_script_path" ]] || {
            echo "$board_name runtime gate failed: missing /usr/bin/$bt_script" >&2
            exit 1
        }
        grep -Eq '(^|[[:space:]/])bluealsad([[:space:]]|$)' "$bt_script_path" || {
            echo "$board_name runtime gate failed: /usr/bin/$bt_script does not invoke bluealsad" >&2
            exit 1
        }
        for bt_arg in --all-codecs --a2dp-force-audio-cd; do
            grep -Fq -- "$bt_arg" "$bt_script_path" || {
                echo "$board_name runtime gate failed: /usr/bin/$bt_script is missing $bt_arg" >&2
                exit 1
            }
        done
        sh -n "$bt_script_path"
    done
    # These scripts are optional across firmware revisions, but when present
    # they must manage the BlueALSA 5 daemon rather than the legacy daemon.
    for bt_script in bt_suspend bluealsa_profile; do
        bt_script_path="$work/root/usr/bin/$bt_script"
        [[ -f "$bt_script_path" ]] || continue
        # The R3II 2025's stock bt_suspend leaves the daemons running (its
        # kill lines are commented out); only a live legacy call is wrong.
        if [[ $board == r3ii_2025 ]] &&
            ! grep -Fq -- 'bluealsad' "$bt_script_path" &&
            ! grep -Eq '^[^#]*(^|[[:space:]/])bluealsa([[:space:]]|$)' "$bt_script_path"; then
            sh -n "$bt_script_path"
            continue
        fi
        grep -Fq -- 'bluealsad' "$bt_script_path" || {
            echo "$board_name runtime gate failed: /usr/bin/$bt_script does not manage bluealsad" >&2
            exit 1
        }
        sh -n "$bt_script_path"
    done
fi

# Keep the two board packages on the same known-good font set. The manifest
# contains hashes extracted from the approved R1 package and is checked after
# every stock asset and overlay has been applied.
font_manifest="$repo/scripts/firmware/fonts/r1_firmware_fonts.sha256"
[[ -s "$font_manifest" ]] || {
    echo "Missing font parity manifest: $font_manifest" >&2
    exit 1
}
if ! (cd "$work/root" && sha256sum -c "$font_manifest"); then
    echo "Firmware font parity check failed; R1 and R3 packages must use the same font files" >&2
    exit 1
fi

# Last look at the touch files before they are packed.
if [[ -z $vendor_drivers ]]; then
    case $board in
        r1)
            [[ $(sha256sum "$work/root/module_driver/cst8xx_touch.sh" | awk '{print $1}') == "$touch_hash_r1" ]] || {
                echo "cst8xx_touch.sh changed after the contact limit was raised" >&2
                exit 1
            }
            ;;
        r3proii) open_touch_module verify "$work/root" "$work/xImage" ;;
    esac
fi
mksquashfs "$work/root" "$work/new-rootfs.squashfs" \
    -comp lzo -all-root -noappend -no-xattrs >/dev/null

package_image() {
    local image=$1
    local name=$2
    local destination=$3
    local initial_md5 size chain previous part part_md5

    initial_md5=$(md5sum "$image" | awk '{print $1}')
    size=$(stat -c%s "$image")
    chain="$destination/ota_md5_${name}.${initial_md5}"
    : > "$chain"
    split -b 512k --numeric-suffixes=0 -a 4 "$image" "$destination/${name}."
    previous=$initial_md5
    for part in "$destination/${name}."[0-9]*; do
        part_md5=$(md5sum "$part" | awk '{print $1}')
        echo "$part_md5" >> "$chain"
        mv "$part" "$part.$previous"
        previous=$part_md5
    done

    printf '%s %s %s\n' "$name" "$size" "$initial_md5"
}

read -r root_name root_size root_md5 < <(
    package_image "$work/new-rootfs.squashfs" rootfs.squashfs "$work/new-iso/ota_v0"
)
read -r kernel_name kernel_size kernel_md5 < <(
    package_image "$work/xImage" xImage "$work/new-iso/ota_v0"
)

cat > "$work/new-iso/ota_v0/ota_update.in" <<EOF
ota_version=0
img_type=kernel
img_name=$kernel_name
img_size=$kernel_size
img_md5=$kernel_md5
img_type=rootfs
img_name=$root_name
img_size=$root_size
img_md5=$root_md5
EOF
: > "$work/new-iso/ota_v0/ota_v0.ok"
echo 'current_version=0' > "$work/new-iso/ota_config.in"

genisoimage -f -U -J -joliet-long -r -allow-lowercase -allow-multidot \
    -o "$output" "$work/new-iso" >/dev/null

# The device's update path has a practical 45 MiB firmware ceiling.
max_size=$((45 * 1024 * 1024))
actual_size=$(stat -c%s "$output")
if (( actual_size > max_size )); then
    echo "Repacked OTA is too large: $actual_size bytes (limit $max_size)" >&2
    exit 1
fi

echo "Created $output ($actual_size bytes)"
sha256sum "$output"
