#!/usr/bin/env python3
"""List UI text string literals in src/ that are not wrapped for translation.

Usage: find_untranslated.py [--all] [--fix] [FILE...]

Without FILE arguments every src/**/*.c except tests is scanned. A literal is
reported when it is passed to a known UI text call (lv_label_set_text, the toast
and popup helpers, the screen builders, ...), is the label of a pill_list_item_t
or icon_grid_item_t initializer, or is a ternary branch that looks like text.
It counts as translated when it sits directly inside TR(), TR_N(), TR_C() or
N_(). Intentional English goes in scripts/i18n/untranslated_allow.txt.

--all  also lists every other text-looking literal (for manual audits).
--fix  wraps reported literals in TR() (N_() outside function bodies) in place.
Exits 1 when anything is reported (and --fix is not given).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# callee -> argument indices that hold user-visible text
UI_CALLS = {
    "lv_label_set_text": [1], "lv_label_set_text_fmt": [1], "lv_label_set_text_static": [1],
    "lv_textarea_set_placeholder_text": [1], "lv_dropdown_set_options": [1], "lv_roller_set_options": [1],
    "show_error_toast": [0], "show_info_toast": [0], "show_info_toast_for": [0],
    "build_pill_list_screen": [0], "build_subsonic_list_screen": [0], "build_compact_list_screen": [0],
    "build_category_menu_screen": [0], "build_icon_grid_screen": [0], "build_launcher_menu_screen": [0],
    "build_confirm_popup": [0, 3, 4, 8], "build_confirm_popup_with_labels": [0, 3, 5, 10],
    "gui_busy_show": [0, 1], "gui_busy_set_detail": [1], "show_text_entry": [0],
    "add_info_line": [0, 1], "add_info_section": [0], "add_pill_chevron_row": [1], "add_pill_toggle_row": [1],
    "add_pill_row_base": [1], "add_section_header": [1], "add_action_row": [1], "add_playlist_row_base": [1],
    "accent_text": [1], "build_list_message": [1, 2], "build_list_section": [1], "build_screen_header": [1],
    "build_title": [1], "add_plugin_empty_state": [1], "queue_label": [0], "row_label_set_identity": [2, 3],
    "start_subsonic_browse": [2], "show_subsonic_download_confirm_popup": [1],
    "quick_drawer_set_toggle_state_text": [2], "eq_make_slider_card": [2], "set_label_if_changed": [1],
    "show_group_songs": [0], "build_music_list_row": [1, 2], "add_file_row": [0],
    "gui_shell_update_quick_drawer_format": [0], "set_failed": [0], "finish": [1], "error_text": [2], "add_pill_option_row": [1],
}
IGNORE_CALLEES = {"library_teardown_diag", "lv_obj_set_name_static", "lv_obj_set_name", "player_find_role", "player_layouts_find_timeline",
                  "mkdirat", "openat", "unlinkat", "remove_queue_checkpoint_at", "memcmp", "strncasecmp",
                  "strcasecmp", "strcmp", "strncmp", "strstr", "fopen", "access", "stat", "getenv", "lv_xml_register_event_cb"}
WRAPPERS = {"TR", "TR_N", "TR_C", "N_", "_"}
LOG_CALLS = {"DBG_LOG", "DB_LOG", "printf", "fprintf", "puts", "fputs", "perror", "syslog", "db_log", "log_msg", "boot_checkpoint",
             "LV_LOG", "LV_LOG_WARN", "LV_LOG_ERROR", "LV_LOG_INFO", "LV_LOG_USER", "LV_LOG_TRACE"}
ITEM_TYPES = {"pill_list_item_t": 0, "icon_grid_item_t": 2}
TEXT_RE = re.compile(r"^[A-Z][^\"]*[a-z]")


STRUCT_RE = re.compile(r"typedef struct(?:\s+\w+)?\s*\{\s*const char\s*\*\s*(label|title|text)\s*;[^}]*\}\s*(\w+)\s*;")


def register_label_structs():
    """typedef'd structs whose first member is a text label count as UI item tables."""
    for dp, _, fns in os.walk(os.path.join(ROOT, "src", "ui")):
        for fn in fns:
            if fn.endswith((".c", ".h")):
                txt = open(os.path.join(dp, fn), encoding="utf-8", errors="replace").read()
                for m in STRUCT_RE.finditer(txt):
                    ITEM_TYPES.setdefault(m.group(2), 0)


