#include "gui_shell_internal.h"

lv_obj_t * status_bar_band = NULL;
lv_obj_t * clock_topbar_group = NULL;
lv_obj_t * clock_topbar_digit[5] = { NULL };
lv_obj_t * clock_topbar_ampm = NULL;
lv_obj_t * volume_topbar_group = NULL;
lv_obj_t * volume_topbar_icon = NULL;
lv_obj_t * volume_topbar_digit[3] = { NULL };
lv_obj_t * volume_topbar_headphone = NULL;
static int volume_warn_threshold_percent = -1;
lv_obj_t * battery_topbar_group;
static lv_obj_t * battery_topbar_digit[3];
lv_obj_t * battery_icon_frame;
/* Width last used to anchor the percentage group.  LVGL's align_to stores
 * coordinates; it does not follow later content/font/theme size changes. */
static int32_t battery_topbar_anchored_width = -1;
static uint32_t last_topbar_hidden_elements = UINT32_MAX;
static int last_topbar_clock_position = -1;
static bool last_topbar_show_battery_percent;
lv_obj_t * wifi_icon;
lv_obj_t * bt_status_icon;
lv_obj_t * a2dp_status_icon;
lv_obj_t * usb_audio_status_icon;
lv_obj_t * play_pause_status_icon;
lv_obj_t * bt_codec_status_icon;
void gui_shell_set_status_bar_screen_context(lv_obj_t * screen) {
    if (!status_bar_band) return;
    shell_theme_styles_init();

    /* The bar is the theme's screen color with glyphs in its text color.
     * Player and Lyrics draw edge-to-edge artwork, as can Home with a
     * background image, so there it is translucent. Keeping this on the
     * persistent band also lets transition snapshots composite it. */
    bool home_background = screen && screen == home_screen &&
                           lv_obj_get_style_bg_image_src(screen, LV_PART_MAIN) != NULL;
    bool over_artwork = screen == gui_player_get_screen() || screen == gui_lyrics_get_screen() ||
                        home_background;
    /* An open drawer is a themed surface of its own, whatever is under it. */
    lv_color_t band = shell_bar_color, glyph = shell_bar_glyph_color;
    bool recolor = !shell_theme_native;
    if (over_artwork && !quick_drawer_open) {
        /* Over the frost, follow what the player's text picked for it.
         * Lyrics and Home wallpapers stay on the classic dark scrim. */
        lv_style_value_t text;
        bool player = screen == gui_player_get_screen() &&
                      lv_style_get_prop(gui_player_backdrop_text_style(false), LV_STYLE_TEXT_COLOR, &text) ==
                          LV_STYLE_RES_FOUND;
        if (player && gui_theme_color_is_dark(text.color)) {
            glyph = text.color;
            recolor = true;
        } else {
            band = lv_color_black();
            glyph = lv_color_white();
            recolor = false;
        }
    }
    lv_obj_set_style_bg_color(status_bar_band, band, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(status_bar_band, over_artwork && !quick_drawer_open ? LV_OPA_50 : LV_OPA_COVER,
                            LV_PART_MAIN);
    shell_set_topbar_glyphs(glyph, recolor);
}
#if defined(BOARD_TEMPOTEC_V1)
bool gui_shell_player_topbar_hidden(lv_obj_t * screen) {
    if (!screen) return false;

    bool player_or_lyrics = screen == gui_player_get_screen() || screen == gui_lyrics_get_screen();
    lv_obj_t * player_screen = gui_player_get_screen();
    if (player_screen && screen == player_screen) return true;
    return current_settings.hide_player_topbar && player_or_lyrics;
}
#endif
void sync_player_topbar_visibility(lv_obj_t * screen) {
    /* Settings > Display > "Hide Player/Lyrics Top Bar" -- hides the global
     * status bar while the Player or its fullscreen Lyrics view is active;
     * every other screen keeps its status bar as normal regardless of this
     * setting. player_dismiss_btn (Player's own standalone back arrow) is
     * additionally tied to the same setting, Player-only -- when the
     * status bar is hidden there's no other visible way back short of the
     * swipe/hardware-button gesture, matching the immersive intent; Lyrics
     * has no equivalent standalone back button of its own. Real, live
     * object state here is allowed to reflect "whatever the user last
     * navigated to" -- correctness for the Phase 2 transition CACHE (built
     * while Player is inactive, so this function's own object-flag state
     * can't be trusted for it) is handled independently by
     * build_flattened_transition_frame()'s own temporary-flag-then-restore
     * approach, not by this function. */
    bool hide = gui_shell_player_topbar_hidden(screen) || screen == gui_lock_screen_get_screen();
    /* The quick drawer wins while it is open. It deliberately keeps the
     * status bar above itself (see open_quick_drawer()), which does nothing
     * when the Player has hidden it outright -- the drawer would slide down
     * over an immersive Player with no clock or battery on it at all. Any
     * path that re-syncs while the drawer is open gets the same answer, so
     * navigating away from a drawer long-press cannot leave it stuck on. */
    if (quick_drawer_open && screen != gui_lock_screen_get_screen()) hide = false;
    if (status_bar_band) {
        gui_shell_set_status_bar_screen_context(screen);
        lv_obj_set_flag(status_bar_band, LV_OBJ_FLAG_HIDDEN, hide);
    }
    gui_player_sync_topbar_visibility(screen);

    /* player_transition_rebuild_cache() (see its own doc comment) refuses to
     * run while gui_player_get_screen() is still the active one -- which, since
     * whatever marked the cache dirty (track change, cover art, play/pause,
     * accent color) almost always happens WHILE the user is looking at the
     * Player screen, is exactly the state the cache is usually dirtied in.
     * Its own lv_async_call() only ever fires once, right after being
     * scheduled, so without this it would stay permanently dirty from that
     * point on -- confirmed on-device (every "PERF transition" line showing
     * player_cache=0 cache_dirty=1, never once actually using the cache).
     * This function already runs as the last step of every real navigation
     * (nav_push/nav_pop/screen_transition_slide's cut fallback/
     * slide_transition_done_cb's commit/nav_reset_to_home), i.e. exactly
     * "after the Player has settled" -- so retrying here, once per actual
     * screen change away from Player, is the natural moment. */
    if (screen != gui_player_get_screen() && player_transition_cache_is_dirty())
        lv_async_call(player_transition_cache_async_cb, NULL);
}
void build_status_bar(void) {
#if defined(BOARD_TEMPOTEC_V1)
    topbar_v1_layout_signature_valid = false;
    topbar_v1_art_scale_percent = 100;
#endif
    battery_topbar_anchored_width = -1;
    lv_obj_t * bar = lv_layer_top();

    /* Every plain lv_obj_create() gets LV_OBJ_FLAG_SCROLLABLE by default
     * (confirmed in lv_obj.c's base constructor), including layer_top
     * itself -- nothing ever removes it since we only ever add children to
     * this layer, never scroll it. Left alone, lv_indev_find_scroll_obj()
     * walks the pressed object's FULL ancestor chain (see lv_indev_scroll.c)
     * looking for a scrollable object with overflow, and can end up
     * "claiming" a touch as a scroll of layer_top instead of delivering it
     * as a normal press/click/gesture to whatever real widget was actually
     * touched (this is exactly what silently broke the quick-drawer's
     * swipe-up-to-close gesture, and is a very plausible cause of the
     * drawer's on-screen buttons appearing unresponsive on the real
     * touchscreen -- a real finger tap always has a few px of jitter, unlike
     * a synthetic zero-movement click, and that's enough to trigger this
     * scroll-vs-click arbitration). */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* A dedicated band sized exactly to STATUS_BAR_CLEARANCE, with every
     * status bar element vertically MID-aligned within it, rather than
     * each element aligned to the full-screen top layer with a small
     * hand-tuned Y offset -- the old per-element offsets (1, -3) put
     * everything within a few px of the true screen top regardless of how
     * tall STATUS_BAR_CLEARANCE actually was, so shrinking the clearance
     * left all the real content hugging y=0 with dead space below it
     * instead of using the newly smaller band evenly. */
    lv_obj_t * band = lv_obj_create(bar);
    status_bar_band = band;
    lv_obj_remove_style_all(band);
    /* Status glyphs and digits are white independently of theme colors.
     * Give them a stable surface when Home or another page uses a light theme. */
    lv_obj_set_style_bg_color(band, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_set_size(band, lv_pct(100), STATUS_BAR_CLEARANCE);
    lv_obj_set_pos(band, 0, 0);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);

    /* Centered clock -- use fixed board-specific faces so the clock,
     * volume and battery readouts share one natural, accessibility-independent
     * text size. */
    clock_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(clock_topbar_group);
    lv_obj_set_size(clock_topbar_group, LV_SIZE_CONTENT, STATUS_BAR_CLEARANCE);
    lv_obj_set_flex_flow(clock_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock_topbar_group, 0, 0);
    lv_obj_remove_flag(clock_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; i++) {
        clock_topbar_digit[i] = lv_label_create(clock_topbar_group);
        lv_label_set_text(clock_topbar_digit[i], i == 2 ? ":" : "0");
        style_topbar_text(clock_topbar_digit[i], topbar_digit_font());
    }
    clock_topbar_ampm = lv_label_create(clock_topbar_group);
    lv_label_set_text(clock_topbar_ampm, TR("AM"));
    style_topbar_text(clock_topbar_ampm, topbar_ampm_font());
    lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN); /* refresh_clock_label() unhides this if clock_24h is off */

    /* LAST, after every child exists -- see the matching comment on
     * volume_topbar_group's own align() call below for why (LV_SIZE_CONTENT
     * doesn't retroactively re-run an earlier alignment as children grow
     * it). refresh_clock_label() (called right after build_status_bar() in
     * gui_init) immediately overwrites these placeholder "0"/":" sprites
     * with the real time, so there's no visible flash of "00:00". */
    lv_obj_align(clock_topbar_group, LV_ALIGN_CENTER, 0, 0);

    /* Icon row: speaker icon, red volume number, headphone-out icon, etc.
     * Anchored at the left margin independently of the centered clock.
     * A flex row lets hidden digit slots (see
     * refresh_volume_topbar()) collapse cleanly instead of leaving a gap. */
    volume_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(volume_topbar_group);
    /* Keep the row's vertical center stable as status icons appear.  Some
     * normalized image canvases are taller than the initially-visible 30 px
     * speaker (for example po.png becomes 38 px).  With content height,
     * unhiding one grows this already-aligned group downward because
     * lv_obj_align_to() does not continuously re-center it. */
    lv_obj_set_size(volume_topbar_group, LV_SIZE_CONTENT, STATUS_BAR_CLEARANCE);
    lv_obj_set_flex_flow(volume_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(volume_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* Digit sprites already carry a small transparent inset on the full-size
     * boards; keep their existing zero-gap row there. The V1's scaled icon
     * set needs one physical pixel between its denser status glyphs. */
#if defined(BOARD_TEMPOTEC_V1)
    /* On the V1, proportional glyph scaling makes a one-pixel flex gap
     * legible between the denser set of output/status indicators. */
    lv_obj_set_style_pad_column(volume_topbar_group, BOARD_SCALE_PX(2), 0);
#else
    lv_obj_set_style_pad_column(volume_topbar_group, 0, 0);
#endif
    lv_obj_remove_flag(volume_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    /* Normalize against the sprite's measured visible glyph bounds. */
    volume_topbar_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_icon, topbar_icon("volume_2"));
    layout_volume_topbar_image(volume_topbar_icon);

    /* White by default (the sprite's own native color, no recolor style
     * applied at creation); refresh_volume_topbar() below switches each
     * digit to a flat (255,0,0) recolor once the level reaches
     * volume_warn_threshold_percent, matching the stock player's own
     * config-driven behavior (see device_config.h) instead of the flat
     * always-red guess from the previous round. */
    for (int i = 0; i < 3; i++) {
        volume_topbar_digit[i] = lv_label_create(volume_topbar_group);
        lv_label_set_text(volume_topbar_digit[i], "0");
        style_topbar_text(volume_topbar_digit[i], topbar_digit_font());
    }

    /* Headphone-out glyph (topbar/po.png) -- starts hidden and is shown
     * by refresh_headphone_icon() when a headphone/dongle is plugged in
     * (see headphone_status.h). */
    volume_topbar_headphone = lv_image_create(volume_topbar_group);
    lv_image_set_src(volume_topbar_headphone, topbar_icon("headphones"));
    layout_headphone_topbar_image(volume_topbar_headphone);
    lv_obj_add_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);

    /* Same flex row as the headphone-jack glyph above, not a separate fixed
     * position -- shown/hidden independently by its own real A2DP state
     * (poll_refresh_bt_icon()), so it naturally sits right next to the jack
     * glyph when both a wired and a Bluetooth output are connected at once,
     * or takes the jack glyph's spot on its own when only Bluetooth is (the
     * flex row's own hidden-children-collapse behavior, already relied on
     * by the volume digit slots above, does this for free -- no manual
     * "replace" logic needed). */
    a2dp_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(a2dp_status_icon, topbar_icon("audio_lines"));
    layout_a2dp_topbar_image(a2dp_status_icon);
    lv_obj_add_flag(a2dp_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by poll_refresh_bt_icon() once an A2DP source PCM exists */

    /* Same flex-collapse shape as the headphone/A2DP glyphs above -- shown/
     * hidden by poll_usb_audio_output() once an external USB audio device
     * (DAC/amp) is detected, entirely automatically, no Settings toggle
     * anywhere (unlike Storage/USB DAC/ADB in the manual USB Mode screen --
     * this is meant to feel like the wired headphone jack, not a mode you
     * switch into). */
    usb_audio_status_icon = lv_image_create(volume_topbar_group);
    /* Uses topbar/usb.png for the topbar status row (distinct from
     * usb/usb.png used by the full-screen USB DAC mode overlay). */
    lv_image_set_src(usb_audio_status_icon, topbar_icon("usb"));
    layout_usb_audio_topbar_image(usb_audio_status_icon);
    lv_obj_add_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Rightmost in this row -- always after whichever headphone-output
     * glyph(s) above are currently shown, per the same flex-collapse
     * reasoning. play.png while actually playing, pause.png while paused,
     * hidden entirely when stopped/nothing loaded --
     * refresh_play_pause_topbar(). */
    play_pause_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(play_pause_status_icon, topbar_icon("play"));
    layout_play_pause_topbar_image(play_pause_status_icon);
    lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);

    /* Negotiated Bluetooth codec indicator (e.g. sbc.png, aac.png, aptx.png,
     * aptx_hd.png, ldac.png, ssc.png, uat.png) -- rightmost in volume_topbar_group,
     * lowest priority on the left side. Shows when an A2DP source PCM is
     * connected and fits without colliding with clock_topbar_group.
     * Hidden by default. */
    bt_codec_status_icon = lv_image_create(volume_topbar_group);
    lv_image_set_src(bt_codec_status_icon, bt_codec_get_asset(BT_CODEC_TYPE_SBC));
    layout_codec_topbar_image(bt_codec_status_icon);
    lv_obj_add_flag(bt_codec_status_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align(volume_topbar_group, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(4), 0);

    /* One Lucide image owns the complete battery silhouette. Swapping whole
     * states avoids the old independently-rounded frame/fill/terminal seams. */
    battery_icon_frame = lv_image_create(band);
    lv_image_set_src(battery_icon_frame, topbar_icon("battery"));
    layout_battery_topbar_image(battery_icon_frame);
    lv_obj_align(battery_icon_frame, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(4), 0);

    /* Sprite digits (topbar/N.png + percent.png), same treatment as the
     * clock/volume readouts above -- up to 3 digit slots (0-100, same
     * leading-slot-hiding scheme as volume_topbar_digit) plus a trailing
     * percent sign. The whole group is hidden outright when the real
     * percent is unknown (battery_get_percent() < 0, e.g. host with no
     * /sys/class/power_supply) -- same "icon only, no fake reading" honesty
     * the old blank-text label had. */
    battery_topbar_group = lv_obj_create(band);
    lv_obj_remove_style_all(battery_topbar_group);
    lv_obj_set_size(battery_topbar_group, LV_SIZE_CONTENT, STATUS_BAR_CLEARANCE);
    lv_obj_set_flex_flow(battery_topbar_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_topbar_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_topbar_group, 0, 0);
    lv_obj_remove_flag(battery_topbar_group, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 3; i++) {
        battery_topbar_digit[i] = lv_label_create(battery_topbar_group);
        lv_label_set_text(battery_topbar_digit[i], "0");
        style_topbar_text(battery_topbar_digit[i], topbar_digit_font());
    }
    lv_obj_t * battery_topbar_percent = lv_label_create(battery_topbar_group);
    lv_label_set_text(battery_topbar_percent, "%");
    style_topbar_text(battery_topbar_percent, topbar_digit_font());

    /* Anchored to battery_icon itself (not a hand-tuned x) rather than a
     * fixed band offset, since the group's own width varies with the
     * digit count (1-3) -- LAST, after every child exists, same reasoning
     * as volume_topbar_group's align() below. */
    lv_obj_align_to(battery_topbar_group, battery_icon_frame, LV_ALIGN_OUT_LEFT_MID, -BOARD_SCALE_PX(2), 0);

    wifi_icon = lv_image_create(band);
    lv_image_set_src(wifi_icon, topbar_icon("wifi_off"));
    layout_radio_topbar_image(wifi_icon);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(105), 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_wifi_icon() once wifi_control_is_enabled() */

    bt_status_icon = lv_image_create(band);
    lv_image_set_src(bt_status_icon, topbar_icon("bluetooth"));
    layout_bluetooth_topbar_image(bt_status_icon);
    lv_obj_align(bt_status_icon, LV_ALIGN_RIGHT_MID, -BOARD_SCALE_PX(145), 0);
    lv_obj_add_flag(bt_status_icon, LV_OBJ_FLAG_HIDDEN); /* shown by refresh_bt_icon() once bt_control_is_powered() */

    shell_theme_styles_init();
    topbar_add_glyph_style(band);
}

/* LVGL's local style setters invalidate even when the value is unchanged,
 * and the status tick reapplies these on every pass. Writing only on change
 * keeps an unchanged tick from dirtying the top bar. A missing local value
 * still gets written, so overrides behave as before. */
bool topbar_local_style_is(lv_obj_t * obj, lv_style_prop_t prop, int32_t value) {
    lv_style_value_t current;
    return lv_obj_get_local_style_prop(obj, prop, &current, 0) == LV_STYLE_RES_FOUND &&
           current.num == value;
}

/* lv_image_set_src() invalidates and re-reads the header even for the same
 * file, so status refreshes go through this. Returns whether it changed. */
bool topbar_set_image(lv_obj_t * image, const char * src) {
    if (image_file_source_matches(lv_image_get_src(image), src)) return false;
    lv_image_set_src(image, src);
    return true;
}
static void topbar_set_label(lv_obj_t * label, const char * text) {
    if (!ui_label_text_matches(label, text)) lv_label_set_text(label, text);
}

/* Right-aligned digit slots: leading slots stay hidden, the rest show one
 * digit each. Labels are only rewritten when their text changes. */
static void topbar_set_digits(lv_obj_t * const slots[3], int value) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", value);
    int len = (int) strlen(digits);
    for (int i = 0; i < 3; i++) {
        int digit_index = i - (3 - len);
        if (digit_index < 0) {
            lv_obj_add_flag(slots[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            char digit[2] = { digits[digit_index], '\0' };
            topbar_set_label(slots[i], digit);
            lv_obj_remove_flag(slots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
void refresh_play_pause_topbar(void) {
    bool playing = audio_is_playing();
    bool paused = !playing && audio_is_paused();
    bool was_hidden = lv_obj_has_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    bool should_hide = !playing && !paused;

    if (!should_hide) {
        const char * source = topbar_icon(playing ? "play" : "pause");
        if (!image_file_source_matches(lv_image_get_src(play_pause_status_icon), source)) {
            lv_image_set_src(play_pause_status_icon, source);
            layout_play_pause_topbar_image(play_pause_status_icon);
        }
        if (was_hidden) lv_obj_remove_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    } else if (!was_hidden) {
        lv_obj_add_flag(play_pause_status_icon, LV_OBJ_FLAG_HIDDEN);
    }

    if (was_hidden != should_hide) {
        sync_bt_codec_status_icon();
    }
}
static void refresh_battery_topbar_anchor(bool force) {
    /* Hidden children and label/font changes can alter a flex container's
     * LV_SIZE_CONTENT width without changing the number of visible digits.
     * Resolve layout before measuring, then re-anchor only when the effective
     * width changed (or the group itself just became visible/hidden). */
    lv_obj_update_layout(battery_topbar_group);
    int32_t width = lv_obj_get_width(battery_topbar_group);
    if (force || width != battery_topbar_anchored_width) {
        battery_topbar_anchored_width = width;
        reflow_topbar_layout();
    }
}
void refresh_battery_topbar(void) {
    int percent = battery_get_display_percent();

    /* The battery icon has its own topbar visibility preference;
     * current_settings.show_battery_percent (Settings > Power > "Battery
     * Percentage") controls only the "NN%" digit readout below. Edge-triggered
     * (compares against the group's own current
     * hidden-flag rather than setting it unconditionally every call) since
     * this whole function runs every tick the screen is on -- re-syncing
     * the wifi/bt icon positions that often, on every tick, for a flag that
     * only ever changes on a battery-unplugged/replugged edge or a Settings
     * toggle, would be pure churn. */
    bool percent_should_show = percent >= 0 && current_settings.show_battery_percent;
    bool percent_was_shown = !lv_obj_has_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN);
    if (percent_should_show != percent_was_shown) {
        lv_obj_set_flag(battery_topbar_group, LV_OBJ_FLAG_HIDDEN, !percent_should_show);
    }

    if (percent < 0) {
        if (percent_should_show != percent_was_shown) refresh_battery_topbar_anchor(true);
        topbar_set_image(battery_icon_frame, topbar_icon("battery"));
        lv_obj_remove_local_style_prop(battery_icon_frame, LV_STYLE_IMAGE_RECOLOR, 0);
        lv_obj_remove_local_style_prop(battery_icon_frame, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
        return;
    }
    if (percent > 100) percent = 100;

    /* charge_limiter_is_holding() (voltage cap applied) is true for the
     * entire time the limiter is on and charging is active, independent of
     * percentage -- not a useful "suppress the bolt" signal by itself.
     * charge_limiter_is_confirmed_off() only goes true once charging has
     * actually tapered off/completed under the cap (same physically-
     * verified signal led_control.c uses for the charge-complete LED), so
     * the bolt stays lit for as long as current is genuinely still
     * flowing. */
    bool limiter_capped_now = charge_limiter_is_confirmed_off();
    bool charging = !limiter_capped_now && battery_is_charging();
    bool low = !charging && percent < 5;

    const char * battery_asset = charging ? "battery_charging" :
                                 percent >= 85 ? "battery_full" :
                                 percent >= 40 ? "battery_medium" :
                                 percent >= 10 ? "battery_low" :
                                                 "battery";
    if (topbar_set_image(battery_icon_frame, topbar_icon(battery_asset)))
        layout_battery_topbar_image(battery_icon_frame);
    if (low && !topbar_local_style_is(battery_icon_frame, LV_STYLE_IMAGE_RECOLOR_OPA, LV_OPA_COVER)) {
        lv_obj_set_style_image_recolor(battery_icon_frame, lv_color_make(255, 64, 64), 0);
        lv_obj_set_style_image_recolor_opa(battery_icon_frame, LV_OPA_COVER, 0);
    } else if (!low) {
        lv_obj_remove_local_style_prop(battery_icon_frame, LV_STYLE_IMAGE_RECOLOR, 0);
        lv_obj_remove_local_style_prop(battery_icon_frame, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
    }

    topbar_set_digits(battery_topbar_digit, percent);

    /* The content width can also change with same-length digit text or a
     * font/theme update.  Measuring after layout catches those cases while
     * keeping coordinate updates and dependent icon movement edge-triggered. */
    refresh_battery_topbar_anchor(percent_should_show != percent_was_shown);
}

/* Called at startup and whenever the displayed volume changes. Skip
 * identical strings so LVGL does not re-decode unchanged digit images. */
void refresh_volume_topbar(int32_t percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    quick_drawer_sync_volume(percent);

    /* Plugins can set startup volume before the shell is constructed. Leave
     * the digit cache untouched so shell initialization renders that volume. */
    for (int i = 0; i < 3; i++) {
        if (!volume_topbar_digit[i]) return;
    }

    topbar_set_digits(volume_topbar_digit, (int) percent);

    /* volume_warn_threshold_percent is -1 when the feature is off (see its
     * declaration) -- guard it explicitly rather than just comparing
     * percent >= threshold, since percent >= -1 is always true. */
    bool warn = volume_warn_threshold_percent >= 0 && percent >= volume_warn_threshold_percent;

    for (int i = 0; i < 3; i++) {
        if (warn) lv_obj_set_style_text_color(volume_topbar_digit[i], lv_color_make(255, 0, 0), 0);
        else lv_obj_remove_local_style_prop(volume_topbar_digit[i], LV_STYLE_TEXT_COLOR, 0);
    }
    reflow_topbar_layout();
    sync_bt_codec_status_icon();
}

/* Polled every timer tick alongside refresh_battery_topbar() -- like
 * battery.c's sysfs read, this is a single cheap fopen/fgets with no
 * subprocess fork, so it doesn't need wifi/bt's throttled polling. */
void refresh_headphone_icon(void) {
    bool connected = get_headphone_state() != HEADPHONE_STATE_NONE;
    bool was_hidden = lv_obj_has_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flag(volume_topbar_headphone, LV_OBJ_FLAG_HIDDEN, !connected);
    if (was_hidden == connected) {
        reflow_topbar_layout();
        sync_bt_codec_status_icon();
    }
}
void gui_shell_apply_topbar_settings(void) {
    if (!status_bar_band) return;
    uint32_t hidden = current_settings.topbar_hidden_elements & TOPBAR_HIDE_ALL;
    int position = topbar_clock_position();
    bool show_percent = current_settings.show_battery_percent;
    if (hidden == last_topbar_hidden_elements && position == last_topbar_clock_position &&
        show_percent == last_topbar_show_battery_percent) return;

    last_topbar_hidden_elements = hidden;
    last_topbar_clock_position = position;
    last_topbar_show_battery_percent = show_percent;
    battery_topbar_anchored_width = -1;
    invalidate_bt_codec_status_cache();
    apply_topbar_visibility_preferences();
    if (battery_topbar_group) refresh_battery_topbar();
    sync_topbar_status_icon_positions();
    quick_drawer_mark_snapshot_dirty();
    player_transition_mark_dirty();
    gui_navigation_invalidate_theme_snapshots();
}

/* Fully automatic, no Settings entry -- meant to feel like the wired
 * headphone jack (refresh_headphone_icon() above), not a mode the user
 * switches into (unlike Storage/USB DAC/ADB in the manual USB Mode
 * screen). usb_audio_output_is_connected() is a plain /proc file read (no
 * subprocess), same cheap class of check as get_headphone_state()'s own
 * direct sysfs reads, so this is safe to call directly on the UI thread at
 * the same low cadence as the wifi/Bluetooth polls (see their own
 * WIFI_POLL_TICKS call site) rather than needing its own background
 * thread. Toast fires only on the actual connect transition (was_connected
 * tracked across calls), matching how "Paused: headphones disconnected"
 * only fires once per real disconnect rather than every poll tick. */
void poll_usb_audio_output(void) {
    static bool was_connected = false;
    char alsa_device[32];
    /* When USB_MODE_DAC is active, the USB port operates in gadget mode rather
     * than host mode, so external host-accessory audio devices cannot be
     * connected. Bypassing detection while in USB_MODE_DAC prevents false
     * positives from redirecting audio output away from the DAC bridge. */
    bool connected = current_settings.usb_mode != USB_MODE_DAC &&
                      usb_audio_output_is_connected(alsa_device, sizeof(alsa_device));

    if (connected && !was_connected) show_error_toast(TR("USB audio device detected"));
    was_connected = connected;

    audio_set_usb_output(connected, alsa_device);
    bool was_hidden = lv_obj_has_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flag(usb_audio_status_icon, LV_OBJ_FLAG_HIDDEN, !connected);
    if (was_hidden == connected) {
        sync_bt_codec_status_icon();
    }
}
void refresh_clock_label(void) {
    struct tm tm_info;
    app_clock_localtime(&tm_info);
    char buf[8];
    /* %I (12h) zero-pads to 2 digits just like %H (24h) does -- "01".."12",
     * never a single digit -- so buf is always "HH:MM" (5 chars) either
     * way, mapping 1:1 onto the 5 fixed slots with no leading-slot-hiding
     * needed here, unlike the volume/battery readouts. */
    strftime(buf, sizeof(buf), current_settings.clock_24h ? "%H:%M" : "%I:%M", &tm_info);

    for (int i = 0; i < 5; i++) {
        char glyph[2] = { buf[i], '\0' };
        topbar_set_label(clock_topbar_digit[i], glyph);
    }

    if (current_settings.clock_24h) {
        lv_obj_add_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        topbar_set_label(clock_topbar_ampm, tm_info.tm_hour < 12 ? TR("AM") : TR("PM"));
        lv_obj_remove_flag(clock_topbar_ampm, LV_OBJ_FLAG_HIDDEN);
    }
    /* Widths can change with AM/PM and localized glyph metrics. */
    reflow_topbar_layout();
    sync_bt_codec_status_icon();
}

/* Split out of gui_shell_init() so gui_reload.c's in-process UI reload can
 * rebuild Home/the status bar/the quick drawer without also re-triggering
 * start_bt_dac_startup_reapply_if_needed()/start_refresh_bt_icon() (gui_shell_bluetooth.c) --
 * those are Bluetooth-adjacent startup side effects (the latter spawns a
 * background pthread with no idempotency guard) a reload must never repeat.
 * gui_shell_init() itself (gui_shell.c) still calls this first, then those two,
 * in the exact same order as before this split -- boot behavior unchanged. */
void topbar_load_volume_warn_threshold(void) {
    if (!device_config_get_volume_warn_threshold(&volume_warn_threshold_percent)) {
        volume_warn_threshold_percent = -1;
    }
}
void topbar_teardown(void) {
    if (status_bar_band) {
        lv_obj_delete(status_bar_band);
        status_bar_band = NULL;
    }
    clock_topbar_group = NULL;
    clock_topbar_ampm = NULL;
    for (int i = 0; i < 5; i++) clock_topbar_digit[i] = NULL;
    volume_topbar_group = NULL;
    volume_topbar_icon = NULL;
    for (int i = 0; i < 3; i++) volume_topbar_digit[i] = NULL;
    volume_topbar_headphone = NULL;
    battery_topbar_group = NULL;
    battery_icon_frame = NULL;
    for (int i = 0; i < 3; i++) battery_topbar_digit[i] = NULL;
    wifi_icon = NULL;
    bt_status_icon = NULL;
    a2dp_status_icon = NULL;
    usb_audio_status_icon = NULL;
    play_pause_status_icon = NULL;
    bt_codec_status_icon = NULL;
    topbar_layout_teardown();
    battery_topbar_anchored_width = -1;
    last_topbar_hidden_elements = UINT32_MAX;
    last_topbar_clock_position = -1;
    last_topbar_show_battery_percent = false;
}
