#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lvgl.h>

#include "../../assets/assets.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_notifications.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_shell.h"
#include "../../text/rendering/gui_text_input.h"
#include "../../appearance/gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../../audio/effects/peq.h"
#include "../../widgets/builders/screen_builders.h"

#define TRACE_CAPACITY 32768
static char trace_buffer[TRACE_CAPACITY];
static size_t trace_size;
static int plugin_profiles_available;
static int profile_query_count;
static int download_rows;
static int nav_push_count;
static int nav_pop_count;
static int finalize_count;
static int popup_build_count;
static int popup_show_count;
static int popup_hide_count;
static int popup_delete_count;
static int dead_zone_count;
static int accent_color_count;
static int error_toasts;
static int info_toasts;
static lv_obj_t * last_pushed_screen;
static lv_obj_t * last_confirm_button;
static lv_obj_t * last_cancel_button;
static lv_obj_t * delete_confirm_button;
static lv_obj_t * delete_cancel_button;
static const char * text_entry_title;
static char text_entry_initial[256];
static void (*text_entry_done)(const char *, void *);
static void *text_entry_user_data;
static lv_style_t test_accent_style;
static lv_style_t test_accent_knob_style;

static bool test_font_get_glyph(const lv_font_t * font, lv_font_glyph_dsc_t * descriptor,
                                uint32_t codepoint, uint32_t next_codepoint) {
    (void)next_codepoint;
    if (!codepoint) return false;
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->resolved_font = font;
    descriptor->adv_w = 8;
    descriptor->box_w = 8;
    descriptor->box_h = 16;
    return true;
}

static const void * test_font_get_bitmap(lv_font_glyph_dsc_t * descriptor,
                                         lv_draw_buf_t * draw_buffer) {
    (void)descriptor; (void)draw_buffer; return NULL;
}

static void tracef(const char * format, ...);
static bool eq_download_profiles_available(void);
static void music_profiles_menu_cb(lv_event_t * event);
const char * basename_of(const char * path);
int count_plugin_rows_in_group(const char * group, int count, const char * section);
int plugin_manager_get_music_audio_list_item_count(void);
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * action);

#ifdef EQ_SPEC_BASELINE
#include "baseline_fixture.c"
#else
#include "gui_equalizer.c"
#endif

static void tracef(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(trace_buffer + trace_size, TRACE_CAPACITY - trace_size, format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < TRACE_CAPACITY - trace_size);
    trace_size += (size_t)n;
}

static bool eq_download_profiles_available(void) {
    ++profile_query_count;
    tracef("profiles-query:%d\n", plugin_profiles_available);
    return plugin_profiles_available != 0;
}

static void music_profiles_menu_cb(lv_event_t * event) {
    (void)event;
    tracef("download-profiles-click\n");
}

int plugin_manager_get_music_audio_list_item_count(void) {
    return plugin_profiles_available;
}

int count_plugin_rows_in_group(const char * group, int count, const char * section) {
    assert(strcmp(group, "music_audio") == 0);
    assert(strcmp(section, "profiles") == 0);
    ++profile_query_count;
    tracef("profiles-query:%d\n", plugin_profiles_available);
    return count > 0 ? plugin_profiles_available : 0;
}

#define DEFINE_TEST_STYLE(name) lv_style_t name
DEFINE_TEST_STYLE(list_row_style);
DEFINE_TEST_STYLE(list_row_pressed_style);
DEFINE_TEST_STYLE(pill_row_bg_style);
DEFINE_TEST_STYLE(native_row_min_style);
DEFINE_TEST_STYLE(icon_press_style);
DEFINE_TEST_STYLE(style_theme_screen_bg);
DEFINE_TEST_STYLE(style_theme_card_bg);
DEFINE_TEST_STYLE(style_theme_text_primary);
DEFINE_TEST_STYLE(style_theme_text_muted);
DEFINE_TEST_STYLE(style_theme_tile);
DEFINE_TEST_STYLE(style_theme_tile_pressed);
DEFINE_TEST_STYLE(style_theme_scrollbar);
DEFINE_TEST_STYLE(style_theme_switch_track);
DEFINE_TEST_STYLE(style_theme_switch_knob);

