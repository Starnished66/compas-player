#!/usr/bin/env python3
"""Read-only hardware probe for porting CompasOS to the TempoTec V3 Blaze."""

import argparse
import datetime as dt
import difflib
import json
import shutil
import statistics
import sys
import time
from pathlib import Path

from collect_device_info import OUTPUT_COMMANDS, redact, run_capture, select_device
from device_button_mapping import capture_events, collect_button_mapping

VERSION = "v3_blaze_probe 1"
ADB_HINT = ("If no device is found, the stock setting System settings > USB device mode > Dock "
            "should enable ADB. Select it, reconnect the USB cable and authorize this computer.")
INTRO = (
    "This tool collects hardware information from a TempoTec V3 Blaze over ADB so CompasOS can be "
    "ported to it. It is read-only: it never changes settings, writes to the device or installs "
    "anything. It runs automatic probes, then asks you to press buttons, tap the screen, plug in "
    "audio cables and change a few stock audio settings while it listens. It takes about 10 minutes "
    "and any step can be skipped. The result is a folder and a zip file you can review and send back."
)

MODULES = ["gt9xx_touch", "keyboard_adc", "keyboard_gpio_add", "sa_sound_switch", "codec_ak4493_dual",
           "x1600_tempotec_v3a_sound_card", "leds_sgm31324_add", "axp2101", "pwm_backlight", "cywdhd"]
MOD_LIST = " ".join(MODULES)
MIXER = dict(OUTPUT_COMMANDS)["mixer_routes"]
SWITCHES = ("for d in /sys/class/switch/*; do [ -d \"$d\" ] || continue; "
            "echo \"$d name=$(cat $d/name 2>/dev/null) state=$(cat $d/state 2>/dev/null)\"; done")
PLAYBACK = ("for f in /proc/asound/card*/pcm*p/sub*/hw_params /proc/asound/card*/pcm*p/sub*/status; "
            "do echo \"$f:\"; cat \"$f\"; done")


def sysfs_attrs(base: str, attrs: list[str]) -> str:
    """Shell loop printing the given attributes of every entry under base."""
    return (f"for d in {base}/*; do [ -d \"$d\" ] || continue; echo \"$d:\"; "
            f"for a in {' '.join(attrs)}; do if [ -r \"$d/$a\" ]; then echo \"$a=$(cat $d/$a 2>/dev/null)\"; fi; done; done")


def labelled(*pairs: tuple[str, str]) -> str:
    """Join commands, each preceded by an '== label ==' line."""
    return "; ".join(f"echo '== {label} =='; {cmd} 2>&1" for label, cmd in pairs)


def module_params(mods: str) -> str:
    return (f"for m in {mods}; do for f in /sys/module/$m/parameters/*; do [ -f \"$f\" ] || continue; "
            "echo \"$m.${f##*/}=$(cat $f 2>/dev/null)\"; done; done")


STATIC_COMMANDS = [
    ("identity", labelled(("uname -a", "uname -a"), ("/proc/version", "cat /proc/version"),
                          ("/proc/cmdline", "cat /proc/cmdline"),
                          ("firmware version", "grep -o '\"version\":\"[^\"]*\"' /usr/resource/config_2025.json /usr/resource/config.json"),
                          ("/proc/cpuinfo", "cat /proc/cpuinfo"))),
    ("memory", "cat /proc/meminfo"),
    ("modules", labelled(("lsmod", "lsmod"), ("/module_driver", "ls -1 /module_driver"))),
    ("driver_scripts", f"for m in {MOD_LIST}; do f=/module_driver/$m.sh; [ -r \"$f\" ] || continue; echo \"== $f ==\"; cat \"$f\"; done"),
    ("module_params", module_params(MOD_LIST)),
    ("input_devices", "cat /proc/bus/input/devices"),
    ("framebuffer", "for f in /sys/class/graphics/fb0/name /sys/class/graphics/fb0/modes /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride; do if [ -r \"$f\" ]; then echo \"$f:\"; cat \"$f\"; else echo \"$f: unavailable\"; fi; done"),
    ("backlight", sysfs_attrs("/sys/class/backlight", ["brightness", "max_brightness", "actual_brightness"])),
    ("leds", sysfs_attrs("/sys/class/leds", ["brightness", "max_brightness", "trigger"])),
    ("switches", SWITCHES),
    ("power_supply", "for d in /sys/class/power_supply/*; do [ -r \"$d/uevent\" ] || continue; echo \"$d:\"; cat \"$d/uevent\"; done"),
    ("alsa", "for f in /proc/asound/cards /proc/asound/pcm /proc/asound/devices /proc/asound/card*/id; do [ -r \"$f\" ] || continue; echo \"$f:\"; cat \"$f\"; done"),
    ("mixer", MIXER),
    ("earpods", "if [ -e /sys/devices/platform/earpods_adc ]; then echo present; else echo absent; fi"),
    ("wireless", labelled(("identity_state", "cat /sys/devices/platform/bcm_wlbt_power/identity_state"),
                          ("/sys/class/net", "ls /sys/class/net"))),
    ("usb", labelled(("/sys/class/udc", "ls /sys/class/udc"), ("/sys/kernel/config/usb_gadget", "ls /sys/kernel/config/usb_gadget"),
                     ("tcs1421_cfg", "cat /sys/devices/platform/tcs1421/tcs1421_cfg"))),
]

