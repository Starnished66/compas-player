#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
baseline_source="${GUI_EQ_BASELINE_SOURCE:-}"
build_dir="${GUI_EQ_SPEC_BUILD_DIR:-$repo_root/build_ui_test/equalizer}"
mkdir -p "$build_dir"

if [[ ! -d "$repo_root/build_host/lvgl" || ! -f "$repo_root/build_host/ui/ui_fonts.o" ]]; then
    echo "GUI equalizer spec requires cached build_host/lvgl and build_host/ui/ui_fonts.o" >&2
    exit 2
fi

final_module=0
if [[ -f "$repo_root/src/ui/gui_equalizer.c" && -f "$repo_root/src/ui/gui_equalizer.h" ]]; then
    final_module=1
fi
if [[ -z "$baseline_source" && "$final_module" == 0 ]]; then
    if rg -q '^void gui_settings_open_eq\(' "$repo_root/src/ui/gui_settings.c"; then
        baseline_source="$repo_root/src/ui/gui_settings.c"
    else
        echo "set GUI_EQ_BASELINE_SOURCE to characterize the pre-move implementation" >&2
        exit 2
    fi
fi
if [[ -n "$baseline_source" && ! -f "$baseline_source" ]]; then
    echo "requested EQ baseline does not exist: $baseline_source" >&2
    exit 2
fi

if [[ -n "$baseline_source" || "$final_module" == 1 ]]; then
python3 - "$baseline_source" "$build_dir/baseline_fixture.c" "$repo_root/src/ui/gui_settings.c" "$repo_root/src/ui/gui_equalizer.c" "$final_module" "$repo_root/src/ui/screen_builders.c" <<'PY'
from pathlib import Path
import re
import sys

source_name, fixture_name, live_settings_name, final_module_name, final_module_flag, builders_name = sys.argv[1:]
source_path = Path(source_name) if source_name else None
source = source_path.read_text(encoding="utf-8") if source_path else ""
fixture_path = Path(fixture_name)


def matching_brace(text, opening):
    depth, state, escaped, i = 0, "code", False, opening
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "line":
            if ch == "\n": state = "code"
        elif state == "block":
            if ch == "*" and nxt == "/": state = "code"; i += 1
        elif state in ("string", "char"):
            if escaped: escaped = False
            elif ch == "\\": escaped = True
            elif (state == "string" and ch == '"') or (state == "char" and ch == "'"):
                state = "code"
        elif ch == "/" and nxt == "/": state = "line"; i += 1
        elif ch == "/" and nxt == "*": state = "block"; i += 1
        elif ch == '"': state = "string"
        elif ch == "'": state = "char"
        elif ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: return i
        i += 1
    raise RuntimeError("unbalanced function body")


def function(name, text=source):
    for match in re.finditer(r"\b" + re.escape(name) + r"\s*\(", text):
        opening_paren = text.find("(", match.start())
        depth, i = 1, opening_paren + 1
        while i < len(text) and depth:
            if text[i] == "(": depth += 1
            elif text[i] == ")": depth -= 1
            i += 1
        while i < len(text) and text[i].isspace(): i += 1
        if i < len(text) and text[i] == "{":
            start = text.rfind("\n", 0, match.start()) + 1
            end = matching_brace(text, i)
            return text[start:end + 1] + "\n"
    raise RuntimeError(f"function definition not found: {name}")


