#include "../application/gui.h"
#include "../text/rendering/ui_label_text.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../shell/gui_shell.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include "../../network/airplay/airplay_control.h"
#include "../../network/airplay/airplay_bridge.h"
#include "../../network/airplay/airplay_metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include "gui_network_internal.h"

lv_obj_t * usb_dac_overlay_screen;
static lv_obj_t * usb_dac_hint_label;
static lv_obj_t * usb_dac_input_label;
static lv_obj_t * usb_dac_path_label;
static lv_obj_t * airplay_screen;
static lv_obj_t * airplay_overlay_screen;
static lv_obj_t * airplay_overlay_cover_img;
static lv_obj_t * airplay_overlay_title_label;
static bool airplay_overlay_showing = false;
static bool airplay_enable_pending;

bool gui_network_airplay_enable_pending(void) { return airplay_enable_pending; }
static lv_image_dsc_t airplay_overlay_cover_dsc;
static uint8_t * airplay_overlay_cover_bytes = NULL;

/* Local playback, Bluetooth DAC and AirPlay share the hw:0 output and are
 * mutually exclusive; these are the "turn the other one off" halves. */
void stop_local_playback(void) {
    if (audio_is_playing() || audio_is_paused()) {
        audio_stop();
        set_play_button_state(false);
        plugin_manager_notify_stopped();
    }
}

void disable_airplay_mode(void) {
    airplay_enable_pending = false;
    current_settings.wifi_dac_mode_enabled = false;
    settings_save_async(&current_settings);
    airplay_control_stop();
}

/* "Leave <DAC> mode?" popup shared by both DAC overlays. dismiss serves as
 * both the Cancel button and the tap-outside backdrop. */
static void build_dac_leave_popup(gui_popup_t * popup, const char * title,
                                  lv_event_cb_t confirm, lv_event_cb_t dismiss) {
    popup->popup = build_confirm_popup(title, LV_LABEL_LONG_WRAP, NULL, NULL, TR("Leave"),
                                       lv_color_hex(GUI_BREEZE_NEGATIVE), confirm, NULL, TR("Cancel"),
                                       accent_lv_color(), dismiss, NULL,
                                       dismiss, &popup->backdrop);
}

/* USB DAC takeover frame: black screen with header,
 * content container (a scrolling flex column on TEMPOTEC_V1), the source icon
 * and the title-font status label. v1_icon_px sizes the icon on V1 only
 * (0 keeps the native size). */
static lv_obj_t * build_dac_overlay_frame(lv_event_cb_t back_cb, const char * icon_src,
                                          int32_t v1_icon_px, const char * title,
                                          lv_obj_t ** out_content, lv_obj_t ** out_title) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, back_cb, NULL, NULL);

    lv_obj_t * content = scr;
