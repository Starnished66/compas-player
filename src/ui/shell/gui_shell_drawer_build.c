#include "gui_shell_internal.h"

#if defined(BOARD_TEMPOTEC_V1)
#define QUICK_DRAWER_EXPANSION_TOP BOARD_SCALE_PY(265)
#endif
#if defined(BOARD_TEMPOTEC_V1)
#else
#define QUICK_DRAWER_EXPANSION_TOP BOARD_SCALE_PY(211)
#endif
#define QUICK_DRAWER_PANEL_Y BOARD_SCALE_PY(59)
#define QUICK_DRAWER_PANEL_H BOARD_SCALE_PY(726)
#define QUICK_DRAWER_CARD_X (QUICK_DRAWER_PANEL_X + QUICK_DRAWER_CARD_INSET)
#define QUICK_DRAWER_CARD_Y (QUICK_DRAWER_PANEL_Y + QUICK_DRAWER_PANEL_H - \
                             QUICK_DRAWER_CARD_INSET - QUICK_DRAWER_CARD_H)
/* EQ badge in the card's upper-right corner, inset equally from both edges. */
#define QUICK_DRAWER_EQ_SIZE BOARD_SCALE_PX(36)
#define QUICK_DRAWER_EQ_INSET BOARD_SCALE_PX(8)
#define QUICK_DRAWER_EQ_X (QUICK_DRAWER_CARD_X + QUICK_DRAWER_CARD_W - QUICK_DRAWER_EQ_INSET - QUICK_DRAWER_EQ_SIZE)
#define QUICK_DRAWER_EQ_Y (QUICK_DRAWER_CARD_Y + QUICK_DRAWER_EQ_INSET)
/* Row 2 taps use LV_EVENT_SHORT_CLICKED, which LVGL does not emit after a
 * long press, so opening a tile's page cannot also toggle it on release. */
static void quick_drawer_airplay_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    gui_network_toggle_airplay();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}
static void quick_drawer_bt_dac_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    gui_network_toggle_bt_dac();
}
static void quick_drawer_bt_dac_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_network_open_bt_dac();
}
static void quick_drawer_dlna_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    gui_network_toggle_dlna();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}
static void quick_drawer_gapless_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    /* Shared with the Settings switch so the crossfade implication, the
     * re-arm and both toggles' refreshes happen identically from either --
     * see gui_player_set_gapless_enabled(). */
    gui_player_set_gapless_enabled(!current_settings.gapless_enabled);
}
static void quick_drawer_rc_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    gui_network_toggle_remote_control();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}
static void quick_drawer_airplay_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_network_open_airplay();
}
static void quick_drawer_dlna_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_network_open_dlna();
}
static void quick_drawer_gapless_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_settings_open_playback();
}
static void quick_drawer_rc_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_network_open_remote_control();
}
static void quick_drawer_car_mode_event_cb(lv_event_t * e) {
    /* LV_EVENT_SHORT_CLICKED is not emitted after LVGL recognizes a long
     * press, so opening settings cannot also toggle Car Mode on release. */
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    gui_player_set_car_mode_enabled(!current_settings.car_mode_enabled);
}
static void quick_drawer_car_mode_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    /* Match Wi-Fi/Bluetooth: finish the drawer's bitmap transition before
     * navigating, since the settings screen will cover the overlay. */
    quick_drawer_dismiss_for_navigation();
    gui_settings_open_car_mode();
}
static void quick_drawer_sleep_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_settings_open_sleep_timer();
}
static void quick_drawer_crossfade_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    quick_drawer_dismiss_for_navigation();
    gui_settings_open_playback();
}

/* Registered for both the drawer's EQ long press and its EQ button click. */
static void quick_drawer_open_eq_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_CLICKED) return;
    quick_drawer_dismiss_for_navigation();
    gui_settings_open_eq();
}
static void quick_drawer_plugin_toggle_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= quick_drawer_plugin_toggle_count) return;
    plugin_manager_quick_toggle_set(index, !plugin_manager_get_quick_toggle_value(index));
    gui_shell_refresh_quick_drawer_expansion_toggles();
}
static void quick_drawer_plugin_toggle_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= quick_drawer_plugin_toggle_count ||
        !plugin_manager_quick_toggle_has_on_hold(index)) return;
    quick_drawer_dismiss_for_navigation();
    plugin_manager_quick_toggle_open_settings(index);
}
#define BRIGHTNESS_HW_APPLY_INTERVAL_MS 50

/* One drawer slider: 0-100 range, accent styling and a forgiving touch
 * margin. Position is the V1 or default coordinate pair; callers add their
 * own callback and value. */
