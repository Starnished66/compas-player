#!/usr/bin/env python3
"""Build an opt-in R1 radio overlay from the frozen verified root and modules."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import tempfile
from pathlib import Path


EXPECTED = {
    "usr/bin/bt_init": "1460c647c3cdaaa8505b6376fc424c8876952940d1529d126732c0f739e2beb6",
    "usr/bin/bt_resume": "f6e40b2204c212f01db71cc8b803eda790c21d9d54a3fe8180fad85ea77f0974",
    "usr/bin/bt_suspend": "9f6fcb93a8d0705af7e32deee90f34223119ff0023a4caac6039ee3e32ae5c6a",
    "usr/bin/wifi_on.sh": "216255f96db756d3ffc28fe8d7f0b9833146f8af5b77c61385511f8a051f6eff",
    "usr/bin/wifi_up.sh": "4e79b4589f606c6a29a34d9f15794a352f59dc7d7acfd6a6e325c12c9c47f5b3",
    "usr/bin/wifi_down.sh": "d1c8d61521a228ac27e9c7b1f0246edaaf2968de557ad5b2d656f6dda80b68f6",
    "etc/init.d/S43wifi_bcm_init_config": "e925155a163a5b89830b1d3e2430c0a0c2c42e2cba5698f74205a50bac18be7c",
    "module_driver/driver_default_init_script.sh": "2b86803ef51e3bd285d1f0239e62ca0555caedaf9a31d55a94c54db881ea7b7e",
    "usr/bin/wifi_off.sh": "559c2278c9a819e532a5f0cb8eb93a1aeb3c8bdd81f38d8d3a90d3d3d5521d5a",
    "usr/bin/compas-wifi-mac": "732eb94d6826bbabcdd283278abee2133b17e26071d3c6eb47dbf7db27e78b3c",
    "usr/libexec/compas/wifi_on.vendor.sh": "b67adf0f387ff24de1a312dcb5e04365894eaae52e39633dbc4257db045c5e87",
    "usr/bin/bt_enable_bsa.sh": "7c8148d6aca510d65926b2bd9b8c77695fa5d1393791a6efff5e708135fe8740",
}
WIFI_NAMES = ("cyw43438-7.46.58.35.bin", "fw_bcm43438a1.bin", "nvram_azw372.txt", "nvram_ap6212a.txt")
BT_NAMES = ("BCM4343A1_001.002.009.0122.0538.hcd", "BCM4343A1_001.002.009.1010.1030.hcd")
EXPECTED_FIRMWARE = {
    "cyw43438-7.46.58.35.bin": "37575bf1f36011c187adab4961fd3c96c9976f86f1211890826734a0cbeb6674",
    "fw_bcm43438a1.bin": "eb802839dab31cf5098096e187b1b4a5b616def0fc7cc0de2bc78cbc77c89118",
    "nvram_ap6212a.txt": "ca5901b0ae8f54d6c6b14e8ef6672062b29ba8e6ef308588b8fe19c45e1c4eed",
    "nvram_azw372.txt": "daf054a00f1ba1fee4f7167f939989adb18b0da785523c89e65244a1510bebc5",
    "BCM4343A1_001.002.009.0122.0538.hcd": "9873217acdeaee78bd502a31fa876dc1b120c9d07ca8c16420853bc44ba64972",
    "BCM4343A1_001.002.009.1010.1030.hcd": "4a84bb62d3154a9ad6ee271bfab7ff0540b8f1b98f9f21a85c8b44e8f219da33",
}
EXPECTED_POWER_INIT = (
    "insmod bcm_wlbt_power.ko wl_reg_on=PB03 wl_host_wake=PA08 wl_mmc=0 "
    "bt_reg_on=PB04 host_wake_bt=PB05 profile=ap6212a\n"
)
EXPECTED_POWER_INIT_SHA256 = "70bfcad10a832fcaafef77a5227e5f43a95474a7f839d21f6b2317c400e6aff4"
REQUIRED_MODULES = ("brcmfmac.ko", "brcmutil.ko", "bcm_wlbt_power.ko")
ABI_EXPECTED_MODULES = {
    "brcmfmac.ko": "brcmfmac",
    "brcmutil.ko": "brcmutil",
    "bcm_wlbt_power.ko": "bcm_wlbt_power",
}
REBUILT_DRIVER_MODULES = (
    "codec_cs43131.ko", "cst8xx_touch.ko", "cw2015.ko", "i2c_gpio_add.ko",
    "keyboard_adc_multifunc.ko", "keyboard_gpio_add.ko", "lcd_lg35583.ko",
    "leds_pwm_add.ko", "pwm_backlight.ko", "rmem_manager.ko", "sa_config_module.ko",
    "sa_earpods_adc.ko", "sa_hgl_dma.ko", "sa_sound_switch.ko", "soc_adc.ko",
    "soc_aic.ko", "soc_efuse.ko", "soc_gpio.ko", "soc_utils.ko", "tcs1421_add.ko",
    "utils.ko", "x1600_hiby_r1_sound_card.ko",
)
RETAINED_VENDOR_MODULES = ("axp2101.ko", "sau.ko", "soc_fb.ko", "soc_i2c.ko", "soc_msc.ko", "soc_pwm.ko")
BUILT_MODULES = (*REBUILT_DRIVER_MODULES, "bcm_wlbt_power.ko", "brcmfmac.ko", "brcmutil.ko")
ABI_MODULES = (*BUILT_MODULES, *RETAINED_VENDOR_MODULES)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regular_file(path: Path, label: str) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{label} must be a regular, non-symlink file: {path}")


def pinned_provenance(path: Path, modules: Path, repo: Path, base: Path) -> dict[str, object]:
    regular_file(path, "provenance")
    try:
        provenance = json.loads(path.read_text())
        kernel = provenance["kernel"]
        abi = provenance["mixed_static_abi"]
        report_path = Path(abi["report"])
        regular_file(report_path, "ABI report")
        if digest(report_path) != abi["report_sha256"]:
            raise ValueError("ABI report hash does not match provenance")
        report = json.loads(report_path.read_text())
        if report.get("valid") is not True or report.get("selected_module_count") != len(ABI_MODULES):
            raise ValueError("provenance does not point to a passing 31-module ABI report")
        if report.get("missing_imports_by_module") or report.get("vermagic_errors"):
            raise ValueError("ABI report contains missing imports or vermagic errors")
        selected = report.get("selected_modules", {})
        expected_names = {Path(filename).stem for filename in ABI_MODULES}
        if set(selected) != expected_names:
            raise ValueError("ABI report selected module names do not match the pinned 31-module firmware set")
        abi_modules = abi.get("modules", {})
        if set(abi_modules) != set(ABI_MODULES):
            raise ValueError("provenance module hashes do not cover the complete 31-module ABI set")
        module_hashes: dict[str, str] = {}
        for filename in BUILT_MODULES:
            module_path = modules / filename
            regular_file(module_path, f"module input {filename}")
            expected_record = abi_modules[filename]
            report_record = selected[Path(filename).stem]
            if report_record.get("source") != "built":
                raise ValueError(f"ABI report does not classify {filename} as a built module")
            expected_hash = expected_record["sha256"]
            if report_record.get("sha256") != expected_hash:
                raise ValueError(f"ABI report and provenance disagree for {filename}")
            actual = digest(module_path)
            if actual != expected_hash:
                raise ValueError(f"module input is not pinned by ABI provenance: {filename} sha256={actual}")
            module_hashes[filename] = actual

        module_paths = list(modules.glob("*.ko"))
        if any(path.is_symlink() or not path.is_file() for path in module_paths):
            raise ValueError("module input directory must not contain symlink or non-regular .ko entries")
        actual_module_names = {path.name for path in module_paths}
        if actual_module_names != set(BUILT_MODULES):
            raise ValueError("module input directory must contain exactly the 25 built ABI modules")
        retained_dir = base / "module_driver"
        retained_hashes: dict[str, str] = {}
        for filename in RETAINED_VENDOR_MODULES:
            name = Path(filename).stem
            report_record = selected[name]
            base_module = retained_dir / filename
            regular_file(base_module, f"retained vendor module {filename}")
            if report_record.get("source") != "vendor":
                raise ValueError(f"ABI report does not classify retained {filename} as vendor")
            expected_hash = abi_modules[filename]["sha256"]
            if report_record.get("sha256") != expected_hash or digest(base_module) != expected_hash:
                raise ValueError(f"base retained vendor module does not match ABI provenance: {filename}")
            retained_hashes[filename] = expected_hash
            module_hashes[filename] = expected_hash

        # The provider's standalone compile record and mixed-set entry must agree.
        if provenance["provider_module"]["sha256"] != module_hashes["bcm_wlbt_power.ko"]:
            raise ValueError("provider module hash differs from frozen compile provenance")
        provider_module_path = Path(provenance["provider_module"]["path"])
        regular_file(provider_module_path, "provider module build output")
        if digest(provider_module_path) != provenance["provider_module"]["sha256"]:
            raise ValueError("provider module build output differs from frozen provenance")
        for key in ("ximage", "config", "system_map"):
            record = kernel[key]
            artifact = Path(record["path"])
            regular_file(artifact, f"kernel {key}")
            if digest(artifact) != record["sha256"]:
                raise ValueError(f"kernel {key} does not match frozen provenance")
        selected_config = Path(kernel["config"]["path"]).read_text()
        if ("CONFIG_BRCMFMAC=m" not in selected_config or
                "CONFIG_BRCMUTIL=m" not in selected_config or
                "# CONFIG_MODULE_FORCE_UNLOAD is not set" not in selected_config):
            raise ValueError("selected kernel config is not the guarded brcmfmac candidate profile")
        if report.get("system_map_sha256") != kernel["system_map"]["sha256"]:
            raise ValueError("ABI report was not checked against the selected kernel System.map")
        lifetime_patch = kernel["lifetime_patch"]
        lifetime_path = Path(lifetime_patch["path"])
        regular_file(lifetime_path, "MMC lifetime patch")
        if digest(lifetime_path) != lifetime_patch["sha256"]:
            raise ValueError("MMC lifetime patch does not match frozen provenance")
        wifi_config = repo / "firmware/kernel/compas-r1-brcmfmac.config"
        regular_file(wifi_config, "brcmfmac config fragment")
        if digest(wifi_config) != kernel["wifi_config_sha256"]:
            raise ValueError("brcmfmac config fragment does not match frozen provenance")
        patch_records = provenance.get("module_patches", [])
        expected_patch_names = ["compas-radio-input-safety.patch", "compas-radio-lifecycle.patch"]
        if [Path(record.get("path", "")).name for record in patch_records] != expected_patch_names:
            raise ValueError("provenance lacks the ordered reviewed radio module patch pair")
        for patch_record in patch_records:
            patch_path = Path(patch_record["path"])
            regular_file(patch_path, "module patch")
            if digest(patch_path) != patch_record["sha256"]:
                raise ValueError(f"module patch does not match frozen provenance: {patch_path}")
        source = Path(provenance["provider_source"]["path"])
        regular_file(source, "provider source")
        if digest(source) != provenance["provider_source"]["sha256"]:
            raise ValueError("provider source does not match frozen provenance")
        return {
            "kernel": {key: kernel[key] for key in ("ximage", "config", "system_map", "lifetime_patch", "wifi_config_sha256")},
            "module_abi_report": {
                "path": str(report_path), "sha256": abi["report_sha256"],
                "valid": True, "selected_count": report["selected_module_count"],
            },
            "module_hashes": module_hashes,
            "retained_vendor_modules": retained_hashes,
            "provider_source": provenance["provider_source"],
            "provider_module": provenance["provider_module"],
            "module_patches": provenance.get("module_patches", []),
            "provenance_path": str(path),
            "provenance_sha256": digest(path),
        }
    except (KeyError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError(f"malformed candidate provenance: {exc}") from exc


def checked_power_init(repo: Path) -> Path:
    path = repo / "firmware/kernel/wifi-experimental/rootfs/module_driver/bcm_wlbt_power.sh"
    regular_file(path, "power-init source")
    if path.read_text() != EXPECTED_POWER_INIT or digest(path) != EXPECTED_POWER_INIT_SHA256:
        raise ValueError("power-init source differs from the reviewed AP6212A profile and R1 pin map")
    return path


def checked_player(path: Path) -> None:
    regular_file(path, "player binary")
    header = path.read_bytes()[:20]
    if (len(header) < 20 or header[:4] != b"\x7fELF" or header[4] != 1 or
            header[5] != 1 or int.from_bytes(header[16:18], "little") != 2 or
            int.from_bytes(header[18:20], "little") != 8):
        raise ValueError("player input must be a 32-bit little-endian MIPS executable")


def read_checked(base: Path, rel: str) -> str:
    path = base / rel
    if not path.is_file():
        raise ValueError(f"missing stock input: {path}")
    actual = digest(path)
    if actual != EXPECTED[rel]:
        raise ValueError(f"stock input changed: {path} sha256={actual}")
    return path.read_text()


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"expected exactly one {label} anchor, found {text.count(old)}")
    return text.replace(old, new, 1)


def replace_block(text: str, pattern: str, replacement: str, label: str) -> str:
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise ValueError(f"could not locate supported {label} block")
    return updated


def bt_profile_block() -> str:
    return '''# Compas R1 provider is authoritative; unknown identities fail closed.
RADIO=/sys/devices/platform/bcm_wlbt_power
[ "$(cat "$RADIO/identity_state" 2>/dev/null)" = ready ] || exit 1
[ "$(cat "$RADIO/sdio_vendor" 2>/dev/null)" = 0x02d0 ] || exit 1
[ "$(cat "$RADIO/sdio_device" 2>/dev/null)" = 0xa9a6 ] || exit 1
radio_profile=$(cat "$RADIO/profile" 2>/dev/null) || exit 1
chipvendor=$(cat "$RADIO/chipvendor" 2>/dev/null) || exit 1
case "$radio_profile:$chipvendor" in
    azurewave:0x81)
        chip_type=bcm4343a1_aw
        firmware=BCM4343A1_001.002.009.0122.0538.hcd
        ;;
    ap6212a:0x00)
        chip_type=bcm4343a1
        firmware=BCM4343A1_001.002.009.1010.1030.hcd
        ;;
    *) echo "Unsupported R1 Bluetooth identity" >&2; exit 1 ;;
esac
FIRMWARE_PATH="/lib/firmware/bt_bcm/$firmware"
[ -s "$FIRMWARE_PATH" ] || exit 1
'''


def transform_bt_init(text: str) -> str:
    text = replace_block(text, r"get_rfkill_index\(\) \{.*?\n\}\n\n", "", "rfkill fallback helper")
    text = replace_once(text, "rfkill_bt_index=$(get_rfkill_index \"bluetooth\" \"0\")\n\necho 1 > /sys/class/rfkill/rfkill$rfkill_bt_index/state",
                        "/usr/bin/compas-radio _rfkill board-bluetooth 1 || exit 1", "Bluetooth rfkill selection")
    text = replace_block(text, r"BCM_SDIO_BASE=.*?(?=if \[ \"\$chip_type\" = \"aic8800\" \])",
                         bt_profile_block(), "Bluetooth chip autodetection")
    text = replace_block(text, r"if \[ \"\$chip_type\" = \"aic8800\" \]; then.*?else\n    # AP6212 / AW-NB372SM --- BCM4343A1\n(.*?)\nfi",
                         r"# Initialize the provider-validated BCM4343A1 profile.\n\1", "unsupported AIC/4345 branches")
    text = text.replace('echo 0 > /sys/class/rfkill/rfkill$rfkill_bt_index/state 2>/dev/null',
                        '/usr/bin/compas-radio _rfkill board-bluetooth 0 || :')
    if "rfkill$rfkill_bt_index" in text or "get_rfkill_index" in text:
        raise ValueError("unsupported residual rfkill-index logic in bt_init")
    text, count = re.subn(r"hciconfig hci0 up", "/usr/bin/compas-radio _rfkill hci0 1 || exit 1\nhciconfig hci0 up || exit 1", text)
    if count < 1:
        raise ValueError("missing HCI activation in bt_init")
    return text


def transform_bt_resume(text: str) -> str:
    text = replace_once(text, "echo 1 > /sys/class/rfkill/rfkill0/state",
                        "/usr/bin/compas-radio _rfkill board-bluetooth 1 || exit 1", "BT resume board rfkill")
    text = replace_block(text, r"SDIO_BASE=.*?(?=if \[ \"\$chip_type\" = \"bcm4345c5\" \])",
                         bt_profile_block(), "BT resume chip autodetection")
    text = text.replace('echo 0 > /sys/class/rfkill/rfkill0/state 2>/dev/null',
                        '/usr/bin/compas-radio _rfkill board-bluetooth 0 || :')
    if "rfkill0/state" in text or "SDIO_BASE=" in text:
        raise ValueError("unsupported residual rfkill logic in bt_resume")
    text, count = re.subn(r"hciconfig hci0 up", "/usr/bin/compas-radio _rfkill hci0 1 || exit 1\nhciconfig hci0 up || exit 1", text)
    if count < 1:
        raise ValueError("missing HCI activation in bt_resume")
    return text


def transform_bt_suspend(text: str) -> str:
    if digest_text(text) != EXPECTED["usr/bin/bt_suspend"]:
        raise ValueError("unsupported stock bt_suspend script")
    return '''#!/bin/sh
command -v killall >/dev/null 2>&1 && command -v pidof >/dev/null 2>&1 || exit 1
hciconfig hci0 down >/dev/null 2>&1 || :
killall brcm_patchram_plus hciattach >/dev/null 2>&1 || :
elapsed=0
while { pidof brcm_patchram_plus >/dev/null 2>&1 || pidof hciattach >/dev/null 2>&1; } && [ "$elapsed" -lt 60 ]; do
    usleep 50000 2>/dev/null || sleep 0.05
    elapsed=$((elapsed + 1))
done
if pidof brcm_patchram_plus >/dev/null 2>&1 || pidof hciattach >/dev/null 2>&1; then
    killall -s KILL brcm_patchram_plus hciattach >/dev/null 2>&1 || :
    usleep 100000 2>/dev/null || sleep 0.1
fi
if pidof brcm_patchram_plus >/dev/null 2>&1 || pidof hciattach >/dev/null 2>&1; then
    echo "Bluetooth firmware process did not stop" >&2
    exit 1
fi
/usr/bin/compas-radio _rfkill board-bluetooth 0 || exit 1
'''


def transform_wifi_vendor(text: str) -> str:
    text = replace_block(text, r"nvram_patch=`sa_config.*?(?=ifconfig \$INTERFACE up)",
                         "# brcmfmac reads profile aliases installed by compas-radio.\n", "legacy bcmdhd firmware override")
    text = replace_once(text, "ifconfig $INTERFACE up", "ifconfig $INTERFACE up || return 1", "WiFi interface activation")
    text = replace_once(text, 'if [ ! -f "$WPA_CONF" ]; then\n    cp $WPA_CONF_DEFAULT $WPA_CONF\nfi',
                        'if [ ! -f "$WPA_CONF" ]; then\n    cp "$WPA_CONF_DEFAULT" "$WPA_CONF" || return 1\nfi\n[ -r "$WPA_CONF" ] || return 1',
                        "WPA configuration preparation")
    text = replace_once(text, "wpa_supplicant -Dnl80211 -i$INTERFACE -c$WPA_CONF -B",
                        "wpa_supplicant -Dnl80211 -i$INTERFACE -c$WPA_CONF -B || return 1", "WPA startup status")
    text = replace_once(text, "usleep 1300000\necho $HOSTNAME",
                        "usleep 1300000\npidof wpa_supplicant >/dev/null 2>&1 || return 1\necho $HOSTNAME",
                        "WPA process liveness")
    text = replace_once(text, "udhcpc -b -i $INTERFACE -q -x hostname:$HOSTNAME &",
                        "command -v udhcpc >/dev/null 2>&1 || return 1\nudhcpc -b -i $INTERFACE -q -x hostname:$HOSTNAME &\nusleep 100000\npidof udhcpc >/dev/null 2>&1 || return 1",
                        "DHCP startup status")
    if "COMPAS_WIFI_MAC_PRE_WPA_GUARD" not in text:
        raise ValueError("stock WiFi vendor pre-WPA MAC guard is missing")
    return text


def digest_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def private_wifi_off() -> str:
    return '''#!/bin/sh
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
command -v killall >/dev/null 2>&1 && command -v pidof >/dev/null 2>&1 || exit 1
killall udhcpc >/dev/null 2>&1 || :
killall wpa_supplicant >/dev/null 2>&1 || :
elapsed=0
while { pidof udhcpc >/dev/null 2>&1 || pidof wpa_supplicant >/dev/null 2>&1; } && [ "$elapsed" -lt 40 ]; do
    usleep 50000 2>/dev/null || sleep 0.05
    elapsed=$((elapsed + 1))
done
if pidof udhcpc >/dev/null 2>&1 || pidof wpa_supplicant >/dev/null 2>&1; then
    echo "WiFi connection processes did not stop" >&2
    exit 1
fi
ifconfig wlan0 down || exit 1
'''


def wrapper(command: str) -> str:
    return f'''#!/bin/sh
exec /usr/bin/compas-radio {command} "$@"
'''


def wifi_up_wrapper() -> str:
    return '''#!/bin/sh
if [ "$#" -ne 0 ]; then
    echo "wifi_up.sh: custom arguments are unsupported by the guarded radio path" >&2
    exit 2
fi
exec /usr/bin/compas-radio wifi-up
'''


def disabled_bsa_wrapper() -> str:
    return '''#!/bin/sh
echo "bt_enable_bsa.sh is disabled by the opt-in radio overlay; use the guarded Bluetooth startup path" >&2
exit 1
'''


def copy_mode(src: Path, dst: Path, mode: int | None = None) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    if mode is None:
        mode = src.stat().st_mode & 0o777
    dst.chmod(mode)


def runtime_file_hashes(root: Path) -> dict[str, str]:
    paths = {
        "usr/bin/bt_enable_bsa.sh", "usr/bin/bt_init", "usr/bin/bt_resume", "usr/bin/bt_suspend",
        "usr/bin/compas-radio", "usr/bin/compas-wifi-mac", "usr/bin/wifi_down.sh",
        "usr/bin/wifi_off.sh", "usr/bin/wifi_on.sh", "usr/bin/wifi_up.sh",
        "etc/init.d/S43wifi_bcm_init_config", "module_driver/bcm_wlbt_power.sh",
        "module_driver/driver_default_init_script.sh",
    }
    result: dict[str, str] = {}
    for relative in sorted(paths):
        path = root / relative
        regular_file(path, f"runtime file {relative}")
        result[relative] = digest(path)
    private = root / "usr/libexec/compas"
    if private.exists():
        if private.is_symlink() or not private.is_dir():
            raise ValueError(f"private runtime script path is unsafe: {private}")
        for path in sorted(private.rglob("*")):
            if path.is_file() and not path.is_symlink():
                result[path.relative_to(root).as_posix()] = digest(path)
    return dict(sorted(result.items()))


def stage(base: Path, modules: Path, output: Path, repo: Path,
          provenance_path: Path, player: Path | None = None) -> None:
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists; refusing to overwrite: {output}")
    if modules.is_symlink() or not modules.is_dir():
        raise ValueError(f"module input must be a real directory: {modules}")
    for rel in EXPECTED:
        read_checked(base, rel)
    provenance_inputs = pinned_provenance(provenance_path, modules, repo, base)
    if player is not None:
        checked_player(player)
    for name in WIFI_NAMES:
        path = base / "lib/firmware/wifi_bcm" / name
        if not path.is_file():
            raise ValueError(f"missing WiFi firmware input: {name}")
    for name in BT_NAMES:
        path = base / "lib/firmware/bt_bcm" / name
        if not path.is_file():
            raise ValueError(f"missing Bluetooth firmware input: {name}")
    firmware_hashes = {}
    for name in WIFI_NAMES:
        firmware_hashes[name] = digest(base / "lib/firmware/wifi_bcm" / name)
    for name in BT_NAMES:
        firmware_hashes[name] = digest(base / "lib/firmware/bt_bcm" / name)
    for name, expected in EXPECTED_FIRMWARE.items():
        if firmware_hashes[name] != expected:
            raise ValueError(f"firmware source changed: {name} sha256={firmware_hashes[name]}")
    for rel in ("lib/firmware/brcm/brcmfmac43430-sdio.bin", "lib/firmware/brcm/brcmfmac43430-sdio.txt",
                "usr/lib/compas-radio/firmware-sha256"):
        path = base / rel
        if path.exists() or path.is_symlink():
            raise ValueError(f"stock root already contains a conflicting experimental path: {path}")
    needed = BUILT_MODULES
    power_init = checked_power_init(repo)
    radio_control = repo / "firmware/kernel/wifi-experimental/rootfs/usr/bin/compas-radio"
    regular_file(radio_control, "radio control script")
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=output.name + ".tmp.", dir=output.parent))
    root = tmp / "rootfs"
    try:
        # Install MAC manager with the additive reversible invalidation API.
        mac = repo / "firmware/overlay/usr/bin/compas-wifi-mac"
        copy_mode(mac, root / "usr/bin/compas-wifi-mac", 0o755)
        if player is not None:
            copy_mode(player, root / "usr/bin/compas_player", 0o755)
        copy_mode(radio_control, root / "usr/bin/compas-radio", 0o755)
        for path, cmd in (("usr/bin/wifi_on.sh", "wifi-up"), ("usr/bin/wifi_off.sh", "wifi-down"),
                          ("usr/bin/bt_init", "run-bt init"), ("usr/bin/bt_resume", "run-bt resume"),
                          ("usr/bin/bt_suspend", "run-bt suspend")):
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(wrapper(cmd))
            target.chmod(0o755)
        # Replace stock entry points that bypass the serialized transaction.
        (root / "usr/bin/wifi_up.sh").write_text(wifi_up_wrapper())
        (root / "usr/bin/wifi_up.sh").chmod(0o755)
        (root / "usr/bin/wifi_down.sh").write_text(wrapper("wifi-down"))
        (root / "usr/bin/wifi_down.sh").chmod(0o755)
        (root / "usr/bin/bt_enable_bsa.sh").write_text(disabled_bsa_wrapper())
        (root / "usr/bin/bt_enable_bsa.sh").chmod(0o755)
        for name in ("brcmfmac.ko", "brcmutil.ko"):
            copy_mode(modules / name, root / "usr/lib/compas-radio/modules" / name, 0o644)
        for name in (*REBUILT_DRIVER_MODULES, "bcm_wlbt_power.ko"):
            copy_mode(modules / name, root / "module_driver" / name, 0o644)
        copy_mode(power_init, root / "module_driver/bcm_wlbt_power.sh", 0o755)
        for name in WIFI_NAMES:
            copy_mode(base / "lib/firmware/wifi_bcm" / name, root / "lib/firmware/wifi_bcm" / name)
        for name in BT_NAMES:
            copy_mode(base / "lib/firmware/bt_bcm" / name, root / "lib/firmware/bt_bcm" / name)
        hash_path = root / "usr/lib/compas-radio/firmware-sha256"
        hash_path.parent.mkdir(parents=True, exist_ok=True)
        hash_path.write_text("".join(f"{firmware_hashes[name]}  {name}\n" for name in sorted(firmware_hashes)))
        for alias, target in (("brcmfmac43430-sdio.bin", "selected.bin"),
                              ("brcmfmac43430-sdio.txt", "selected.txt")):
            link = root / "lib/firmware/brcm" / alias
            link.parent.mkdir(parents=True, exist_ok=True)
            if link.exists() or link.is_symlink():
                raise ValueError(f"refusing to replace existing firmware alias in overlay: {link}")
            link.symlink_to(f"/usr/data/.compas/wifi-firmware/{target}")

        vendor = transform_wifi_vendor(read_checked(base, "usr/libexec/compas/wifi_on.vendor.sh"))
        p = root / "usr/libexec/compas/wifi_on.brcmfmac.vendor.sh"
        p.parent.mkdir(parents=True, exist_ok=True); p.write_text(vendor); p.chmod(0o755)
        p = root / "usr/libexec/compas/wifi_off.brcmfmac.vendor.sh"
        p.write_text(private_wifi_off()); p.chmod(0o755)
        bt_init = transform_bt_init(read_checked(base, "usr/bin/bt_init"))
        bt_resume = transform_bt_resume(read_checked(base, "usr/bin/bt_resume"))
        bt_suspend = transform_bt_suspend(read_checked(base, "usr/bin/bt_suspend"))
        for name, content in (("bt_init.vendor.sh", bt_init), ("bt_resume.vendor.sh", bt_resume),
                              ("bt_suspend.vendor.sh", bt_suspend)):
            p = root / "usr/libexec/compas/radio-bt" / name
            p.parent.mkdir(parents=True, exist_ok=True); p.write_text(content); p.chmod(0o755)

        s43 = '''#!/bin/sh
case "$1" in
  start)
    [ "$(cat /sys/devices/platform/bcm_wlbt_power/identity_state 2>/dev/null)" = ready ] || exit 0
    [ "$(cat /sys/devices/platform/bcm_wlbt_power/wifi_power 2>/dev/null)" = 1 ] || exit 0
    exec /usr/bin/compas-wifi-mac start ;;
  stop)
    COMPAS_WIFI_MAC_DOWN_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh
    COMPAS_WIFI_MAC_OFF_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh
    export COMPAS_WIFI_MAC_DOWN_SCRIPT COMPAS_WIFI_MAC_OFF_SCRIPT
    exec /usr/bin/compas-wifi-mac stop ;;
  *) echo "Usage: $0 {start|stop}" >&2; exit 1 ;;
esac
'''
        p = root / "etc/init.d/S43wifi_bcm_init_config"
        p.parent.mkdir(parents=True, exist_ok=True); p.write_text(s43); p.chmod(0o755)
        driver = read_checked(base, "module_driver/driver_default_init_script.sh")
        driver = replace_once(driver, "sh cywdhd.sh", "sh bcm_wlbt_power.sh", "WiFi module startup")
        p = root / "module_driver/driver_default_init_script.sh"
        p.parent.mkdir(parents=True, exist_ok=True); p.write_text(driver); p.chmod(0o755)

        manifest = ["Opt-in experimental radio overlay. Not part of default rootfs.",
                    "Stock inputs are hash-checked by stage_experimental_radio.py.",
                    "Firmware blobs are copied unchanged; runtime creates selected symlinks in persistent /usr/data/.compas/wifi-firmware.",
                    "Modules:"]
        manifest.extend(f"  {name} sha256={digest(modules / name)}" for name in needed)
        manifest.extend(f"  retained {name} sha256={provenance_inputs['retained_vendor_modules'][name]}"
                        for name in RETAINED_VENDOR_MODULES)
        manifest.append(f"  bcm_wlbt_power.sh sha256={digest(power_init)}")
        manifest.append(f"  compas-radio sha256={digest(radio_control)}")
        manifest.extend((
            f"  kernel xImage sha256={provenance_inputs['kernel']['ximage']['sha256']}",
            f"  kernel config sha256={provenance_inputs['kernel']['config']['sha256']}",
            f"  kernel System.map sha256={provenance_inputs['kernel']['system_map']['sha256']}",
            f"  mixed ABI report sha256={provenance_inputs['module_abi_report']['sha256']} valid=true",
            f"  provenance sha256={provenance_inputs['provenance_sha256']}",
        ))
        (root / "EXPERIMENTAL-RADIO-MANIFEST.txt").write_text("\n".join(manifest) + "\n")
        assembly = {
            "schema_version": 1,
            "status": "host-only candidate; not validated on device; not a flash/package artifact",
            "board": "R1",
            "wifi_profile": "ap6212a",
            "power_init": {"path": str(power_init), "sha256": digest(power_init),
                           "pins": {"wl_reg_on": "PB03", "wl_host_wake": "PA08", "wl_mmc": "0",
                                    "bt_reg_on": "PB04", "host_wake_bt": "PB05"}},
            "radio_control": {"path": str(radio_control), "sha256": digest(radio_control),
                              "overlay_path": "usr/bin/compas-radio"},
            "runtime_files": runtime_file_hashes(root),
            "payload_metadata": {
                "usr/lib/compas-radio/firmware-sha256": digest(root / "usr/lib/compas-radio/firmware-sha256"),
                "EXPERIMENTAL-RADIO-MANIFEST.txt": digest(root / "EXPERIMENTAL-RADIO-MANIFEST.txt"),
            },
            "base_root": {
                "path": str(base),
                "pinned_stock_files": {rel: digest(base / rel) for rel in EXPECTED},
                "firmware_files": {f"lib/firmware/wifi_bcm/{name}": firmware_hashes[name]
                                   for name in WIFI_NAMES} | {
                    f"lib/firmware/bt_bcm/{name}": firmware_hashes[name] for name in BT_NAMES
                },
            },
            "kernel": provenance_inputs["kernel"],
            "module_abi_report": provenance_inputs["module_abi_report"],
            "modules": provenance_inputs["module_hashes"],
            "retained_vendor_modules": provenance_inputs["retained_vendor_modules"],
            "provider_source": provenance_inputs["provider_source"],
            "provider_module": provenance_inputs["provider_module"],
            "player": ({"path": str(player), "sha256": digest(player),
                        "overlay_path": "usr/bin/compas_player"} if player is not None else None),
            "module_patches": provenance_inputs["module_patches"],
            "provenance": {"path": provenance_inputs["provenance_path"],
                           "sha256": provenance_inputs["provenance_sha256"]},
            "stock_bypass_paths": {
                "usr/bin/wifi_up.sh": "routes to compas-radio wifi-up",
                "usr/bin/wifi_down.sh": "existing guarded wrapper retained",
                "usr/bin/bt_enable_bsa.sh": "disabled; legacy direct rfkill/BSA path bypasses guard",
            },
        }
        (root / "CANDIDATE-ASSEMBLY.json").write_text(json.dumps(assembly, indent=2, sort_keys=True) + "\n")
        os.replace(root, output)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-root", type=Path, required=True, help="verified stock rootfs")
    parser.add_argument("--modules", type=Path, required=True, help="directory containing reviewed R1 module package")
    parser.add_argument("--provenance", type=Path, required=True,
                        help="explicit frozen build and mixed-module ABI provenance JSON")
    parser.add_argument("--player", type=Path,
                        help="optional current MIPS32 little-endian compas_player_target to include")
    parser.add_argument("--output", type=Path, required=True, help="new output rootfs overlay directory")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    try:
        stage(args.base_root.absolute(), args.modules.absolute(), args.output.absolute(), repo,
              args.provenance.absolute(), args.player.absolute() if args.player else None)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"staged opt-in experimental radio overlay: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