#if defined(BOARD_TEMPOTEC_V1)
    content = lv_obj_create(scr);
    int32_t content_y = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_size(content, lv_pct(100),
                    BOARD_SCREEN_HEIGHT - content_y - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_pos(content, 0, content_y);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_hor(content, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_top(content, BOARD_SCALE_PY(8), 0);
    lv_obj_set_style_pad_bottom(content, BOARD_SCALE_PY(8), 0);
    lv_obj_set_style_pad_row(content, BOARD_SCALE_PY(8), 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
#endif

    lv_obj_t * icon = lv_image_create(content);
    lv_image_set_src(icon, asset_path(icon_src));
#if defined(BOARD_TEMPOTEC_V1)
    if (v1_icon_px > 0) {
        /* Fit the glyph to the portrait panel so it clears the status/header
         * rows and leaves room for the labels below it. */
        lv_obj_set_size(icon, BOARD_SCALE_PX(v1_icon_px), BOARD_SCALE_PX(v1_icon_px));
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    }
#else
    (void) v1_icon_px;
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-40));
#endif

    lv_obj_t * status_label = lv_label_create(content);
    lv_label_set_text(status_label, title);
    lv_obj_add_style(status_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(status_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_width(status_label, LV_PCT(90));
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
#else
    lv_obj_align_to(status_label, icon, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));
#endif
    *out_content = content;
    *out_title = status_label;
    return scr;
}

/* Muted, centered, fixed-width line under `below` (the labels stay centered
 * when their text updates). text may be NULL when it is filled in later. */
static lv_obj_t * dac_overlay_add_line(lv_obj_t * content, lv_obj_t * below, const char * text,
                                       int32_t gap) {
    lv_obj_t * label = lv_label_create(content);
    if (text) lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(label, LV_PCT(90));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
#if !defined(BOARD_TEMPOTEC_V1)
    lv_obj_align_to(label, below, LV_ALIGN_OUT_BOTTOM_MID, 0, gap);
#else
    (void) below;
    (void) gap;
#endif
    return label;
}

/* ---- USB DAC mode overlay + leave-confirmation popup --------------------
 * Full-screen takeover matching the stock firmware's hiby_usb.view (a
 * centered USB icon + a top-left back button, nothing else) -- deliberately
 * skips finalize_screen_navigation()'s swipe-to-back/swipe-to-player-screen
 * wiring: the
 * only way out is the back button, which asks for confirmation rather than
 * leaving immediately, and an accidental swipe bypassing that would defeat
 * the point. The confirmation popup itself reuses bt_action_popup's own
 * hand-built top-layer overlay shape (this codebase doesn't use LVGL's
 * lv_msgbox anywhere). ---- */
static gui_popup_t usb_dac_leave_popup;

static void usb_dac_leave_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&usb_dac_leave_popup);
}

static void usb_dac_leave_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&usb_dac_leave_popup);
    nav_pop(); /* leave the DAC overlay screen */
    start_usb_mode_switch(USB_MODE_STORAGE); /* "switch back to Storage (Default)" */
}

static void usb_dac_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_show(&usb_dac_leave_popup);
}

static lv_obj_t * build_usb_dac_overlay_screen(void) {
    lv_obj_t * content;
    lv_obj_t * status_label;
    lv_obj_t * scr = build_dac_overlay_frame(usb_dac_overlay_back_cb, "usb/usb.png", 0,
                                             TR("USB DAC mode"), &content, &status_label);

    usb_dac_hint_label = dac_overlay_add_line(content, status_label,
                                              TR("Waiting for USB audio…"), BOARD_SCALE_PX(8));
    usb_dac_input_label = dac_overlay_add_line(content, usb_dac_hint_label, NULL, BOARD_SCALE_PX(8));
    usb_dac_path_label = dac_overlay_add_line(content, usb_dac_input_label, NULL, BOARD_SCALE_PX(4));

    return scr;
}

static void set_label_if_changed(lv_obj_t * label, const char * text) {
    if (label && !ui_label_text_matches(label, text)) lv_label_set_text(label, text);
}

void format_rate(char * out, size_t size, unsigned int rate) {
    if (rate && rate % 1000 == 0) snprintf(out, size, "%u kHz", rate / 1000);
    else if (rate) snprintf(out, size, "%.1f kHz", (double) rate / 1000.0);
    else snprintf(out, size, "%s", TR("Unknown rate"));
}

static lv_timer_t * dac_stream_labels_timer = NULL;

static void dac_stream_labels_timer_cb(lv_timer_t * timer) {
    (void) timer;
    gui_network_bt_dac_refresh();
    char text[160];
    usb_dac_stream_info_t usb;
    usb_dac_bridge_get_stream_info(&usb);
    set_label_if_changed(usb_dac_hint_label, usb.streaming ? TR("This device is now a USB sound card") : TR("Waiting for USB audio…"));
    char input_rate[32], output_rate[32];
    format_rate(input_rate, sizeof(input_rate), usb.input_sample_rate);
    format_rate(output_rate, sizeof(output_rate), usb.output_sample_rate);
    snprintf(text, sizeof(text), TR("USB input: %s · %u-bit"), input_rate, usb.input_bit_depth);
    set_label_if_changed(usb_dac_input_label, text);
    snprintf(text, sizeof(text), TR("DAC path: %s · %u-bit"), output_rate, usb.output_bit_depth);
    set_label_if_changed(usb_dac_path_label, text);
}