# (label, expected evdev name, expected code)
BUTTONS = [("Power (short press only)", "KEY_POWER", 116), ("Play/Pause", "KEY_PLAYPAUSE", 164),
           ("Next", "KEY_NEXTSONG", 163), ("Previous", "KEY_PREVIOUSSONG", 165),
           ("Volume up", "KEY_VOLUMEUP", 115), ("Volume down", "KEY_VOLUMEDOWN", 114)]
KEY_NAMES = {105: "KEY_LEFT", 106: "KEY_RIGHT", 114: "KEY_VOLUMEDOWN", 115: "KEY_VOLUMEUP", 116: "KEY_POWER",
             163: "KEY_NEXTSONG", 164: "KEY_PLAYPAUSE", 165: "KEY_PREVIOUSSONG"}
KEY_LEFT, KEY_RIGHT = 105, 106
ABS_X, ABS_Y, ABS_MT_X, ABS_MT_Y = 0x00, 0x01, 0x35, 0x36
PANEL_W, PANEL_H = 320, 480
TOUCH_TOLERANCE = 0.25
JACKS = ["3.5 mm headphones", "4.4 mm balanced headphones", "3.5 mm optical (mini-Toslink) cable"]
SETTING_SUGGESTIONS = ["gain Low / Middle / High", "each digital filter", "line out on 3.5 mm and on 4.4 mm",
                       "any output or DAC option in the stock settings"]


def key_name(code: int) -> str:
    return KEY_NAMES.get(code, str(code))


def confirm_start(title: str, lines: list[str]) -> bool:
    """Print step instructions; return False if the user skips the step."""
    print(f"\n=== {title} ===")
    for line in lines:
        print(line)
    return not input("Press Enter to start, or type s to skip: ").strip().lower().startswith("s")


def run_probes(adb: str, serial: str, outdir: Path) -> tuple[dict, dict]:
    """Run the static probes; return (manifest entries, text by name)."""
    entries, texts = {}, {}
    try:
        for name, command in STATIC_COMMANDS:
            print(f"  {name}...", flush=True)
            body, status, duration = run_capture(adb, serial, command)
            (outdir / f"{name}.txt").write_text(body, encoding="utf-8")
            if status == "ok" and not body.strip():
                status = "unavailable"
            entries[name] = {"status": status, "duration_seconds": duration}
            texts[name] = body
    except KeyboardInterrupt:
        print("Interrupted; skipping the remaining automatic probes.")
        for name, _ in STATIC_COMMANDS:
            entries.setdefault(name, {"status": "interrupted", "duration_seconds": 0})
    return entries, texts


def diff_text(before: str, after: str, names: tuple[str, str]) -> str:
    return "".join(f"{line}\n" for line in difflib.unified_diff(before.splitlines(), after.splitlines(),
                                                                names[0], names[1], lineterm=""))


