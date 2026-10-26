#!/usr/bin/env python3
"""Install checked startup tracing into supported vendor boot scripts."""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile


RC_MARKER = "# COMPAS_STARTUP_TRACE_RC"
MODULE_MARKER = "# COMPAS_STARTUP_TRACE_MODULES"
CHECKPOINT_BEGIN = "# COMPAS_STARTUP_CHECKPOINT_BEGIN"
CHECKPOINT_END = "# COMPAS_STARTUP_CHECKPOINT_END"


def fail(message: str) -> None:
    raise SystemExit(message)


def command(command: list[str]) -> None:
    subprocess.run(command, check=True)


def validate_shell(path: pathlib.Path) -> None:
    command(["sh", "-n", str(path)])


def canonical_checkpoint_block(helper: pathlib.Path | None = None) -> str:
    if helper is None:
        helper = pathlib.Path(__file__).resolve().parents[3] / "firmware/overlay/usr/libexec/compas/boot-trace.sh"
    lines = helper.read_text().splitlines(keepends=True)
    begins = [i for i, line in enumerate(lines) if line.rstrip("\r\n") == CHECKPOINT_BEGIN]
    ends = [i for i, line in enumerate(lines) if line.rstrip("\r\n") == CHECKPOINT_END]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        fail("Runtime helper must contain exactly one ordered startup checkpoint block")
    block = "".join(lines[begins[0]:ends[0] + 1])
    if block.count("compas_startup_checkpoint() {") != 1:
        fail("Runtime helper checkpoint block does not contain one canonical function")
    if "$(" in block or "`" in block:
        fail("Runtime helper checkpoint block must use shell builtins only")
    # Validate the block itself as shell, independently of the worker code.
    with tempfile.NamedTemporaryFile("w", suffix=".sh") as staged:
        staged.write("#!/bin/sh\n" + block)
        staged.flush()
        validate_shell(pathlib.Path(staged.name))
    return block


def _remove_exact_lines(lines: list[str], removals: set[str]) -> str:
    return "".join(line for line in lines if line.rstrip("\r\n") not in removals)


