#include "gui_native_audio.h"
#include "gui_settings.h"
#include "../shell/gui_shell.h"
#include "../application/gui.h"
#include "../widgets/builders/screen_builders.h"
#include "../text/rendering/gui_text_input.h"
#include "../text/i18n/i18n.h"
#include "../../core/persistence/native_audio_settings.h"
#include "../../core/persistence/settings.h"
#include "../../core/persistence/storage_paths.h"
#include "../../audio/effects/peq.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { PAGE_MSEB, PAGE_STAGE, PAGE_PRESETS, PAGE_MANAGE, PAGE_CONFIRM, PAGE_COUNT };
static lv_obj_t * screens[PAGE_COUNT], * lists[PAGE_COUNT], * titles[PAGE_COUNT];
static setting_slider_t tone_sliders[11], stage_sliders[10];
static lv_obj_t * mseb_enabled_row, * stage_enabled_row, * stage_preset_menu;
static lv_obj_t * mseb_range_menu, * tone_overflow[11];
static unsigned mseb_shape;
static bool mseb_legacy;
static int selected_preset, pending_action;
static bool selected_damaged, pending_enable_mseb;
static lv_obj_t * gain_switches[2]; /* Sound and Car mode; cleared on deletion. */
static const char * const tone_labels[] = {
    N_("Overall temperature"), N_("Bass extension"), N_("Bass texture"), N_("Note thickness"),
    N_("Vocals"), N_("Female overtones"), N_("Sibilance LF"), N_("Sibilance HF"),
    N_("Impulse response"), N_("Air"), N_("Stereo width")
};
static const struct { const char * label; int min, max, step; double scale; } stage_controls[] = {
    {N_("Speaker angle (deg)"),10,60,5,1}, {N_("Amount (%)"),0,100,1,.01},
    {N_("Side level (dB)"),-12,6,1,1}, {N_("Bass width (dB)"),-12,6,1,1},
    {N_("Bass frequency (Hz)"),50,500,10,1}, {N_("Treble width (dB)"),-12,6,1,1},
    {N_("Treble frequency (Hz)"),2000,12000,500,1}, {N_("Reflections (%)"),0,100,1,.01},
    {N_("Room size (%)"),0,100,1,.01}, {N_("Damping (%)"),0,100,1,.01}
};
static const char * const stage_presets[] = {N_("Custom"), N_("Natural"), N_("Studio monitors"),
    N_("Small room"), N_("Hall"), N_("Focused center"), N_("Wide")};
static void populate_mseb(void);
static void populate_stage(void);
static void open_named_presets(lv_event_t * e);
static void confirm_action(int action, const char * title);

static void clear_sliders(int id) {
    setting_slider_t * sliders = id == PAGE_MSEB ? tone_sliders : stage_sliders;
    int count = id == PAGE_MSEB ? 11 : 10;
    if (id != PAGE_MSEB && id != PAGE_STAGE) return;
    for (int i = 0; i < count; i++) {
        sliders[i] = (setting_slider_t){0};
    }
}

static lv_obj_t * page(int id, const char * title) {
    if (!screens[id]) {
        lv_obj_t * label;
        screens[id] = build_subsonic_list_screen(title, &label, &lists[id]);
        titles[id] = label;
    }
    lv_label_set_text(titles[id], title);
    return lists[id];
}

static void show_page(int id) { nav_push(screens[id]); }
static bool clicked(lv_event_t * e) { return lv_event_get_code(e) == LV_EVENT_CLICKED; }
static void failure(const char * text) { show_error_toast(TR(text)); }
static void integer_format(char * out, size_t size, int value) { snprintf(out, size, "%d", value); }