def snapshot(adb: str, serial: str, switches_too: bool = True) -> str:
    """Switch states, mixer controls and sa_sound_switch parameters as one text."""
    parts = [("switches", SWITCHES)] if switches_too else []
    parts += [("mixer", MIXER)]
    if switches_too:
        parts.append(("sa_sound_switch parameters", module_params("sa_sound_switch")))
    return "".join(f"== {label} ==\n{run_capture(adb, serial, cmd)[0]}" for label, cmd in parts)


def clean_events(events: list[dict], serial: str) -> list[dict]:
    for event in events:
        event["input"] = redact(event["input"], serial)
    return events


def step_buttons(adb: str, serial: str, result: dict) -> None:
    if not confirm_start("A. Buttons", [
            "You will press each button once, then hold Volume up. Use a short press for Power,",
            "and press it again afterwards if the screen turned off. Unplug headphones first:",
            "holding Volume up raises the stock player's volume.",
            "The stock player still reacts to the presses; that is normal."]):
        result.update(status="skipped", reason="user skipped")
        return
    result.update(buttons=[], extra_controls=None)
    seen_keys: set[int] = set()
    for label, expected_name, expected in BUTTONS:
        input(f"Press Enter, then press {label} once (listening up to 8 seconds): ")
        events, status = capture_events(adb, serial)
        clean_events(events, serial)
        codes = {e["code"] for e in events if e["type"] == 1}
        seen_keys |= codes
        verdict = "match" if expected in codes else ("different" if codes else "no_events")
        print(f"  {verdict}")
        result["buttons"].append({"label": label, "expected": f"{expected_name} {expected}", "verdict": verdict,
                                  "other_codes": sorted(codes - {expected}), "capture_status": status, "events": events})
    input("Press Enter, then hold Volume up for about 2 seconds (listening for 6 seconds): ")
    # include_abs only disables the quiet-gap early stop so the whole hold is seen.
    events, status = capture_events(adb, serial, seconds=6, include_abs=True)
    events = clean_events([e for e in events if e["type"] == 1], serial)
    seen_keys |= {e["code"] for e in events}
    repeat = any(e["value"] == 2 for e in events)
    print(f"  autorepeat (value 2): {'seen' if repeat else 'not seen'}")
    result["volume_up_hold"] = {"autorepeat_seen": repeat, "capture_status": status, "events": events}
    result["rotary_keys_seen"] = sorted(key_name(c) for c in seen_keys & {KEY_LEFT, KEY_RIGHT})
    if input("Press Enter to map any extra physical controls (e.g. a switch), or type s to skip: ").strip().lower().startswith("s"):
        return
    result["extra_controls"] = collect_button_mapping(adb, serial, redact)


def axis_value(events: list[dict], codes: tuple[int, int]) -> tuple[float | None, list[int]]:
    """Median of the distinct observed values for the first of codes that was seen."""
    for code in codes:
        values = [e["value"] for e in events if e["type"] == 3 and e["code"] == code]
        if values:
            return statistics.median(values), values
    return None, []


def touch_point(events: list[dict]) -> dict:
    """Observed tap position: median of the distinct values per axis, plus raw min/max."""
    point = {}
    for axis, codes in (("x", (ABS_MT_X, ABS_X)), ("y", (ABS_MT_Y, ABS_Y))):
        value, values = axis_value(events, codes)
        point[axis] = value
        point[f"{axis}_range"] = [min(values), max(values)] if values else None
    return point


def touch_verdict(tl: dict, br: dict) -> str:
    if None in (tl["x"], tl["y"], br["x"], br["y"]):
        return "unclear"
    tol = TOUCH_TOLERANCE
    if (tl["x"] <= tol * PANEL_W and tl["y"] <= tol * PANEL_H
            and br["x"] >= (1 - tol) * PANEL_W and br["y"] >= (1 - tol) * PANEL_H):
        return "ok"
    dx, dy = br["x"] - tl["x"], br["y"] - tl["y"]
    if abs(dx) > abs(dy):
        return "axes_swapped"
    flags = (["x_flipped"] if dx < 0 else []) + (["y_flipped"] if dy < 0 else [])
    return "+".join(flags) or "unclear"