names = [
    "style_settings_dropdown", "gui_settings_refresh_font_geometry", "freq_to_slider", "slider_to_freq",
    "eq_format_gain", "eq_format_q", "eq_format_crossfeed", "eq_numeric_entry_done_cb",
    "eq_field_label_click_cb", "eq_screen_loaded_cb", "crossfeed_screen_loaded_cb",
    "eq_screen_btn_event_cb", "crossfeed_screen_btn_event_cb",
    "eq_bypass_switch_event_cb", "refresh_eq_band_widgets", "eq_band_step_cb",
    "eq_band_options_open_cb", "eq_type_dropdown_event_cb", "eq_preamp_slider_event_cb",
    "eq_crossfeed_slider_event_cb", "eq_band_enabled_switch_event_cb", "eq_freq_slider_event_cb",
    "eq_gain_slider_event_cb", "eq_q_slider_event_cb", "gui_settings_open_eq", "create_eq_slider_card",
    "eq_set_current_profile_from_path", "refresh_all_eq_widgets", "eq_refresh_graph", "eq_profiles_free_paths",
    "eq_profile_name_is_valid", "eq_profile_path_from_name", "eq_save_named_profile", "eq_profile_row_cb",
    "eq_profile_replace_row_cb", "hide_eq_profile_delete_popup", "eq_profile_delete_popup_backdrop_cb",
    "eq_profile_delete_cancel_cb", "eq_profile_delete_confirm_cb", "eq_profile_delete_row_cb",
    "eq_profile_rename_done_cb", "eq_profile_rename_row_cb", "populate_eq_profiles_screen",
    "eq_profiles_screen_unloaded_cb", "eq_profiles_screen_loaded_cb", "eq_profiles_edit_btn_cb",
    "build_eq_profile_delete_popup", "build_eq_profiles_screen", "eq_load_profile_btn_cb",
    "eq_open_profiles_for_replace", "eq_save_profile_name_done_cb", "eq_hide_save_choice_popup",
    "eq_save_choice_backdrop_cb", "eq_save_choice_new_cb", "eq_save_choice_replace_cb",
    "build_eq_save_choice_popup", "eq_open_save_choice_popup", "eq_save_profile_pressed_cb",
    "eq_save_profile_btn_cb", "eq_make_slider_card", "eq_flat_btn_cb", "eq_crossfeed_angle_dropdown_event_cb",
    "build_eq_band_options_screen", "build_crossfeed_screen", "eq_graph_grid_draw_cb", "build_eq_screen",
    "gui_settings_get_eq_screen",
]