/* Read back applied values so failed writes and presets update the existing controls. */
static void refresh_stage_controls(void) {
    soundstage_config_t c; int preset;
    native_audio_get_soundstage(&c, &preset);
    if (stage_preset_menu) gui_option_menu_set_selected(stage_preset_menu, preset);
    gui_pill_toggle_row_set_checked(stage_enabled_row, c.enabled);
    for (int i = 0; i < 10; ++i) {
        if (!stage_sliders[i].slider) continue;
        double value = *(double *)((char *)&c + soundstage_fields[i].offset) /
                       stage_controls[i].scale;
        int shown = (int)(floor(value / stage_controls[i].step + .5) *
                          stage_controls[i].step);
        setting_slider_set_index(&stage_sliders[i], shown);
    }
}

static setting_slider_t slider(lv_obj_t * list, const char * label, int min, int max,
                               int value, lv_event_cb_t callback, int index) {
    setting_slider_t ui = build_setting_slider(list, &(setting_slider_cfg_t){
        .caption = label, .min_index = min, .max_index = max, .index = value,
        .fmt = integer_format, .control_always_on = true
    });
    lv_obj_add_event_cb(ui.slider, callback, LV_EVENT_ALL, (void *)(intptr_t)index);
    return ui;
}

void gui_native_audio_refresh_gain(void) {
    for (int i = 0; i < 2; ++i)
        if (gain_switches[i]) {
            if (native_audio_gain_mode() == 1) lv_obj_add_state(gain_switches[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(gain_switches[i], LV_STATE_CHECKED);
        }
}

static void gain_switch_deleted(lv_event_t * e) {
    for (int i = 0; i < 2; ++i)
        if (gain_switches[i] == lv_event_get_target(e)) gain_switches[i] = NULL;
}

static void gain_clicked(lv_event_t * e) {
    if (!clicked(e)) return;
    gui_native_audio_quick_toggle_set(0, !gui_native_audio_quick_toggle_value(0));
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

lv_obj_t * gui_native_audio_add_gain_row(lv_obj_t * list) {
    lv_obj_t * row = add_pill_letter_toggle_row(list, TR("Gain"),
                                               native_audio_gain_mode() == 1, "LH", gain_clicked);
    lv_obj_t * sw = lv_obj_get_child(row, -1);
    lv_obj_add_event_cb(sw, gain_switch_deleted, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < 2; ++i) {
        if (!gain_switches[i]) { gain_switches[i] = sw; break; }
    }
    return sw;
}

static void mseb_failure(bool enabling) {
    failure(enabling && native_audio_boost_active() ? N_("Turn off loudness boost first")
                                                    : N_("Could not apply changes. Try again."));
}

static void mseb_enabled(lv_event_t * e) {
    if (!clicked(e)) return;
    native_mseb_state_t state; native_audio_get_mseb(&state);
    state.config.enabled = !state.config.enabled;
    if (!native_audio_set_mseb(&state)) mseb_failure(state.config.enabled);
    native_audio_get_mseb(&state);
    gui_pill_toggle_row_set_checked(mseb_enabled_row, state.config.enabled);
}

static void stage_enabled(lv_event_t * e) {
    if (!clicked(e)) return;
    soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset);
    c.enabled = !c.enabled;
    if (!native_audio_set_soundstage(&c, preset)) failure(N_("Could not apply soundstage. Try again."));
    native_audio_get_soundstage(&c, &preset);
    gui_pill_toggle_row_set_checked(stage_enabled_row, c.enabled);
}

static void range_changed(unsigned selected) {
    static const int ranges[] = {20,40,100};
    native_mseb_state_t state; native_audio_get_mseb(&state);
    state.range = ranges[selected];
    if (!native_audio_set_mseb(&state)) failure(N_("Could not save range. Try again."));
    populate_mseb();
}

static void tone_changed(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int value = lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) setting_slider_set_index(&tone_sliders[i], value);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    native_mseb_state_t state; native_audio_get_mseb(&state);
    if (i == MSEB_CONTROL_COUNT) state.config.width = 1 + value / 100.0;
    else state.config.values[i] = value;
    if (!native_audio_set_mseb(&state)) failure(N_("Could not apply changes. Try again."));
}

static void stage_changed(lv_event_t * e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int value = lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) setting_slider_set_index(&stage_sliders[i], value);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;
    soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset);
    int step = stage_controls[i].step;
    double v = floor((double)value / step + .5) * step;
    v = fmax(stage_controls[i].min, fmin(stage_controls[i].max, v));
    *(double *)((char *)&c + soundstage_fields[i].offset) = v * stage_controls[i].scale;
    if (!native_audio_set_soundstage(&c, 0)) failure(N_("Could not apply soundstage. Try again."));
    refresh_stage_controls();
}