def step_touch(adb: str, serial: str, result: dict) -> None:
    if not confirm_start("B. Touch screen", [
            "Wake the screen if it is off. You will tap two corners of the screen.",
            "Expected after the stock driver's axis flip:",
            f"top-left near (0, 0), bottom-right near ({PANEL_W - 1}, {PANEL_H - 1})."]):
        result.update(status="skipped", reason="user skipped")
        return
    points = {}
    result["taps"] = points
    for key, prompt in (("top_left", "Tap the top-left corner once"), ("bottom_right", "Tap the bottom-right corner once")):
        input(f"Press Enter, then: {prompt} (listening for 6 seconds): ")
        events, status = capture_events(adb, serial, seconds=6, include_abs=True)
        point = touch_point(events)
        point.update(capture_status=status, position_method="median of distinct observed values",
                     events=clean_events([e for e in events if e["type"] == 3], serial))
        points[key] = point
        print(f"  observed x={point['x']} y={point['y']}")
    result["verdict"] = touch_verdict(points["top_left"], points["bottom_right"])
    result["expected"] = {"top_left": [0, 0], "bottom_right": [PANEL_W - 1, PANEL_H - 1], "tolerance": TOUCH_TOLERANCE}
    print(f"  touch verdict: {result['verdict']}")


def step_jacks(adb: str, serial: str, result: dict) -> None:
    if not confirm_start("C. Audio jacks", [
            "Unplug everything from the audio jacks (keep USB connected). You will then plug in",
            "each cable you have, one at a time, and unplug it again."]):
        result.update(status="skipped", reason="user skipped")
        return
    baseline = snapshot(adb, serial)
    result.update(baseline=baseline, jacks=[])
    for jack in JACKS:
        if not input(f"Do you have {jack}? [y/N]: ").strip().lower().startswith("y"):
            result["jacks"].append({"label": jack, "status": "skipped", "reason": "not available"})
            continue
        entry = {"label": jack, "status": "complete"}
        result["jacks"].append(entry)
        input(f"Press Enter, then plug in {jack} within 10 seconds: ")
        events, status = capture_events(adb, serial, seconds=10)
        entry["switch_events"] = clean_events([e for e in events if e["type"] == 5], serial)
        entry["capture_status"] = status
        time.sleep(1.0)
        after = snapshot(adb, serial)
        entry["diff_vs_baseline"] = diff_text(baseline, after, ("baseline", jack))
        print(f"  {len(entry['switch_events'])} switch events, diff {'present' if entry['diff_vs_baseline'] else 'empty'}")
        input(f"Unplug {jack} and press Enter: ")


def step_settings(adb: str, serial: str, result: dict) -> None:
    if not confirm_start("D. Stock audio settings to mixer mapping", [
            "Plug in headphones. After each stock setting change the tool records the mixer state.",
            "Suggestions: " + "; ".join(SETTING_SUGGESTIONS) + "."]):
        result.update(status="skipped", reason="user skipped")
        return
    previous = snapshot(adb, serial, switches_too=False)
    result.update(start_mixer=previous, captures=[])
    while True:
        label = input("Change one stock audio setting, then type its name and value (empty to finish): ").strip()
        if not label:
            return
        label = redact(label, serial)
        current = snapshot(adb, serial, switches_too=False)
        result["captures"].append({"setting": label, "mixer": current,
                                   "diff_vs_previous": diff_text(previous, current, ("previous", label))})
        previous = current


def step_playback(adb: str, serial: str, result: dict) -> None:
    if not confirm_start("E. Playback formats", [
            "Play tracks in the stock player, one format at a time. Include a DSD file if you have one."]):
        result.update(status="skipped", reason="user skipped")
        return
    result["captures"] = []
    while True:
        label = input("Start playing a track in the stock player, then describe its format, e.g. FLAC 44.1 kHz 16-bit, "
                      "FLAC 96 kHz 24-bit, DSD64 (empty to finish): ").strip()
        if not label:
            return
        result["captures"].append({"format": redact(label, serial),
                                   "pcm": run_capture(adb, serial, PLAYBACK)[0],
                                   "mixer": run_capture(adb, serial, MIXER)[0]})


STEPS = [("buttons", step_buttons), ("touch", step_touch), ("jacks", step_jacks),
         ("settings", step_settings), ("playback", step_playback)]