lv_style_t * gui_theme_accent_style(void) { return &test_accent_style; }
lv_style_t * gui_theme_accent_knob_style(void) { return &test_accent_knob_style; }
lv_style_t * gui_theme_accent_outline_style(void) { return &test_accent_style; }
const lv_font_t * gui_theme_font(gui_font_role_t role) { (void)role; return &lv_font_montserrat_16; }
lv_color_t accent_lv_color(void) { ++accent_color_count; return lv_color_hex(0x4499cc); }
int32_t ui_list_row_width(void) { return 360; }
int32_t ui_list_row_width_wide(void) { return 440; }
int32_t ui_list_row_height(void) { return 48; }
void generic_back_cb(lv_event_t * event) { (void)event; }
void align_screen_header_action(lv_obj_t * object, int32_t padding) { (void)object; (void)padding; }
void reserve_title_width_before(lv_obj_t * title, lv_obj_t * action) { (void)title; (void)action; }

lv_obj_t * build_screen_header(lv_obj_t * parent, const char * title, lv_event_cb_t back,
                               const char * right_text, lv_event_cb_t right_cb) {
    (void)back; (void)right_text; (void)right_cb;
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, title);
    tracef("header:%s\n", title);
    return label;
}

lv_obj_t * build_subsonic_list_screen(const char * title, lv_obj_t ** out_title, lv_obj_t ** out_list) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 480, 800);
    lv_obj_t * label = lv_label_create(screen);
    lv_label_set_text(label, title);
    lv_obj_t * list = lv_obj_create(screen);
    lv_obj_set_pos(list, 0, 80);
    lv_obj_set_size(list, 480, 700);
    if (out_title) *out_title = label;
    if (out_list) *out_list = list;
    tracef("list-screen:%s\n", title);
    return screen;
}

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * text, lv_event_cb_t callback) {
    lv_obj_t * row = lv_btn_create(parent);
    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, NULL);
    if (strcmp(text, "Download profiles") == 0) ++download_rows;
    tracef("row:%s\n", text);
    return row;
}

lv_obj_t * build_confirm_popup(const char * title, lv_label_long_mode_t mode,
                               lv_obj_t ** out_title, const char * body,
                               const char * confirm, lv_color_t confirm_color,
                               lv_event_cb_t confirm_cb, lv_obj_t ** confirm_row,
                               const char * cancel, lv_color_t cancel_color,
                               lv_event_cb_t cancel_cb, lv_obj_t ** cancel_row,
                               lv_event_cb_t backdrop_cb, lv_obj_t ** backdrop) {
    (void)mode; (void)body; (void)confirm; (void)confirm_color;
    (void)cancel; (void)cancel_color;
    lv_obj_t * popup = lv_obj_create(lv_layer_top());
    last_confirm_button = lv_btn_create(popup);
    last_cancel_button = lv_btn_create(popup);
    if (confirm_cb) lv_obj_add_event_cb(last_confirm_button, confirm_cb, LV_EVENT_CLICKED, NULL);
    if (cancel_cb) lv_obj_add_event_cb(last_cancel_button, cancel_cb, LV_EVENT_CLICKED, NULL);
    if (confirm_row) *confirm_row = last_confirm_button;
    if (cancel_row) *cancel_row = last_cancel_button;
    lv_obj_t * label = lv_label_create(popup);
    lv_label_set_text(label, title);
    if (out_title) *out_title = label;
    if (backdrop) {
        *backdrop = lv_obj_create(lv_layer_top());
        if (backdrop_cb) lv_obj_add_event_cb(*backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);
    }
    ++popup_build_count;
    if (strcmp(title, "Delete this profile?") == 0) {
        delete_confirm_button = last_confirm_button;
        delete_cancel_button = last_cancel_button;
    }
    tracef("popup-build:%s\n", title);
    return popup;
}

void gui_popup_show(gui_popup_t * popup) {
    if (popup) popup->visible = true;
    ++popup_show_count;
    tracef("popup-show\n");
}
void gui_popup_hide(gui_popup_t * popup) {
    if (popup) popup->visible = false;
    ++popup_hide_count;
    tracef("popup-hide\n");
}
void gui_popup_teardown(gui_popup_t * popup) {
    if (!popup) return;
    if (popup->popup) lv_obj_delete(popup->popup);
    if (popup->backdrop) lv_obj_delete(popup->backdrop);
    popup->popup = popup->backdrop = NULL;
    popup->visible = false;
    ++popup_delete_count;
    tracef("popup-delete\n");
}