def load_allow():
    allow = set()
    path = os.path.join(HERE, "untranslated_allow.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                allow.add(line)
    return allow


def tokenize(src):
    """Yield (kind, text, line, start, end). kind: id, str, p (punctuation). Comments and
    preprocessor lines are dropped; adjacent string literals are merged."""
    toks = []
    i, n, line = 0, len(src), 1
    at_line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            at_line_start = True
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if c == "#" and at_line_start:
            while i < n and src[i] != "\n":
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    line += 1
                    i += 1
                i += 1
            continue
        at_line_start = False
        if src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += src.count("\n", i, j)
            i = j
            continue
        if c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            j += 1
            text = src[i + 1:j - 1]
            if toks and toks[-1][0] == "str":
                k = toks[-1]
                toks[-1] = ("str", k[1] + text, k[2], k[3], j)
            else:
                toks.append(("str", text, line, i, j))
            i = j
            continue
        if c == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            toks.append(("p", "'c'", line, i, j + 1))
            i = j + 1
            continue
        m = re.match(r"[A-Za-z_][A-Za-z_0-9]*", src[i:i + 80])
        if m:
            toks.append(("id", m.group(0), line, i, i + len(m.group(0))))
            i += len(m.group(0))
            continue
        m = re.match(r"[0-9][A-Za-z0-9_.]*", src[i:i + 40])
        if m:
            toks.append(("p", "0", line, i, i + len(m.group(0))))
            i += len(m.group(0))
            continue
        toks.append(("p", c, line, i, i + 1))
        i += 1
    return toks


def scan(path, show_all, rel=""):
    src = open(path, encoding="utf-8", errors="replace").read()
    toks = tokenize(src)
    findings = []  # (line, start, end, text, in_function, reason)
    # frames: dict(kind='(' or '{', callee, idx, itemtype, root_depth)
    stack = []
    in_func = 0
    stmt_type = None
    stmt_array = False
    item_root_depth = None
    for ti, (kind, text, line, s, e) in enumerate(toks):
        prev = toks[ti - 1] if ti else None
        nxt = toks[ti + 1] if ti + 1 < len(toks) else None
        if kind == "id" and text in ITEM_TYPES:
            stmt_type, stmt_array = text, False
        if kind == "p":
            if text == "[" and stmt_type:
                stmt_array = True
            if text == ";" and not any(f["kind"] == "(" for f in stack):
                if not in_func:
                    stmt_type = None
            if text == "(":
                callee = prev[1] if prev and prev[0] == "id" else None
                cast = None
                if prev and prev[0] == "id" and prev[1] in ITEM_TYPES and nxt and nxt[1] == ")":
                    cast = prev[1]
                stack.append({"kind": "(", "callee": callee, "idx": 0, "cast": cast})
            elif text == ")":
                if stack and stack[-1]["kind"] == "(":
                    stack.pop()
            elif text == "{":
                itype = None
                prevtok = prev
                if prevtok and prevtok[1] == ")" and ti >= 3 and toks[ti - 2][1] in ITEM_TYPES and toks[ti - 3][1] == "(":
                    itype = toks[ti - 2][1]
                    f = {"kind": "{", "item": True, "idx": 0, "itype": itype}
                elif stmt_type and prev and prev[1] == "=":
                    f = {"kind": "{", "item": not stmt_array, "idx": 0, "itype": stmt_type, "array_root": stmt_array}
                elif stack and stack[-1]["kind"] == "{" and stack[-1].get("array_root"):
                    f = {"kind": "{", "item": True, "idx": 0, "itype": stack[-1]["itype"]}
                else:
                    f = {"kind": "{", "item": False, "idx": 0, "itype": None}
                    if not stack and prev and prev[1] == ")":
                        in_func += 1
                        f["func"] = True
                    elif not any(x["kind"] == "{" for x in stack) and prev and prev[1] != "=":
                        pass
                stack.append(f)
            elif text == "}":
                if stack and stack[-1]["kind"] == "{":
                    f = stack.pop()
                    if f.get("func"):
                        in_func -= 1
                    if not stack and not in_func:
                        stmt_type = None
            elif text == ",":
                if stack:
                    stack[-1]["idx"] += 1
        if kind != "str":
            continue
        if not show_all and not re.search(r"[A-Za-z]", re.sub(r"\\.|%[-+ #0-9.*lhzjt]*[a-zA-Z%]", "", text)):
            continue
        if all(len(seg) <= 1 for seg in text.split("\\n")):
            continue  # single characters, e.g. the A-Z index strip
        top = stack[-1] if stack else None
        callee = top["callee"] if top and top["kind"] == "(" else None
        idx = top["idx"] if top else 0
        # directly wrapped?
        if callee in WRAPPERS:
            continue
        if callee in LOG_CALLS or callee in IGNORE_CALLEES:
            continue
        reason = None
        if callee in UI_CALLS and idx in UI_CALLS[callee]:
            reason = "%s arg %d" % (callee, idx)
        elif top and top["kind"] == "{" and top.get("item") and top["itype"] in ITEM_TYPES and top["idx"] == ITEM_TYPES[top["itype"]] \
                and prev and prev[1] in ("{", ","):
            reason = "%s label" % top["itype"]
        elif prev and prev[1] == "=" and ti >= 3 and toks[ti - 2][1] in ("label", "title", "text", "caption") \
                and toks[ti - 3][1] == ".":
            reason = ".%s =" % toks[ti - 2][1]
        elif rel.startswith("src/ui/") and prev and prev[1] in ("?", ":") and TEXT_RE.match(text) and callee not in LOG_CALLS:
            reason = "ternary/branch text"
        elif show_all and text and re.search(r"[A-Za-z]{3}", text) and callee not in LOG_CALLS:
            reason = "other (%s)" % (callee or "-")
        if reason and ((nxt and nxt[0] == "id" and nxt[1] not in ("else",)) or (prev and prev[0] == "id" and prev[1] not in ("return", "case", "else") and prev[1] not in UI_CALLS)):
            reason += " [CONCAT with macro, fix by hand]"
        if reason:
            findings.append((line, s, e, text, bool(in_func), reason))
    return src, findings


def main(argv):
    show_all = "--all" in argv
    fix = "--fix" in argv
    files = [a for a in argv[1:] if not a.startswith("--")]
    if not files:
        for dp, _, fns in os.walk(os.path.join(ROOT, "src")):
            for fn in fns:
                if fn.endswith(".c") and not re.search(r"_(test|check|regression)\.c$", fn) and fn != "i18n_catalog.c":
                    files.append(os.path.join(dp, fn))
    register_label_structs()
    allow = load_allow()
    total = 0
    for path in sorted(files):
        rel = os.path.relpath(os.path.abspath(path), ROOT)
        src, findings = scan(path, show_all, rel)
        findings = [f for f in findings if f[3] not in allow and (rel + "::" + f[3]) not in allow]
        if fix and findings:
            for line, s, e, text, infn, reason in sorted(findings, key=lambda f: -f[1]):
                if "CONCAT" in reason:
                    continue
                wrap = "TR" if infn else "N_"
                src = src[:s] + wrap + "(" + src[s:e] + ")" + src[e:]
            open(path, "w", encoding="utf-8").write(src)
        for line, s, e, text, infn, reason in findings:
            print("%s:%d: %s%s: \"%s\"" % (rel, line, "" if infn else "[static] ", reason, text))
            total += 1
    if total:
        sys.stderr.write("%d unwrapped UI string(s)\n" % total)
    return 0 if (fix or not total) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
