#!/usr/bin/env python3
"""Host-only behavioral checks for the opt-in radio backend and stager."""

from __future__ import annotations

import importlib.util
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BACKEND = REPO / "firmware/kernel/wifi-experimental/rootfs/usr/bin/compas-radio"
STAGER = REPO / "scripts/kernel/stage_experimental_radio.py"
PINNING_TEST = REPO / "scripts/kernel/test_stage_experimental_radio_pinning.py"
BASE = REPO.parent / "compas-ui-stall-fixes-20261008/flash/verify/root"


def load_stager():
    spec = importlib.util.spec_from_file_location("stage_experimental_radio", STAGER)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


def load_pinning_fixture():
    spec = importlib.util.spec_from_file_location("radio_pinning_fixture", PINNING_TEST)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


class RadioHarness:
    def __init__(self, parent: Path, profile="azurewave", chipvendor="0x81", data_fstype="ext4"):
        self.root = parent / "root"
        self.bin = parent / "bin"
        self.log = parent / "operations.log"
        self.root.mkdir()
        self.bin.mkdir()
        self.radio = self.root / "sys/devices/platform/bcm_wlbt_power"
        self.radio.mkdir(parents=True)
        for name, value in {
            "identity_state": "ready", "profile": profile, "chipvendor": chipvendor,
            "sdio_vendor": "0x02d0", "sdio_device": "0xa9a6", "wifi_power": "0", "wifi_fault": "0",
        }.items():
            (self.radio / name).write_text(value)
        (self.root / "proc").mkdir()
        (self.root / "proc/mounts").write_text(f"/dev/fake /usr/data {data_fstype} rw 0 0\n")
        (self.root / "usr/data").mkdir(parents=True)
        (self.root / "lib/firmware/wifi_bcm").mkdir(parents=True)
        for name in ("cyw43438-7.46.58.35.bin", "fw_bcm43438a1.bin", "nvram_azw372.txt", "nvram_ap6212a.txt"):
            (self.root / "lib/firmware/wifi_bcm" / name).write_bytes(b"firmware-" + name.encode())
        hash_dir = self.root / "usr/lib/compas-radio"
        hash_dir.mkdir(parents=True)
        hashes = []
        for name in ("cyw43438-7.46.58.35.bin", "fw_bcm43438a1.bin", "nvram_azw372.txt", "nvram_ap6212a.txt"):
            data = (self.root / "lib/firmware/wifi_bcm" / name).read_bytes()
            hashes.append(f"{hashlib.sha256(data).hexdigest()}  {name}\n")
        (hash_dir / "firmware-sha256").write_text("".join(hashes))
        (self.root / "lib/firmware/brcm").mkdir(parents=True)
        self.net = self.root / "sys/class/net"
        self.net.mkdir(parents=True)
        (self.root / "sys/class/rfkill").mkdir(parents=True)
        rf = self.root / "sys/class/rfkill/rfkill8"
        rf.mkdir()
        (rf / "name").write_text("compas-bluetooth")
        (rf / "type").write_text("bluetooth")
        (rf / "state").write_text("1")
        hci = self.root / "sys/class/rfkill/rfkill9"
        hci.mkdir()
        (hci / "name").write_text("hci0")
        (hci / "type").write_text("bluetooth")
        (hci / "state").write_text("0")
        self.modules = self.root / "usr/lib/compas-radio/modules"
        self.modules.mkdir(parents=True)
        for name in ("brcmfmac.ko", "brcmutil.ko"):
            (self.modules / name).write_bytes(b"module")
        mac = self.root / "usr/bin/compas-wifi-mac"
        mac.parent.mkdir(parents=True, exist_ok=True)
        mac.write_text('''#!/bin/sh
printf 'mac:%s\\n' "$*" >> "$RADIO_TEST_LOG"
STATE="$COMPAS_RADIO_ROOT/var/run/compas-wifi-mac"
case "$1" in
  connection-begin) exit 0 ;;
  connection-end) exit 0 ;;
  wait) mkdir -p "$STATE"; printf '02:11:22:33:44:55\\n' > "$STATE/mac"; printf 'ready\\n' > "$STATE/status"; printf '02:11:22:33:44:55\\n' > "$COMPAS_RADIO_ROOT/sys/class/net/wlan0/address"; exit 0 ;;
  start) exit 0 ;;
  invalidate) rm -f "$STATE/status" "$STATE/mac"; exit 0 ;;
  *) exit 2 ;;
esac
''')
        mac.chmod(0o755)
        vendor = self.root / "usr/libexec/compas"
        vendor.mkdir(parents=True)
        for name in ("wifi_on.brcmfmac.vendor.sh", "wifi_off.brcmfmac.vendor.sh"):
            path = vendor / name
            end = "return 0" if "wifi_on" in name else "exit 0"
            bring_iface_up = '"$COMPAS_WIFI_MAC_IFCONFIG" wlan0 up || return 1\n' if "wifi_on" in name else ""
            path.write_text('#!/bin/sh\nprintf "vendor:' + name + '\\n" >> "$RADIO_TEST_LOG"\n' + bring_iface_up + end + '\n')
            path.chmod(0o755)
        bt = self.root / "usr/libexec/compas/radio-bt"
        bt.mkdir(parents=True)
        for action in ("init", "resume", "suspend"):
            path = bt / f"bt_{action}.vendor.sh"
            path.write_text('#!/bin/sh\nprintf "bt:' + action + '\\n" >> "$RADIO_TEST_LOG"\n'
                            + ('/bin/sh "$RADIO_TEST_BACKEND" _rfkill board-bluetooth 0 || exit 1\n' if action == "suspend" else ''))
            path.chmod(0o755)
        self._command("insmod", '''name=${1##*/}; name=${name%.ko}; printf 'insmod:%s\\n' "$name" >> "$RADIO_TEST_LOG"
mkdir -p "$COMPAS_RADIO_ROOT/sys/module/$name"
        if [ "$name" = brcmfmac ]; then
            mkdir -p "$COMPAS_RADIO_ROOT/sys/class/net/wlan0"
            printf '00:11:22:33:44:66\\n' > "$COMPAS_RADIO_ROOT/sys/class/net/wlan0/address"
            printf '0x0\\n' > "$COMPAS_RADIO_ROOT/sys/class/net/wlan0/flags"
            printf 'down\\n' > "$COMPAS_RADIO_ROOT/sys/class/net/wlan0/operstate"
        fi
''')
        self._command("rmmod", '''printf 'rmmod:%s\\n' "$1" >> "$RADIO_TEST_LOG"
if [ "${RMMOD_FAIL:-}" = "$1" ]; then exit 1; fi
rm -rf "$COMPAS_RADIO_ROOT/sys/module/$1"
if [ "$1" = brcmfmac ]; then rm -rf "$COMPAS_RADIO_ROOT/sys/class/net/wlan0"; fi
''')
        self._command("ifconfig", '''printf 'ifconfig:%s\\n' "$*" >> "$RADIO_TEST_LOG"
iface="$COMPAS_RADIO_ROOT/sys/class/net/$1"
case "$2" in
  hw) printf '%s\\n' "$4" > "$iface/address" ;;
  up) printf '0x1\\n' > "$iface/flags"; printf 'up\\n' > "$iface/operstate" ;;
  down) printf '0x0\\n' > "$iface/flags"; printf 'down\\n' > "$iface/operstate" ;;
esac
exit 0
''')
        self._command("killall", 'exit 0\n')
        self._command("pidof", 'exit 1\n')
        self._command("usleep", 'exit 0\n')
        self._command("wpa_cli", 'printf "PONG\\n"\nexit 0\n')
        self._command("wpa_supplicant", 'printf "wpa:start\\n" >> "$RADIO_TEST_LOG"\nexit 0\n')

    def _command(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)

    def run(self, *args, **extra):
        env = os.environ.copy()
        env.update({"COMPAS_RADIO_ROOT": str(self.root), "RADIO_TEST_LOG": str(self.log),
                    "PATH": f"{self.bin}:/usr/bin:/bin", "COMPAS_RADIO_IDENTITY_WAIT": "1",
                    "COMPAS_RADIO_IFACE_WAIT": "1", "COMPAS_RADIO_INSMOD": str(self.bin / "insmod"),
                    "COMPAS_RADIO_RMMOD": str(self.bin / "rmmod"), "RADIO_TEST_BACKEND": str(BACKEND),
                    "COMPAS_RADIO_WPA_CLI": str(self.bin / "wpa_cli"),
                    "COMPAS_RADIO_PROC_ROOT": "/proc", "COMPAS_WIFI_MAC_STATE_DIR": str(self.root / "var/run/compas-wifi-mac"),
                    "COMPAS_WIFI_MAC_SYS_CLASS_NET": str(self.root / "sys/class/net"),
                    "COMPAS_WIFI_MAC_PROC_ROOT": "/proc", "COMPAS_WIFI_PROCESS_ROOT": "/proc",
                    "COMPAS_WIFI_MAC_DATA_DIR": str(self.root / "usr/data"),
                    "COMPAS_WIFI_MAC_IFCONFIG": str(self.bin / "ifconfig")})
        env.update(extra)
        return subprocess.run(["/bin/sh", str(BACKEND), *args], env=env, text=True, capture_output=True, timeout=10)

    def operations(self):
        return self.log.read_text().splitlines() if self.log.exists() else []