if source_path:
    # Derive constants and mutable state from the source snapshot; do not
    # maintain a hand-copied subset of the EQ implementation.
    path_start = source.index("#ifdef HOST_BUILD\n  #define MUSIC_ROOT_DIR")
    path_end = source.index("#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR \"/PEQ_Profiles\"", path_start)
    path_end += len("#define PEQ_PROFILES_DIR MUSIC_ROOT_DIR \"/PEQ_Profiles\"")
    path_macros = source[path_start:path_end]

    def before(start_marker, end_marker):
        start = source.index(start_marker)
        end = source.index(end_marker, start)
        return source[start:end]

    state = [
        "static lv_obj_t * settings_crossfeed_screen;\n",
        before("static lv_obj_t * eq_screen;", "static void eq_profile_display_name"),
        function("eq_profile_display_name"),
        before("static lv_obj_t * eq_bypass_switch;", "static void eq_refresh_graph(void);")
        + "static void eq_refresh_graph(void);\n",
        before("static lv_obj_t * eq_profiles_list;", "static void eq_set_current_profile_from_path"),
        before("static char ** eq_profile_paths = NULL;", "static gui_popup_t eq_profile_delete_popup")
        + "static gui_popup_t eq_profile_delete_popup;\n",
        before("static gui_popup_t eq_save_choice_popup;", "static void eq_hide_save_choice_popup"),
        before("#define EQ_SAVE_PROFILE_HOLD_MS", "static void eq_save_profile_pressed_cb"),
        "static lv_obj_t * settings_eq_summary;\n",
    ]
    freq_start = source.index("#define EQ_FREQ_MIN_HZ")
    freq_end = source.index("#define EQ_FREQ_SLIDER_MAX", freq_start)
    freq_end = source.index("\n", freq_end) + 1
    macros = source[freq_start:freq_end]
    enum_start = source.index("typedef enum {\n    EQ_FIELD_PREAMP,")
    enum_end = source.index("} eq_field_t;", enum_start) + len("} eq_field_t;")
    field_enum = source[enum_start:enum_end] + "\n"
    functions = "static void populate_eq_profiles_screen(void);\n" + "\n".join(function(name) for name in names)

    # Extract the actual EQ subblocks from the shared parent callbacks.
    summary = function("settings_summary_loaded_cb")
    summary_start = summary.index("    if (settings_eq_summary) {")
    summary_end = matching_brace(summary, summary.index("{", summary_start))
    summary_block = summary[summary_start:summary_end + 1]
    accent = function("gui_settings_accent_changed")
    accent_lines = [line for line in accent.splitlines()
                    if re.match(r"\s*if \(eq_graph_(?:series|chart)\)", line)]
    if len(accent_lines) != 2:
        raise RuntimeError("expected both original EQ accent-update statements")

    # Preserve the baseline's exact screen/popup lifecycle statements.
    init = function("gui_settings_init")
    teardown = function("gui_settings_teardown")
    init_calls = "\n".join(re.findall(r"(?m)^    (?:eq_screen = build_eq_screen\(\);|eq_band_options_screen = build_eq_band_options_screen\(\);|settings_crossfeed_screen = build_crossfeed_screen\(\);|eq_profiles_screen = build_eq_profiles_screen\(\);|build_eq_profile_delete_popup\(\);|build_eq_save_choice_popup\(\);)$", init))
    if len(init_calls.splitlines()) != 6:
        raise RuntimeError("expected two separated phases of four screens and two popups in baseline")
    start = teardown.index("    gui_popup_teardown(&eq_profile_delete_popup);")
    end = teardown.index("    gui_popup_teardown(&factory_reset_popup);", start)
    popup_calls = teardown[start:end]
    start = teardown.index("    if (eq_screen) { lv_obj_delete(eq_screen); eq_screen = NULL; }")
    end = teardown.index("eq_profiles_free_paths();", start) + len("eq_profiles_free_paths();")
    screen_calls = teardown[start:end]

    fixture = (
        "/* Generated from frozen complete gui_settings.c; bodies are source slices. */\n"
        + path_macros + "\n" + "\n".join(state) + "\n" + macros + field_enum + functions
        + "\nvoid eq_spec_build_screens(void) {\n"
        + "\n".join(init_calls.splitlines()[:4]) + "\n}\n"
        + "void eq_spec_build_popups(void) {\n"
        + "\n".join(init_calls.splitlines()[4:]) + "\n}\n"
        + "void eq_spec_teardown_popups(void) {\n" + popup_calls + "}\n"
        + "void eq_spec_teardown_screens(void) {\n" + screen_calls + "\n}\n"
        + "void eq_spec_update_summary(lv_obj_t * label) {\n    settings_eq_summary = label;\n"
        + summary_block + "\n}\n"
        + "void eq_spec_accent_changed(void) {\n" + "\n".join(accent_lines) + "\n}\n"
    )
    fixture_path.write_text(fixture, encoding="utf-8")

if final_module_flag == "1":
    settings = Path(live_settings_name).read_text(encoding="utf-8")
    live_module = Path(final_module_name).read_text(encoding="utf-8")
    builders = Path(builders_name).read_text(encoding="utf-8")
    # Validate all four parent lifecycle call sites in final-only mode too.
    live_init = function("gui_settings_init", settings)
    live_teardown = function("gui_settings_teardown", settings)
    init_hooks = ["gui_equalizer_build_screens(", "gui_equalizer_build_popups("]
    teardown_hooks = ["gui_equalizer_teardown_popups(", "gui_equalizer_teardown_screens("]
    if any(hook not in live_init for hook in init_hooks) or live_init.index(init_hooks[0]) > live_init.index(init_hooks[1]):
        raise RuntimeError("live settings init lacks ordered EQ screen/popup lifecycle calls")
    if any(hook not in live_teardown for hook in teardown_hooks) or live_teardown.index(teardown_hooks[0]) > live_teardown.index(teardown_hooks[1]):
        raise RuntimeError("live settings teardown lacks ordered EQ popup/screen lifecycle calls")
    def code_lines(body):
        return [line.strip() for line in body.splitlines()
                if line.strip() and not line.lstrip().startswith(("/*", "*", "//"))]

    def require_neighbors(body, left, middle, right):
        lines = code_lines(body)
        matches = [i for i, line in enumerate(lines) if middle in line]
        if len(matches) != 1:
            raise RuntimeError(f"expected one lifecycle call containing {middle}")
        i = matches[0]
        if i == 0 or i + 1 >= len(lines) or lines[i - 1] != left or lines[i + 1] != right:
            raise RuntimeError(f"EQ lifecycle call {middle} moved from its exact neighboring operations")

    require_neighbors(live_init, "settings_screen = build_settings_screen();",
                      "gui_equalizer_build_screens(", "gui_updates_build_popups();")
    require_neighbors(live_init, "gui_button_mapping_build_reset_popup();",
                      "gui_equalizer_build_popups();", "build_factory_reset_popup();")
    require_neighbors(live_teardown, "gui_button_mapping_teardown_reset_popup();",
                      "gui_equalizer_teardown_popups();", "gui_popup_teardown(&factory_reset_popup);")
    require_neighbors(live_teardown,
                      "if (settings_screen) { lv_obj_delete(settings_screen); settings_screen = NULL; }",
                      "gui_equalizer_teardown_screens();", "}")
    for api in ["gui_equalizer_build_screens", "gui_equalizer_build_popups",
                "gui_equalizer_teardown_popups", "gui_equalizer_teardown_screens",
                "gui_equalizer_update_summary", "gui_equalizer_accent_changed",
                "gui_equalizer_eq_row_event_cb", "gui_equalizer_crossfeed_row_event_cb",
                "gui_settings_open_eq", "gui_settings_get_eq_screen",
                "gui_settings_refresh_font_geometry"]:
        function(api, live_module)
    style_source = function("gui_style_settings_dropdown", builders)
    style_fixture = Path(fixture_name).parent / "gui_style_settings_dropdown_fixture.c"
    style_fixture.write_text('#include "screen_builders.h"\n' + style_source, encoding="utf-8")