/* ---- AirPlay screen -- WiFi analogue of Bluetooth DAC mode, see
 * airplay_control.h for the real mechanism (stock firmware's own shairport
 * binary). Same shape as build_bt_dac_screen(): a toggle plus an
 * explanation, and the same three-way mutual exclusion with local playback
 * and Bluetooth DAC. ---- */

static lv_obj_t * airplay_list;

static void airplay_toggle_cb(lv_event_t * e);

void populate_airplay_screen(void) {
    lv_obj_clean(airplay_list);
    add_pill_toggle_row(airplay_list, TR("AirPlay"), current_settings.wifi_dac_mode_enabled, airplay_toggle_cb);

    add_network_explanation(airplay_list,
                      TR("When on, this device is visible to AirPlay senders on your Wi-Fi network -- stream "
                      "audio from an iPhone, iPad, or Mac to be played through this device's own output."));
}

/* /usr/resource/hostname is the same file wifi_on.sh already reads for the
 * DHCP hostname it advertises -- reusing it here keeps the name AirPlay
 * senders see consistent with how this device already identifies itself on
 * the network, rather than inventing a second name. "HiBy_Music" matches
 * that script's own fallback default when the file is missing. */
static void get_device_name(char * out, size_t out_size) {
    snprintf(out, out_size, "HiBy_Music");
    FILE * f = fopen("/usr/resource/hostname", "r");
    if (f) {
        if (fgets(out, (int) out_size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

/* Anything reachable from the quick drawer (which can toggle these long
 * before their own screen has ever been opened) refreshes through these
 * rather than calling populate_*() directly -- lv_obj_clean() on a NULL
 * list is not survivable. */
static void refresh_airplay_screen_if_built(void) {
    if (airplay_list) populate_airplay_screen();
}

/* Shared by the AirPlay settings screen's toggle row and the quick drawer's
 * AirPlay tile -- the drawer must not reimplement any of the Wi-Fi guard,
 * transactional-start, or BT-DAC mutual-exclusion logic below. Returns the
 * resulting enabled state, which is NOT always the requested one (a rejected
 * enable returns false). */
bool gui_network_toggle_airplay(void) {
    bool turning_on = !current_settings.wifi_dac_mode_enabled && !airplay_enable_pending;
    /* Guard the enable path even though the tile itself is already guarded
     * (airplay_tile_cb() below) -- this screen can already be open when
     * Wi-Fi goes away, so a stale toggle tap must still be rejected here.
     * Checked before touching the setting at all, so a rejected enable
     * never even briefly persists as on. Turning OFF is always allowed,
     * Wi-Fi state notwithstanding -- an already-enabled feature must still
     * be toggleable off. */
    if (turning_on && !wifi_feature_guard()) {
        refresh_airplay_screen_if_built(); /* nothing changed, but keeps the toggle row's own drawn state honest */
        return current_settings.wifi_dac_mode_enabled;
    }
    if (turning_on && (gui_network_usb_dac_mode_active() || gui_network_usb_dac_pending())) {
        show_error_toast(TR("Failed to enable AirPlay"));
        return false;
    }
    airplay_enable_pending = false;
    if (turning_on && (current_settings.bt_dac_mode_enabled || gui_shell_bt_dac_busy())) {
        disable_bt_dac_mode();
        airplay_enable_pending = true;
        refresh_airplay_screen_if_built();
        return true;
    }
    current_settings.wifi_dac_mode_enabled = turning_on;
    settings_save_async(&current_settings);

    if (current_settings.wifi_dac_mode_enabled) {
        char name[64];
        get_device_name(name, sizeof(name));
        if (!airplay_control_start(name)) {
            /* Startup is transactional (see airplay_control_start()'s own
             * comment) -- a false return means nothing was actually left
             * running, so just undo the toggle rather than reporting the
             * device as "on" when it can't produce sound or be discovered. */
            current_settings.wifi_dac_mode_enabled = false;
            settings_save_async(&current_settings);
            show_error_toast(TR("Failed to enable AirPlay"));
            refresh_airplay_screen_if_built();
            return current_settings.wifi_dac_mode_enabled;
        }

    } else {
        /* Turning off must actually stop shairport and the bridge, not just
         * clear the setting -- otherwise the device stays discoverable and
         * an incoming stream still raises the overlay. */
        airplay_control_stop();
    }

    refresh_airplay_screen_if_built();
    return current_settings.wifi_dac_mode_enabled;
}

static void airplay_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_toggle_airplay();
}

void gui_network_open_airplay(void) {
    if (!wifi_feature_guard()) return;
    populate_airplay_screen();
    nav_push(airplay_screen);
}

void airplay_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_network_open_airplay();
}

/* ---- AirPlay overlay: shown only while a stream is actually active -----
 * Dedicated overlay screen displaying cover art, song name, and stream info
 * updated dynamically via airplay_metadata.c. Navigating back leaves the
 * stream running in the background until it completes or is stopped. ---- */

static void airplay_overlay_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_pop();
}

static lv_obj_t * build_airplay_overlay_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    build_screen_header(scr, NULL, airplay_overlay_back_cb, NULL, NULL);

    airplay_overlay_cover_img = lv_image_create(scr);
    lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
#if defined(BOARD_TEMPOTEC_V1)
    /* The fallback cover is 480px square; the V1 uses this overlay on a
     * 240x320 display, where its native size obscures the title and logo. */
    lv_obj_set_size(airplay_overlay_cover_img, BOARD_SCALE_PX(240), BOARD_SCALE_PX(240));
    lv_image_set_inner_align(airplay_overlay_cover_img, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(airplay_overlay_cover_img, LV_ALIGN_CENTER, 0, BOARD_SCALE_PY(-60));
#else
    lv_obj_align(airplay_overlay_cover_img, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(-60));
#endif

    airplay_overlay_title_label = lv_label_create(scr);
    lv_label_set_text(airplay_overlay_title_label, TR("AirPlay"));
    lv_obj_add_style(airplay_overlay_title_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(airplay_overlay_title_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_set_width(airplay_overlay_title_label, lv_pct(85));
#if defined(BOARD_TEMPOTEC_V1)
    /* Titles are replaced with live AirPlay track metadata. Keep them on a
     * single title-font line so long names cannot collide with the footer
     * logo on V1's short screen. */
    lv_obj_set_height(airplay_overlay_title_label,
                      lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_TITLE)));
    lv_label_set_long_mode(airplay_overlay_title_label, LV_LABEL_LONG_DOT);
#else
    lv_label_set_long_mode(airplay_overlay_title_label, LV_LABEL_LONG_WRAP);
#endif
    lv_obj_set_style_text_align(airplay_overlay_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(airplay_overlay_title_label, airplay_overlay_cover_img, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(24));

    /* The AirPlay settings tile's own "wireless/airplay.png" is a stock
     * theme2 asset with a completely different (colored, wireless-grid)
     * style -- not the recognizable AirPlay-audio glyph (concentric arcs
     * over an upward triangle). This is a project-owned custom asset
     * instead: assets/theme2/playing_plane/airplay_logo_white.png, a
     * solid-white recolor of the standard glyph (transparent background,
     * alpha-preserved from the original anti-aliased edges) sized for this
     * overlay specifically. Resolved via asset_path()'s own THEME_OVERRIDE_
     * ROOT mechanism on target (see assets.c's own comment -- exactly the
     * "asset this app adds with no stock equivalent" case that exists for)
     * and committed directly under assets/theme2/ for the host build,
     * matching stream_media/subsonic.png's own precedent. */
    lv_obj_t * airplay_logo = lv_image_create(scr);
    lv_image_set_src(airplay_logo, asset_path("playing_plane/airplay_logo_white.png"));
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(airplay_logo, BOARD_SCALE_PX(120), BOARD_SCALE_PX(112));
    lv_image_set_inner_align(airplay_logo, LV_IMAGE_ALIGN_CONTAIN);
#endif
    lv_obj_align(airplay_logo, LV_ALIGN_BOTTOM_MID, 0, BOARD_SCALE_PX(-24));

    return scr;
}

/* Poll from update_timer_cb, same shape as every other background-thread-
 * to-UI-thread poll in this codebase. Shows/hides the overlay based on
 * airplay_bridge_is_streaming() transitions and applies whatever metadata
 * has arrived meanwhile -- decoupled from whether the overlay happens to
 * be the currently active screen, so metadata keeps updating even if the
 * user navigated away from it. */
void gui_network_poll_airplay_overlay(void) {
    if (airplay_enable_pending && !gui_shell_bt_dac_busy()) {
        airplay_enable_pending = false;
        gui_network_toggle_airplay();
    }
    /* Between gui_network_teardown() and the next init the widgets below do
     * not exist; the state catches up on the first tick after the rebuild. */
    if (!airplay_overlay_screen) return;
    /* Self-heals a respawn failure from airplay_control_disconnect_active_
     * stream()'s kill-and-respawn (gui_player.c, triggered by local playback
     * resuming while AirPlay was streaming) -- that path deliberately
     * ignores airplay_control_start()'s return value (see its own comment),
     * so this is the only place such a failure ever gets noticed and the
     * persisted setting corrected, same as airplay_toggle_cb()'s own
     * synchronous failure handling a few hundred lines up in this file. */
    if (current_settings.wifi_dac_mode_enabled && !airplay_control_is_active()) {
        current_settings.wifi_dac_mode_enabled = false;
        settings_save_async(&current_settings);
        show_error_toast(TR("AirPlay stopped unexpectedly"));
        populate_airplay_screen();
    }

    bool streaming = airplay_bridge_is_streaming();
    if (streaming != airplay_overlay_showing) {
        airplay_overlay_showing = streaming;
        if (streaming) {
            /* airplay_bridge.c's own reader thread already called
             * audio_stop() before flipping is_streaming() true (see its own
             * comment) -- that only stops the backend, same as usb_mode_
             * control_apply()'s own DAC-mode takeover a few hundred lines up
             * in this file; the play button icon and plugin state still
             * need their own explicit refresh here on the UI thread, or
             * they'd keep claiming playback is active/paused for a track
             * that AirPlay just silently took the output device out from
             * under. */
            set_play_button_state(false);
            plugin_manager_notify_stopped();

            nav_push(airplay_overlay_screen);
        } else {
            /* Reset to defaults here, on SESSION END, not on the next
             * session's start -- metadata for a new session can (and
             * regularly does) arrive slightly before airplay_bridge_is_
             * streaming() flips true for it (see the unconditional-apply
             * comment below), and resetting on start would wipe that
             * early update right back out, leaving the "AirPlay"
             * placeholder up for the entire session since nothing else
             * would trigger a re-apply. Resetting here instead means by
             * the time a NEW session's early metadata can possibly arrive,
             * this session's leftover content is already gone -- closing
             * the same staleness gap without racing a legitimate early
             * update for the next session. */
            lv_label_set_text(airplay_overlay_title_label, TR("AirPlay"));
            free(airplay_overlay_cover_bytes);
            airplay_overlay_cover_bytes = NULL;
            lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));

            if (lv_screen_active() == airplay_overlay_screen) {
                /* Only auto-dismiss if the user is still looking at it. */
                nav_pop();
            } else {
                /* User already navigated elsewhere with the overlay still
                 * buried somewhere in their back-navigation history --
                 * splice its now-defunct slot out entirely rather
                 * than leaving it there for a later "back" to resurface a
                 * screen with no active stream behind it. Found by screen
                 * identity, not a remembered index: the overlay may have
                 * been popped since the push, and its old slot reused by
                 * an unrelated screen. */
                for (int i = gui_navigation_get_depth() - 1; i >= 0; i--) {
                    if (gui_navigation_get_screen_at(i) == airplay_overlay_screen) nav_remove_stack_slot(i);
                }
            }
        }
    }

    airplay_metadata_update_t upd;
    if (!airplay_metadata_consume_update(&upd)) return;

    /* Applied regardless of `streaming` -- the metadata and PCM bridge are
     * two independent threads/connections (airplay_metadata.c vs. airplay_
     * bridge.c), so a title/cover update for a session can arrive slightly
     * before airplay_bridge_is_streaming() actually flips true. Discarding
     * it here (an earlier version of this function did) meant the overlay
     * showed the plain "AirPlay" placeholder for the entire first track of
     * every session unless a later track change happened to resend it.
     * Applying it always is safe: the streaming->false reset above already
     * runs the instant a session ends, before a NEW session's own metadata
     * could possibly arrive, so nothing from a PREVIOUS session is ever
     * still sitting in these widgets by the time an early update for a new
     * one shows up here. */
    lv_label_set_text(airplay_overlay_title_label, upd.title[0] ? upd.title : TR("AirPlay"));

    if (!upd.has_cover) {
        free(airplay_overlay_cover_bytes);
        airplay_overlay_cover_bytes = NULL;
        lv_image_set_src(airplay_overlay_cover_img, asset_path("playing_plane/default_cover_565.png"));
        return;
    }

    free(airplay_overlay_cover_bytes);
    airplay_overlay_cover_bytes = (uint8_t *) upd.cover_pixels;

    /* LV_IMAGE_HEADER_MAGIC is required so LVGL's image decoders recognize
     * the custom RGB565 image descriptor without attempting format fixups. */
    memset(&airplay_overlay_cover_dsc, 0, sizeof(airplay_overlay_cover_dsc));
    airplay_overlay_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    airplay_overlay_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    airplay_overlay_cover_dsc.header.w = AIRPLAY_COVER_WIDTH;
    airplay_overlay_cover_dsc.header.h = AIRPLAY_COVER_HEIGHT;
    airplay_overlay_cover_dsc.header.stride = AIRPLAY_COVER_WIDTH * 2;
    airplay_overlay_cover_dsc.data = airplay_overlay_cover_bytes;
    airplay_overlay_cover_dsc.data_size = (uint32_t) AIRPLAY_COVER_WIDTH * AIRPLAY_COVER_HEIGHT * 2;
    lv_image_set_src(airplay_overlay_cover_img, &airplay_overlay_cover_dsc);
}

