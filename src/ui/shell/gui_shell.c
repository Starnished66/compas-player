#include "gui_shell_internal.h"

lv_obj_t * home_screen = NULL;
static lv_obj_t * dac_home_screen = NULL;
static lv_obj_t * home_indicator_band = NULL;
/* Theme-dependent caption colors; see gui_shell_refresh_theme_colors(). */
lv_color_t shell_drawer_off_color;


/* The drawer and status bar paint their own surfaces from the theme slots
 * (Breeze Dark natively), with text that fails to contrast replaced by
 * Breeze's. Shared styles, so a theme change reaches the live widgets
 * without a rebuild. The native palette's white status glyph art is used
 * as drawn; any other palette recolors it. */
lv_style_t shell_drawer_bg_style, shell_drawer_panel_style, shell_drawer_row_style,
    shell_drawer_card_style, shell_drawer_handle_style, shell_topbar_glyph_style;
static lv_style_t * const bg_styles[] = {
    &shell_drawer_bg_style, &shell_drawer_panel_style, &shell_drawer_row_style,
    &shell_drawer_card_style, &shell_drawer_handle_style };
static bool shell_theme_styles_ready;
bool shell_theme_native = true;
lv_color_t shell_bar_color, shell_bar_glyph_color, shell_drawer_surface;
static lv_color_t shell_theme_slot(lv_style_t * style, lv_style_prop_t prop, uint32_t fallback) {
    lv_style_value_t value;
    return lv_style_get_prop(style, prop, &value) == LV_STYLE_RES_FOUND ? value.color : lv_color_hex(fallback);
}

/* Theme text over a theme surface, or the Breeze color that reads on it. */
static lv_color_t shell_legible(lv_color_t text, lv_color_t bg, float minimum, bool muted) {
    if (gui_theme_contrast_ratio(text, bg) >= minimum) return text;
    bool light = gui_theme_prefers_light_text(bg);
    return lv_color_hex(muted ? (light ? GUI_BREEZE_MUTED_ON_DARK : GUI_BREEZE_MUTED_ON_LIGHT)
                              : (light ? GUI_BREEZE_TEXT_ON_DARK : GUI_BREEZE_TEXT_ON_LIGHT));
}
void shell_theme_styles_init(void) {
    if (shell_theme_styles_ready) return;
    for (unsigned i = 0; i < sizeof(bg_styles) / sizeof(bg_styles[0]); i++) lv_style_init(bg_styles[i]);
    lv_style_init(&shell_topbar_glyph_style);
    shell_theme_styles_ready = true;
    gui_shell_refresh_theme_colors();
}