class ExperimentalRadioTests(unittest.TestCase):
    def test_stager_accepts_only_frozen_source_and_emits_private_wrappers(self):
        stage = load_stager()
        pinning = load_pinning_fixture()
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            base = temp / "base"
            pinning.copy_base_fixture(base)
            modules = temp / "modules"
            modules.mkdir()
            provenance = pinning.make_provenance(temp, modules, base)
            out = temp / "staged"
            stage.stage(base, modules, out, REPO, provenance)
            root = out
            self.assertTrue((root / "usr/bin/compas-radio").exists())
            init = (root / "usr/libexec/compas/radio-bt/bt_init.vendor.sh").read_text()
            self.assertIn("Unsupported R1 Bluetooth identity", init)
            self.assertNotIn("rfkill0/state", init)
            self.assertNotIn("get_rfkill_index", init)
            self.assertIn("sh bcm_wlbt_power.sh", (root / "module_driver/driver_default_init_script.sh").read_text())
            s43 = (root / "etc/init.d/S43wifi_bcm_init_config").read_text()
            stop_branch = s43.split("stop)", 1)[1].split(";;", 1)[0]
            self.assertIn('COMPAS_WIFI_MAC_DOWN_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh', stop_branch)
            self.assertIn('COMPAS_WIFI_MAC_OFF_SCRIPT=/usr/libexec/compas/wifi_off.brcmfmac.vendor.sh', stop_branch)
            self.assertEqual("/usr/data/.compas/wifi-firmware/selected.bin",
                             os.readlink(root / "lib/firmware/brcm/brcmfmac43430-sdio.bin"))
            for path in root.rglob("*"):
                if path.is_file() and path.read_bytes().startswith(b"#!"):
                    syntax = subprocess.run(["/bin/sh", "-n", str(path)], text=True, capture_output=True)
                    self.assertEqual(0, syntax.returncode, f"{path}: {syntax.stderr}")
            with self.assertRaises(ValueError):
                stage.stage(base, modules, out, REPO, provenance)

    def test_unknown_identity_fails_before_module_mutation(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td), profile="unknown", chipvendor="0x00")
            result = h.run("wifi-up")
            self.assertNotEqual(result.returncode, 0, result.stderr)
            self.assertEqual([], h.operations())

    def test_ready_identity_with_wifi_fault_rejects_wifi_but_keeps_bluetooth_available(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            (h.radio / "wifi_fault").write_text("1\n")
            result = h.run("wifi-up")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("Wi-Fi fault", result.stderr)
            self.assertEqual([], h.operations())
            bt = h.run("run-bt", "init")
            self.assertEqual(0, bt.returncode, bt.stderr)
            self.assertIn("bt:init", h.operations())

    def test_ram_backed_usr_data_mount_is_rejected(self):
        for fstype in ("tmpfs", "ramfs"):
            with self.subTest(fstype=fstype), tempfile.TemporaryDirectory() as td:
                h = RadioHarness(Path(td), data_fstype=fstype)
                result = h.run("wifi-up")
                self.assertNotEqual(0, result.returncode)
                self.assertIn("persistent storage", result.stderr)
                self.assertEqual([], h.operations())

    def test_ignored_wifi_power_write_is_detected(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            power = h.radio / "wifi_power"
            power.unlink()
            power.symlink_to("/dev/null")
            result = h.run("wifi-up")
            self.assertNotEqual(0, result.returncode)
            self.assertFalse((h.root / "sys/module/brcmfmac").exists())

    def test_wifi_down_revalidates_provider_power_even_when_already_off(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            power = h.radio / "wifi_power"
            power.unlink()
            power.symlink_to("/dev/null")
            result = h.run("wifi-down")
            self.assertNotEqual(0, result.returncode)
            self.assertNotIn("mac:invalidate wlan0", h.operations())

    def test_ignored_rfkill_write_is_detected(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            state = h.root / "sys/class/rfkill/rfkill9/state"
            state.unlink()
            state.symlink_to("/dev/null")
            result = h.run("run-bt", "init")
            self.assertNotEqual(0, result.returncode)

    def test_bounded_timeout_kills_child_process_group(self):
        source = BACKEND.read_text()
        stop = source.split("stop_child_tree() {", 1)[1].split("\nradio_exit() {", 1)[0]
        bounded = source.split("run_bounded() {", 1)[1].split("\nwait_identity() {", 1)[0]
        with tempfile.TemporaryDirectory() as td:
            state = Path(td) / "state"
            state.mkdir()
            marker = Path(td) / "late-child-ran"
            snippet = "\n".join((
                "stop_child_tree() {" + stop,
                "run_bounded() {" + bounded,
                f'STATE_DIR="{state}"; CHILD_PID=; LOCK_HELD=0',
                f'run_bounded 1 /bin/sh -c \'sleep 2; echo late > "{marker}"\'',
                "status=$?",
                "[ \"$status\" -eq 124 ] || exit 10",
                "sleep 1.3",
                f'[ ! -e "{marker}" ] || exit 11',
            ))
            result = subprocess.run(["/bin/sh", "-c", snippet], text=True,
                                    capture_output=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stderr)

    def test_child_mount_does_not_substitute_for_persistent_usr_data_mount(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            (h.root / "proc/mounts").write_text("/dev/child /usr/data/.compas ext4 rw 0 0\n")
            result = h.run("wifi-up")
            self.assertNotEqual(0, result.returncode)
            self.assertIn("persistent storage", result.stderr)
            self.assertEqual([], h.operations())

    def test_missing_profile_firmware_and_bt_only_are_bounded(self):
        with tempfile.TemporaryDirectory() as td:
            parent = Path(td)
            h = RadioHarness(parent)
            (h.root / "lib/firmware/wifi_bcm/cyw43438-7.46.58.35.bin").unlink()
            result = h.run("wifi-up")
            self.assertNotEqual(0, result.returncode)
            self.assertEqual([], h.operations())
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            result = h.run("run-bt", "init")
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual([], [op for op in h.operations() if op.startswith("insmod:")])
            self.assertIn("bt:init", h.operations())
            self.assertEqual("1", (h.root / "sys/class/rfkill/rfkill9/state").read_text().strip())

    def test_concurrent_wifi_up_serializes_module_and_vendor_start(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            vendor = h.root / "usr/libexec/compas/wifi_on.brcmfmac.vendor.sh"
            vendor.write_text('#!/bin/sh\nprintf "vendor:start\\n" >> "$RADIO_TEST_LOG"\n"$COMPAS_WIFI_MAC_IFCONFIG" wlan0 up || return 1\nsleep 0.2\nprintf "vendor:end\\n" >> "$RADIO_TEST_LOG"\nreturn 0\n')
            vendor.chmod(0o755)
            env = os.environ.copy()
            env.update({"COMPAS_RADIO_ROOT": str(h.root), "RADIO_TEST_LOG": str(h.log),
                        "PATH": f"{h.bin}:/usr/bin:/bin", "COMPAS_RADIO_IDENTITY_WAIT": "1",
                        "COMPAS_RADIO_IFACE_WAIT": "1", "COMPAS_RADIO_INSMOD": str(h.bin / "insmod"),
                        "COMPAS_RADIO_RMMOD": str(h.bin / "rmmod"), "COMPAS_RADIO_PROC_ROOT": "/proc",
                        "COMPAS_WIFI_MAC_STATE_DIR": str(h.root / "var/run/compas-wifi-mac"),
                        "COMPAS_WIFI_MAC_SYS_CLASS_NET": str(h.root / "sys/class/net"),
                        "COMPAS_WIFI_MAC_IFCONFIG": str(h.bin / "ifconfig")})
            processes = [subprocess.Popen(["/bin/sh", str(BACKEND), "wifi-up"], env=env,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                         for _ in range(2)]
            results = [process.communicate(timeout=10) + (process.returncode,) for process in processes]
            ops = h.operations()
            self.assertEqual([0, 0], [result[2] for result in results], (results, ops))
            self.assertEqual(1, ops.count("insmod:brcmutil"))
            self.assertEqual(2, ops.count("insmod:brcmfmac"))
            self.assertEqual(2, ops.count("vendor:start"))
            self.assertEqual(2, ops.count("vendor:end"))
            self.assertLess(ops.index("vendor:end"), ops.index("vendor:start", ops.index("vendor:start") + 1))

    def test_wifi_up_down_order_and_refused_unload_keeps_power(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            result = h.run("wifi-up")
            self.assertEqual(0, result.returncode, result.stderr)
            ops = h.operations()
            self.assertLess(ops.index("insmod:brcmutil"), ops.index("insmod:brcmfmac"))
            self.assertIn("mac:invalidate wlan0", ops)
            self.assertTrue((h.radio / "wifi_power").read_text().strip() == "1")
            selected = h.root / "usr/data/.compas/wifi-firmware/selected.bin"
            self.assertEqual(str(h.root / "lib/firmware/wifi_bcm/cyw43438-7.46.58.35.bin"), os.readlink(selected))
            self.assertFalse((h.root / "lib/firmware/brcm/brcmfmac43430-sdio.bin").exists())
            result = h.run("wifi-down", RMMOD_FAIL="brcmfmac")
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("1", (h.radio / "wifi_power").read_text().strip())
            self.assertIn("mac:invalidate wlan0", h.operations())
            # A successful retry invalidates leases before unloading and only then drops power.
            result = h.run("wifi-down")
            self.assertEqual(0, result.returncode, result.stderr)
            ops = h.operations()
            self.assertLess(ops.index("mac:invalidate wlan0", ops.index("vendor:wifi_off.brcmfmac.vendor.sh")),
                            ops.index("rmmod:brcmfmac"))
            self.assertEqual("0", (h.radio / "wifi_power").read_text().strip())
            self.assertEqual(0, h.run("wifi-up").returncode)
            self.assertEqual(0, h.run("wifi-down").returncode)
            self.assertEqual("0", (h.radio / "wifi_power").read_text().strip())

    def test_suspend_marker_blocks_new_up_and_end_only_opens_gate_for_native_restore(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            self.assertEqual(0, h.run("wifi-up").returncode)
            self.assertEqual(0, h.run("suspend-prepare").returncode)
            marker = h.root / "var/run/compas-radio/suspending"
            self.assertTrue(marker.exists())
            self.assertNotEqual(0, h.run("wifi-up").returncode)
            self.assertEqual("0", (h.radio / "wifi_power").read_text().strip())
            self.assertEqual(0, h.run("suspend-end").returncode)
            self.assertFalse(marker.exists())
            self.assertEqual("0", (h.radio / "wifi_power").read_text().strip())

    def test_suspend_failure_keeps_marker_and_still_attempts_bluetooth_shutdown(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            self.assertEqual(0, h.run("wifi-up").returncode)
            result = h.run("suspend-prepare", RMMOD_FAIL="brcmfmac")
            self.assertNotEqual(0, result.returncode)
            self.assertTrue((h.root / "var/run/compas-radio/suspending").exists())
            self.assertIn("bt:suspend", h.operations())
            self.assertEqual("1", (h.radio / "wifi_power").read_text().strip())

    def test_bluetooth_shutdown_does_not_depend_on_board_identity(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            (h.radio / "identity_state").write_text("error\n")
            result = h.run("run-bt", "suspend")
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertIn("bt:suspend", h.operations())
            self.assertEqual("0", (h.root / "sys/class/rfkill/rfkill8/state").read_text().strip())

    def test_mac_invalidate_is_reversible_and_advances_generation(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            state = root / "state"
            net = root / "net/wlan0"
            net.mkdir(parents=True)
            (state).mkdir()
            (state / "status").write_text("ready\n")
            (state / "mac").write_text("02:11:22:33:44:55\n")
            (state / "generation.wlan0").write_text("4\n")
            (net / "address").write_text("02:11:22:33:44:55\n")
            env = os.environ.copy()
            env.update({"COMPAS_WIFI_MAC_STATE_DIR": str(state), "COMPAS_WIFI_MAC_SYS_CLASS_NET": str(root / "net"),
                        "COMPAS_WIFI_PROCESS_ROOT": "/proc", "COMPAS_WIFI_MAC_PROC_ROOT": "/proc"})
            manager = REPO / "firmware/overlay/usr/bin/compas-wifi-mac"
            first = subprocess.run(["/bin/sh", str(manager), "invalidate", "wlan0"], env=env,
                                   text=True, capture_output=True, timeout=5)
            self.assertEqual(0, first.returncode, first.stderr)
            self.assertEqual("5", (state / "generation.wlan0").read_text().strip())
            self.assertFalse((state / "status").exists())
            self.assertFalse((state / "mac").exists())
            self.assertFalse((state / "cancel").exists())
            self.assertFalse((state / "stopping").exists())
            (state / "status").write_text("ready\n")
            (state / "mac").write_text("02:11:22:33:44:55\n")
            second = subprocess.run(["/bin/sh", str(manager), "invalidate", "wlan0"], env=env,
                                    text=True, capture_output=True, timeout=5)
            self.assertEqual(0, second.returncode, second.stderr)
            self.assertEqual("6", (state / "generation.wlan0").read_text().strip())

    def test_missing_mac_generation_read_does_not_create_partial_state(self):
        manager_source = (REPO / "firmware/overlay/usr/bin/compas-wifi-mac").read_text()
        function = manager_source.split("generation_value() {", 1)[1].split("\n}\n", 1)[0]
        with tempfile.TemporaryDirectory() as td:
            generation = Path(td) / "generation.wlan0"
            # The function intentionally reads GENERATION_FILE from its environment.
            env = os.environ.copy()
            env["GENERATION_FILE"] = str(generation)
            result = subprocess.run(["/bin/sh", "-c", "generation_value() {" + function + "\n}\ngeneration_value"],
                                    env=env, text=True, capture_output=True, timeout=3)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual("0", result.stdout.strip())
            self.assertFalse(generation.exists())

    def test_real_mac_manager_recognizes_backend_process_lease(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            state = h.root / "var/run/compas-wifi-mac"
            state.mkdir(parents=True)
            (state / "status").write_text("ready\n")
            (state / "mac").write_text("02:11:22:33:44:55\n")
            (state / "generation.wlan0").write_text("3\n")
            iface = h.net / "wlan0"
            iface.mkdir()
            (iface / "address").write_text("02:11:22:33:44:55\n")
            (iface / "flags").write_text("0x0\n")
            env = os.environ.copy()
            (h.radio / "identity_state").write_text("pending\n")
            env.update({"COMPAS_RADIO_ROOT": str(h.root), "COMPAS_RADIO_PROC_ROOT": "/proc",
                        "COMPAS_RADIO_IDENTITY_WAIT": "30", "PATH": f"{h.bin}:/usr/bin:/bin"})
            process = subprocess.Popen(["/bin/sh", str(BACKEND), "wifi-up"], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                owner = h.root / "var/run/compas-radio/owner"
                for _ in range(100):
                    if owner.exists():
                        break
                    if process.poll() is not None:
                        stdout, stderr = process.communicate()
                        self.fail(f"backend exited before acquiring its lock: {stdout} {stderr}")
                    import time
                    time.sleep(0.02)
                self.assertTrue(owner.exists())
                owner_pid = int(owner.read_text().splitlines()[0])
                manager_env = os.environ.copy()
                manager_env.update({"COMPAS_WIFI_MAC_STATE_DIR": str(state),
                                   "COMPAS_WIFI_MAC_SYS_CLASS_NET": str(h.net),
                                   "COMPAS_WIFI_PROCESS_ROOT": "/proc", "COMPAS_WIFI_MAC_PROC_ROOT": "/proc",
                                   "COMPAS_WIFI_MAC_OFF_SCRIPT": "/bin/true",
                                   "COMPAS_WIFI_MAC_DOWN_SCRIPT": "/bin/true"})
                manager = REPO / "firmware/overlay/usr/bin/compas-wifi-mac"
                begin = subprocess.run(["/bin/sh", str(manager), "connection-begin", str(owner_pid)],
                                       env=manager_env, text=True, capture_output=True, timeout=5)
                self.assertEqual(0, begin.returncode, begin.stderr)
                start = subprocess.run(["/bin/sh", str(manager), "start"], env=manager_env,
                                       text=True, capture_output=True, timeout=5)
                self.assertEqual(0, start.returncode, start.stderr)
                self.assertTrue((state / "connection.lock").is_dir())
                end = subprocess.run(["/bin/sh", str(manager), "connection-end", str(owner_pid), "1"],
                                     env=manager_env, text=True, capture_output=True, timeout=5)
                self.assertEqual(0, end.returncode, end.stderr)
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=3)

    def test_real_manager_second_wifi_up_is_healthy_noop(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            manager = REPO / "firmware/overlay/usr/bin/compas-wifi-mac"
            installed_manager = h.root / "usr/bin/compas-wifi-mac"
            shutil.copyfile(manager, installed_manager)
            installed_manager.chmod(0o755)
            (h.root / "usr/data/macaddr.txt").write_text("02:11:22:33:44:55\n")
            vendor = h.root / "usr/libexec/compas/wifi_on.brcmfmac.vendor.sh"
            vendor.write_text('''#!/bin/sh
printf 'vendor:start\\n' >> "$RADIO_TEST_LOG"
"$COMPAS_WIFI_MAC_IFCONFIG" wlan0 up || return 1
"$COMPAS_WIFI_MAC_MANAGER" connection-prepare "$COMPAS_WIFI_CONNECTION_PID" || return 1
"$COMPAS_RADIO_WPA_CLI" -i wlan0 ping | grep -qx PONG || return 1
return 0
''')
            vendor.chmod(0o755)

            first = h.run("wifi-up")
            self.assertEqual(0, first.returncode, first.stderr)
            mac_state = h.root / "var/run/compas-wifi-mac"
            self.assertEqual("ready", (mac_state / "status").read_text().strip())
            self.assertTrue((h.net / "wlan0/flags").exists())
            self.assertEqual("02:11:22:33:44:55", (h.net / "wlan0/address").read_text().strip())
            before = h.operations().count("vendor:start")
            self.assertEqual(1, before)

            second = h.run("wifi-up")
            self.assertEqual(0, second.returncode, second.stderr)
            self.assertEqual(before, h.operations().count("vendor:start"))
            self.assertEqual("ready", (mac_state / "status").read_text().strip())
            self.assertTrue((h.root / "sys/module/brcmfmac").is_dir())
            self.assertEqual("1", (h.radio / "wifi_power").read_text().strip())

    def test_real_manager_stop_uses_private_off_script_under_control_lock(self):
        with tempfile.TemporaryDirectory() as td:
            h = RadioHarness(Path(td))
            manager = REPO / "firmware/overlay/usr/bin/compas-wifi-mac"
            installed_manager = h.root / "usr/bin/compas-wifi-mac"
            shutil.copyfile(manager, installed_manager)
            installed_manager.chmod(0o755)
            state = h.root / "var/run/compas-wifi-mac"
            state.mkdir(parents=True)
            (state / "status").write_text("ready\n")
            direct_off = h.root / "usr/libexec/compas/wifi_off.brcmfmac.vendor.sh"
            direct_off.parent.mkdir(parents=True, exist_ok=True)
            direct_off.write_text('#!/bin/sh\nprintf "private-off\\n" >> "$RADIO_TEST_LOG"\nexit 0\n')
            direct_off.chmod(0o755)
            env = os.environ.copy()
            env.update({"COMPAS_WIFI_MAC_STATE_DIR": str(state),
                        "COMPAS_WIFI_MAC_SYS_CLASS_NET": str(h.net),
                        "COMPAS_WIFI_MAC_PROC_ROOT": "/proc", "COMPAS_WIFI_PROCESS_ROOT": "/proc",
                        "COMPAS_WIFI_MAC_DOWN_SCRIPT": str(direct_off),
                        "COMPAS_WIFI_MAC_OFF_SCRIPT": str(direct_off),
                        "RADIO_TEST_LOG": str(h.log), "PATH": f"{h.bin}:/usr/bin:/bin"})
            result = subprocess.run(["/bin/sh", str(installed_manager), "stop"], env=env,
                                    text=True, capture_output=True, timeout=5)
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue((state / "stopping").exists())
            self.assertEqual(["private-off"], h.operations())


if __name__ == "__main__":
    unittest.main()