def instrument_rcs(text: str, checkpoint_block: str) -> str:
    lines = text.splitlines(keepends=True)
    if not lines or lines[0].strip() != "#!/bin/sh":
        fail("Unsupported rcS: expected #!/bin/sh")
    marker_lines = [i for i, line in enumerate(lines) if line.strip() == RC_MARKER]
    if marker_lines:
        if len(marker_lines) != 1:
            fail("Multiple startup trace markers in rcS")
        body = "".join(lines)
        block_markers = sum(line.rstrip("\r\n") in (CHECKPOINT_BEGIN, CHECKPOINT_END) for line in lines)
        if block_markers != 2 or body.count(checkpoint_block) != 1:
            fail("Stale rcS startup tracing template; checkpoint block differs from the runtime helper")
        bootstrap = {RC_MARKER, 'IFS= read -r COMPAS_STARTUP_BOOT_ID < /proc/sys/kernel/random/boot_id || COMPAS_STARTUP_BOOT_ID=',
                     'export COMPAS_STARTUP_BOOT_ID', 'compas_startup_checkpoint boot begin start'}
        trace_lines = {
            '     compas_startup_checkpoint init-before "${i##*/}" -',
            '     _compas_init_status=$?',
            '     compas_startup_checkpoint init-after "${i##*/}" "$_compas_init_status"',
            '     if [ "${i##*/}" = S21mount_ubifs ] && [ "$_compas_startup_writer_started" -eq 0 ]; then',
            '         /usr/libexec/compas/boot-trace.sh start-writer',
            '         _compas_startup_writer_started=1',
            '     fi',
            '     (exit "$_compas_init_status")',
            '_compas_rc_status=$?',
            'compas_startup_checkpoint boot-end rcS "$_compas_rc_status"',
            'exit "$_compas_rc_status"',
        }
        bootstrap.add('_compas_startup_writer_started=0')
        clean = _remove_exact_lines(lines, bootstrap | trace_lines).replace(checkpoint_block, "", 1)
        if instrument_rcs(clean, checkpoint_block) != body:
            fail("Unrecognized or incomplete startup trace instrumentation in rcS")
        return body

    for_lines = [i for i, line in enumerate(lines) if re.fullmatch(r"[ \t]*for i in /etc/init\.d/S\?\?\*[ \t]*;[ \t]*do[ \t]*(?:\r?\n)?", line)]
    if len(for_lines) != 1:
        fail("Unsupported rcS: expected one ordered /etc/init.d/S??* loop")
    start = for_lines[0]
    cases = [i for i in range(start + 1, len(lines)) if lines[i].strip() == 'case "$i" in']
    if len(cases) != 1:
        fail("Unsupported rcS: expected one init-script case statement")
    case = cases[0]
    endings = [i for i in range(case + 1, len(lines)) if lines[i].strip() == "esac"]
    if len(endings) != 1:
        fail("Unsupported rcS: expected one matching esac")
    esac = endings[0]
    case_body = "".join(lines[case:esac + 1])
    commands = [line.strip() for line in lines[case:esac + 1]
                if line.strip() and not line.lstrip().startswith("#")]
    expected = [
        'case "$i" in', '*.sh)', '(', 'trap - INT QUIT TSTP', 'set start',
        '. $i', ')', ';;', '*)', '$i start', ';;', 'esac',
    ]
    if commands != expected or case_body.count("$i start") != 1 or case_body.count(". $i") != 1:
        fail("Unsupported rcS: init case does not match a supported vendor layout")
    # Require the historical guard to remain directly before the case. This
    # preserves skip behavior for dangling entries and directories.
    if not any(line.strip() == '[ ! -f "$i" ] && continue' for line in lines[start + 1:case]):
        fail("Unsupported rcS: missing vendor init-file guard")
    done = [i for i in range(esac + 1, len(lines)) if lines[i].strip() == "done"]
    if len(done) != 1:
        fail("Unsupported rcS: expected one loop terminator")

    # Insert after the shebang, before the vendor's profile and profiling.
    bootstrap = [RC_MARKER + "\n", checkpoint_block]
    bootstrap.extend([
        'IFS= read -r COMPAS_STARTUP_BOOT_ID < /proc/sys/kernel/random/boot_id || COMPAS_STARTUP_BOOT_ID=\n',
        'export COMPAS_STARTUP_BOOT_ID\n',
        '_compas_startup_writer_started=0\n',
        'compas_startup_checkpoint boot begin start\n',
    ])
    lines[1:1] = bootstrap
    # Offsets shift by the bootstrap length.
    start += len(bootstrap)
    case += len(bootstrap)
    esac += len(bootstrap)
    done[0] += len(bootstrap)
    # Existing `before`/`after` profiling lines are retained verbatim.
    lines.insert(case, '     compas_startup_checkpoint init-before "${i##*/}" -\n')
    esac += 1
    lines.insert(esac + 1, '     _compas_init_status=$?\n')
    lines.insert(esac + 2, '     compas_startup_checkpoint init-after "${i##*/}" "$_compas_init_status"\n')
    lines[esac + 3:esac + 3] = [
        '     if [ "${i##*/}" = S21mount_ubifs ] && [ "$_compas_startup_writer_started" -eq 0 ]; then\n',
        '         /usr/libexec/compas/boot-trace.sh start-writer\n',
        '         _compas_startup_writer_started=1\n',
        '     fi\n',
        '     (exit "$_compas_init_status")\n',
    ]
    if not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    lines.extend([
        '_compas_rc_status=$?\n',
        'compas_startup_checkpoint boot-end rcS "$_compas_rc_status"\n',
        'exit "$_compas_rc_status"\n',
    ])
    return "".join(lines)