/* "On" captions use the accent where it reads on the drawer, else the text color. */
lv_color_t shell_drawer_on_color(void) {
    lv_color_t accent = accent_lv_color();
    if (gui_theme_contrast_ratio(accent, shell_drawer_surface) >= GUI_CONTRAST_MUTED) return accent;
    return shell_legible(shell_theme_slot(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, GUI_COLOR_PRIMARY),
                         shell_drawer_surface, GUI_CONTRAST_TEXT, false);
}
void shell_set_topbar_glyphs(lv_color_t color, bool recolor) {
    lv_style_value_t current;
    if (lv_style_get_prop(&shell_topbar_glyph_style, LV_STYLE_TEXT_COLOR, &current) == LV_STYLE_RES_FOUND &&
        lv_color_eq(current.color, color) &&
        lv_style_get_prop(&shell_topbar_glyph_style, LV_STYLE_IMAGE_RECOLOR_OPA, &current) == LV_STYLE_RES_FOUND &&
        (current.num == LV_OPA_COVER) == recolor) return;
    lv_style_set_text_color(&shell_topbar_glyph_style, color);
    lv_style_set_image_recolor(&shell_topbar_glyph_style, color);
    lv_style_set_image_recolor_opa(&shell_topbar_glyph_style, recolor ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_obj_report_style_change(&shell_topbar_glyph_style);
}

/* Every label and image on the bar takes the glyph color; warnings (red
 * volume digits, low battery) override it locally. */
void topbar_add_glyph_style(lv_obj_t * parent) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t * child = lv_obj_get_child(parent, (int32_t) i);
        if (lv_obj_check_type(child, &lv_label_class) || lv_obj_check_type(child, &lv_image_class))
            lv_obj_add_style(child, &shell_topbar_glyph_style, 0);
        topbar_add_glyph_style(child);
    }
}
lv_style_t * gui_shell_drawer_row_style(void) {
    shell_theme_styles_init();
    return &shell_drawer_row_style;
}
void gui_shell_refresh_theme_colors(void) {
    if (!shell_theme_styles_ready) {
        shell_theme_styles_init();
        return;
    }
    lv_color_t screen = shell_theme_slot(&style_theme_screen_bg, LV_STYLE_BG_COLOR, GUI_COLOR_SCREEN);
    lv_color_t card = shell_theme_slot(&style_theme_card_bg, LV_STYLE_BG_COLOR, GUI_COLOR_PANEL);
    lv_color_t row = shell_theme_slot(&list_row_style, LV_STYLE_BG_COLOR, GUI_COLOR_ROW);
    lv_color_t primary = shell_theme_slot(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, GUI_COLOR_PRIMARY);
    lv_color_t muted = shell_theme_slot(&style_theme_text_muted, LV_STYLE_TEXT_COLOR, GUI_COLOR_SECONDARY);
    shell_theme_native = lv_color_eq(screen, lv_color_hex(GUI_COLOR_SCREEN)) &&
                         lv_color_eq(card, lv_color_hex(GUI_COLOR_PANEL)) &&
                         lv_color_eq(row, lv_color_hex(GUI_COLOR_ROW)) &&
                         lv_color_eq(primary, lv_color_hex(GUI_COLOR_PRIMARY)) &&
                         lv_color_eq(muted, lv_color_hex(GUI_COLOR_SECONDARY));
    lv_color_t old_off = shell_drawer_off_color;
    lv_color_t off = shell_legible(muted, card, GUI_CONTRAST_MUTED, true);
    lv_style_set_bg_color(&shell_drawer_bg_style, screen);
    lv_style_set_bg_color(&shell_drawer_panel_style, card);
    lv_style_set_bg_color(&shell_drawer_row_style, row);
    lv_style_set_bg_color(&shell_drawer_card_style, row);
    lv_style_set_bg_color(&shell_drawer_handle_style, lv_color_mix(off, card, LV_OPA_60));
    shell_drawer_surface = card;
    shell_drawer_off_color = off;
    shell_bar_color = screen;
    shell_bar_glyph_color = shell_legible(primary, screen, GUI_CONTRAST_TEXT, false);
    for (unsigned i = 0; i < sizeof(bg_styles) / sizeof(bg_styles[0]); i++) {
        lv_style_set_bg_opa(bg_styles[i], LV_OPA_COVER);
        lv_obj_report_style_change(bg_styles[i]);
    }
    quick_drawer_retint_toggle_states(old_off);
    if (status_bar_band) gui_shell_set_status_bar_screen_context(lv_screen_active());
    quick_drawer_mark_snapshot_dirty();
}

/* Volume popup moved to gui_player.c */


/* Android-style home indicator: a small pill fixed to the bottom edge,
 * living on lv_layer_top() (drawn above every screen) so a swipe-up
 * starting there is caught by this object.
 * Position tracking is handled via raw coordinate polling in
 * poll_quick_drawer_drag() / gesture_home_state_is_eligible(). */