static void preset_changed(unsigned selected) {
    int chosen = (int)selected;
    soundstage_config_t c; int previous; native_audio_get_soundstage(&c, &previous);
    bool enabled = c.enabled;
    if (chosen) native_audio_soundstage_preset(&c, chosen);
    c.enabled = enabled;
    if (!native_audio_set_soundstage(&c, chosen)) failure(N_("Could not apply soundstage. Try again."));
    refresh_stage_controls();
}

static void reset_stage(lv_event_t * e) {
    if (clicked(e)) confirm_action(2, TR("Reset soundstage to defaults?"));
}
static void reset_mseb(lv_event_t * e) {
    if (clicked(e)) confirm_action(1, TR("Reset to defaults"));
}

static bool valid_selected(native_mseb_state_t * state, char * name) {
    if (native_mseb_preset_read(selected_preset, true, state, name)) return true;
    failure(N_("Preset changed; reopen the preset list"));
    return false;
}

static void name_done(const char * text, void * data) {
    int slot = (int)(intptr_t)data;
    if (!text) return;
    char name[NATIVE_MSEB_NAME_MAX + 1];
    size_t len = strlen(text);
    while (len && (unsigned char)*text <= ' ') { text++; len--; }
    while (len && (unsigned char)text[len - 1] <= ' ') len--;
    if (!len || len > NATIVE_MSEB_NAME_MAX) { failure(N_("Enter a short preset name without line breaks")); return; }
    memcpy(name, text, len); name[len] = 0;
    for (size_t i = 0; i < len; i++) if ((unsigned char)name[i] < 32 || name[i] == 127) {
        failure(N_("Enter a short preset name without line breaks")); return;
    }
    for (int i = 1; i <= NATIVE_MSEB_PRESET_COUNT; i++) {
        native_mseb_state_t other; char existing[NATIVE_MSEB_NAME_MAX + 1];
        if (i != abs(slot) && native_mseb_preset_read(i, true, &other, existing) && !strcasecmp(name, existing)) {
            failure(N_("A preset with that name already exists")); return;
        }
    }
    native_mseb_state_t state; native_audio_get_mseb(&state);
    if (slot < 0) { char previous[NATIVE_MSEB_NAME_MAX + 1];
        if (!native_mseb_preset_read(-slot, true, &state, previous)) {
            failure(N_("Preset changed; reopen the preset list")); return;
        }
    }
    if (!native_mseb_preset_write(abs(slot), true, &state, name)) failure(N_("Could not save preset. Try again."));
    else show_info_toast(slot < 0 ? TR("Preset renamed") : TR("Preset saved"));
}

static void new_named(lv_event_t * e) {
    if (!clicked(e)) return;
    for (int i = 1; i <= NATIVE_MSEB_PRESET_COUNT; i++) if (!native_mseb_preset_exists(i, true)) {
        show_text_entry(TR("New preset name"), "", false, false, name_done, (void *)(intptr_t)i);
        return;
    }
    failure(N_("Preset limit reached; delete one first"));
}