void finalize_screen_navigation(lv_obj_t * screen) {
    (void)screen;
    ++finalize_count;
    tracef("finalize\n");
}
const char * i18n_tr(const char * text) { return text; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
void nav_push(lv_obj_t * screen) {
    ++nav_push_count;
    last_pushed_screen = screen;
    tracef("push\n");
}
void nav_pop(void) { ++nav_pop_count; tracef("pop\n"); }
void register_swipe_dead_zone(lv_obj_t * object) { assert(object); ++dead_zone_count; tracef("dead-zone\n"); }
void unregister_swipe_dead_zone(lv_obj_t * object) { (void)object; --dead_zone_count; }

void show_error_toast(const char * message) { ++error_toasts; tracef("error:%s\n", message); }
void show_info_toast(const char * message) { ++info_toasts; tracef("info:%s\n", message); }
void show_text_entry(const char * title, const char * initial, bool password, bool numeric,
                     void (*done)(const char *, void *), void * user_data) {
    (void)password; (void)numeric;
    text_entry_title = title;
    snprintf(text_entry_initial, sizeof(text_entry_initial), "%s", initial ? initial : "");
    text_entry_done = done;
    text_entry_user_data = user_data;
    tracef("text-entry:%s:%s\n", title, text_entry_initial);
}

const char * asset_path(const char * path) { return path; }
const char * basename_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void init_styles(void) {
    lv_style_t * styles[] = { &list_row_style, &list_row_pressed_style, &pill_row_bg_style,
        &native_row_min_style, &icon_press_style, &style_theme_screen_bg, &style_theme_card_bg,
        &style_theme_text_primary, &style_theme_text_muted, &style_theme_tile,
        &style_theme_tile_pressed, &style_theme_scrollbar, &style_theme_switch_track,
        &style_theme_switch_knob, &test_accent_style, &test_accent_knob_style };
    for (size_t i = 0; i < sizeof(styles) / sizeof(styles[0]); ++i) lv_style_init(styles[i]);
    lv_font_montserrat_16.get_glyph_dsc = test_font_get_glyph;
    lv_font_montserrat_16.get_glyph_bitmap = test_font_get_bitmap;
    lv_font_montserrat_16.line_height = 18;
    lv_font_montserrat_16.base_line = 4;
}

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}

#if defined(EQ_SPEC_BASELINE)
static void harness_build_screens(lv_event_cb_t callback, bool (*available)(void)) {
    (void)callback; (void)available;
    eq_spec_build_screens();
}
static void harness_build_popups(void) { eq_spec_build_popups(); }
static void harness_teardown_popups(void) { eq_spec_teardown_popups(); }
static void harness_teardown_screens(void) { eq_spec_teardown_screens(); }
static void harness_update_summary(lv_obj_t * label) {
    eq_spec_update_summary(label);
}
static void harness_accent_changed(void) { eq_spec_accent_changed(); }
static void harness_eq_row_event_cb(lv_event_t * event) { eq_screen_btn_event_cb(event); }
static void harness_crossfeed_row_event_cb(lv_event_t * event) { crossfeed_screen_btn_event_cb(event); }
#else
static void harness_build_screens(lv_event_cb_t callback, bool (*available)(void)) {
    gui_equalizer_build_screens(callback, available);
}
static void harness_build_popups(void) { gui_equalizer_build_popups(); }
static void harness_teardown_popups(void) { gui_equalizer_teardown_popups(); }
static void harness_teardown_screens(void) { gui_equalizer_teardown_screens(); }
static void harness_update_summary(lv_obj_t * label) { gui_equalizer_update_summary(label); }
static void harness_accent_changed(void) { gui_equalizer_accent_changed(); }
static void harness_eq_row_event_cb(lv_event_t * event) { gui_equalizer_eq_row_event_cb(event); }
static void harness_crossfeed_row_event_cb(lv_event_t * event) { gui_equalizer_crossfeed_row_event_cb(event); }
#endif

static void make_profile_dir(void) {
    assert(access("music/PEQ_Profiles", F_OK) == 0);
    assert(access(".compas", F_OK) == 0);
}

static int profile_file_exists(const char * name) {
    char path[512];
    snprintf(path, sizeof(path), "music/PEQ_Profiles/%s.peq", name);
    return access(path, F_OK) == 0;
}