void gui_network_receivers_build(void) {
    gui_network_bt_dac_build();
    usb_dac_overlay_screen = build_usb_dac_overlay_screen();
    airplay_screen = network_list_screen(TR("AirPlay"), &airplay_list);
    airplay_overlay_screen = build_airplay_overlay_screen();
    build_dac_leave_popup(&usb_dac_leave_popup, TR("Leave USB DAC mode?"),
                          usb_dac_leave_confirm_cb, usb_dac_leave_dismiss_cb);
    dac_stream_labels_timer_cb(NULL);
    /* Guarded like library_az_index.c's drag timer -- gui_network_init()
     * can run again after a UI reload, and an unguarded lv_timer_create()
     * here would leak/duplicate a recurring timer each time. */
    if (!dac_stream_labels_timer) dac_stream_labels_timer = lv_timer_create(dac_stream_labels_timer_cb, 500, NULL);
}

/* Deliberately does NOT touch dac_stream_labels_timer (already
 * guarded/reused correctly by gui_network_receivers_build()). */
void gui_network_receivers_teardown(void) {
    gui_popup_teardown(&usb_dac_leave_popup);
    gui_network_bt_dac_teardown();
    network_delete_screen(&usb_dac_overlay_screen);
    network_delete_screen(&airplay_screen);
    network_delete_screen(&airplay_overlay_screen);
    /* The widgets died with their screens; the 500 ms label timer and the
     * refresh_*_if_built guards survive a reload and must see NULL. */
    usb_dac_hint_label = NULL;
    usb_dac_input_label = NULL;
    usb_dac_path_label = NULL;
    airplay_list = NULL;
    airplay_overlay_cover_img = NULL;
    airplay_overlay_title_label = NULL;
}

lv_obj_t * gui_network_get_usb_dac_overlay(void) { return usb_dac_overlay_screen; }

bool gui_network_boot_prompt_blocked(void) {
    lv_obj_t * active = lv_screen_active();
    return gui_network_usb_prompt_invalidated() || active == gui_network_get_bt_dac_screen() ||
           active == usb_dac_overlay_screen || active == airplay_overlay_screen;
}