static lv_obj_t * quick_drawer_create_slider(int32_t track_w, int32_t v1_y, int32_t y) {
    lv_obj_t * track = lv_slider_create(quick_drawer);
#if defined(BOARD_TEMPOTEC_V1)
    (void) track_w;
    (void) y;
    lv_obj_set_size(track, BOARD_SCALE_PX(270), BOARD_SCALE_PY(20));
    lv_obj_align(track, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(104), v1_y);
#else
    (void) v1_y;
    lv_obj_set_size(track, track_w, SLIDER_TRACK_HEIGHT);
    lv_obj_align(track, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(103), y);
#endif
    lv_slider_set_range(track, 0, 100);
    lv_obj_add_style(track, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(track, gui_theme_accent_knob_style(), LV_PART_KNOB);
    /* Configures rail styling to prevent visual artifacts on the track edge. */
    configure_native_slider_rail(track);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(track, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    /* A generous rail target that leaves the handle and neighboring rail
     * outside its touch bounds. Ownership checks use these same bounds. */
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_ext_click_area(track, BOARD_SCALE_PX(14));
#else
    lv_obj_set_ext_click_area(track, BOARD_SCALE_PX(26));
#endif
    return track;
}

/* Full-screen drawer root and the rounded panel behind every control. */
static void quick_drawer_build_surface(void) {
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    quick_drawer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(quick_drawer);
    lv_obj_set_size(quick_drawer, w, h);
    lv_obj_set_pos(quick_drawer, 0, -h); /* fully off-screen above until opened */
    shell_theme_styles_init();
    lv_obj_add_style(quick_drawer, &shell_drawer_bg_style, 0);
    lv_obj_remove_flag(quick_drawer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(quick_drawer, LV_OBJ_FLAG_CLICKABLE); /* swallow touches to whatever's behind while open */

    /* The reference uses one continuous rounded drawer surface.  Keep the
     * panel independent of the vendor backdrop asset so every native 480x800
     * coordinate remains explicit and stable across theme revisions. */
    lv_obj_t * drawer_panel = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(drawer_panel);
    lv_obj_set_pos(drawer_panel, QUICK_DRAWER_PANEL_X, QUICK_DRAWER_PANEL_Y);
    /* 726, not 683: the panel used to stop at y=742 on an 800px screen and
     * leave 58px of dead space below it. Ends at 785 now, keeping the same
     * ~15px bottom margin the sides already use. */
    lv_obj_set_size(drawer_panel, QUICK_DRAWER_PANEL_W, QUICK_DRAWER_PANEL_H);
    lv_obj_add_style(drawer_panel, &shell_drawer_panel_style, 0);
    lv_obj_set_style_radius(drawer_panel, BOARD_SCALE_PX(30), 0);
    lv_obj_remove_flag(drawer_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(drawer_panel);
}

/* Pull handle between the toggle rows and the sliders. */
static void quick_drawer_build_expansion_handle(void) {
    quick_drawer_expansion_handle = lv_obj_create(quick_drawer);
    lv_obj_t * expansion_handle = quick_drawer_expansion_handle;
    lv_obj_remove_style_all(expansion_handle);
#if defined(BOARD_TEMPOTEC_V1)
    /* Centered grab target between the toggle states and slider pills. */
    lv_obj_set_pos(expansion_handle, BOARD_SCALE_PX(210), BOARD_SCALE_PY(272));
#else
    lv_obj_set_pos(expansion_handle, BOARD_SCALE_PX(210), BOARD_SCALE_PY(219));
#endif
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(expansion_handle, BOARD_SCALE_PX(60), BOARD_SCALE_PY(5));
#else
    lv_obj_set_size(expansion_handle, BOARD_SCALE_PX(59), BOARD_SCALE_PY(8));
#endif
    lv_obj_add_style(expansion_handle, &shell_drawer_handle_style, 0);
    lv_obj_set_style_radius(expansion_handle, BOARD_SCALE_PX(4), 0);
    /* A bar this thin is far too small a touch target on its own; the drag
     * itself is claimed by coordinate hit-test in poll_quick_drawer_drag()
     * over the same padded rectangle this advertises. */
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_ext_click_area(expansion_handle, BOARD_SCALE_PX(4));
#else
    lv_obj_set_ext_click_area(expansion_handle, BOARD_SCALE_PX(8));
#endif
    lv_obj_add_flag(expansion_handle, LV_OBJ_FLAG_CLICKABLE);
    /* The Compas drawer's pull handle also reveals secondary controls when
     * the optional toggles have been rearranged or disabled. */
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_remove_flag(expansion_handle, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_add_event_cb(expansion_handle, quick_drawer_expansion_handle_cb, LV_EVENT_CLICKED, NULL);
}

/* Row backgrounds for the brightness and volume sliders. Returns the
 * brightness container, which the shift registration needs. */
static lv_obj_t * quick_drawer_build_slider_containers(void) {
    /* Brightness first, volume second -- swapped from the reference's own
     * order at the user's request. Only the container Y (and the matching
     * icon/track/label Y offsets below) moved; every X position, size, and
     * per-row delta stays exactly as measured before. */
#if defined(BOARD_TEMPOTEC_V1)
    quick_drawer_brightness_container = lv_obj_create(quick_drawer);
    lv_obj_t * brightness_container = quick_drawer_brightness_container;
#else
    lv_obj_t * brightness_container = lv_obj_create(quick_drawer);
#endif
    lv_obj_remove_style_all(brightness_container);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_pos(brightness_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(285));
    lv_obj_set_size(brightness_container, QUICK_DRAWER_CARD_W, BOARD_SCALE_PY(55));
#else
    lv_obj_set_pos(brightness_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(238));
    lv_obj_set_size(brightness_container, QUICK_DRAWER_CARD_W, BOARD_SCALE_PY(72));
#endif
    lv_obj_add_style(brightness_container, &shell_drawer_row_style, 0);
    lv_obj_set_style_bg_opa(brightness_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brightness_container, BOARD_SCALE_PX(23), 0);

    quick_drawer_volume_container = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(quick_drawer_volume_container);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_pos(quick_drawer_volume_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(370));
    lv_obj_set_size(quick_drawer_volume_container, QUICK_DRAWER_CARD_W, BOARD_SCALE_PY(55));
#else
    lv_obj_set_pos(quick_drawer_volume_container, BOARD_SCALE_PX(34), BOARD_SCALE_PY(325));
    lv_obj_set_size(quick_drawer_volume_container, QUICK_DRAWER_CARD_W, BOARD_SCALE_PY(73));
#endif
    lv_obj_add_style(quick_drawer_volume_container, &shell_drawer_row_style, 0);
    lv_obj_set_style_bg_opa(quick_drawer_volume_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(quick_drawer_volume_container, BOARD_SCALE_PX(23), 0);
    return brightness_container;
}

/* X of a toggle icon inside its column cell (the V1 centres the icon in the
 * wider cell; the other boards draw it flush with the column). */
static int32_t quick_drawer_toggle_icon_x(int32_t column_x) {
#if defined(BOARD_TEMPOTEC_V1)
    return column_x + (QUICK_DRAWER_TOGGLE_CELL_W - QUICK_DRAWER_TOGGLE_ICON_PX) / 2;
#else
    return column_x;
#endif
}

/* Row 1: every toggle icon (Bluetooth / Wifi / sleep timer / output gain)
 * together in one row -- 4 icons x 84px + 5 gaps of 21px exactly fills the
 * panel's measured 440px content width (19 to 459). No clock, no volume
 * slider here anymore (clock duplicated the always-visible main status bar;
 * a second volume control duplicated the hardware volume buttons' own popup)
 * -- brightness (row 2 below) is the only slider left in this drawer. */
static const struct {
    lv_obj_t ** icon;
    const char * asset; /* NULL: the icon's artwork is set by its own refresh */
    lv_event_code_t click_code;
    lv_event_cb_t click_cb;
    lv_event_cb_t long_press_cb;
    lv_event_cb_t pressed_cb; /* NULL: none */
} quick_drawer_row1[] = {
    { &quick_drawer_wifi_icon, "pull_down/wifi.png", LV_EVENT_CLICKED,
      quick_drawer_wifi_event_cb, quick_drawer_wifi_long_press_cb, quick_drawer_wifi_pressed_cb },
    { &quick_drawer_bt_icon, "pull_down/bt.png", LV_EVENT_CLICKED,
      quick_drawer_bt_event_cb, quick_drawer_bt_long_press_cb, quick_drawer_bt_pressed_cb },
    { &quick_drawer_sleep_icon, "pull_down/sleep_switch.png", LV_EVENT_SHORT_CLICKED,
      quick_drawer_sleep_event_cb, quick_drawer_sleep_long_press_cb, NULL },
    { &quick_drawer_crossfade_icon, NULL, LV_EVENT_SHORT_CLICKED,
      quick_drawer_crossfade_event_cb, quick_drawer_crossfade_long_press_cb, NULL },
};

/* Row 1 toggle icons: Wi-Fi / Bluetooth / sleep timer / crossfade. */
static void quick_drawer_build_toggle_icons(void) {
    for (int i = 0; i < 4; i++) {
        lv_obj_t * icon = lv_image_create(quick_drawer);
        *quick_drawer_row1[i].icon = icon;
        quick_drawer_toggle_icon[i] = icon;
        if (quick_drawer_row1[i].asset)
            quick_drawer_set_toggle_image(icon, asset_path(quick_drawer_row1[i].asset));
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, quick_drawer_toggle_icon_x(quick_drawer_toggle_column_x(i, 4)),
                     QUICK_DRAWER_ROW1_TOP);
        lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(icon, quick_drawer_row1[i].click_cb, quick_drawer_row1[i].click_code, NULL);
        lv_obj_add_event_cb(icon, quick_drawer_row1[i].long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
        if (quick_drawer_row1[i].pressed_cb)
            lv_obj_add_event_cb(icon, quick_drawer_row1[i].pressed_cb, LV_EVENT_PRESSED, NULL);
    }
    refresh_quick_drawer_crossfade_icon();
}

/* Caption under a row 1 icon. Fixed 12 px (fallback_font_ui_12(), which
 * follows the custom font), not gui_theme_font(GUI_FONT_ROLE_SUBTEXT) -- that
 * role scales with Settings > Font Size (app_font_16 underneath), which is
 * right for reading text but was already too big to fit "Bluetooth" in this
 * 84px-wide slot below its icon even at the default tier, let alone BlindMF.
 * Fixed and non-scaling, same "accessibility-independent" choice
 * build_status_bar() already makes for its own clock/volume/battery faces. */
static void quick_drawer_build_toggle_name(int i, int32_t x, const char * text) {
    lv_obj_t * name = lv_label_create(quick_drawer);
    quick_drawer_toggle_name[i] = name;
    lv_label_set_text(name, text);
    lv_obj_add_style(name, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(name,
#if defined(BOARD_TEMPOTEC_V1)
                               gui_theme_font(GUI_FONT_ROLE_SUBTEXT),
#else
                               fallback_font_ui_12(),
#endif
                               0);
    lv_obj_set_width(name, QUICK_DRAWER_TOGGLE_CELL_W);
    quick_drawer_size_toggle_caption(name);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, x,
                 QUICK_DRAWER_ROW1_TOP + QUICK_DRAWER_TOGGLE_ICON_PX + QUICK_DRAWER_TOGGLE_CAPTION_GAP);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_height(name, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
#endif
}

/* State label ("Off" until refreshed) under a row 1 caption. */
static void quick_drawer_build_toggle_state(int i, int32_t x) {
    lv_obj_t * state = lv_label_create(quick_drawer);
    quick_drawer_toggle_state[i] = state;
    lv_obj_set_width(state, QUICK_DRAWER_TOGGLE_CELL_W);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_height(state, lv_font_get_line_height(&lv_font_montserrat_12));
    lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
#endif
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(state, fallback_font_ui_12(), 0);
    lv_obj_align(state, LV_ALIGN_TOP_LEFT, x, quick_drawer_toggle_state_y(QUICK_DRAWER_ROW1_TOP));
    lv_label_set_text(state, TR("Off"));
    lv_obj_set_style_text_color(state, shell_drawer_off_color, 0);
}

/* Captions and state labels under the row 1 icons. */
static void quick_drawer_build_toggle_captions(void) {
    const char * names[] = { N_("Wi-Fi"), N_("Bluetooth"), N_("Sleep"), N_("Crossfade") };
    for (int i = 0; i < 4; i++) {
        int32_t x = quick_drawer_toggle_column_x(i, 4);
        quick_drawer_build_toggle_name(i, x, TR(names[i]));
        quick_drawer_build_toggle_state(i, x);
    }
    quick_drawer_set_toggle_state(0, gui_shell_wifi_effective_enabled());
    quick_drawer_set_toggle_state(1, bt_control_is_powered());
    quick_drawer_set_toggle_state(2, sleep_timer_active);
    quick_drawer_set_toggle_state(3, current_settings.crossfade_enabled);
}

/* Clipping box that holds expanded rows 2 and 3. */
static void quick_drawer_build_expansion_box(void) {
    quick_drawer_expansion_box = lv_obj_create(quick_drawer);
    lv_obj_remove_style_all(quick_drawer_expansion_box);
    lv_obj_set_pos(quick_drawer_expansion_box, 0, QUICK_DRAWER_EXPANSION_TOP);
    lv_obj_set_width(quick_drawer_expansion_box, BOARD_SCALE_PX(480));
    lv_obj_set_height(quick_drawer_expansion_box, 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_add_flag(quick_drawer_expansion_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(quick_drawer_expansion_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(quick_drawer_expansion_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(quick_drawer_expansion_box, quick_drawer_expansion_scroll_event_cb,
                        LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(quick_drawer_expansion_box, quick_drawer_expansion_scroll_event_cb,
                        LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(quick_drawer_expansion_box, quick_drawer_expansion_scroll_event_cb,
                        LV_EVENT_SCROLL_END, NULL);
#else
    lv_obj_remove_flag(quick_drawer_expansion_box, LV_OBJ_FLAG_SCROLLABLE);
#endif
    lv_obj_set_style_bg_opa(quick_drawer_expansion_box, 0, 0);

    quick_drawer_plugin_toggle_count = plugin_manager_get_quick_toggle_count();
    if (quick_drawer_plugin_toggle_count > PLUGIN_MAX_QUICK_TOGGLES)
        quick_drawer_plugin_toggle_count = PLUGIN_MAX_QUICK_TOGGLES;
}

/* Clickable toggle icon inside the expansion box at (x, row_y). */
static lv_obj_t * quick_drawer_create_expansion_icon(int32_t x, int32_t row_y) {
    lv_obj_t * icon = lv_image_create(quick_drawer_expansion_box);
    lv_obj_set_pos(icon, quick_drawer_toggle_icon_x(x), row_y);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    return icon;
}

/* Expanded rows 2 and 3 live inside the clipping box. Row 2 is AirPlay /
 * BT DAC / DLNA / Gapless / RC, in the QD_TOGGLE_AIRPLAY.. slot order. "RC", not
 * "HiBy Link": the stock asset is hibylink.png but what it actually drives
 * here is remote_control.h's own phone web UI, and the full name does not fit
 * an 84px slot at any font tier. */
static const struct {
    const char * name;
    lv_obj_t ** icon;
    lv_event_cb_t click_cb;
    lv_event_cb_t long_press_cb;
} quick_drawer_row2[] = {
    { N_("AirPlay"), &quick_drawer_airplay_icon, quick_drawer_airplay_event_cb,
      quick_drawer_airplay_long_press_cb },
    { N_("BT DAC"), &quick_drawer_bt_dac_icon, quick_drawer_bt_dac_event_cb,
      quick_drawer_bt_dac_long_press_cb },
    { N_("DLNA"), &quick_drawer_dlna_icon, quick_drawer_dlna_event_cb,
      quick_drawer_dlna_long_press_cb },
    { N_("Gapless"), &quick_drawer_gapless_icon, quick_drawer_gapless_event_cb,
      quick_drawer_gapless_long_press_cb },
    { N_("RC"), &quick_drawer_rc_icon, quick_drawer_rc_event_cb, quick_drawer_rc_long_press_cb },
};

static void quick_drawer_build_expansion_row2(void) {
    for (unsigned i = 0; i < sizeof(quick_drawer_row2) / sizeof(quick_drawer_row2[0]); i++) {
        int32_t x = quick_drawer_toggle_column_x(i % 4, 4); /* reflow places them */
        lv_obj_t * icon = quick_drawer_create_expansion_icon(x, 0);

        int slot = QD_TOGGLE_AIRPLAY + i;
        quick_drawer_toggle_icon[slot] = icon;
        quick_drawer_make_toggle_labels(slot, x, 0, TR(quick_drawer_row2[i].name));
        lv_label_set_text(quick_drawer_toggle_state[slot], TR("Off"));
        lv_obj_set_style_text_color(quick_drawer_toggle_state[slot], shell_drawer_off_color, 0);

        *quick_drawer_row2[i].icon = icon;
        lv_obj_add_event_cb(icon, quick_drawer_row2[i].click_cb, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(icon, quick_drawer_row2[i].long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
        quick_drawer_set_toggle_image(icon, quick_drawer_toggle_src((qd_toggle_t) slot, false));
    }
}

/* Row 3 cell i (0 = Car Mode, then plugin toggles): column X and row Y. */
static void quick_drawer_row3_cell(int i, int32_t * x, int32_t * row_y) {
#if defined(BOARD_TEMPOTEC_V1)
    *row_y = QUICK_DRAWER_TOGGLE_ROW_PITCH * (i / 4 + 1);
    *x = quick_drawer_toggle_column_x(i % 4, 4);
#else
    if (1 + quick_drawer_plugin_toggle_count <= 4) {
        *x = quick_drawer_toggle_column_x(i, 4);
    } else {
        /* Five tiles need the full panel width: five 84px icons at
         * 90px spacing, with 18px side margins. */
        *x = BOARD_SCALE_PX(18 + 90 * i);
    }
    *row_y = QUICK_DRAWER_TOGGLE_ROW_PITCH;
#endif
}

static void quick_drawer_build_car_mode_toggle(int32_t x, int32_t row_y) {
    bool on = current_settings.car_mode_enabled;
    lv_obj_t * icon = lv_image_create(quick_drawer_expansion_box);
    quick_drawer_set_toggle_image(icon, quick_drawer_toggle_src(QD_TOGGLE_CAR_MODE, on));
    lv_obj_set_pos(icon, quick_drawer_toggle_icon_x(x), row_y);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    quick_drawer_car_mode_icon = icon;
    quick_drawer_toggle_icon[QD_TOGGLE_CAR_MODE] = icon;
    lv_obj_add_event_cb(icon, quick_drawer_car_mode_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(icon, quick_drawer_car_mode_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    quick_drawer_make_toggle_labels(QD_TOGGLE_CAR_MODE, x, row_y, TR("Car Mode"));
    quick_drawer_set_toggle_state(QD_TOGGLE_CAR_MODE, on);
}

static void quick_drawer_build_plugin_toggle(int plugin_index, int32_t x, int32_t row_y) {
    bool on = plugin_manager_get_quick_toggle_value(plugin_index);
    lv_obj_t * icon = lv_image_create(quick_drawer_expansion_box);
    quick_drawer_set_toggle_image(icon, quick_drawer_plugin_toggle_src(plugin_index, on));
    lv_obj_set_pos(icon, quick_drawer_toggle_icon_x(x), row_y);
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(icon, quick_drawer_plugin_toggle_event_cb, LV_EVENT_SHORT_CLICKED,
                        (void *) (intptr_t) plugin_index);
    lv_obj_add_event_cb(icon, quick_drawer_plugin_toggle_long_press_cb, LV_EVENT_LONG_PRESSED,
                        (void *) (intptr_t) plugin_index);
    quick_drawer_plugin_icon[plugin_index] = icon;
    quick_drawer_toggle_icon[QD_TOGGLE_COUNT + plugin_index] = icon;
    int slot = QD_TOGGLE_COUNT + plugin_index;
    quick_drawer_make_toggle_labels(slot, x, row_y, plugin_manager_get_quick_toggle_label(plugin_index));
    quick_drawer_set_toggle_state_text(slot, on, plugin_manager_get_quick_toggle_state_text(plugin_index, on));
}

/* Expanded row 3: Car Mode, then any plugin quick toggles. */
static void quick_drawer_build_expansion_row3(void) {
    for (int i = 0; i < 1 + quick_drawer_plugin_toggle_count; i++) {
        int32_t x, row_y;
        quick_drawer_row3_cell(i, &x, &row_y);
        if (i == 0) quick_drawer_build_car_mode_toggle(x, row_y);
        else quick_drawer_build_plugin_toggle(i - 1, x, row_y);
    }
}

static void quick_drawer_set_expansion_full_height(void) {
#if defined(BOARD_TEMPOTEC_V1)
    int expanded_rows = (quick_drawer_plugin_toggle_count + 4) / 4;
    if (expanded_rows < 1) expanded_rows = 1;
    quick_drawer_expansion_full = quick_drawer_expansion_visible_height(
        QUICK_DRAWER_TOGGLE_ROW_PITCH * expanded_rows);
#else
    quick_drawer_expansion_full = QUICK_DRAWER_TOGGLE_ROW_PITCH * 2;
#endif
}

/* Vertical offset that centres the fixed-size percentage on its track. */
static int32_t quick_drawer_slider_pct_dy(void) {
    int32_t slider_pct_h = lv_font_get_line_height(&app_font_player_meta);
    return (SLIDER_TRACK_HEIGHT - slider_pct_h) / 2;
}

/* Slider row icon at the V1 or default coordinate pair. */
static lv_obj_t * quick_drawer_create_slider_icon(const void * src, int32_t v1_y, int32_t y) {
    lv_obj_t * icon = lv_image_create(quick_drawer);
    lv_image_set_src(icon, src);
#if defined(BOARD_TEMPOTEC_V1)
    (void) y;
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_size(icon, BOARD_SCALE_PX(36), BOARD_SCALE_PY(45));
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(54), v1_y);
#else
    (void) v1_y;
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, BOARD_SCALE_PX(55), y);
#endif
    return icon;
}

/* Slider row percentage label. Fixed size, so it does not follow the Font
 * Size tier, and centred on its own track (pct_dy) rather than pinned to a
 * hardcoded Y; only the non-V1 layout offsets by pct_dy. */
static lv_obj_t * quick_drawer_create_pct_label(int32_t v1_y, int32_t y, int32_t pct_dy) {
    lv_obj_t * label = lv_label_create(quick_drawer);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
#if defined(BOARD_TEMPOTEC_V1)
    (void) y;
    (void) pct_dy;
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_size(label, BOARD_SCALE_PX(54), BOARD_SCALE_PY(48));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(label, BOARD_SCALE_PX(392), v1_y);
#else
    (void) v1_y;
    lv_obj_set_style_text_font(label, &app_font_player_meta, 0);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -BOARD_SCALE_PX(52), y + pct_dy);
#endif
    return label;
}

/* Slider width, sized from the widest percentage label ("100") so the track
 * never overlaps it. The percentage is anchored at x=428; leave a 16px visual
 * gap before its actual text box and keep the knob's overhang inside that
 * gap. */
static int32_t quick_drawer_slider_track_width(void) {
#if defined(BOARD_TEMPOTEC_V1)
    int32_t track_w = BOARD_SCALE_PX(270);
#else
    lv_point_t label_size;
    lv_text_get_size(&label_size, "100", &app_font_player_meta, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t track_w = BOARD_SCALE_PX(428) - label_size.x - BOARD_SCALE_PX(16) - BOARD_SCALE_PX(103);
#endif
    if (track_w > BOARD_SCALE_PX(300)) track_w = BOARD_SCALE_PX(300);
    if (track_w < BOARD_SCALE_PX(120)) track_w = BOARD_SCALE_PX(120); /* sane floor so the track never collapses to nothing */
    return track_w;
}

/* Brightness icon, percentage label and slider. Returns the track width,
 * which the volume slider shares. Screen brightness is a real control, via
 * the standard Linux backlight sysfs class (backlight.h); there is no
 * dedicated slider-track asset in this theme, so it reuses the volume
 * slider's own. Brightness sits ahead of volume at the user's request. */
static int32_t quick_drawer_build_brightness_row(int32_t slider_pct_dy) {
    const void * decoded = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                         ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    quick_drawer_brightness_icon = quick_drawer_create_slider_icon(
        decoded ? decoded : asset_path("pull_down/blk.png"), BOARD_SCALE_PY(290), BOARD_SCALE_PY(259));
    quick_drawer_brightness_label = quick_drawer_create_pct_label(
        BOARD_SCALE_PY(290), BOARD_SCALE_PY(267), slider_pct_dy);

    int32_t brightness_track_w = quick_drawer_slider_track_width();

    /* Full 0-100 -- backlight.c maps this logical range to its own safe
     * raw range internally (see backlight.h's own comment), so the slider
     * itself shows a clean, honest 0%-100%. Initial value is set below by
     * refresh_quick_drawer_brightness() (also called on every
     * open_quick_drawer(), see its own comment). */
    quick_drawer_brightness_track = quick_drawer_create_slider(
        brightness_track_w, BOARD_SCALE_PY(302), BOARD_SCALE_PY(267));
    lv_obj_add_event_cb(quick_drawer_brightness_track, quick_drawer_brightness_changed_cb,
                        LV_EVENT_ALL, NULL);
    if (!brightness_hw_apply_timer) {
        brightness_hw_apply_timer = lv_timer_create(brightness_hw_apply_timer_cb,
                                                    BRIGHTNESS_HW_APPLY_INTERVAL_MS, NULL);
        if (brightness_hw_apply_timer) lv_timer_pause(brightness_hw_apply_timer);
    }

    refresh_quick_drawer_brightness();
    return brightness_track_w;
}

/* Volume icon, percentage label and slider, in brightness's former slot. */
static void quick_drawer_build_volume_row(int32_t brightness_track_w, int32_t slider_pct_dy) {
    quick_drawer_volume_icon = quick_drawer_create_slider_icon(
        asset_path("volume/vol.png"), BOARD_SCALE_PY(375), BOARD_SCALE_PY(346));
    lv_obj_add_flag(quick_drawer_volume_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_volume_icon, quick_drawer_open_eq_cb, LV_EVENT_LONG_PRESSED, NULL);
    quick_drawer_volume_track = quick_drawer_create_slider(
        brightness_track_w, BOARD_SCALE_PY(388), BOARD_SCALE_PY(354));
    lv_obj_add_event_cb(quick_drawer_volume_track, quick_drawer_volume_event_cb, LV_EVENT_ALL, NULL);
    lv_slider_set_value(quick_drawer_volume_track, gui_player_get_volume_percent(), LV_ANIM_OFF);
    quick_drawer_volume_label = quick_drawer_create_pct_label(
        BOARD_SCALE_PY(375), BOARD_SCALE_PY(354), slider_pct_dy);
    lv_label_set_text_fmt(quick_drawer_volume_label, "%d", gui_player_get_volume_percent());
    gui_shell_refresh_quick_drawer_volume_visibility();
}

/* Everything below the expansion box slides down with it. */
static void quick_drawer_register_shift_objs(lv_obj_t * brightness_container, int32_t slider_pct_dy) {
    (void) slider_pct_dy; /* only the non-V1 layout offsets by it */
    /* Everything below the expansion box slides down by whatever height it
     * currently has. Registered with the collapsed y each object was just
     * built at, so quick_drawer_apply_expansion() never has to re-derive
     * any of this geometry. Both slider hit tests read live
     * lv_obj_get_coords(), so they follow these automatically. */
    quick_drawer_shift_count = 0;
#if defined(BOARD_TEMPOTEC_V1)
    quick_drawer_register_shift_obj(quick_drawer_expansion_handle, BOARD_SCALE_PY(272));
    quick_drawer_register_shift_obj(brightness_container, BOARD_SCALE_PY(285));
    quick_drawer_register_shift_obj(quick_drawer_brightness_icon, BOARD_SCALE_PY(290));
    quick_drawer_register_shift_obj(quick_drawer_brightness_label, BOARD_SCALE_PY(290));
    quick_drawer_register_shift_obj(quick_drawer_brightness_track, BOARD_SCALE_PY(302));
    quick_drawer_register_shift_obj(quick_drawer_volume_container, BOARD_SCALE_PY(370));
    quick_drawer_register_shift_obj(quick_drawer_volume_icon, BOARD_SCALE_PY(375));
    quick_drawer_register_shift_obj(quick_drawer_volume_label, BOARD_SCALE_PY(375));
    quick_drawer_register_shift_obj(quick_drawer_volume_track, BOARD_SCALE_PY(388));
#else
    quick_drawer_register_shift_obj(quick_drawer_expansion_handle, BOARD_SCALE_PY(219));
    quick_drawer_register_shift_obj(brightness_container, BOARD_SCALE_PY(238));
    quick_drawer_register_shift_obj(quick_drawer_brightness_icon, BOARD_SCALE_PY(259));
    quick_drawer_register_shift_obj(quick_drawer_brightness_label, BOARD_SCALE_PY(267) + slider_pct_dy);
    quick_drawer_register_shift_obj(quick_drawer_brightness_track, BOARD_SCALE_PY(267));
    quick_drawer_register_shift_obj(quick_drawer_volume_container, BOARD_SCALE_PY(325));
    quick_drawer_register_shift_obj(quick_drawer_volume_icon, BOARD_SCALE_PY(346));
    quick_drawer_register_shift_obj(quick_drawer_volume_label, BOARD_SCALE_PY(354) + slider_pct_dy);
    quick_drawer_register_shift_obj(quick_drawer_volume_track, BOARD_SCALE_PY(354));
#endif
}

/* Now-playing card shell and its cover thumbnail. Returns the card. */
static lv_obj_t * quick_drawer_build_card_frame(void) {
    /* Mini now-playing card: cover thumbnail (left), track title/artist
     * (right of it), and transport controls (bottom row). Sized to fit the
     * second panel's bounds with a balanced bottom margin. style_theme_card_bg
     * is the same theme-aware panel-surface-plus-border style used for cards
     * elsewhere (popups, the idle action rows, the sleep timer button) -- gives this card a
     * lighter-than-backdrop fill and a subtle, theme-mixed border outline
     * instead of sitting fully transparent over the drawer's own black/gray
     * backdrop artwork. */
    quick_drawer_card = lv_obj_create(quick_drawer);
    lv_obj_t * card = quick_drawer_card;
    lv_obj_remove_style_all(card);
    /* Anchor to the gray panel rather than independently rounding a y
     * coordinate: bottom, left and right must expose the same panel inset. */
    lv_obj_set_pos(card, QUICK_DRAWER_CARD_X, QUICK_DRAWER_CARD_Y);
    lv_obj_set_size(card, QUICK_DRAWER_CARD_W, QUICK_DRAWER_CARD_H);
    lv_obj_add_style(card, &shell_drawer_card_style, 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Cover thumbnail -- reuses whatever the Player screen already decoded
     * (gui_player_get_current_cover_dsc(), same accessor gui_lock_screen.c
     * already relies on for its own small cover display) rather than a
     * fresh disk read/decode. COVER_ART_WIDTH is the known native size of
     * both that buffer and its default-cover fallback PNG, so one fixed
     * scale factor is correct for either source; gui_shell_refresh_quick_
     * drawer_cover() (called on every track/cover update) fills the src in. */
    quick_drawer_cover_frame = lv_obj_create(card);
    lv_obj_remove_style_all(quick_drawer_cover_frame);
    lv_obj_set_size(quick_drawer_cover_frame, QUICK_DRAWER_CARD_W, QUICK_DRAWER_CARD_H);
    lv_obj_align(quick_drawer_cover_frame, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(quick_drawer_cover_frame, LV_OBJ_FLAG_SCROLLABLE);
    quick_drawer_cover_img = lv_image_create(quick_drawer_cover_frame);
    lv_obj_remove_flag(quick_drawer_cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(quick_drawer_cover_img, asset_path("playing_plane/default_cover_565.png"));
    quick_drawer_fit_cover();
    return card;
}

/* Geometry shared by the now-playing card's text labels. */
typedef struct {
    const lv_font_t * meta_font; /* artist and album face */
    int32_t width, artist_w;
    int32_t title_h, artist_h;
    int32_t y_title, y_artist, y_album, y_format;
} quick_drawer_card_text_layout_t;

static void quick_drawer_card_text_layout(quick_drawer_card_text_layout_t * l) {
#if defined(BOARD_TEMPOTEC_V1)
    /* The V1 card follows the R1 layout's stacked metadata and transport. */
    l->meta_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    l->title_h = lv_font_get_line_height(&app_font_player_title);
    l->artist_h = lv_font_get_line_height(l->meta_font);
    l->width = BOARD_SCALE_PX(284);
    l->artist_w = BOARD_SCALE_PX(380);
    l->y_title = BOARD_SCALE_PY(15);
    l->y_artist = BOARD_SCALE_PY(95);
    l->y_album = BOARD_SCALE_PY(157);
    l->y_format = BOARD_SCALE_PY(162);
#else
    int32_t text_gap = BOARD_SCALE_PX(4);
    l->meta_font = &app_font_player_meta;
    l->title_h = row_label_bounded_height(&app_font_player_title);
    l->artist_h = row_label_bounded_height(&app_font_player_meta);
    l->width = QUICK_DRAWER_CARD_W - 2 * BOARD_SCALE_PX(15);
    l->artist_w = l->width;
    l->y_title = BOARD_SCALE_PY(40);
    l->y_artist = l->y_title + l->title_h + text_gap;
    l->y_album = l->y_artist + l->artist_h + text_gap;
    l->y_format = l->y_album + l->artist_h + BOARD_SCALE_PY(12);
#endif
}

/* Transparent full-card band that parents the text labels. */
static lv_obj_t * quick_drawer_build_card_band(lv_obj_t * card) {
    lv_obj_t * band = lv_obj_create(card);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, lv_pct(100), lv_pct(100));
    lv_obj_align(band, LV_ALIGN_TOP_MID, 0, 0);
    /* No tint: the frosted source is already darkened, and blending a scrim
     * over it re-quantizes the dithered RGB565 into visible bands. */
    lv_obj_set_style_bg_opa(band, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return band;
}

/* One centred, marquee-scrolling line of card text. v1_h is the V1's fixed
 * label height; the other boards use the font's bounded row height. */
static lv_obj_t * quick_drawer_card_line(lv_obj_t * band, const char * text, bool secondary,
                                         const lv_font_t * font, int32_t width, int32_t v1_h,
                                         int32_t y) {
    lv_obj_t * label = lv_label_create(band);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, gui_player_backdrop_text_style(secondary), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_height(label, v1_h);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
#else
    (void) v1_h;
    row_label_apply_bounded_height(label, font);
#endif
    row_label_enable_marquee(label);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

/* Rounded technical-format badge below the album line. */
static void quick_drawer_build_card_format(lv_obj_t * band, const quick_drawer_card_text_layout_t * l) {
    lv_obj_t * label = lv_label_create(band);
    quick_drawer_format_label = label;
    lv_label_set_text(label, "");
    lv_obj_add_style(label, gui_player_backdrop_text_style(true), 0);
#if defined(BOARD_TEMPOTEC_V1)
    /* The technical token is ASCII, so use the compact fixed font while the
     * user-provided title/artist/album retain fallback-aware row heights. */
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(label, l->width);
    lv_obj_set_height(label, lv_font_get_line_height(&lv_font_montserrat_12) + BOARD_SCALE_PY(4));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
#else
    lv_obj_set_style_text_font(label, &app_font_player_meta, 0);
#endif
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(label, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_20, 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_width(label, BOARD_SCALE_PX(300));
    lv_obj_set_height(label, lv_font_get_line_height(&lv_font_montserrat_12));
#else
    lv_obj_set_style_pad_hor(label, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_ver(label, BOARD_SCALE_PY(4), 0);
#endif
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, l->y_format);
}

/* Title / artist / album / format labels inside the card's band. Returns the band. */
static lv_obj_t * quick_drawer_build_card_text(lv_obj_t * card) {
    quick_drawer_card_text_layout_t l;
    quick_drawer_card_text_layout(&l);
    lv_obj_t * band = quick_drawer_build_card_band(card);

    quick_drawer_title_label = quick_drawer_card_line(band, TR("No track loaded"), false,
                                                      &app_font_player_title, l.width, l.title_h, l.y_title);
    quick_drawer_artist_label = quick_drawer_card_line(band, "", true, l.meta_font, l.artist_w,
                                                       l.artist_h, l.y_artist);
    quick_drawer_album_label = quick_drawer_card_line(band, "", true, l.meta_font, l.width,
                                                      l.artist_h, l.y_album);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_add_flag(quick_drawer_album_label, LV_OBJ_FLAG_HIDDEN);
#endif
    quick_drawer_build_card_format(band, &l);
    return band;
}

/* Transport button (prev/next): backdrop-styled icon at its reference X. */
static void quick_drawer_create_transport_button(lv_obj_t * parent, const char * asset,
                                                 int32_t reference_x, lv_event_cb_t cb) {
    lv_obj_t * btn = lv_image_create(parent);
    lv_image_set_src(btn, asset_path(asset));
    lv_obj_add_style(btn, gui_player_backdrop_icon_style(), 0);
    quick_drawer_layout_transport_icon(btn, reference_x);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_ext_click_area(btn, BOARD_SCALE_PX(8));
#else
    lv_obj_set_ext_click_area(btn, BOARD_SCALE_PX(4));
#endif
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
}

/* Centre play/pause button of the transport row. */
static void quick_drawer_build_play_button(lv_obj_t * controls_row) {
    quick_drawer_play_btn = lv_image_create(controls_row);
    lv_image_set_src(quick_drawer_play_btn, gui_player_play_btn_image_src(audio_is_playing()));
#if BOARD_COMPACT_UI
    /* Size the layout box as well as the image so its center and touch
     * target follow the smaller panel, including after play/pause changes. */
#if defined(BOARD_TEMPOTEC_V1)
    int32_t play_size = BOARD_SCALE_PY(90);
    int32_t row_h = BOARD_SCALE_PY(90);
#else
    int32_t play_size = BOARD_SCALE_PY(84);
    int32_t row_h = BOARD_SCALE_PY(86);
#endif
    lv_obj_set_size(quick_drawer_play_btn, play_size, play_size);
    lv_image_set_inner_align(quick_drawer_play_btn, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_pos(quick_drawer_play_btn, BOARD_SCALE_PX(206) - play_size / 2, (row_h - play_size) / 2);
#else
    lv_obj_set_pos(quick_drawer_play_btn, BOARD_SCALE_PX(164), (BOARD_SCALE_PY(86) - 84) / 2);
    lv_image_set_scale(quick_drawer_play_btn, (BOARD_SCALE_PY(86) * LV_SCALE_NONE + 42) / 84);
#endif
    lv_obj_add_flag(quick_drawer_play_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(quick_drawer_play_btn, play_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

/* Transport row: prev/play/next, all three in one row. */
static void quick_drawer_build_transport_row(lv_obj_t * band) {
    int32_t controls_bottom = BOARD_SCALE_PY(24);
#if defined(BOARD_TEMPOTEC_V1)
    controls_bottom = BOARD_SCALE_PY(12);
#endif
    lv_obj_t * controls_row = lv_obj_create(band);
    /* 84, not 70 -- btn_play.png/btn_pause.png are 84x84 (confirmed via the
     * actual asset files), and a shorter row was clipping the top/bottom of
     * that icon, confirmed on a real device. */
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(controls_row, lv_pct(100), BOARD_SCALE_PY(90));
#else
    lv_obj_set_size(controls_row, lv_pct(100), BOARD_SCALE_PY(86));
#endif
    lv_obj_align(controls_row, LV_ALIGN_BOTTOM_MID, 0, -controls_bottom);
    lv_obj_set_style_bg_opa(controls_row, 0, 0);
    lv_obj_set_style_border_width(controls_row, 0, 0);
    lv_obj_set_style_pad_all(controls_row, 0, 0);
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);
    quick_drawer_create_transport_button(controls_row, "playing_plane/btn_prev.png", 84, prev_btn_event_cb);
    quick_drawer_build_play_button(controls_row);
    quick_drawer_create_transport_button(controls_row, "playing_plane/btn_next.png", 290, next_btn_event_cb);
}

/* Label inside the EQ shortcut button. */
static void quick_drawer_build_eq_label(lv_obj_t * button) {
    lv_obj_t * eq_label = lv_label_create(button);
    lv_obj_add_style(eq_label, &style_theme_text_primary, 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_style_text_font(eq_label, fallback_font_fixed(12), 0);
#else
    lv_obj_set_style_text_font(eq_label, fallback_font_fixed(16), 0);
#endif
    lv_label_set_text(eq_label, TR("EQ"));
    lv_obj_center(eq_label);
}

/* EQ shortcut. A card child on V1, a drawer child (shifted with the
 * expansion) elsewhere. Keep it visible even when volume controls are hidden
 * or the drawer's expansion box hides the mini-player card. It sits the same
 * distance from the card's top and right edges; at 480x800 the 36x36px badge
 * occupies x=402..437, y=424..459 and its 44x44px target clears the volume
 * rail's extended click area (ending at y=413) and the title band (starting
 * at y=463). As expansion opens it shifts down by the same amount as the rest
 * of the drawer, below row-3 content. */
static void quick_drawer_build_eq_button(lv_obj_t * card) {
    lv_obj_t * parent = quick_drawer;
#if defined(BOARD_TEMPOTEC_V1)
    parent = card;
#else
    (void) card;
#endif
    quick_drawer_eq_button = lv_button_create(parent);
    lv_obj_remove_style_all(quick_drawer_eq_button);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_pos(quick_drawer_eq_button, BOARD_SCALE_PX(360), BOARD_SCALE_PY(5));
    lv_obj_set_size(quick_drawer_eq_button, BOARD_SCALE_PX(36), BOARD_SCALE_PX(36));
#else
    lv_obj_set_pos(quick_drawer_eq_button, QUICK_DRAWER_EQ_X, QUICK_DRAWER_EQ_Y);
    lv_obj_set_size(quick_drawer_eq_button, QUICK_DRAWER_EQ_SIZE, QUICK_DRAWER_EQ_SIZE);
#endif
    lv_obj_add_style(quick_drawer_eq_button, &style_theme_card_bg, 0);
    lv_obj_set_style_radius(quick_drawer_eq_button, BOARD_SCALE_PX(5), 0);
    lv_obj_add_style(quick_drawer_eq_button, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_bg_opa(quick_drawer_eq_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(quick_drawer_eq_button, BOARD_SCALE_PX(1), 0);
    lv_obj_set_ext_click_area(quick_drawer_eq_button, BOARD_SCALE_PX(4));
    lv_obj_add_event_cb(quick_drawer_eq_button, quick_drawer_open_eq_cb, LV_EVENT_CLICKED, NULL);
    quick_drawer_build_eq_label(quick_drawer_eq_button);
#if !defined(BOARD_TEMPOTEC_V1)
    /* On V1 it is a card child, so it hides and moves with all card metadata. */
    quick_drawer_register_shift_obj(quick_drawer_eq_button, QUICK_DRAWER_EQ_Y);
#endif
}


void build_quick_drawer(void) {
    quick_drawer_build_surface();
    quick_drawer_build_expansion_handle();
    lv_obj_t * brightness_container = quick_drawer_build_slider_containers();

    /* Decode the four "on" icons with the current accent before anything
     * asks for one below -- quick_drawer_toggle_src() falls back to the
     * plain (stock-blue) asset while these are unloaded. */
    load_quick_drawer_toggle_images();
    quick_drawer_build_toggle_icons();
    quick_drawer_build_toggle_captions();
    quick_drawer_build_expansion_box();
    quick_drawer_build_expansion_row2();
    quick_drawer_build_expansion_row3();
    quick_drawer_set_expansion_full_height();

    int32_t slider_pct_dy = quick_drawer_slider_pct_dy();
    int32_t brightness_track_w = quick_drawer_build_brightness_row(slider_pct_dy);
    quick_drawer_build_volume_row(brightness_track_w, slider_pct_dy);
    quick_drawer_register_shift_objs(brightness_container, slider_pct_dy);

    lv_obj_t * card = quick_drawer_build_card_frame();
    lv_obj_t * band = quick_drawer_build_card_text(card);
    quick_drawer_build_transport_row(band);
    quick_drawer_build_eq_button(card);

    gui_shell_refresh_quick_drawer_cover();

    gui_shell_apply_customization();

    /* Font loading dirties this cache and queues its final capture at boot.
     * UI reloads already have final fonts, so keep their eager capture. */
    if (fallback_font_is_loaded()) quick_drawer_rebuild_snapshot();
}
