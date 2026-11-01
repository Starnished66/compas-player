"""Interactive, read-only capture of physical input events over ADB."""

from __future__ import annotations

import re
import subprocess
import threading
import time
from queue import Empty, Full, Queue

CAPTURE_SECONDS = 8.0
RELEASE_GRACE_SECONDS = 0.6
MAX_QUEUED_LINES = 4096
LINE_RE = re.compile(
    r"^\s*\[?\s*\d+(?:\.\d+)?\]?\s+(/dev/input/event\d+):\s*"
    r"([0-9a-fA-F]{4})\s+([0-9a-fA-F]{4})\s+([0-9a-fA-F]{8})\s*$"
)


def parse_event_line(line: str, include_abs: bool = False) -> dict | None:
    """Parse one numeric getevent -t line; discard everything else.

    include_abs also keeps EV_ABS events and BTN_TOUCH (for touch captures).
    """
    match = LINE_RE.match(line)
    if match:
        source = match.group(1)
        event_type, code, raw_value = (int(match.group(i), 16) for i in (2, 3, 4))
        value = raw_value - (1 << 32) if raw_value & 0x80000000 else raw_value
    else:
        # HiBy/Ingenic's vendor getevent ignores -t and prints decimal codes.
        vendor = re.match(r"^(.+?)\s+:\s+(EV_KEY|EV_REL|EV_ABS|EV_SW)\s+(\d+)\s+(-?\d+)\s+\+\d+\.\d+sec\s*$", line.strip())
        if not vendor:
            return None
        source = vendor.group(1)
        event_type = {"EV_KEY": 1, "EV_REL": 2, "EV_ABS": 3, "EV_SW": 5}[vendor.group(2)]
        code, value = int(vendor.group(3)), int(vendor.group(4))
    # Ignore Linux button/touch-class key codes and multitouch absolute data.
    if event_type == 1 and 0x140 <= code <= 0x14f and not (include_abs and code == 0x14a):
        return None
    if event_type not in (1, 2, 5) and not (include_abs and event_type == 3):
        return None
    return {"input": source, "type": event_type, "code": code, "value": value}


def _adb_base(adb: str, serial: str) -> list[str]:
    return [adb, "-s", serial]


def capture_events(adb: str, serial: str, seconds: float = CAPTURE_SECONDS,
                   include_abs: bool = False) -> tuple[list[dict], str]:
    """Capture one prompt window without retaining unparsed device output.

    include_abs keeps touch data and disables the quiet-gap early stop.
    """
    proc = None
    events: list[dict] = []
    seen: set[tuple[str, int, int, int]] = set()
    lines: Queue[dict | None] = Queue(MAX_QUEUED_LINES)
    reader = None
    try:
        proc = subprocess.Popen(_adb_base(adb, serial) + ["shell", "getevent", "-t"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                text=True, encoding="utf-8", errors="replace",
                                bufsize=1, close_fds=True)

        def pump() -> None:
            assert proc is not None and proc.stdout is not None
            try:
                for line in iter(lambda: proc.stdout.readline(4096), ""):
                    # Parse in the reader so raw lines never accumulate in memory.
                    event = parse_event_line(line, include_abs)
                    if event is not None:
                        try:
                            lines.put_nowait(event)
                        except Full:
                            pass
            finally:
                try:
                    lines.put_nowait(None)
                except Full:
                    pass

        reader = threading.Thread(target=pump, daemon=True)
        reader.start()
        start = time.monotonic()
        deadline = start + seconds
        quiet_deadline = None
        while time.monotonic() < deadline:
            now = time.monotonic()
            if quiet_deadline is not None and now >= quiet_deadline:
                break
            try:
                event = lines.get(timeout=min(0.1, deadline - now,
                                              max(0.01, quiet_deadline - now) if quiet_deadline else 0.1))
            except Empty:
                if proc.poll() is not None:
                    break
                continue
            if event is None:
                break
            key = (event["input"], event["type"], event["code"], event["value"])
            if key not in seen:
                events.append(event)
                seen.add(key)
            if not include_abs:
                quiet_deadline = min(deadline, time.monotonic() + RELEASE_GRACE_SECONDS)
        status = "mapped" if events else ("error" if proc.poll() not in (None, 0) else "no_events")
        return events, status
    except OSError:
        return events, "error"
    finally:
        if proc is not None:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if proc.stdout is not None:
                proc.stdout.close()
        if reader is not None:
            reader.join(timeout=0.5)


def collect_button_mapping(adb: str, serial: str, redact) -> dict:
    """Prompt for button names and capture their numeric kernel events."""
    print("\nPress every physical button separately, including volume, power and rotary directions.")
    print("Use a short press for power; stock firmware will still respond to button presses.")
    result = {"status": "ok", "mappings": []}
    try:
        try:
            check = subprocess.run(_adb_base(adb, serial) + ["shell", "command", "-v", "getevent"],
                                  capture_output=True, text=True, timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired):
            result["status"] = "unavailable"
            return result
        if check.returncode != 0 or not check.stdout.strip():
            result["status"] = "unavailable"
            return result
        while True:
            try:
                label = input("Name a physical button or rotary direction (empty to finish): ").strip()
            except (EOFError, KeyboardInterrupt):
                result["status"] = "interrupted"
                break
            if not label:
                break
            safe_label = redact(label, serial)
            print(f"Press {safe_label} once now (listening for up to 8 seconds).", flush=True)
            events, status = capture_events(adb, serial)
            for event in events:
                event["input"] = redact(event["input"], serial)
            print(f"  {status}: {len(events)} distinct events captured")
            result["mappings"].append({"label": safe_label, "status": status, "events": events})
    except KeyboardInterrupt:
        result["status"] = "interrupted"
    return result