static void load_selected(lv_event_t * e) {
    if (!clicked(e)) return;
    native_mseb_state_t state, live; char name[NATIVE_MSEB_NAME_MAX + 1];
    if (!valid_selected(&state, name)) return;
    if (state.legacy) { failure(N_("Older MSEB controls are unsupported; preset was not loaded")); return; }
    native_audio_get_mseb(&live); state.config.enabled = live.config.enabled;
    if (!native_audio_set_mseb(&state)) failure(N_("Could not load preset. Try again."));
    else { show_info_toast(TR("Preset loaded")); populate_mseb(); }
}
static void overwrite_selected(lv_event_t * e) {
    if (clicked(e)) confirm_action(3, TR("Overwrite preset?"));
}
static void rename_selected(lv_event_t * e) {
    if (!clicked(e)) return;
    native_mseb_state_t state; char name[NATIVE_MSEB_NAME_MAX + 1];
    if (valid_selected(&state, name)) show_text_entry(TR("Rename preset"), name, false, false,
                                                   name_done, (void *)(intptr_t)-selected_preset);
}
static void delete_selected(lv_event_t * e) {
    if (clicked(e)) confirm_action(4, selected_damaged ? TR("Delete damaged preset?") : TR("Delete preset?"));
}

static void manage_preset(lv_event_t * e) {
    if (!clicked(e)) return;
    selected_preset = (int)(intptr_t)lv_event_get_user_data(e);
    native_mseb_state_t state; char name[NATIVE_MSEB_NAME_MAX + 1];
    selected_damaged = !native_mseb_preset_read(selected_preset, true, &state, name);
    lv_obj_t * list = page(PAGE_MANAGE, selected_damaged ? TR("Damaged preset") : name);
    unsigned rows = selected_damaged ? 1 : 4;
    if (lv_obj_get_child_count(list) != rows) lv_obj_clean(list);
    if (lv_obj_get_child_count(list) == 0) {
        if (!selected_damaged) {
            add_pill_chevron_row(list, TR("Load"), load_selected);
            add_pill_chevron_row(list, TR("Overwrite"), overwrite_selected);
            add_pill_chevron_row(list, TR("Rename"), rename_selected);
        }
        add_pill_chevron_row(list, TR("Delete"), delete_selected);
    }
    show_page(PAGE_MANAGE);
}

typedef struct { int id; char name[NATIVE_MSEB_NAME_MAX + 1]; } preset_entry_t;
static int compare_presets(const void * a, const void * b) {
    return strcasecmp(((const preset_entry_t *)a)->name, ((const preset_entry_t *)b)->name);
}
static void open_named_presets(lv_event_t * e) {
    if (e && !clicked(e)) return;
    lv_obj_t * list = page(PAGE_PRESETS, TR("Named presets"));
    preset_entry_t entries[NATIVE_MSEB_PRESET_COUNT]; int count = 0;
    for (int i = 1; i <= NATIVE_MSEB_PRESET_COUNT; i++) {
        native_mseb_state_t state;
        if (native_mseb_preset_read(i, true, &state, entries[count].name)) entries[count++].id = i;
    }
    qsort(entries, count, sizeof(entries[0]), compare_presets);
    for (int i = 1; i <= NATIVE_MSEB_PRESET_COUNT; i++) {
        native_mseb_state_t state; char name[NATIVE_MSEB_NAME_MAX + 1];
        if (!native_mseb_preset_exists(i, true) || native_mseb_preset_read(i, true, &state, name)) continue;
        entries[count].id = i;
        snprintf(entries[count++].name, sizeof(entries[0].name), TR("Damaged preset %d"), i);
    }
    static int row_ids[NATIVE_MSEB_PRESET_COUNT];
    bool rebuild = lv_obj_get_child_count(list) != (unsigned)count + 1;
    for (int i = 0; i < count; ++i) if (row_ids[i] != entries[i].id) rebuild = true;
    if (rebuild) lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        if (rebuild) {
            lv_obj_t * row = add_pill_chevron_row(list, entries[i].name, NULL);
            lv_obj_add_event_cb(row, manage_preset, LV_EVENT_CLICKED, (void *)(intptr_t)entries[i].id);
        } else lv_label_set_text(lv_obj_get_child(lv_obj_get_child(list, i), 0), entries[i].name);
        row_ids[i] = entries[i].id;
    }
    if (rebuild) add_pill_chevron_row(list, TR("Save named preset"), new_named);
    show_page(PAGE_PRESETS);
}