def instrument_modules(text: str, checkpoint_block: str | None = None) -> str:
    if checkpoint_block is None:
        checkpoint_block = canonical_checkpoint_block()
    lines = text.splitlines(keepends=True)
    if not lines or lines[0].strip() != "#!/bin/sh":
        fail("Unsupported module loader: expected #!/bin/sh")
    body = "".join(lines)
    if MODULE_MARKER in body:
        if body.count(MODULE_MARKER) != 1:
            fail("Unrecognized or incomplete startup trace instrumentation in module loader")
        if sum(line.rstrip("\r\n") in (CHECKPOINT_BEGIN, CHECKPOINT_END) for line in lines) != 2:
            fail("Stale module loader startup tracing template; checkpoint block is missing")
        if body.count(checkpoint_block) != 1:
            fail("Stale module loader startup tracing template; checkpoint block differs from the runtime helper")
        clean_lines = []
        bootstrap = {
            'if [ -z "${COMPAS_STARTUP_BOOT_ID:-}" ]; then',
            '    IFS= read -r COMPAS_STARTUP_BOOT_ID < /proc/sys/kernel/random/boot_id || COMPAS_STARTUP_BOOT_ID=',
            '    export COMPAS_STARTUP_BOOT_ID',
            'fi',
        }
        for line in lines:
            stripped = line.rstrip("\r\n")
            if (stripped == MODULE_MARKER or stripped in bootstrap
                    or "compas_startup_checkpoint module-before " in stripped
                    or "compas_startup_checkpoint module-after " in stripped
                    or stripped == "_compas_module_status=$?"
                    or stripped == 'exit "$_compas_module_status"'
                    or stripped == '(exit "$_compas_module_status")'):
                continue
            clean_lines.append(line)
        clean = "".join(clean_lines).replace(checkpoint_block, "", 1)
        if instrument_modules(clean, checkpoint_block) != body:
            fail("Unrecognized or incomplete startup trace instrumentation in module loader")
        return body
    active: list[tuple[int, str]] = []
    for n, line in enumerate(lines):
        stripped = line.strip()
        if n == 0 or not stripped or stripped.startswith("#"):
            continue
        match = re.fullmatch(r"sh[ \t]+([A-Za-z0-9_.+-]+\.sh)[ \t]*", stripped)
        if not match:
            fail(f"Unsupported active command in module loader at line {n + 1}: {stripped}")
        active.append((n, match.group(1)))
    if not active:
        fail("Unsupported module loader: no module scripts found")
    result = [lines[0], MODULE_MARKER + "\n", checkpoint_block]
    result.extend([
        'if [ -z "${COMPAS_STARTUP_BOOT_ID:-}" ]; then\n',
        '    IFS= read -r COMPAS_STARTUP_BOOT_ID < /proc/sys/kernel/random/boot_id || COMPAS_STARTUP_BOOT_ID=\n',
        '    export COMPAS_STARTUP_BOOT_ID\n',
        'fi\n',
    ])
    active_by_line = dict(active)
    for n, line in enumerate(lines[1:], 1):
        stripped = line.strip()
        if n not in active_by_line:
            result.append(line)
            continue
        module = active_by_line[n]
        result.extend([
            f'compas_startup_checkpoint module-before "{module}" -\n',
            line if line.endswith("\n") else line + "\n",
            "_compas_module_status=$?\n",
            f'compas_startup_checkpoint module-after "{module}" "$_compas_module_status"\n',
            '(exit "$_compas_module_status")\n',
        ])
    result.append('exit "$_compas_module_status"\n')
    return "".join(result)


def main(root: pathlib.Path) -> None:
    repo = pathlib.Path(__file__).resolve().parents[3]
    helper = repo / "firmware/overlay/usr/libexec/compas/boot-trace.sh"
    rcs = root / "etc/init.d/rcS"
    modules = root / "module_driver/driver_default_init_script.sh"
    for path in (helper, rcs, modules):
        if not path.is_file():
            fail(f"Missing startup tracing input: {path}")
    checkpoint_block = canonical_checkpoint_block(helper)

    # Prepare all outputs in a private scratch directory. No target is changed
    # unless every source format and generated shell file validates.
    with tempfile.TemporaryDirectory(prefix="compas-startup-trace-") as temp:
        scratch = pathlib.Path(temp)
        staged_helper = scratch / "boot-trace.sh"
        shutil.copyfile(helper, staged_helper)
        staged_helper.chmod(0o755)
        staged_rcs = scratch / "rcS"
        staged_rcs.write_text(instrument_rcs(rcs.read_text(), checkpoint_block))
        staged_modules = scratch / "driver_default_init_script.sh"
        staged_modules.write_text(instrument_modules(modules.read_text(), checkpoint_block))
        for staged in (staged_helper, staged_rcs, staged_modules):
            validate_shell(staged)
        # Preserve vendor modes on the staged files.
        targets = [
            (staged_helper, root / "usr/libexec/compas/boot-trace.sh", 0o755),
            (staged_rcs, rcs, stat.S_IMODE(rcs.stat().st_mode)),
            (staged_modules, modules, stat.S_IMODE(modules.stat().st_mode)),
        ]
        replacements: list[tuple[pathlib.Path, pathlib.Path]] = []
        try:
            for staged, target, mode in targets:
                target.parent.mkdir(parents=True, exist_ok=True)
                fd, temp_name = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
                temp_target = pathlib.Path(temp_name)
                with os.fdopen(fd, "wb") as out, staged.open("rb") as src:
                    shutil.copyfileobj(src, out)
                temp_target.chmod(mode)
                replacements.append((temp_target, target))
            for temp_target, target in replacements:
                os.replace(temp_target, target)
        finally:
            for temp_target, _ in replacements:
                temp_target.unlink(missing_ok=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail(f"Usage: {sys.argv[0]} SQUASHFS_ROOT")
    main(pathlib.Path(sys.argv[1]).resolve())