def run_step(name: str, fn, adb: str, serial: str) -> dict:
    """Run one interactive step; Ctrl+C or EOF keeps what was collected so far."""
    result = {"status": "complete"}
    try:
        fn(adb, serial, result)
    except (EOFError, KeyboardInterrupt):
        print("\nStep interrupted; continuing with the report.")
        result["status"] = "interrupted"
    return result


def next_line(text: str, prefix: str) -> str:
    """Line after the first line starting with prefix, or 'unknown'."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(prefix) and i + 1 < len(lines):
            return lines[i + 1].strip()
    return "unknown"


def build_readme(stamp: str, texts: dict, manifest: dict, steps: dict) -> str:
    identity = texts.get("identity", "")
    kernel = next_line(identity, "== uname -a ==")
    firmware = next_line(identity, "== firmware version ==")
    mem = next((l.split(":", 1)[1].strip() for l in texts.get("memory", "").splitlines() if l.startswith("MemTotal")), "unknown")
    fb = next_line(texts.get("framebuffer", ""), "/sys/class/graphics/fb0/virtual_size")
    earpods = texts.get("earpods", "").strip() or "unknown"
    mixer = texts.get("mixer", "")
    out = [f"# TempoTec V3 Blaze probe report\n\nGenerated: {stamp} UTC by {VERSION}\n",
           "Collected with read-only ADB commands. Serials and MAC addresses are redacted. "
           "Nothing here proves that playback or a given route works.\n",
           "## Device basics\n", f"- Kernel: {kernel}", f"- Stock firmware: {firmware}", f"- MemTotal: {mem}",
           f"- Framebuffer virtual_size: {fb} (panel expected {PANEL_W}x{PANEL_H})",
           f"- earpods_adc node: {earpods} (present on R3 II 2025)\n"]
    diffs = []
    if earpods == "absent":
        diffs.append("No inline remote node (earpods_adc absent), as expected; the R3 II has one.")
    elif earpods == "present":
        diffs.append("earpods_adc is present, unlike the expected V3 Blaze layout.")
    if "AK4493" in mixer:
        diffs.append("AK4493 mixer controls found (dual AK4493 instead of the R3 II ES9219).")
    if "Output Port Switch" in mixer:
        diffs.append("Sound card exposes 'Output Port Switch'; check its enum items in mixer.txt.")
    sw = [l for l in texts.get("switches", "").splitlines() if l.strip()]
    if sw:
        diffs.append(f"{len(sw)} switch node(s) under /sys/class/switch; see switches.txt.")

    out.append("## Buttons\n")
    b = steps["buttons"]
    if b["status"] == "skipped" or "buttons" not in b:
        out.append(f"Step {b['status']}.\n")
    else:
        out += [f"Step status: {b['status']}\n", "| Button | Expected | Observed | Verdict |", "| --- | --- | --- | --- |"]
        for item in b["buttons"]:
            seen = sorted({key_name(e["code"]) for e in item["events"] if e["type"] == 1})
            out.append(f"| {item['label']} | {item['expected']} | {', '.join(seen) or 'none'} | {item['verdict']} |")
        hold = b.get("volume_up_hold")
        if hold:
            out.append(f"\nVolume up hold: autorepeat (value 2) {'seen' if hold['autorepeat_seen'] else 'not seen'}.")
        rotary = b.get("rotary_keys_seen", [])
        if rotary:
            diffs.append(f"{', '.join(rotary)} seen; a rotary volume ring may be present as on the R3 II.")
        elif "rotary_keys_seen" in b:
            diffs.append("No KEY_LEFT/KEY_RIGHT seen; volume is on keys, not the R3 II rotary ring.")
        bad = [i["label"] for i in b["buttons"] if i["verdict"] != "match"]
        if bad:
            diffs.append(f"Buttons not matching the stock firmware codes: {', '.join(bad)}.")
        extra = b.get("extra_controls")
        if extra:
            out.append(f"\nExtra controls mapped: {len(extra.get('mappings', []))} (status {extra['status']}).")
        out.append("")

    out.append("## Touch\n")
    t = steps["touch"]
    if "verdict" in t:
        tl, br = t["taps"]["top_left"], t["taps"]["bottom_right"]
        out.append(f"Verdict: {t['verdict']}. Top-left observed ({tl['x']}, {tl['y']}), "
                   f"bottom-right observed ({br['x']}, {br['y']}). Raw ranges: TL x {tl['x_range']} y {tl['y_range']}, "
                   f"BR x {br['x_range']} y {br['y_range']}.\n")
        diffs.append("Touch orientation matches expectation." if t["verdict"] == "ok"
                     else f"Touch orientation verdict '{t['verdict']}'; check touch.json.")
    else:
        out.append(f"Step {t['status']}.\n")

    out.append("## Jacks\n")
    j = steps["jacks"]
    if "jacks" in j:
        for entry in j["jacks"]:
            if entry["status"] == "skipped":
                out.append(f"- {entry['label']}: not tested ({entry['reason']})")
                continue
            events = entry.get("switch_events", [])
            changed = bool(events) or bool(entry.get("diff_vs_baseline"))
            out.append(f"- {entry['label']}: {len(events)} switch events, "
                       f"switch/mixer change {'seen' if changed else 'not seen'}")
            if "optical" in entry["label"] and changed:
                diffs.append("Optical cable produced a switch or mixer change (optical output detection).")
    else:
        out.append(f"Step {j['status']}.")
    out.append("")

    for key, title, field in (("settings", "Stock settings captures", "captures"), ("playback", "Playback captures", "captures")):
        s = steps[key]
        out.append(f"## {title}\n")
        out.append(f"Status {s['status']}; {len(s.get(field, []))} captures.\n")

    out.append("## Differences vs R3 II 2025 to check\n")
    out += [f"- {d}" for d in diffs] or ["- Nothing derived (steps skipped)."]
    out += ["", "## Files\n"] + [f"- `{n}.txt`: {m['status']}" for n, m in manifest["probes"].items()]
    out += [f"- `{n}.json`: {steps[n]['status']}" for n, _ in STEPS]
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="ADB device serial (not written to report)")
    parser.add_argument("--output", default=None, help="host report directory (default: ./v3-blaze-report-UTCtimestamp)")
    parser.add_argument("--adb", default=None, help="path to adb executable (default: PATH lookup)")
    parser.add_argument("--skip-interactive", action="store_true", help="only run the automatic probes")
    args = parser.parse_args()
    adb = args.adb or shutil.which("adb")
    if not adb:
        print("adb was not found; install Android platform-tools or use --adb", file=sys.stderr)
        return 2
    serial, error = select_device(adb, args.serial)
    if error or not serial:
        print(error, file=sys.stderr)
        print(ADB_HINT, file=sys.stderr)
        return 2
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    outdir = Path(args.output) if args.output else Path(f"./v3-blaze-report-{stamp}")
    try:
        outdir.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        print(f"Cannot create report directory: {exc}", file=sys.stderr)
        return 2
    print(INTRO)
    print(f"\nCollecting read-only device information into {outdir}")
    probes, texts = run_probes(adb, serial, outdir)

    interactive = not args.skip_interactive and sys.stdin.isatty()
    reason = "requested" if args.skip_interactive else "requires interactive terminal"
    steps = {}
    for name, fn in STEPS:
        steps[name] = run_step(name, fn, adb, serial) if interactive else {"status": "skipped", "reason": reason}
        (outdir / f"{name}.json").write_text(json.dumps(steps[name], indent=2) + "\n", encoding="utf-8")
    if not interactive:
        print(f"Interactive steps skipped: {reason}")

    manifest = {"generated_utc": stamp, "script": f"{Path(__file__).name} ({VERSION})", "probes": probes,
                "steps": {n: {"status": s["status"], **({"reason": s["reason"]} if "reason" in s else {})}
                          for n, s in steps.items()}}
    (outdir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (outdir / "README.md").write_text(build_readme(stamp, texts, manifest, steps), encoding="utf-8")
    archive = shutil.make_archive(str(outdir), "zip", root_dir=outdir.parent, base_dir=outdir.name)
    print(f"\nReport folder: {outdir.resolve()}")
    print(f"Zip file: {Path(archive).resolve()}")
    print("Send the zip file to the CompasOS developer. Review it first if you like; serials and MAC addresses are redacted.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