static void slot_action(lv_event_t * e) {
    if (!clicked(e)) return;
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    native_mseb_state_t state; native_audio_get_mseb(&state);
    if (slot > 0) {
        if (native_mseb_preset_write(slot, false, &state, NULL)) show_info_toast(TR("Saved to slot"));
        else failure(N_("Could not save slot. Try again."));
    } else {
        native_mseb_state_t next;
        if (!native_mseb_preset_exists(-slot, false)) { show_info_toast(TR("Slot is empty")); return; }
        if (!native_mseb_preset_read(-slot, false, &next, NULL)) {
            failure(N_("Older MSEB controls are unsupported; slot was not loaded")); return;
        }
        next.config.enabled = state.config.enabled; next.range = state.range;
        if (!native_audio_set_mseb(&next)) failure(N_("Could not load slot. Try again."));
        else { show_info_toast(TR("Slot loaded")); populate_mseb(); }
    }
}

static void convert_legacy(lv_event_t * e) {
    if (clicked(e)) { pending_enable_mseb = false; confirm_action(5, TR("This saved layout is unsupported. Switching replaces instrument presence, bass bite, and treble bite with the stock controls.")); }
}

static void apply_confirmed(lv_event_t * e) {
    if (!clicked(e)) return;
    native_mseb_state_t state; native_audio_get_mseb(&state);
    bool ok = false;
    if (pending_action == 1) {
        bool enabled = state.config.enabled;
        state = (native_mseb_state_t){ .config = {.enabled = enabled, .width = 1}, .range = state.range };
        ok = native_audio_set_mseb(&state);
    } else if (pending_action == 2) {
        soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset);
        bool enabled = c.enabled; native_audio_soundstage_preset(&c, 1); c.enabled = enabled;
        ok = native_audio_set_soundstage(&c, 1);
    } else if (pending_action == 3) {
        native_mseb_state_t existing; char name[NATIVE_MSEB_NAME_MAX + 1];
        ok = valid_selected(&existing, name) && native_mseb_preset_write(selected_preset, true, &state, name);
    } else if (pending_action == 4) ok = native_mseb_preset_delete(selected_preset);
    else if (pending_action == 5) ok = native_audio_convert_mseb(pending_enable_mseb);
    if (!ok && pending_action == 5) mseb_failure(pending_enable_mseb || state.config.enabled);
    else if (!ok) failure(N_("Could not apply changes. Try again."));
    else show_info_toast(TR("Saved"));
    pending_enable_mseb = false;
    nav_pop();
    if (lists[PAGE_MSEB]) populate_mseb();
    if (lists[PAGE_STAGE]) {
        if (pending_action == 2) refresh_stage_controls();
        else populate_stage();
    }
}
static void cancel_confirm(lv_event_t * e) { if (clicked(e)) { pending_enable_mseb = false; nav_pop(); } }
static void confirm_action(int action, const char * title) {
    pending_action = action;
    lv_obj_t * list = page(PAGE_CONFIRM, title);
    if (lv_obj_get_child_count(list) == 0) {
        add_pill_chevron_row(list, TR("Confirm"), apply_confirmed);
        add_pill_chevron_row(list, TR("Cancel"), cancel_confirm);
    }
    show_page(PAGE_CONFIRM);
}

static int tone_value(const native_mseb_state_t * state, int i) {
    return i == MSEB_CONTROL_COUNT ? (int)round((state->config.width - 1) * 100)
                                   : state->config.values[i];
}