static void build_home_indicator_bar(void) {
    lv_obj_t * top = lv_layer_top();

    home_indicator_band = lv_obj_create(top);
    lv_obj_remove_style_all(home_indicator_band);
    /* Match the clickable object to the complete raw-coordinate press-down
     * surface accepted by gesture_home_state_poll(): the normal 24px band
     * plus HOME_SWIPE_HIT_EXTRA_PX.  The band remains visually transparent;
     * only the centered pill below is drawn. */
    lv_obj_set_size(home_indicator_band, lv_pct(100),
                    HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX);
    lv_obj_align(home_indicator_band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(home_indicator_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(home_indicator_band, LV_OBJ_FLAG_CLICKABLE); /* claims touches in this strip before any list underneath can -- the actual swipe-up trigger is poll_quick_drawer_drag()'s raw position polling, not a click/gesture event on this object */

    /* The visible pill itself -- plain light-gray rounded bar, matching
     * Android's own gesture-nav home indicator. */
    lv_obj_t * pill = lv_obj_create(home_indicator_band);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, BOARD_SCALE_PX(120), HOME_INDICATOR_PILL_HEIGHT);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pill, lv_color_make(220, 220, 220), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(pill, 2, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE); /* purely visual -- home_indicator_band above is what's actually clickable */

    lv_obj_add_flag(home_indicator_band, current_settings.swipe_up_home_enabled ? 0 : LV_OBJ_FLAG_HIDDEN);
}
void gui_shell_build_screens(uint32_t screen_width, uint32_t screen_height) {
    (void) screen_width;
    (void) screen_height;
    dac_home_screen = build_dac_home_screen();
    home_screen = build_home_screen();
    build_status_bar();
    build_home_indicator_bar();
    build_quick_drawer();
    if (gui_player_has_active_track()) {
        gui_shell_update_quick_drawer_track(gui_player_get_now_playing_title(),
                                            gui_player_get_now_playing_folder(),
                                            gui_player_get_now_playing_album());
    }
    refresh_format_badge();
    refresh_clock_label();
    refresh_battery_topbar();
    refresh_wifi_icon();
    refresh_play_pause_topbar();
    refresh_headphone_icon();
    poll_usb_audio_output();

    topbar_load_volume_warn_threshold();
    refresh_volume_topbar((int32_t) (audio_get_volume() * 100.0f));
    gui_shell_apply_topbar_settings();

    gui_shell_gestures_start();
}

/* Deletes every screen/top-layer object gui_shell.c itself owns -- for
 * gui_reload.c's in-process UI reload, so gui_shell_build_screens() can
 * rebuild these from a clean slate without leaking the old objects. Only
 * three root containers need an explicit lv_obj_delete(): status_bar_band/
 * home_indicator_band/quick_drawer own every other status-bar/quick-drawer
 * child (clock digits, battery icon, wifi/bt icons, sliders, ...) as an
 * LVGL child, so deleting the root recursively frees them -- no need to
 * null each child pointer individually, since build_status_bar()/build_
 * home_indicator_bar()/build_quick_drawer() reassign every one of them
 * again immediately after, with nothing running in between that could read
 * a stale pointer. quick_drawer_motion_image is the one exception -- a
 * transient drag-snapshot object created directly under lv_layer_top(),
 * not as quick_drawer's own child, so it needs its own explicit delete
 * when a mid-drag reload catches it still present. */
void gui_shell_teardown(void) {
    quick_drawer_motion_teardown_cancel();
    quick_drawer_brightness_teardown_flush();
    quick_drawer_motion_teardown_image();
    quick_drawer_volume_teardown_flush();
    quick_drawer_teardown_widgets();
    topbar_teardown();
    if (home_indicator_band) {
        lv_obj_delete(home_indicator_band);
        home_indicator_band = NULL;
    }
    if (dac_home_screen) {
        lv_obj_delete(dac_home_screen);
        dac_home_screen = NULL;
    }
    if (home_screen) {
        lv_obj_delete(home_screen);
        home_screen = NULL;
    }
}
void gui_shell_refresh_home(void) {
    lv_obj_t * old = home_screen;
    lv_obj_t * fresh = build_home_screen();
    if (!fresh) return;
    home_screen = fresh;
    gui_navigation_replace_home(old, fresh);
    if (lv_screen_active() == fresh) gui_shell_set_status_bar_screen_context(fresh);
    if (old) lv_obj_delete(old);
}
void gui_shell_init(uint32_t screen_width, uint32_t screen_height) {
    gui_shell_build_screens(screen_width, screen_height);
    start_bt_source_codec_reconcile_if_needed();
    start_bt_dac_startup_reapply_if_needed();
    if (current_settings.wifi_restore_at_startup && current_settings.wifi_last_on)
        gui_shell_wifi_ensure_enabled();
    if (current_settings.bt_restore_at_startup && current_settings.bt_last_on)
        gui_shell_bt_request_power(true);
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon about to be called");
#endif
    start_refresh_bt_icon();
#ifndef HOST_BUILD
    boot_checkpoint("start_refresh_bt_icon done");
#endif
}
void gui_shell_poll(void) {
    quick_drawer_poll_snapshot_wake();
    poll_wifi_status();
    poll_wifi_status_screen_off();
    poll_usb_audio_output();
    poll_sleep_timer();
    poll_wifi_toggle();
    poll_bt_toggle();
    start_bt_source_codec_reconcile_if_needed();
    poll_bt_source_codec_reconcile();
    poll_bt_dac_startup_reapply();
    poll_bt_apply_output_settings();
    poll_bt_dac_session();
    static bool drawer_bt_dac_enabled;
    if (drawer_bt_dac_enabled != current_settings.bt_dac_mode_enabled) {
        drawer_bt_dac_enabled = current_settings.bt_dac_mode_enabled;
        gui_shell_refresh_quick_drawer_expansion_toggles();
    }
    poll_refresh_bt_icon();
    start_bt_source_codec_reconcile_if_needed();
    poll_bt_monitor_lifecycle();
    poll_bt_reconnect();
}
void gui_shell_resume_connections(bool wifi_was_on, bool bt_was_on) {
#ifndef HOST_BUILD
    wifi_resume(wifi_was_on);
    bt_resume(bt_was_on);
#else
    (void) wifi_was_on;
    (void) bt_was_on;
#endif
}
void gui_shell_suspend_connections(bool * wifi_was_on, bool * bt_was_on) {
#ifndef HOST_BUILD
    gui_shell_cancel_bt_reconnect();
    *wifi_was_on = wifi_control_is_enabled();
    *bt_was_on = bt_is_powered_cached;
    wifi_suspend_launch(*wifi_was_on);
    bt_suspend_launch(*bt_was_on);
#else
    (void) wifi_was_on;
    (void) bt_was_on;
#endif
}
#define VISIBLE_STATUS_POLL_TICKS 4
/* wpa_cli forks a process per call (see wifi_status.c), so this is only
 * polled every WIFI_POLL_TICKS timer ticks rather than every tick like the
 * clock/battery -- wifi signal doesn't change fast enough to need
 * sub-second polling anyway. */
#define WIFI_POLL_TICKS 10
static int visible_status_poll_tick_counter = 0;
static int wifi_poll_tick_counter = 0;
void gui_shell_update_topbar(bool screen_just_woke) {
    /* Cheap startup-only marker check.  This runs every 500ms so the first
     * authoritative Bluetooth refresh begins promptly when bt_init finishes,
     * without delaying the rest of the UI or polling BlueZ prematurely.  It
     * also releases a pending Bluetooth-DAC apply behind the same gate. */
    if (!bt_startup_ready) {
        start_bt_dac_startup_reapply_if_needed();
        start_refresh_bt_icon();
    }

    if (screen_just_woke || ++visible_status_poll_tick_counter >= VISIBLE_STATUS_POLL_TICKS) {
        visible_status_poll_tick_counter = 0;
        refresh_clock_label();
        refresh_battery_topbar();
        refresh_headphone_icon();
        poll_usb_audio_output();
    }
    refresh_play_pause_topbar();
    if (screen_just_woke || ++wifi_poll_tick_counter >= WIFI_POLL_TICKS) {
        wifi_poll_tick_counter = 0;
        refresh_wifi_icon();
        start_bt_dac_startup_reapply_if_needed();
        start_refresh_bt_icon();
        gui_network_extend_bt_pairing_window();
    }
    reflow_topbar_layout();
    sync_bt_codec_status_icon();
}
bool gui_shell_has_background_work(void) {
    return bt_workers_active() || wifi_background_active() || bt_monitor_or_reconnect_active();
}
void gui_shell_cancel_background_work(void) {
    bt_background_cancel_monitors();
    wifi_background_cancel_status();
    bt_background_cancel_toggle();
    wifi_background_cancel_toggle();
    bt_background_cancel_workers();
    (void) gui_shell_bt_dac_shutdown(true);
}
lv_obj_t * gui_shell_get_home_screen(void) { return home_screen; }
lv_obj_t * gui_shell_get_dac_home_screen(void) { return dac_home_screen; }
lv_obj_t * gui_shell_get_status_bar_band(void) {
    return status_bar_band;
}
lv_obj_t * gui_shell_get_home_indicator_band(void) {
    return home_indicator_band;
}
void gui_shell_set_home_indicator_visible(bool visible) {
    if (home_indicator_band) {
        lv_obj_set_flag(home_indicator_band, LV_OBJ_FLAG_HIDDEN, !visible);
    }
}