int main(void) {
    make_profile_dir();
    lv_init();
    init_styles();
    lv_display_t * display = lv_display_create(480, 800);
    assert(display);
    static uint8_t display_buffer[480 * 24 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    peq_init();

    lv_obj_t * summary_host = lv_obj_create(NULL);
    lv_obj_t * summary = lv_label_create(summary_host);
    harness_update_summary(summary);
    assert(strstr(lv_label_get_text(summary), "Custom") != NULL);

    int accent_before_graph = accent_color_count;
    harness_accent_changed();
    assert(accent_color_count == accent_before_graph);

    plugin_profiles_available = 0;
    harness_build_screens(music_profiles_menu_cb, eq_download_profiles_available);
    assert(eq_screen && eq_band_options_screen && settings_crossfeed_screen && eq_profiles_screen);
    assert(finalize_count == 3);
    assert(profile_query_count == 0);
    lv_obj_send_event(eq_screen, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_send_event(settings_crossfeed_screen, LV_EVENT_SCREEN_LOADED, NULL);
    int queries_before = profile_query_count;
    lv_obj_send_event(eq_profiles_screen, LV_EVENT_SCREEN_LOADED, NULL);
    assert(download_rows == 0 && profile_query_count == queries_before + 1);
    plugin_profiles_available = 1;
    populate_eq_profiles_screen();
    assert(download_rows == 1 && profile_query_count == queries_before + 2);
    lv_obj_send_event(lv_obj_get_child(eq_profiles_list, 1), LV_EVENT_CLICKED, NULL);
    plugin_profiles_available = 0;
    populate_eq_profiles_screen();
    assert(download_rows == 1 && profile_query_count == queries_before + 3);

    harness_build_popups();
    assert(popup_build_count == 2);
    accent_before_graph = accent_color_count;
    harness_accent_changed();
    assert(accent_color_count == accent_before_graph + 2);
    gui_settings_refresh_font_geometry();
    lv_obj_t * eq_row = lv_btn_create(lv_screen_active());
    lv_obj_add_event_cb(eq_row, harness_eq_row_event_cb, LV_EVENT_ALL, NULL);
    int push_before_row = nav_push_count;
    lv_obj_send_event(eq_row, LV_EVENT_VALUE_CHANGED, NULL);
    assert(nav_push_count == push_before_row);
    lv_obj_send_event(eq_row, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == push_before_row + 1 && last_pushed_screen == eq_screen);
    lv_obj_t * crossfeed_row = lv_btn_create(lv_screen_active());
    lv_obj_add_event_cb(crossfeed_row, harness_crossfeed_row_event_cb, LV_EVENT_ALL, NULL);
    push_before_row = nav_push_count;
    lv_obj_send_event(crossfeed_row, LV_EVENT_PRESSED, NULL);
    assert(nav_push_count == push_before_row);
    lv_obj_send_event(crossfeed_row, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == push_before_row + 1);
    assert(last_pushed_screen == settings_crossfeed_screen);

    /* Actual PEQ engine state is edited through the same callbacks as widgets. */
    peq_set_preamp_db(3.0);
    eq_numeric_entry_done_cb("15", (void *)(intptr_t)EQ_FIELD_PREAMP);
    assert(peq_get_preamp_db() == 12.0);
    eq_numeric_entry_done_cb("-15", (void *)(intptr_t)EQ_FIELD_GAIN);
    assert(peq_get_band(0)->gain_db == -12.0);
    eq_numeric_entry_done_cb("0.01", (void *)(intptr_t)EQ_FIELD_Q);
    assert(peq_get_band(0)->q >= 0.1 && peq_get_band(0)->q < 0.2);
    eq_numeric_entry_done_cb("125", (void *)(intptr_t)EQ_FIELD_CROSSFEED);
    assert(peq_get_crossfeed() == 1.0);
    eq_numeric_entry_done_cb("-15", (void *)(intptr_t)EQ_FIELD_PREAMP);
    assert(peq_get_preamp_db() == -12.0);

    lv_dropdown_set_selected(eq_type_dropdown, PEQ_TYPE_HIGH_SHELF);
    lv_obj_send_event(eq_type_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    assert(peq_get_band(0)->type == PEQ_TYPE_HIGH_SHELF);
    lv_obj_add_state(eq_bypass_switch, LV_STATE_CHECKED);
    lv_obj_send_event(eq_bypass_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(!peq_get_bypass());
    lv_obj_clear_state(eq_bypass_switch, LV_STATE_CHECKED);
    lv_obj_send_event(eq_bypass_switch, LV_EVENT_VALUE_CHANGED, NULL);
    assert(peq_get_bypass());

    lv_slider_set_value(eq_crossfeed_slider, 72, LV_ANIM_OFF);
    lv_obj_send_event(eq_crossfeed_slider, LV_EVENT_VALUE_CHANGED, NULL);
    assert(peq_get_crossfeed() > 0.70 && peq_get_crossfeed() < 0.74);
    lv_dropdown_set_selected(eq_crossfeed_angle_dropdown, 3);
    lv_obj_send_event(eq_crossfeed_angle_dropdown, LV_EVENT_VALUE_CHANGED, NULL);
    assert(peq_get_crossfeed_angle() == 60);

    lv_obj_t * band_nav = lv_obj_get_child(eq_main_content, 0);
    lv_obj_t * next_band = lv_obj_get_child(band_nav, 2);
    for (int i = 0; i < PEQ_NUM_BANDS; ++i)
        lv_obj_send_event(next_band, LV_EVENT_CLICKED, NULL);
    assert(eq_graph_visible);
    assert(!lv_obj_has_flag(eq_graph_panel, LV_OBJ_FLAG_HIDDEN));
    accent_before_graph = accent_color_count;
    harness_accent_changed();
    assert(accent_color_count == accent_before_graph + 2);
    gui_settings_refresh_font_geometry();

    /* Invalid names fail without changing the loaded profile identity. */
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "Keep Me");
    assert(!eq_save_named_profile(""));
    assert(!eq_save_named_profile("../escape"));
    assert(strcmp(eq_current_profile_name, "Keep Me") == 0);
    assert(access("music/escape.peq", F_OK) != 0);

    assert(rmdir("music/PEQ_Profiles") == 0);
    FILE * blocker = fopen("music/PEQ_Profiles", "w");
    assert(blocker);
    fclose(blocker);
    assert(!eq_save_named_profile("Unwritable"));
    assert(strcmp(eq_current_profile_name, "Keep Me") == 0);
    assert(unlink("music/PEQ_Profiles") == 0);
    assert(mkdir("music/PEQ_Profiles", 0755) == 0);

    lv_obj_t * failed_load_row = lv_btn_create(eq_profiles_list);
    lv_obj_add_event_cb(failed_load_row, eq_profile_row_cb, LV_EVENT_CLICKED,
                        "music/PEQ_Profiles/does-not-exist.peq");
    lv_obj_send_event(failed_load_row, LV_EVENT_CLICKED, NULL);
    assert(strcmp(eq_current_profile_name, "Keep Me") == 0);
    assert(nav_pop_count == 0);
    lv_obj_delete(failed_load_row);

    /* Profile creation, load, rename, replace, and delete use real PEQ files. */
    assert(eq_save_named_profile("Warm Sound"));
    assert(profile_file_exists("Warm Sound"));
    double saved_gain = peq_get_band(0)->gain_db;
    peq_set_band(0, 1000.0, 7.0, 1.0);
    populate_eq_profiles_screen();
    assert(eq_profile_count == 1);
    lv_obj_t * profile_row = lv_obj_get_child(eq_profiles_list, 0);
    lv_obj_send_event(profile_row, LV_EVENT_CLICKED, NULL);
    assert(peq_get_band(0)->gain_db == saved_gain);
    assert(strcmp(eq_current_profile_name, "Warm Sound") == 0);

    eq_profiles_edit_mode = true;
    queries_before = profile_query_count;
    populate_eq_profiles_screen();
    assert(profile_query_count == queries_before);
    profile_row = lv_obj_get_child(eq_profiles_list, 0);
    lv_obj_send_event(profile_row, LV_EVENT_CLICKED, NULL);
    assert(text_entry_done);
    text_entry_done("Warm Renamed", text_entry_user_data);
    assert(profile_file_exists("Warm Renamed") && !profile_file_exists("Warm Sound"));
    eq_set_current_profile_from_path("music/PEQ_Profiles/Warm Renamed.peq");
    peq_set_band(0, 1000.0, 5.0, 1.0);
    eq_profiles_replace_mode = true;
    queries_before = profile_query_count;
    populate_eq_profiles_screen();
    assert(profile_query_count == queries_before);
    profile_row = lv_obj_get_child(eq_profiles_list, 0);
    lv_obj_send_event(profile_row, LV_EVENT_CLICKED, NULL);
    assert(strcmp(eq_current_profile_name, "Warm Renamed") == 0);
    assert(nav_pop_count > 0);

    eq_profiles_edit_mode = true;
    populate_eq_profiles_screen();
    profile_row = lv_obj_get_child(eq_profiles_list, 0);
    lv_obj_t * delete_icon = lv_obj_get_child(profile_row, 1);
    lv_obj_send_event(delete_icon, LV_EVENT_CLICKED, NULL);
    assert(eq_profile_delete_popup.visible);
    lv_obj_send_event(delete_cancel_button, LV_EVENT_CLICKED, NULL);
    assert(!eq_profile_delete_popup.visible && profile_file_exists("Warm Renamed"));
    lv_obj_send_event(delete_icon, LV_EVENT_CLICKED, NULL);
    assert(eq_profile_delete_popup.visible);
    lv_obj_send_event(delete_confirm_button, LV_EVENT_CLICKED, NULL);
    assert(!profile_file_exists("Warm Renamed"));

    /* Keep lifecycle phases separate: popup teardown leaves screens alive. */
    lv_obj_t * retained_eq = eq_screen;
    harness_teardown_popups();
    assert(eq_screen == retained_eq && lv_obj_is_valid(retained_eq));
    assert(eq_profile_delete_popup.popup == NULL && eq_save_choice_popup.popup == NULL);
    snprintf(eq_current_profile_name, sizeof(eq_current_profile_name), "Retained Name");
    char retained_profile_name[sizeof(eq_current_profile_name)];
    snprintf(retained_profile_name, sizeof(retained_profile_name), "%s", eq_current_profile_name);
    int retained_band = current_eq_band;
    harness_teardown_screens();
    assert(!lv_obj_is_valid(retained_eq));
    assert(eq_screen == NULL && eq_profiles_screen == NULL && settings_crossfeed_screen == NULL);
    assert(eq_profile_paths == NULL && eq_profile_count == 0);
    assert(eq_current_profile_name[0] == retained_profile_name[0]);
    assert(current_eq_band == retained_band);
    assert(!eq_graph_visible);
    int pushes_after_teardown = nav_push_count;
    gui_settings_open_eq();
    assert(nav_push_count == pushes_after_teardown);
    eq_row = lv_btn_create(summary_host);
    lv_obj_add_event_cb(eq_row, harness_eq_row_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(eq_row, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == pushes_after_teardown + 1 && last_pushed_screen == NULL);
    crossfeed_row = lv_btn_create(summary_host);
    lv_obj_add_event_cb(crossfeed_row, harness_crossfeed_row_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_send_event(crossfeed_row, LV_EVENT_CLICKED, NULL);
    assert(nav_push_count == pushes_after_teardown + 1);
    lv_obj_delete(eq_row);
    lv_obj_delete(crossfeed_row);

    harness_build_screens(music_profiles_menu_cb, eq_download_profiles_available);
    harness_build_popups();
    assert(eq_screen && eq_profiles_screen && settings_crossfeed_screen);
    assert(strcmp(eq_current_profile_name, retained_profile_name) == 0);
    assert(current_eq_band == retained_band && !eq_graph_visible);
    harness_teardown_popups();
    harness_teardown_screens();

    fputs(trace_buffer, stdout);
    printf("scalar:preamp=%.2f;gain=%.2f;q=%.2f;crossfeed=%.2f;angle=%d;queries=%d;rows=%d;navpush=%d;navpop=%d;finalize=%d;popupbuild=%d;popupdelete=%d;deadzone=%d;errors=%d;infos=%d\n",
           peq_get_preamp_db(), peq_get_band(0)->gain_db, peq_get_band(0)->q,
           peq_get_crossfeed(), peq_get_crossfeed_angle(), profile_query_count,
           download_rows, nav_push_count, nav_pop_count, finalize_count,
           popup_build_count, popup_delete_count, dead_zone_count, error_toasts, info_toasts);

    lv_obj_delete(summary_host);
    lv_style_reset(&list_row_style); lv_style_reset(&list_row_pressed_style);
    lv_style_reset(&pill_row_bg_style); lv_style_reset(&native_row_min_style);
    lv_style_reset(&icon_press_style); lv_style_reset(&style_theme_screen_bg);
    lv_style_reset(&style_theme_card_bg); lv_style_reset(&style_theme_text_primary);
    lv_style_reset(&style_theme_text_muted); lv_style_reset(&style_theme_tile);
    lv_style_reset(&style_theme_tile_pressed); lv_style_reset(&style_theme_scrollbar);
    lv_style_reset(&style_theme_switch_track); lv_style_reset(&style_theme_switch_knob);
    lv_style_reset(&test_accent_style); lv_style_reset(&test_accent_knob_style);
    lv_display_delete(display);
    return 0;
}