PY
fi

cc="${CC:-cc}"
common_flags=(-std=gnu11 -O0 -g -Wall -Wextra -Werror -Wno-unused-function -fwrapv
    -ffunction-sections -fdata-sections -DHOST_BUILD=1 -DBOARD_R1 -DLV_CONF_INCLUDE_SIMPLE=1
    -I"$repo_root" -I"$repo_root/src/ui" -I"$repo_root/src/core" -I"$repo_root/src/audio"
    -I"$repo_root/src/library" -I"$repo_root/src/plugins" -I"$repo_root/src/hardware"
    -I"$repo_root/lvgl" -I"$repo_root/lvgl/src" -I"$build_dir")
if [[ "${SANITIZE:-0}" == "1" ]]; then
    common_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    echo "Sanitizers cover the spec, extracted EQ code, peq.c, and mseb.c; cached LVGL/font objects are uninstrumented."
fi
lvgl_objects=( $(find "$repo_root/build_host/lvgl" -name '*.o' -print) )

build_run() {
    local mode="$1" name="$2"
    local defs=()
    local extra_sources=()
    [[ "$mode" == baseline ]] && defs+=(-DEQ_SPEC_BASELINE)
    [[ "$mode" == final ]] && extra_sources+=("$build_dir/gui_style_settings_dropdown_fixture.c")
    "$cc" "${common_flags[@]}" "${defs[@]}" \
        "$repo_root/src/ui/gui_equalizer_spec.c" "$repo_root/src/audio/peq.c" "$repo_root/src/audio/mseb.c" \
        "${extra_sources[@]}" \
        "${lvgl_objects[@]}" "$repo_root/build_host/ui/ui_fonts.o" \
        -Wl,--gc-sections -lpthread -lm -o "$build_dir/$name"
    rm -rf "$build_dir/run/music" "$build_dir/run/.compas"
    mkdir -p "$build_dir/run/music/PEQ_Profiles" "$build_dir/run/.compas"
    (cd "$build_dir/run" && "$build_dir/$name") > "$build_dir/$name.trace"
}

if [[ -n "$baseline_source" ]]; then
    build_run baseline baseline_spec
fi
if [[ "$final_module" == 1 ]]; then
    build_run final final_spec
    if [[ -n "$baseline_source" ]]; then
        cmp "$build_dir/baseline_spec.trace" "$build_dir/final_spec.trace"
        echo "GUI equalizer characterization matches frozen baseline"
    else
        echo "GUI equalizer final characterization passed"
    fi
elif [[ -n "$baseline_source" ]]; then
    echo "Frozen baseline EQ characterization completed"
fi
