#!/usr/bin/env bash
# Install shared Compás boot support and tune a firmware root while preserving
# its board-specific supervisor and player behavior.
set -euo pipefail

usage() {
    echo "Usage: $0 [--board r1|r3proii|r3ii_2025] SQUASHFS_ROOT" >&2
    exit 2
}

if [[ ${1:-} == --board ]]; then
    [[ $# -ge 3 ]] || usage
    board=$2
    shift 2
    case $board in r1|r3proii|r3ii_2025) ;; *) echo "Unsupported board '$board'" >&2; exit 2 ;; esac
fi
[[ $# -eq 1 ]] || usage
root=$(realpath "$1")
[[ -d $root && -d $root/usr/bin ]] || {
    echo "Invalid squashfs root (expected usr/bin): $root" >&2
    exit 1
}

repo=$(cd "$(dirname "$0")/../../.." && pwd)
overlay="$repo/firmware/overlay"
wrapper="$root/usr/bin/hiby_player.sh"
wifi="$root/usr/bin/wifi_on.sh"
vendor="$root/usr/libexec/compas/wifi_on.vendor.sh"
canonical_wifi="$overlay/usr/libexec/compas/wifi_on.sh"
[[ -f $wrapper ]] || { echo "Missing $wrapper" >&2; exit 1; }

shared_paths=(
    usr/bin/compas-boot-tuning
    usr/bin/compas-wifi-mac
    etc/init.d/S43wifi_bcm_init_config
    usr/libexec/compas/wifi_on.sh
    etc/init.d/S11jpeg_display_shell
    etc/init.d/S22compas_startup_image
)
for rel in "${shared_paths[@]}"; do
    [[ -f $overlay/$rel ]] || { echo "Missing shared boot file: $overlay/$rel" >&2; exit 1; }
done

is_our_wifi_wrapper() {
    local file=$1
    # The marker identifies current and future versions. Recognize the known
    # pre-marker wrapper by its vendor target plus MAC readiness gate so an
    # existing vendor copy does not prevent a safe upgrade.
    grep -Fq '# COMPAS_WIFI_ON_WRAPPER' "$file" || {
        grep -Fq '/usr/libexec/compas/wifi_on.vendor.sh' "$file" &&
            grep -Fq 'compas-wifi-mac' "$file" &&
            grep -Eq '(^|[[:space:]])wait([[:space:]]|$)' "$file"
    }
}

is_known_wifi_up_compat_shim() {
    local file=$1
    # The R1 staging image retains a compatibility entry point even
    # after its vendor implementation has been saved under /usr/libexec. It
    # is safe to coexist only in this exact, behavior-free forwarding form.
    [[ -f $file ]] && cmp -s "$file" <(printf '%s\n' \
        '#!/bin/sh' \
        'exec /usr/bin/wifi_up.sh "$@"')
}

# Validate all canonical scripts before preparing any destination changes.
for rel in "${shared_paths[@]}"; do
    sh -n "$overlay/$rel"
done

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

# Stage the stock splash deferral only for the known vendor script layout.
# Unknown vendor init scripts may encode board-specific setup and must not be
# silently replaced. The marker makes repeated standardization idempotent.
stock_splash="$root/etc/init.d/S11jpeg_display_shell"
[[ -f $stock_splash && ! -L $stock_splash ]] || {
    echo "Missing or non-regular stock splash script: $stock_splash" >&2
    exit 1
}
python3 - "$stock_splash" "$scratch/S11jpeg_display_shell" "$overlay/etc/init.d/S11jpeg_display_shell" <<'PY'
import hashlib
import pathlib, sys

source, destination, canonical = map(pathlib.Path, sys.argv[1:])
text = source.read_text(encoding="utf-8")
marker = "# COMPAS_STOCK_SPLASH_DEFERRED"
if marker in text:
    if text != canonical.read_text(encoding="utf-8"):
        raise SystemExit("Unrecognized Compás S11jpeg_display_shell variant")
elif hashlib.sha256(text.encode("utf-8")).hexdigest() == "a18ed58e3c4e8a2e772988c18a66cac183406709f666ac0a96f84b778936acea":
    # Exact known vendor script, observed identically on R1, R3 Pro II, and
    # R3 II 2025. Do not accept extra commands or altered control flow.
    pass
else:
    raise SystemExit("Unsupported S11jpeg_display_shell layout; refusing to replace unknown vendor script")
destination.write_text(canonical.read_text(encoding="utf-8"))
PY
chmod --reference="$stock_splash" "$scratch/S11jpeg_display_shell"
sh -n "$scratch/S11jpeg_display_shell"

# The guarded radio backend owns its Wi-Fi entry points and S43 lifecycle.
radio_backend=0
if [[ -e $root/usr/bin/compas-radio ]]; then
    [[ ${board:-r1} == r1 ]] || { echo "Compas radio backend is R1-only" >&2; exit 1; }
    python3 "$repo/scripts/kernel/radio/verification/verify_radio_boot.py" "$root"
    radio_backend=1
fi

if [[ $radio_backend == 0 ]]; then
# Resolve the vendor source without ever mistaking a Compás wrapper for stock.
if [[ -e $vendor ]]; then
    [[ -f $vendor ]] || { echo "Saved Wi-Fi vendor implementation is not a file: $vendor" >&2; exit 1; }
    if is_our_wifi_wrapper "$vendor"; then
        echo "Saved Wi-Fi vendor path contains a Compás wrapper, not a vendor implementation" >&2
        exit 1
    fi
    sh -n "$vendor" || { echo "Saved Wi-Fi vendor implementation is not valid shell: $vendor" >&2; exit 1; }
    if [[ -e $wifi ]] && ! is_our_wifi_wrapper "$wifi" && ! is_known_wifi_up_compat_shim "$wifi"; then
        echo "Unexpected /usr/bin/wifi_on.sh alongside saved vendor implementation" >&2
        exit 1
    fi
    cp -p "$vendor" "$scratch/wifi_on.vendor.sh"
elif [[ -f $wifi ]]; then
    if is_our_wifi_wrapper "$wifi"; then
        echo "Compás Wi-Fi wrapper exists but its vendor implementation is missing" >&2
        exit 1
    fi
    cp -p "$wifi" "$scratch/wifi_on.vendor.sh"
    sh -n "$scratch/wifi_on.vendor.sh" || {
        echo "Cannot safely preserve non-shell Wi-Fi implementation: $wifi" >&2
        exit 1
    }
else
    echo "Missing Wi-Fi vendor implementation: expected $wifi or $vendor" >&2
    exit 1
fi

# The Broadcom driver can reset wlan0 to its factory MAC during the vendor
# script's `ifconfig $INTERFACE up`. Validate/repair after that reset, before
# WPA or DHCP can start. Transform only the scratch copy and reject unknown
# vendor layouts before changing the squashfs root.
python3 - "$scratch/wifi_on.vendor.sh" <<'PY'
import pathlib, re, sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
lines = text.splitlines(keepends=True)
marker = "# COMPAS_WIFI_MAC_PRE_WPA_GUARD"
terminal_marker = "# COMPAS_WIFI_VENDOR_RETURN"
marker_count = sum(line.rstrip("\r\n") == marker for line in lines)
terminal_count = sum(line.rstrip("\r\n") == terminal_marker for line in lines)
anchors = [i for i, line in enumerate(lines) if line.strip() == "ifconfig $INTERFACE up"]
wpa_calls = [i for i, line in enumerate(lines) if re.match(r"^[ \t]*wpa_supplicant[ \t]", line)]
dhcp_calls = [i for i, line in enumerate(lines) if re.match(r"^[ \t]*udhcpc[ \t]", line)]
if marker_count > 1 or terminal_count > 1:
    raise SystemExit("Multiple Compás Wi-Fi post-UP guards in vendor script")
if len(anchors) != 1:
    raise SystemExit("Expected exactly one supported `ifconfig $INTERFACE up` in vendor Wi-Fi script")
anchor = anchors[0]
if len(wpa_calls) != 1 or wpa_calls[0] <= anchor:
    raise SystemExit("Expected one WPA invocation after vendor Wi-Fi interface UP")
if len(dhcp_calls) != 1 or dhcp_calls[0] <= wpa_calls[0]:
    raise SystemExit("Expected one DHCP invocation after vendor WPA startup")
guard = [
    marker + "\n",
    'if ! /usr/bin/compas-wifi-mac connection-prepare "${COMPAS_WIFI_CONNECTION_PID:-}"; then\n',
    '    echo "WiFi MAC verification failed; skipping WPA and DHCP" >&2\n',
    "    return 1\n",
    "fi\n",
]
if marker_count:
    current = lines[anchor + 1:anchor + 1 + len(guard)]
    legacy = guard.copy()
    legacy[3] = "    exit 1\n"
    if current == legacy:
        # Upgrade the former exec-based guard to a return-based sourced guard.
        lines[anchor + 1:anchor + 1 + len(legacy)] = guard
    elif current != guard:
        raise SystemExit("Unrecognized or misplaced Compás Wi-Fi post-UP guard")
else:
    lines[anchor + 1:anchor + 1] = guard
nonblank = [i for i, line in enumerate(lines) if line.strip()]
if terminal_count:
    if len(nonblank) < 2 or lines[nonblank[-2]].strip() != terminal_marker or lines[nonblank[-1]].strip() != "return 0":
        raise SystemExit("Unrecognized or misplaced Compás Wi-Fi sourced return")
else:
    exits = [i for i, line in enumerate(lines) if re.match(r"^[ \t]*exit(?:[ \t]|$)", line)]
    terminal_exits = [i for i, line in enumerate(lines) if line.strip() == "exit 0"]
    if len(terminal_exits) != 1 or len(nonblank) == 0 or terminal_exits[0] != nonblank[-1]:
        raise SystemExit("Expected one final `exit 0` in vendor Wi-Fi script")
    # The sourced guard returns failure to the wrapper; other vendor exits
    # would escape the wrapper and bypass its EXIT trap.
    if exits != terminal_exits:
        raise SystemExit("Unsupported nonterminal exit in vendor Wi-Fi script")
    i = terminal_exits[0]
    lines[i:i + 1] = [terminal_marker + "\n", "return 0\n"]
if any(re.match(r"^[ \t]*exit(?:[ \t]|$)", line) for line in lines):
    raise SystemExit("Unsupported exit statement in sourced vendor Wi-Fi script")
path.write_text("".join(lines))
PY
sh -n "$scratch/wifi_on.vendor.sh" || {
    echo "Post-UP Wi-Fi guard produced invalid vendor shell" >&2
    exit 1
}

fi

# Prepare and syntax-check the launcher transformation in scratch storage.
# This keeps unknown wrappers from causing any partial root mutation.
python3 - "$wrapper" "$scratch/hiby_player.sh" <<'PY'
import pathlib, re, sys

source, destination = map(pathlib.Path, sys.argv[1:])
text = source.read_text()
lines = text.splitlines(keepends=True)
markers = [i for i, line in enumerate(lines) if line.strip() == "# --- PERFORMANCE TWEAKS ---"]
if len(markers) > 1:
    raise SystemExit("Multiple inline performance blocks in hiby_player.sh")
if markers:
    i = markers[0]
    expected = [
        "# --- PERFORMANCE TWEAKS ---",
        "# Increase SD card read-ahead buffer",
        "if [ -e /sys/block/mmcblk0/queue/read_ahead_kb ]; then",
        "    echo 2048 > /sys/block/mmcblk0/queue/read_ahead_kb",
        "fi",
        "",
        "# Tune memory caching for responsiveness",
        "sysctl -w vm.vfs_cache_pressure=50",
        "",
    ]
    actual = [line.rstrip("\r\n") for line in lines[i:i + len(expected)]]
    if actual != expected:
        raise SystemExit("Unknown inline performance block in hiby_player.sh")
    del lines[i:i + len(expected)]
text = "".join(lines)
if re.search(r"(?:read_ahead_kb|vfs_cache_pressure)", text):
    raise SystemExit("Unsupported inline boot-tuning reference in hiby_player.sh")

launch_re = re.compile(r"^[ \t]*(?:exec[ \t]+)?/usr/bin/(?:hiby_player|open_hiby_bootloader|compas_bootloader|compas_loader)[ \t]*(?:\r?\n)?$")
launches = [i for i, line in enumerate(lines) if launch_re.match(line)]
if len(launches) != 1:
    raise SystemExit("Expected exactly one supported standalone launcher in hiby_player.sh")
tune_re = re.compile(r"^[ \t]*(?:/usr/bin/)?compas-boot-tuning[ \t]*(?:\r?\n)?$")
tunes = [i for i, line in enumerate(lines) if tune_re.match(line)]
if len(tunes) > 1:
    raise SystemExit("Multiple compas-boot-tuning calls in hiby_player.sh")
launcher = launches[0]
if tunes and tunes[0] >= launcher:
    raise SystemExit("compas-boot-tuning call must precede the standalone launcher")
if not tunes:
    lines.insert(launcher, "/usr/bin/compas-boot-tuning\n")
destination.write_text("".join(lines))
PY
chmod --reference="$wrapper" "$scratch/hiby_player.sh"
sh -n "$scratch/hiby_player.sh"

# The vendor mount_ubifs.sh erases userdata when ubiattach or the UBIFS mount
# fails, and a failed attach or mount is not proof that the flash is blank.
# Stage the no-erase failure paths in scratch storage; any unknown layout is
# rejected before the root is changed.
mount_ubifs="$root/usr/bin/mount_ubifs.sh"
[[ -f $mount_ubifs && ! -L $mount_ubifs ]] || {
    echo "Missing or non-regular userdata mount script: $mount_ubifs" >&2
    exit 1
}
python3 - "$mount_ubifs" "$scratch/mount_ubifs.sh" <<'PY'
import pathlib, re, sys

source, destination = map(pathlib.Path, sys.argv[1:])
text = source.read_text(encoding="utf-8")
lines = text.splitlines(keepends=True)
marker = "# COMPAS_USERDATA_NO_ERASE"
ubimkvol = "\tubimkvol /dev/$ubi_name -N user_data -m"
opts = ("noatime", "sync")
vendor_comment_1 = "\t# ubi的分区能识别但是mount不了的情况\n"
vendor_comment_2 = "\t# 擦除分区,重试一次\n"
attach_vendor = [
    "\tubiattach -m $num -d $ubi\n",
    "\tif [ $? != 0 ]; then\n",
    "\t\tflash_erase /dev/mtd$num 0 0\n",
    "\t\tubiattach -m $num -d $ubi\n",
    "\tfi\n",
]
attach_safe = [
    "\tubiattach -m $num -d $ubi\n",
    "\tif [ $? != 0 ]; then\n",
    "\t\t# COMPAS_USERDATA_NO_ERASE: an attach failure does not prove the flash is blank.\n",
    '\t\techo "<3>compas: userdata ubiattach failed for mtd$num; data not erased" 2>/dev/null >/dev/kmsg\n',
    '\t\terror_exit "error: ubiattach failed for mtd$num; userdata was not erased"\n',
    "\tfi\n",
]


def mount_vendor(opt):
    return [
        f"mount -o {opt} -t ubifs /dev/${{ubi_name}}_0 $mount_path\n",
        "if [ $? != 0 ]; then\n",
        '\tif [ "$try_count" != "" ]; then\n',
        '\t\terror_exit "mount ubi error again"\n',
        "\tfi\n",
        vendor_comment_1,
        vendor_comment_2,
        "\tflash_erase /dev/mtd$num 0 0\n",
        "\tubidetach -m $num\n",
        "\t$prg_name $partition_name $mount_path 1\n",
        "fi\n",
    ]


def mount_safe(opt):
    return [
        f"mount -o {opt} -t ubifs /dev/${{ubi_name}}_0 $mount_path\n",
        "if [ $? != 0 ]; then\n",
        "\t# COMPAS_USERDATA_NO_ERASE: a mount failure must never erase or detach user data.\n",
        '\techo "<3>compas: userdata mount failed for /dev/${ubi_name}_0; data not erased" 2>/dev/null >/dev/kmsg\n',
        '\terror_exit "error: ubifs mount failed for /dev/${ubi_name}_0; userdata was not erased"\n',
        "fi\n",
    ]


def find_all(block):
    n = len(block)
    return [i for i in range(len(lines) - n + 1) if lines[i:i + n] == block]


attach_v = find_all(attach_vendor)
attach_s = find_all(attach_safe)
mount_v = [(opt, i) for opt in opts for i in find_all(mount_vendor(opt))]
mount_s = [(opt, i) for opt in opts for i in find_all(mount_safe(opt))]

if len(attach_v) == 1 and len(mount_v) == 1 and not attach_s and not mount_s:
    # The safe mount block keeps the OPT already present in the vendor line.
    opt, mi = mount_v[0]
    pairs = [
        (attach_v[0], attach_vendor, attach_safe),
        (mi, mount_vendor(opt), mount_safe(opt)),
    ]
    out = lines[:]
    # Replace the later block first so the earlier index stays valid.
    for start, old, new in sorted(pairs, key=lambda p: p[0], reverse=True):
        out[start:start + len(old)] = new
elif len(attach_s) == 1 and len(mount_s) == 1 and not attach_v and not mount_v:
    out = lines[:]
else:
    raise SystemExit("Unrecognized userdata mount_ubifs.sh layout; expected only the vendor or only the Compás no-erase blocks")

if any(re.match(r"^[ \t]*flash_erase[ \t]", line) for line in out):
    raise SystemExit("flash_erase remains in mount_ubifs.sh")
if any(re.match(r"^[ \t]*ubidetach[ \t]", line) for line in out):
    raise SystemExit("ubidetach remains in mount_ubifs.sh")
if any("$prg_name $partition_name" in line for line in out):
    raise SystemExit("Recursive mount retry remains in mount_ubifs.sh")
if sum(line.count(marker) for line in out) != 2:
    raise SystemExit("Expected exactly two Compás no-erase markers in mount_ubifs.sh")
if sum(line.rstrip("\r\n") == ubimkvol for line in out) != 1:
    raise SystemExit("Expected exactly one userdata ubimkvol provisioning line")
destination.write_text("".join(out), encoding="utf-8")
PY
chmod --reference="$mount_ubifs" "$scratch/mount_ubifs.sh"
sh -n "$scratch/mount_ubifs.sh"

# All checks have passed; apply the prepared files, including the idempotently
# guarded vendor script staged above.
for rel in "${shared_paths[@]:0:2}"; do
    install -D -m 0755 "$overlay/$rel" "$root/$rel"
done
if [[ $radio_backend == 0 ]]; then
    install -D -m 0755 "$scratch/wifi_on.vendor.sh" "$vendor"
    install -D -m 0755 "$overlay/etc/init.d/S43wifi_bcm_init_config" "$root/etc/init.d/S43wifi_bcm_init_config"
    install -D -m 0755 "$canonical_wifi" "$root/usr/libexec/compas/wifi_on.sh"
    install -D -m 0755 "$canonical_wifi" "$wifi"
fi
install -m 0755 "$scratch/hiby_player.sh" "$wrapper"
install -m 0755 "$scratch/mount_ubifs.sh" "$mount_ubifs"
install -m 0755 "$scratch/S11jpeg_display_shell" "$stock_splash"
install -D -m 0755 "$overlay/etc/init.d/S22compas_startup_image" "$root/etc/init.d/S22compas_startup_image"

# Sanity-check the installed payload as a final packaging guard.
for rel in "${shared_paths[@]}" usr/bin/wifi_on.sh usr/bin/mount_ubifs.sh; do
    sh -n "$root/$rel"
done
sh -n "$wrapper"

# Install startup tracing only after the boot-pipeline payload has passed its
# sanity checks. The installer validates every target before replacing any.
python3 "$repo/scripts/firmware/boot_pipeline/install_startup_trace.py" "$root"