static void refresh_mseb_controls(const native_mseb_state_t * state) {
    gui_pill_toggle_row_set_checked(mseb_enabled_row, state->config.enabled);
    if (mseb_range_menu) gui_option_menu_set_selected(mseb_range_menu,
        state->range == 20 ? 0 : state->range == 40 ? 1 : 2);
    for (int i = 0; i <= MSEB_CONTROL_COUNT; ++i) {
        int value = tone_value(state, i);
        if (tone_sliders[i].slider) {
            lv_slider_set_range(tone_sliders[i].slider, -state->range, state->range);
            setting_slider_set_index(&tone_sliders[i], value);
        } else if (tone_overflow[i]) {
            lv_label_set_text_fmt(tone_overflow[i], TR("%s: %d (outside ±%d)"),
                                  TR(tone_labels[i]), value, state->range);
        }
    }
}

static void populate_mseb(void) {
    lv_obj_t * list = page(PAGE_MSEB, TR("MSEB"));
    native_mseb_state_t state; native_audio_get_mseb(&state);
    unsigned shape = 0;
    for (int i = 0; i <= MSEB_CONTROL_COUNT; ++i)
        if (abs(tone_value(&state, i)) > state.range) shape |= 1u << i;
    if (lv_obj_get_child_count(list) && mseb_legacy == state.legacy && mseb_shape == shape) {
        if (!state.legacy) refresh_mseb_controls(&state);
        return;
    }
    clear_sliders(PAGE_MSEB);
    memset(tone_overflow, 0, sizeof(tone_overflow));
    mseb_enabled_row = mseb_range_menu = NULL;
    lv_obj_clean(list);
    mseb_legacy = state.legacy;
    mseb_shape = shape;
    if (state.legacy) { add_pill_chevron_row(list, TR("Switch to stock MSEB"), convert_legacy); return; }
    mseb_enabled_row = add_pill_toggle_row(list, TR("Enabled"), state.config.enabled, mseb_enabled);
    mseb_range_menu = add_pill_option_menu_row(list, TR("Adjustment range"), "±20\n±40\n±100", state.range == 20 ? 0 : state.range == 40 ? 1 : 2, range_changed);
    for (int i = 0; i <= MSEB_CONTROL_COUNT; i++) {
        if (i == MSEB_CONTROL_COUNT) {
            add_section_header(list, TR("Sound stage"));
            add_wrapped_section_header(list, TR("Stock MSEB width changes the channel mix."));
        }
        int value = tone_value(&state, i);
        if (abs(value) > state.range) {
            char label[256]; snprintf(label, sizeof(label), TR("%s: %d (outside ±%d)"), TR(tone_labels[i]), value, state.range);
            tone_overflow[i] = add_wrapped_section_header(list, label);
        } else tone_sliders[i] = slider(list, TR(tone_labels[i]), -state.range, state.range, value, tone_changed, i);
    }
    add_pill_chevron_row(list, TR("Reset to defaults"), reset_mseb);
    add_pill_chevron_row(list, TR("Named presets"), open_named_presets);
    add_pill_chevron_row(list, TR("Save named preset"), new_named);
    for (int save = 1; save >= 0; save--) {
        add_section_header(list, save ? TR("Save to slot") : TR("Load from slot"));
        for (int i = 1; i <= 3; i++) {
            char label[64]; snprintf(label, sizeof(label), TR("Slot %d"), i);
            lv_obj_t * row = add_pill_chevron_row(list, label, NULL);
            lv_obj_add_event_cb(row, slot_action, LV_EVENT_CLICKED, (void *)(intptr_t)(save ? i : -i));
        }
    }
}

