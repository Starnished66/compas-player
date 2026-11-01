#!/usr/bin/env python3
"""Collect a privacy-conscious, read-only porting report from a stock player over ADB."""

import argparse
import datetime as dt
import json
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TIMEOUT = 10
LIMIT = 1024 * 1024


def redact(text: str, serial: str | None = None) -> str:
    """Remove common device identifiers and user media paths from captured text."""
    text = re.sub(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b', '<redacted-mac>', text)
    text = re.sub(r'(?im)^(\s*Serial\s*:\s*).+$', r'\1<redacted>', text)
    text = re.sub(r'(?im)^(\s*(?:[UP]:\s*)?(?:Uniq|Phys)\s*=).+$', r'\1<redacted>', text)
    text = re.sub(r'\b(sn_no|bt_mac|wifi_mac|serialno|androidboot\.serialno)=\S+', r'\1=<redacted>', text)
    if serial:
        text = text.replace(serial, "<redacted-device>")
    # Mount output can expose personal file names and media layout. Keep only
    # mount identity and filesystem type/options, dropping the source/target.
    lines = []
    for line in text.splitlines():
        if line.startswith("/dev/") and " on " in line and " type " in line:
            match = re.match(r"^(\S+) on \S+ type (\S+)(.*)$", line)
            if match:
                line = f"{match.group(1)} on <mount> type {match.group(2)}{match.group(3)}"
        lines.append(line)
    return "\n".join(lines).rstrip() + ("\n" if text.endswith("\n") else "")


def adb_command(adb: str, serial: str | None, shell: str) -> list[str]:
    cmd = [adb]
    if serial:
        cmd += ["-s", serial]
    return cmd + ["shell", shell]


def run_capture(adb: str, serial: str | None, command: str) -> tuple[str, str, float]:
    start = time.monotonic()
    try:
        # Spool output to disk so a noisy/broken device cannot consume unbounded RAM.
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            proc = subprocess.Popen(adb_command(adb, serial, command), stdout=out,
                                    stderr=err, close_fds=True)
            try:
                proc.wait(timeout=TIMEOUT)
                status = "ok" if proc.returncode == 0 else f"exit_{proc.returncode}"
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                status = "timeout"
            out.seek(0)
            data = out.read(LIMIT + 1)
            err.seek(0)
            error = err.read(4096).decode("utf-8", "replace").strip()
            truncated = len(data) > LIMIT
            body = data[:LIMIT].decode("utf-8", "replace")
            if truncated:
                body += "\n[output truncated at 1 MiB]\n"
            if status != "ok" and error:
                body += f"\n[adb: {error}]\n"
            return redact(body, serial), ("truncated" if truncated and status == "ok" else status), round(time.monotonic() - start, 3)
    except OSError as exc:
        return redact(f"[collector error: {exc}]\n", serial), "error", round(time.monotonic() - start, 3)


COMMANDS = [
    ("system", "uname -a"),
    ("cpuinfo", "cat /proc/cpuinfo"),
    ("memory", "cat /proc/meminfo"),
    ("kernel", "cat /proc/version"),
    ("mounts", "awk '{print \"<source> <mount>\", $3}' /proc/mounts"),
    ("framebuffer", "for f in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride; do if [ -r \"$f\" ]; then echo \"$f:\"; cat \"$f\"; else echo \"$f: unavailable\"; fi; done"),
    ("input", "cat /proc/bus/input/devices"),
    ("input_sysfs", "for d in /sys/class/input/event*; do [ -e \"$d\" ] || continue; echo \"$d\"; cat \"$d/device/name\" 2>/dev/null; cat \"$d\"/device/capabilities/* 2>/dev/null; done"),
    ("alsa", "cat /proc/asound/cards /proc/asound/pcm"),
    ("kernel_config", "if [ -r /proc/config.gz ]; then zcat /proc/config.gz; else echo '/proc/config.gz unavailable'; exit 1; fi"),
    ("partitions", "cat /proc/mtd /proc/partitions"),
    ("modules", "lsmod"),
    ("module_files", "for d in /module_driver /lib/modules /usr/modules; do if [ -d \"$d\" ]; then echo \"$d\"; ls -1 \"$d\"; else echo \"$d: unavailable\"; fi; done"),
    ("system_listings", "for d in /etc/init.d /dev/input /dev/snd; do if [ -d \"$d\" ]; then echo \"$d\"; ls -1 \"$d\"; else echo \"$d: unavailable\"; fi; done; if command -v file >/dev/null 2>&1; then file /bin/busybox /usr/bin/hiby_player /usr/bin/compas_player 2>/dev/null; else echo 'file command unavailable'; fi"),
    ("device_tree", "if [ -r /proc/device-tree/model ]; then tr '\\000' '\\n' < /proc/device-tree/model; else echo '/proc/device-tree/model unavailable'; exit 1; fi"),
    ("properties", "for p in ro.product.model ro.product.manufacturer ro.product.board ro.product.cpu.abi ro.build.version.release ro.build.version.sdk; do printf '%s=' \"$p\"; getprop \"$p\"; done"),
]


OUTPUT_COMMANDS = [
    ("audio_devices", "for f in /proc/asound/cards /proc/asound/pcm /proc/asound/devices /proc/asound/card*/id /proc/asound/card*/stream* /proc/asound/card*/pcm*p/info; do [ -r \"$f\" ] || continue; echo \"$f:\"; cat \"$f\"; done"),
    ("mixer_routes", "if command -v amixer >/dev/null 2>&1; then for d in /dev/snd/controlC*; do [ -e \"$d\" ] || continue; n=${d##*controlC}; echo \"CARD $n\"; amixer -D \"hw:$n\" contents; done; elif command -v tinymix >/dev/null 2>&1; then tinymix; else echo 'No supported mixer inspection tool'; exit 1; fi"),
    ("jack_interfaces", "for f in /sys/class/switch/*/name /sys/class/switch/*/state /sys/class/extcon/*/name /sys/class/extcon/*/state /sys/class/sound/hwC*D*/vendor_name /sys/class/sound/hwC*D*/chip_name; do [ -r \"$f\" ] || continue; echo \"$f:\"; cat \"$f\"; done"),
    ("bluetooth_outputs", "if command -v hciconfig >/dev/null 2>&1; then hciconfig -a; fi; if command -v bluealsa-cli >/dev/null 2>&1; then bluealsa-cli list-pcms; fi"),
    ("sound_sysfs", "for f in /sys/class/sound/card*/id /sys/class/sound/pcm*/dev /sys/class/sound/pcm*/uevent; do [ -r \"$f\" ] || continue; echo \"$f:\"; cat \"$f\"; done"),
    ("usb_interfaces", "for d in /sys/bus/usb/devices/*; do [ -d \"$d\" ] || continue; for a in idVendor idProduct manufacturer product bDeviceClass bInterfaceClass bInterfaceSubClass; do f=\"$d/$a\"; [ -r \"$f\" ] || continue; echo \"$f:\"; cat \"$f\"; done; done"),
]


def output_snapshot(adb: str, serial: str) -> dict:
    snapshot = {}
    for name, command in OUTPUT_COMMANDS:
        body, status, duration = run_capture(adb, serial, command)
        if status == "ok" and not body.strip():
            status = "unavailable"
        snapshot[name] = {"status": status, "duration_seconds": duration, "evidence": body}
    return snapshot


def collect_output_mapping(adb: str, serial: str) -> dict:
    print("\nMap physical outputs (headphone, balanced, line out, coaxial, USB DAC, Bluetooth).")
    print("Enter each output's name/connector. Leave empty when all outputs are listed.")
    print("Connect a suitable accessory if available; normal stock-player routing may change.")
    entries = []
    try:
        while True:
            label = input("Output name (empty to finish): ").strip()
            if not label:
                break
            accessory = input("Accessory connected? Describe it, or leave empty if unavailable: ").strip()
            input("Press Enter when ready to capture this output's interfaces: ")
            entries.append({"label": redact(label, serial), "accessory": redact(accessory, serial),
                            "verification": "user_identified; playback untested",
                            "snapshot": output_snapshot(adb, serial)})
    except (EOFError, KeyboardInterrupt):
        return {"status": "interrupted", "outputs": entries}
    return {"status": "complete", "outputs": entries,
            "note": "Connector names are user supplied. Snapshots do not prove signal routing or playback."}


def devices(adb: str) -> tuple[list[tuple[str, str]], str | None]:
    try:
        result = subprocess.run([adb, "devices", "-l"], capture_output=True,
                                text=True, timeout=TIMEOUT, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return [], f"Could not run adb devices -l: {exc}"
    if result.returncode:
        return [], f"adb devices -l failed: {result.stderr.strip() or result.returncode}"
    found = []
    for line in result.stdout.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 2:
            found.append((fields[0], fields[1]))
    return found, None


def select_device(adb: str, serial_arg: str | None) -> tuple[str | None, str | None]:
    """Pick one authorized device; return (serial, error_message)."""
    all_devices, error = devices(adb)
    if error:
        return None, error
    if serial_arg:
        selected = [item for item in all_devices if item[0] == serial_arg]
        if len(selected) != 1 or selected[0][1] != "device":
            state = selected[0][1] if selected else "not found"
            return None, f"--serial does not identify one authorized device (state: {state})"
        return selected[0][0], None
    authorized = [item for item in all_devices if item[1] == "device"]
    if len(authorized) != 1:
        return None, f"Expected exactly one authorized ADB device; found {len(authorized)}. Use --serial to select one."
    return authorized[0][0], None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ADB device serial (not written to report)")
    parser.add_argument("--output", default=None, help="host report directory (default: ./device-report-UTCtimestamp)")
    parser.add_argument("--adb", default=None, help="path to adb executable (default: PATH lookup)")
    parser.add_argument("--skip-interactive", action="store_true", help="skip physical button/output mapping")
    args = parser.parse_args()
    adb = args.adb or shutil.which("adb")
    if not adb:
        print("adb was not found; install Android platform-tools or use --adb", file=sys.stderr)
        return 2
    serial, error = select_device(adb, args.serial)
    if error:
        print(error, file=sys.stderr)
        return 2

    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = Path(args.output) if args.output else Path(f"./device-report-{stamp}")
    try:
        outdir.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        print(f"Cannot create report directory: {exc}", file=sys.stderr)
        return 2
    manifest = {"generated_utc": stamp, "commands": {}}
    print(f"Collecting read-only device information into {outdir}")
    for name, command in COMMANDS + OUTPUT_COMMANDS:
        print(f"  {name}...", flush=True)
        body, status, duration = run_capture(adb, serial, command)
        (outdir / f"{name}.txt").write_text(body, encoding="utf-8")
        if status == "ok" and not body.strip():
            status = "unavailable"
        manifest["commands"][name] = {"status": status, "duration_seconds": duration}
    if args.skip_interactive or not sys.stdin.isatty():
        reason = "requested" if args.skip_interactive else "requires interactive terminal"
        button_map = {"status": "skipped", "reason": reason}
        output_map = {"status": "skipped", "reason": reason}
        print(f"Interactive button/output mapping skipped: {reason}")
    else:
        from device_button_mapping import collect_button_mapping
        button_map = collect_button_mapping(adb, serial, redact)
        output_map = collect_output_mapping(adb, serial)
    for name, mapping in [("button_mapping", button_map), ("output_mapping", output_map)]:
        (outdir / f"{name}.json").write_text(json.dumps(mapping, indent=2) + "\n", encoding="utf-8")
        manifest[name] = {"status": mapping["status"]}
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (outdir / "README.md").write_text(
        "# Device porting report\n\n"
        f"Generated: {stamp} UTC\n\n"
        "This report was collected using read-only ADB shell commands. It contains one text file per section. "
        "Unavailable sections are marked in their file and in `manifest.json`. Device serials, MAC addresses, "
        "input device unique identifiers, and mount paths are omitted or redacted.\n\n"
        "## Sections\n\n" + "\n".join(f"- `{name}.txt`" for name, _ in COMMANDS + OUTPUT_COMMANDS) + "\n- `button_mapping.json`: named physical controls and captured events\n- `output_mapping.json`: user-identified connectors and interface snapshots\n\nOutput inventories and snapshots do not verify playback or prove routing.\n",
        encoding="utf-8")
    print(f"Report complete: {outdir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