static void populate_stage(void) {
    lv_obj_t * list = page(PAGE_STAGE, TR("Soundstage"));
    if (lv_obj_get_child_count(list)) { refresh_stage_controls(); return; }
    soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset);
    stage_enabled_row = add_pill_toggle_row(list, TR("Enabled"), c.enabled, stage_enabled);
    char options[512] = "";
    for (int i = 0; i <= 6; i++) {
        size_t used = strlen(options);
        snprintf(options + used, sizeof(options) - used, "%s%s", i ? "\n" : "", TR(stage_presets[i]));
    }
    stage_preset_menu = add_pill_option_menu_row(list, TR("Preset"), options, preset, preset_changed);
    for (int i = 0; i < 10; i++) {
        if (i == 0) add_section_header(list, TR("Virtual speakers"));
        if (i == 2) add_section_header(list, TR("Side EQ"));
        if (i == 7) add_section_header(list, TR("Room"));
        double value = *(double *)((char *)&c + soundstage_fields[i].offset) / stage_controls[i].scale;
        int shown = (int)(floor(value / stage_controls[i].step + .5) * stage_controls[i].step);
        stage_sliders[i] = slider(list, TR(stage_controls[i].label), stage_controls[i].min,
                                  stage_controls[i].max, shown, stage_changed, i);
    }
    add_pill_chevron_row(list, TR("Reset to defaults"), reset_stage);
}
static void open_mseb(lv_event_t * e) { if (clicked(e)) { populate_mseb(); show_page(PAGE_MSEB); } }
static void open_stage(lv_event_t * e) { if (clicked(e)) { populate_stage(); show_page(PAGE_STAGE); } }

void gui_native_audio_add_effect_rows(lv_obj_t * list, lv_obj_t ** mseb_row,
                                     lv_obj_t ** soundstage_row) {
    lv_obj_t * mseb = add_pill_chevron_row(list, TR("MSEB"), open_mseb);
    lv_obj_t * soundstage = add_pill_chevron_row(list, TR("Soundstage"), open_stage);
    if (mseb_row) *mseb_row = mseb;
    if (soundstage_row) *soundstage_row = soundstage;
}
bool gui_native_audio_quick_toggle_value(int kind) {
    if (kind == 0) return native_audio_gain_mode() == 1;
    if (kind == 1) { native_mseb_state_t state; native_audio_get_mseb(&state); return state.config.enabled; }
    soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset); return c.enabled;
}

void gui_native_audio_quick_toggle_set(int kind, bool value) {
    if (kind == 0) { native_audio_set_gain_mode(value); gui_native_audio_refresh_gain(); return; }
    if (kind == 1) {
        native_mseb_state_t state; native_audio_get_mseb(&state);
        if (state.legacy && value) {
            pending_enable_mseb = true;
            confirm_action(5, TR("This saved layout is unsupported. Switching replaces instrument presence, bass bite, and treble bite with the stock controls."));
            return;
        }
        state.config.enabled = value;
        if (!native_audio_set_mseb(&state)) mseb_failure(value);
        native_audio_get_mseb(&state);
        gui_pill_toggle_row_set_checked(mseb_enabled_row, state.config.enabled);
        return;
    }
    soundstage_config_t c; int preset; native_audio_get_soundstage(&c, &preset);
    c.enabled = value;
    if (!native_audio_set_soundstage(&c, preset)) failure(N_("Could not apply changes. Try again."));
    native_audio_get_soundstage(&c, &preset);
    gui_pill_toggle_row_set_checked(stage_enabled_row, c.enabled);
}

void gui_native_audio_quick_toggle_open(int kind) {
    if (kind == 0) { gui_native_audio_refresh_gain(); gui_settings_open_sound(); }
    else if (kind == 1) { populate_mseb(); show_page(PAGE_MSEB); }
    else { populate_stage(); show_page(PAGE_STAGE); }
}

void gui_native_audio_deinit(void) {
    for (int i = 0; i < PAGE_COUNT; i++) {
        clear_sliders(i);
        if (screens[i]) lv_obj_delete(screens[i]);
        screens[i] = lists[i] = titles[i] = NULL;
    }
    mseb_enabled_row = stage_enabled_row = stage_preset_menu = mseb_range_menu = NULL;
    memset(tone_overflow, 0, sizeof(tone_overflow));
}
