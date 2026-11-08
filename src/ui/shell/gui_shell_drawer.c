#include "gui_shell_internal.h"

lv_obj_t * quick_drawer_wifi_icon = NULL;
lv_obj_t * quick_drawer_bt_icon = NULL;
lv_obj_t * quick_drawer = NULL;
lv_obj_t * quick_drawer_brightness_icon = NULL;
asset_decoded_image_t quick_drawer_brightness_image;
bool quick_drawer_customization_reflow_pending = false;
lv_obj_t * quick_drawer_brightness_track = NULL;
lv_obj_t * quick_drawer_brightness_label = NULL;
lv_timer_t * brightness_hw_apply_timer = NULL;
static int brightness_hw_pending = -1;
static bool brightness_drag_active = false;
void refresh_quick_drawer_brightness(void) {
    if (!quick_drawer_brightness_track) return;
    int brightness = backlight_get_percent();
    if (brightness < 0) brightness = current_settings.brightness_percent;
    lv_slider_set_value(quick_drawer_brightness_track, brightness, LV_ANIM_OFF);
    if (quick_drawer_brightness_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", brightness);
        lv_label_set_text(quick_drawer_brightness_label, buf);
    }
}
lv_obj_t * quick_drawer_title_label = NULL;
lv_obj_t * quick_drawer_artist_label = NULL;
lv_obj_t * quick_drawer_album_label = NULL;
lv_obj_t * quick_drawer_format_label = NULL;
lv_obj_t * quick_drawer_cover_img = NULL;
lv_obj_t * quick_drawer_cover_frame = NULL;
lv_obj_t * quick_drawer_play_btn = NULL;
lv_obj_t * quick_drawer_volume_container = NULL;
#if defined(BOARD_TEMPOTEC_V1)
lv_obj_t * quick_drawer_brightness_container = NULL;
#endif
lv_obj_t * quick_drawer_volume_icon = NULL;
lv_obj_t * quick_drawer_eq_button = NULL;
lv_obj_t * quick_drawer_volume_track = NULL;
lv_obj_t * quick_drawer_volume_label = NULL;
/* Shared with gui_player.c's own volume popup via hw_volume_coalesce.h --
 * see that header's own comment for why this replaced a second, simpler,
 * worse copy of the same debounce (this used to call audio_request_
 * volume() directly and untethered on every drag tick, which is what made
 * this slider feel sluggish). Own instance: only one slider can be
 * mid-drag at a time (single touch), but there is no reason to share the
 * popup's own timer/pending state, only this code. */
static hw_volume_coalesce_t quick_drawer_volume_hv = { .pending = -1 };
lv_obj_t * quick_drawer_toggle_state[QUICK_DRAWER_TOGGLE_SLOTS];
/* Stable references let customization move a complete toggle without
 * rebuilding widgets or changing their event callbacks. Native IDs match qd_toggle_t; plugin IDs follow QD_TOGGLE_COUNT. */
lv_obj_t * quick_drawer_toggle_icon[QUICK_DRAWER_TOGGLE_SLOTS];
lv_obj_t * quick_drawer_toggle_name[QUICK_DRAWER_TOGGLE_SLOTS];
#if defined(BOARD_TEMPOTEC_V1)
#else
const int quick_drawer_column_label_x[] = { 39, 143, 251, 358 };
#endif
void quick_drawer_size_toggle_caption(lv_obj_t * label) {
#if defined(BOARD_TEMPOTEC_V1)
    /* Keep the four primary controls on one caption line; long translations
     * scroll within their own cell instead of colliding with the state row. */
    lv_obj_set_style_text_letter_space(label, -1, 0);
    lv_obj_set_height(label, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)));
    row_label_enable_marquee(label);
#else
    (void) label;
#endif
}
#if defined(BOARD_TEMPOTEC_V1)
static void quick_drawer_pack_primary_captions(void) {
    enum { MAX_LABELS = QUICK_DRAWER_TOGGLE_SLOTS,
           LEFT = 10, RIGHT = 230, GAP = 4, FALLBACK_W = 51 };
    lv_obj_t * labels[MAX_LABELS];
    lv_obj_t * icons[MAX_LABELS];
    int32_t widths[MAX_LABELS];
    int32_t xs[MAX_LABELS];
    int count = 0;
    int32_t total = 0;

    for (int i = 0; i < MAX_LABELS; ++i) {
        lv_obj_t * label = quick_drawer_toggle_name[i];
        lv_obj_t * icon = quick_drawer_toggle_icon[i];
        if (!label || !icon || lv_obj_get_parent(label) != quick_drawer ||
            lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) continue;
        labels[count] = label;
        icons[count] = icon;
        count++;
    }
    /* Custom ordering can place a plugin, Car Mode, or Crossfade in the
     * first row. Keep captions paired with their actual icon columns. */
    for (int i = 1; i < count; ++i) {
        lv_obj_t * label = labels[i];
        lv_obj_t * icon = icons[i];
        int32_t icon_x = lv_obj_get_x(icon);
        int j = i;
        while (j > 0 && lv_obj_get_x(icons[j - 1]) > icon_x) {
            labels[j] = labels[j - 1];
            icons[j] = icons[j - 1];
            j--;
        }
        labels[j] = label;
        icons[j] = icon;
    }

    count = count > 4 ? 4 : count;
    for (int i = 0; i < count; ++i) {
        lv_obj_t * label = labels[i];
        lv_obj_t * icon = icons[i];
        const lv_font_t * font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        lv_point_t natural = { .x = 0, .y = 0 };
        const char * text = lv_label_get_text(label);
        lv_text_get_size(&natural, text, font,
                         lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                         0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t width = natural.x;
        if (width < 1) width = 1;
        if (width > QUICK_DRAWER_TOGGLE_CELL_W) width = QUICK_DRAWER_TOGGLE_CELL_W;
        widths[i] = width;
        xs[i] = lv_obj_get_x(icon) + lv_obj_get_width(icon) / 2 - width / 2;
        total += width;
    }
    if (count < 2) return;

    if (total + GAP * (count - 1) > RIGHT - LEFT) {
        total = count * FALLBACK_W;
        for (int i = 0; i < count; ++i) widths[i] = FALLBACK_W;
        int32_t gap = count > 1 ? (RIGHT - LEFT - total) / (count - 1) : 0;
        for (int i = 0; i < count; ++i) xs[i] = LEFT + i * (FALLBACK_W + gap);
    } else {
        for (int i = 0; i < count; ++i) {
            if (xs[i] < LEFT) xs[i] = LEFT;
            if (i > 0 && xs[i] < xs[i - 1] + widths[i - 1] + GAP)
                xs[i] = xs[i - 1] + widths[i - 1] + GAP;
        }
        if (xs[count - 1] + widths[count - 1] > RIGHT) {
            xs[count - 1] = RIGHT - widths[count - 1];
            for (int i = count - 2; i >= 0; --i) {
                int32_t latest = xs[i + 1] - GAP - widths[i];
                if (xs[i] > latest) xs[i] = latest;
            }
            if (xs[0] < LEFT) {
                total = count * FALLBACK_W;
                for (int i = 0; i < count; ++i) {
                    widths[i] = FALLBACK_W;
                    xs[i] = LEFT + i * (FALLBACK_W + GAP);
                }
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        lv_obj_set_width(labels[i], widths[i]);
        lv_obj_set_x(labels[i], xs[i]);
        row_label_enable_marquee(labels[i]);
    }
}
#endif
lv_obj_t * quick_drawer_expansion_box = NULL;
lv_obj_t * quick_drawer_expansion_handle = NULL;
lv_obj_t * quick_drawer_card = NULL;
lv_obj_t * quick_drawer_airplay_icon = NULL;
lv_obj_t * quick_drawer_bt_dac_icon = NULL;
lv_obj_t * quick_drawer_dlna_icon = NULL;
lv_obj_t * quick_drawer_gapless_icon = NULL;
lv_obj_t * quick_drawer_rc_icon = NULL;
lv_obj_t * quick_drawer_car_mode_icon = NULL;
lv_obj_t * quick_drawer_plugin_icon[PLUGIN_MAX_QUICK_TOGGLES];
int quick_drawer_plugin_toggle_count = 0;
void quick_drawer_volume_event_cb(lv_event_t * e) {
    if (!ui_customization_drawer_volume_visible()) return;
    lv_event_code_t code = lv_event_get_code(e);
    int percent = (int) lv_slider_get_value(lv_event_get_target(e));
    if (code == LV_EVENT_PRESSED) {
        hw_volume_coalesce_drag_begin(&quick_drawer_volume_hv);
        return;
    }
    if (code == LV_EVENT_VALUE_CHANGED) {
        percent = audio_clamp_volume_percent(percent);
        if (percent != lv_slider_get_value(lv_event_get_target(e)))
            lv_slider_set_value(lv_event_get_target(e), percent, LV_ANIM_OFF);
        hw_volume_coalesce_drag_update(&quick_drawer_volume_hv, percent);
        if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
        refresh_volume_topbar(percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        percent = audio_clamp_volume_percent(percent);
        if (percent != lv_slider_get_value(lv_event_get_target(e)))
            lv_slider_set_value(lv_event_get_target(e), percent, LV_ANIM_OFF);
        percent = hw_volume_coalesce_drag_end(&quick_drawer_volume_hv, percent);
        /* Authoritative settled value -- sync the Player screen's own
         * volume_slider mirror now, same as the popup's own release
         * handler does, instead of on every drag tick (its previous spot)
         * -- that also fired a cross-file lv_slider_set_value() call on
         * every tick, one of the other costs stacking on top of the
         * un-throttled audio call to make this feel sluggish. */
        gui_player_set_volume_percent(percent);
        gui_player_remember_volume_percent(percent);
        /* Async, not sync, matching this same file's own
         * quick_drawer_brightness_changed_cb() release branch -- a fast
         * release must not block the UI thread on a synchronous file
         * write. */
        settings_save_async(&current_settings);
        quick_drawer_mark_snapshot_dirty();
    }
}
void quick_drawer_refresh_volume(void) {
    if (!quick_drawer_volume_track || quick_drawer_volume_hv.drag_active) return;
    int percent = (int) gui_player_get_volume_percent();
    lv_slider_set_value(quick_drawer_volume_track, percent, LV_ANIM_OFF);
    if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
}

/* `text` overrides the usual "On"/"Off" caption -- a plugin tile can publish
 * its own pair (GainMode's "High"/"Low"). NULL keeps the default. */
void quick_drawer_set_toggle_state_text(int index, bool enabled, const char * text) {
    if (index < 0 || index >= QUICK_DRAWER_TOGGLE_SLOTS) return;
    if (!quick_drawer_toggle_state[index]) return;
    const char * caption = text ? text : (enabled ? TR("On") : TR("Off"));
    lv_color_t color = enabled ? shell_drawer_on_color() : shell_drawer_off_color;
    if (ui_label_text_matches(quick_drawer_toggle_state[index], caption) &&
        lv_color_eq(lv_obj_get_style_text_color(quick_drawer_toggle_state[index], 0), color)) return;
    lv_label_set_text(quick_drawer_toggle_state[index], caption);
    lv_obj_set_style_text_color(quick_drawer_toggle_state[index], color, 0);
    quick_drawer_mark_snapshot_dirty();
}
void quick_drawer_set_toggle_state(int index, bool enabled) {
    quick_drawer_set_toggle_state_text(index, enabled, NULL);
}
void quick_drawer_fit_cover(void) {
    if (!quick_drawer_cover_img || !quick_drawer_cover_frame) return;
    lv_image_header_t header;
    if (lv_image_decoder_get_info(lv_image_get_src(quick_drawer_cover_img), &header) != LV_RESULT_OK ||
        header.w <= 0 || header.h <= 0) return;
    int32_t frame_w = lv_obj_get_width(quick_drawer_cover_frame);
    int32_t frame_h = lv_obj_get_height(quick_drawer_cover_frame);
    if (frame_w <= 0 || frame_h <= 0) {
        frame_w = QUICK_DRAWER_CARD_W;
        frame_h = QUICK_DRAWER_CARD_H;
    }
    lv_obj_set_size(quick_drawer_cover_img, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_image_set_inner_align(quick_drawer_cover_img, LV_IMAGE_ALIGN_DEFAULT);
    if (header.w >= frame_w && header.h >= frame_h) {
        /* Already covers the frame, so crop rather than rescale: the frosted
         * source is dithered for its own pixel grid and resampling it bands. */
        lv_image_set_scale(quick_drawer_cover_img, LV_SCALE_NONE);
    } else {
        uint32_t sx = ((uint32_t) frame_w * LV_SCALE_NONE + header.w - 1U) / header.w;
        uint32_t sy = ((uint32_t) frame_h * LV_SCALE_NONE + header.h - 1U) / header.h;
        lv_image_set_scale(quick_drawer_cover_img, sx > sy ? sx : sy);
    }
    lv_obj_update_layout(quick_drawer_cover_img);
    lv_obj_align(quick_drawer_cover_img, LV_ALIGN_CENTER, 0, 0);
}

/* Captions carry their state color locally; move the "Off" ones over. */
void quick_drawer_retint_toggle_states(lv_color_t old_off) {
    for (int i = 0; i < QUICK_DRAWER_TOGGLE_SLOTS; i++) {
        lv_obj_t * state = quick_drawer_toggle_state[i];
        if (!state) continue;
        bool off = lv_color_eq(lv_obj_get_style_text_color(state, 0), old_off);
        lv_obj_set_style_text_color(state, off ? shell_drawer_off_color : shell_drawer_on_color(), 0);
    }
}

/* Keeps the drawer's volume slider and percentage in step with the volume. */
void quick_drawer_sync_volume(int32_t percent) {
    if (quick_drawer_volume_track && !quick_drawer_volume_hv.drag_active) {
        lv_slider_set_value(quick_drawer_volume_track, percent, LV_ANIM_OFF);
        if (quick_drawer_volume_label) lv_label_set_text_fmt(quick_drawer_volume_label, "%d", percent);
    }
}
static const char * const qd_toggle_on_asset[QD_TOGGLE_COUNT] = {
    "pull_down/wifi_s.png",
    "pull_down/bt_s.png",
    "pull_down/sleep_switch_s.png",
    "pull_down/fade_s.png",
    "pull_down/airplay_s.png",
    "pull_down/bt_dac_s.png",
    "pull_down/dlna_s.png",
    "pull_down/gapless_play_s.png",
    "pull_down/hibylink_s.png",
    "pull_down/car_mode_s.png",
};
static const char * const qd_toggle_off_asset[QD_TOGGLE_COUNT] = {
    "pull_down/wifi.png",
    "pull_down/bt.png",
    "pull_down/sleep_switch.png",
    "pull_down/fade.png",
    "pull_down/airplay.png",
    "pull_down/bt_dac.png",
    "pull_down/dlna.png",
    "pull_down/gapless_play.png",
    "pull_down/hibylink.png",
    "pull_down/car_mode.png",
};
static asset_decoded_image_t qd_toggle_on_img[QD_TOGGLE_COUNT];
/* Decoded only when the off artwork is square (see toggle_corners_opaque()). */
static asset_decoded_image_t qd_toggle_off_img[QD_TOGGLE_COUNT];
static bool quick_drawer_toggle_force_src;

/* Row 3's tiles come from plugin.register_quick_toggle() rather than the
 * fixed table above, so their "on" artwork is decoded per registered toggle
 * (same accent recolor, just a runtime icon path). */
static asset_decoded_image_t qd_plugin_toggle_on_img[PLUGIN_MAX_QUICK_TOGGLES];
static asset_decoded_image_t qd_plugin_toggle_off_img[PLUGIN_MAX_QUICK_TOGGLES];

/* Every toggle tile is round, but some artwork is drawn on an opaque square:
 * the stock theme1 AirPlay, DLNA, Gapless, HibyLink, MSEB and Gain pairs,
 * which the light themes select. Round artwork has transparent corners. */
static bool toggle_corners_opaque(const lv_draw_buf_t * buf) {
    if (!buf || !buf->data || buf->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
        buf->header.w < 8 || buf->header.h < 8) return false;
    uint32_t w = buf->header.w, h = buf->header.h, k = w / 12;
    const uint32_t xs[2] = { k, w - 1 - k }, ys[2] = { k, h - 1 - k };
    for (int i = 0; i < 4; i++) {
        const lv_color32_t * row = (const lv_color32_t *) (buf->data + ys[i / 2] * buf->header.stride);
        if (row[xs[i % 2]].alpha >= 16) return true;
    }
    return false;
}

/* Clip square artwork to a circle with a one-pixel antialiased edge, one
 * pixel inside the inscribed circle: the stock disc's own edge pixels are
 * blended with the square's corners and would show as a dark ring. */
static void mask_toggle_circle(lv_draw_buf_t * buf) {
    if (!toggle_corners_opaque(buf)) return;
    uint32_t w = buf->header.w, h = buf->header.h;
    float cx = w / 2.0f, cy = h / 2.0f, r = (w < h ? w : h) / 2.0f - 1.0f;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * buf->header.stride);
        float dy = y + 0.5f - cy;
        for (uint32_t x = 0; x < w; x++) {
            float dx = x + 0.5f - cx;
            float coverage = r - sqrtf(dx * dx + dy * dy) + 0.5f;
            if (coverage >= 1.0f) continue;
            row[x].alpha = coverage <= 0.0f ? 0 : (uint8_t) (row[x].alpha * coverage);
        }
    }
}

/* Off artwork stays a plain asset path unless it needs rounding. */
static void load_toggle_off_image(asset_decoded_image_t * image, const char * relative_path) {
    asset_decoded_image_close(image);
    if (!relative_path || !asset_decoded_image_open(image, relative_path)) return;
    lv_draw_buf_t * buf = (lv_draw_buf_t *) image->decoder.decoded;
    if (!toggle_corners_opaque(buf)) {
        asset_decoded_image_close(image);
        return;
    }
    mask_toggle_circle(buf);
}
static void recolor_toggle_on_accent(lv_draw_buf_t * buf, lv_color_t accent) {
    if (!buf || !buf->data || !buf->header.w || !buf->header.h ||
        buf->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha == 0) continue; /* outside the circle -- leave transparent */
            uint8_t t = row[x].red;
            if (row[x].green < t) t = row[x].green;
            if (row[x].blue < t) t = row[x].blue;
            row[x].red   = (uint8_t) (accent.red   + ((uint16_t) (255 - accent.red)   * t) / 255);
            row[x].green = (uint8_t) (accent.green + ((uint16_t) (255 - accent.green) * t) / 255);
            row[x].blue  = (uint8_t) (accent.blue  + ((uint16_t) (255 - accent.blue)  * t) / 255);
        }
    }
}
static void prepare_toggle_on_image(asset_decoded_image_t * image, const char * relative_path,
                                    lv_color_t accent) {
    if (!asset_decoded_image_prepare_retint(image, relative_path)) return;
    lv_draw_buf_t * buf = (lv_draw_buf_t *) image->decoder.decoded;
    /* Round first: the recolor below would paint opaque corners in the accent. */
    mask_toggle_circle(buf);
    recolor_toggle_on_accent(buf, accent);
}
static void close_quick_drawer_toggle_images(void) {
    for (int i = 0; i < QD_TOGGLE_COUNT; i++) {
        asset_decoded_image_close(&qd_toggle_on_img[i]);
        asset_decoded_image_close(&qd_toggle_off_img[i]);
    }
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) {
        asset_decoded_image_close(&qd_plugin_toggle_on_img[i]);
        asset_decoded_image_close(&qd_plugin_toggle_off_img[i]);
    }
}
void load_quick_drawer_toggle_images(void) {
    lv_color_t accent = accent_lv_color();
    for (int i = 0; i < QD_TOGGLE_COUNT; i++) {
        prepare_toggle_on_image(&qd_toggle_on_img[i], qd_toggle_on_asset[i], accent);
        load_toggle_off_image(&qd_toggle_off_img[i], qd_toggle_off_asset[i]);
    }
    int plugin_count = plugin_manager_get_quick_toggle_count();
    if (plugin_count > PLUGIN_MAX_QUICK_TOGGLES) plugin_count = PLUGIN_MAX_QUICK_TOGGLES;
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) {
        if (i >= plugin_count) {
            asset_decoded_image_close(&qd_plugin_toggle_on_img[i]);
            asset_decoded_image_close(&qd_plugin_toggle_off_img[i]);
            continue;
        }
        prepare_toggle_on_image(&qd_plugin_toggle_on_img[i],
                                plugin_manager_get_quick_toggle_icon_selected(i), accent);
        load_toggle_off_image(&qd_plugin_toggle_off_img[i], plugin_manager_get_quick_toggle_icon(i));
    }
}

/* The on state is always a decoded, rounded, accent-coloured copy; the off
 * state only when its artwork needed rounding. Either falls back to the plain
 * asset path if no decoded copy exists. */
const void * quick_drawer_toggle_src(qd_toggle_t t, bool on) {
    const void * src = asset_decoded_image_source(on ? &qd_toggle_on_img[t] : &qd_toggle_off_img[t]);
    return src ? src : (const void *) asset_path(on ? qd_toggle_on_asset[t] : qd_toggle_off_asset[t]);
}

/* quick_drawer_toggle_src()'s twin for a row-3 plugin tile, whose two icon
 * paths come from the registration rather than a compile-time table. */
const void * quick_drawer_plugin_toggle_src(int index, bool on) {
    const void * src = NULL;
    if (index >= 0 && index < PLUGIN_MAX_QUICK_TOGGLES)
        src = asset_decoded_image_source(on ? &qd_plugin_toggle_on_img[index] : &qd_plugin_toggle_off_img[index]);
    if (src) return src;
    return asset_path(on ? plugin_manager_get_quick_toggle_icon_selected(index)
                         : plugin_manager_get_quick_toggle_icon(index));
}

/* Toggle sources include 84px stock artwork plus arbitrary plugin assets.
 * Keep every toggle in a fixed width-scaled square and let LVGL contain the
 * actual source, so source swaps never change the icon footprint or touch
 * bounds. The 56px R3II target remains above the 44px minimum hit size. */

bool quick_drawer_set_toggle_image(lv_obj_t * image, const void * source) {
    if (!image) return false;
    bool changed = false;
    if (quick_drawer_toggle_force_src ||
        !image_file_source_matches(lv_image_get_src(image), source)) {
        lv_image_set_src(image, source);
        changed = true;
    }
    if (lv_image_get_inner_align(image) != LV_IMAGE_ALIGN_CONTAIN) {
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
        changed = true;
    }
    if (lv_obj_get_width(image) != QUICK_DRAWER_TOGGLE_ICON_PX ||
        lv_obj_get_height(image) != QUICK_DRAWER_TOGGLE_ICON_PX) {
        lv_obj_set_size(image, QUICK_DRAWER_TOGGLE_ICON_PX, QUICK_DRAWER_TOGGLE_ICON_PX);
        changed = true;
    }
    return changed;
}

/* Applies the on/off artwork of toggle `id`; returns whether anything changed.
 * Captions and snapshot invalidation stay with the caller. */
bool quick_drawer_apply_toggle_image(qd_toggle_t id, lv_obj_t * icon, bool on) {
    return quick_drawer_set_toggle_image(icon, quick_drawer_toggle_src(id, on));
}
void quick_drawer_layout_transport_icon(lv_obj_t * image, int32_t reference_x) {
    int32_t icon_px = BOARD_SCALE_PX(40);
#if defined(BOARD_TEMPOTEC_V1)
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_scale(image, LV_SCALE_NONE);
#else
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
    lv_image_set_scale(image, (icon_px * LV_SCALE_NONE + 20) / 40);
#endif
    lv_obj_set_size(image, icon_px, icon_px);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_pos(image, BOARD_SCALE_PX(reference_x),
                   (BOARD_SCALE_PY(90) - icon_px) / 2);
#else
    lv_obj_set_pos(image, BOARD_SCALE_PX(reference_x), BOARD_SCALE_PY(43) - icon_px / 2);
#endif
}

/* Re-reads every expanded-row toggle from its authoritative source. Called
 * when the drawer opens rather than pushed to on change: three of these are
 * network services whose real state can move underneath us (a Wi-Fi drop
 * disables all of them via gui_network_handle_wifi_disabled()), and the
 * plugin tiles are documented as read-on-open. */
void gui_shell_refresh_quick_drawer_expansion_toggles(void) {
    if (!quick_drawer_expansion_box) return;

    const struct {
        qd_toggle_t id;
        lv_obj_t * icon;
        bool on;
    } toggles[] = {
        { QD_TOGGLE_AIRPLAY, quick_drawer_airplay_icon, current_settings.wifi_dac_mode_enabled },
        { QD_TOGGLE_BT_DAC, quick_drawer_bt_dac_icon, current_settings.bt_dac_mode_enabled },
        { QD_TOGGLE_DLNA, quick_drawer_dlna_icon, current_settings.dlna_renderer_enabled },
        { QD_TOGGLE_GAPLESS, quick_drawer_gapless_icon, current_settings.gapless_enabled },
        { QD_TOGGLE_RC, quick_drawer_rc_icon, current_settings.remote_control_enabled },
        { QD_TOGGLE_CAR_MODE, quick_drawer_car_mode_icon, current_settings.car_mode_enabled },
    };
    for (unsigned i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
        quick_drawer_apply_toggle_image(toggles[i].id, toggles[i].icon, toggles[i].on);
        quick_drawer_set_toggle_state(toggles[i].id, toggles[i].on);
    }

    for (int i = 0; i < quick_drawer_plugin_toggle_count; i++) {
        bool on = plugin_manager_get_quick_toggle_value(i);
        if (quick_drawer_plugin_icon[i])
            quick_drawer_set_toggle_image(quick_drawer_plugin_icon[i], quick_drawer_plugin_toggle_src(i, on));
        quick_drawer_set_toggle_state_text(QD_TOGGLE_COUNT + i, on,
                                           plugin_manager_get_quick_toggle_state_text(i, on));
    }
}

/* Hides the drawer instantly (no slide-out) before navigating to a screen
 * that will cover the overlay. */
void quick_drawer_dismiss_for_navigation(void) {
    quick_drawer_open = false;
    quick_drawer_finish_bitmap_motion();
}

/* Neutral-styled sibling of show_error_toast() -- that one's red color
 * scheme and short 2.5s/400x70 sizing fit a brief failure message, not a
 * longer explanatory one (first use: Car Mode's own explanation on
 * enabling). Bigger box for wrapping, 5s so there's time to actually read
 * it, no error coloring since nothing failed. */
/* info_toast moved to gui_notifications.c */



/* Quick-access drawer (Android-style notification-shade convention): slides
 * down over the whole screen from a swipe-down starting near the status
 * bar. Real pull_down/ theme2 assets throughout. Every row-1 icon (Wifi/
 * Bluetooth, mirroring the same wifi_status.c/bluetooth_control.c state as
 * the main status bar; crossfade; sleep timer) and the now-playing card
 * (real playback state, reusing the exact same callbacks as the player
 * screen's own transport buttons) are backed by real functionality.
 * QUICK_DRAWER_ANIM_MS is defined in gui_navigation.h; TRIGGER_ZONE is
 * defined in gui_navigation.c, for the swipe-down-near-the-top-edge check. */

/* Crossfade toggle control. Synchronized bidirectionally with Settings > Crossfade
 * (refresh_quick_drawer_crossfade_icon() / gui_settings_sync_crossfade_toggle()).
 * Uses pull_down/fade.png and pull_down/fade_s.png. */
lv_obj_t * quick_drawer_crossfade_icon;
void refresh_quick_drawer_crossfade_icon(void) {
    if (!quick_drawer_crossfade_icon) return;
    quick_drawer_apply_toggle_image(QD_TOGGLE_CROSSFADE, quick_drawer_crossfade_icon, current_settings.crossfade_enabled);
    quick_drawer_set_toggle_state(3, current_settings.crossfade_enabled);
    quick_drawer_mark_snapshot_dirty();
}

/* Settings > Music Settings > Playback's Crossfade toggle row is kept in sync
 * with quick drawer toggles via gui_settings_sync_crossfade_toggle(). */

void quick_drawer_crossfade_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    /* Shared with the Settings switch so the gapless implication is applied
     * identically from both -- see gui_player_set_crossfade_enabled(). */
    gui_player_set_crossfade_enabled(!current_settings.crossfade_enabled);
    gui_settings_sync_crossfade_toggle();
}


/* Sleep timer: arms/disarms countdown from current_settings.sleep_timer_minutes.
 * Arming state is session-only and not persisted across restarts. */
bool sleep_timer_active = false;
static uint32_t sleep_timer_start_tick = 0;
lv_obj_t * quick_drawer_sleep_icon;
/* Shared by the drawer icon's own click handler and the Settings > Sleep
 * Timer toggle (quick_drawer_sleep_timer_set_active(), gui_settings.c) --
 * both need the exact same icon/tick bookkeeping, and both then call
 * gui_settings_sync_sleep_timer_toggle() themselves afterward to keep the
 * OTHER one's widget in sync (same bidirectional pattern as crossfade --
 * see refresh_quick_drawer_crossfade_icon()'s own comment). poll_sleep_
 * timer()'s own expiry path below calls this after pausing playback and
 * then calls that same sync. */
static void apply_sleep_timer_active(bool active) {
    sleep_timer_active = active;
    quick_drawer_set_toggle_state(2, sleep_timer_active);
    if (sleep_timer_active) {
        sleep_timer_start_tick = lv_tick_get();
        quick_drawer_apply_toggle_image(QD_TOGGLE_SLEEP, quick_drawer_sleep_icon, true);
    } else {
        quick_drawer_set_toggle_image(quick_drawer_sleep_icon, asset_path("pull_down/sleep_switch.png"));
    }
    quick_drawer_mark_snapshot_dirty();
}

/* Milliseconds left while armed; 0 when not armed or already due. */
static uint32_t sleep_timer_remaining_ms(void) {
    if (!sleep_timer_active) return 0;
    uint32_t total_ms = (uint32_t) current_settings.sleep_timer_minutes * 60000;
    uint32_t elapsed_ms = lv_tick_elaps(sleep_timer_start_tick);
    return elapsed_ms >= total_ms ? 0 : total_ms - elapsed_ms;
}
bool quick_drawer_sleep_timer_is_active(void) {
    return sleep_timer_active;
}

/* For Settings > Sleep Timer's "Show Time Remaining" button (build_sleep_
 * timer_screen(), gui_settings.c) -- same
 * remaining time poll_sleep_timer() below acts on. 0 when not armed, never negative. */
int quick_drawer_sleep_timer_remaining_seconds(void) {
    return (int) (sleep_timer_remaining_ms() / 1000);
}

/* Settings > Sleep Timer enable toggle synchronization. */
void quick_drawer_sleep_timer_set_active(bool active) {
    apply_sleep_timer_active(active);
}
void quick_drawer_sleep_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
    apply_sleep_timer_active(!sleep_timer_active);
    gui_settings_sync_sleep_timer_toggle();
}

/* Called every update_timer_cb tick (500ms). Cheap no-op when not armed. */
void poll_sleep_timer(void) {
    if (!sleep_timer_active) return;

    if (sleep_timer_remaining_ms() == 0) {
        sleep_timer_active = false;
        if (audio_is_playing()) toggle_play_pause(); /* pause, not stop -- resumable, same as any other pause */
        apply_sleep_timer_active(false);
        gui_settings_sync_sleep_timer_toggle(); /* Settings' toggle must not keep showing armed once expiry disarmed it */
        gui_sleep_timer_expired();
    }
}
void gui_shell_refresh_quick_drawer_volume_visibility(void) {
#if defined(BOARD_TEMPOTEC_V1)
    bool collapsed = quick_drawer_expansion_y == 0;
    lv_obj_t * brightness_row[] = { quick_drawer_brightness_container, quick_drawer_brightness_icon,
                                    quick_drawer_brightness_label, quick_drawer_brightness_track };
    for (size_t i = 0; i < sizeof(brightness_row) / sizeof(brightness_row[0]); i++) {
        if (!brightness_row[i]) continue;
        lv_obj_set_flag(brightness_row[i], LV_OBJ_FLAG_HIDDEN, !collapsed);
    }
#endif
    lv_obj_t * row[] = { quick_drawer_volume_container, quick_drawer_volume_icon, quick_drawer_volume_track,
                         quick_drawer_volume_label };
    for (size_t i = 0; i < sizeof(row) / sizeof(row[0]); i++) {
        if (!row[i]) continue;
#if defined(BOARD_TEMPOTEC_V1)
        if (ui_customization_drawer_volume_visible() && quick_drawer_expansion_y == 0)
#else
        if (ui_customization_drawer_volume_visible())
#endif
            lv_obj_remove_flag(row[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(row[i], LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_mark_snapshot_dirty();
}
static void brightness_hw_apply_pending(void) {
    int pending = brightness_hw_pending;
    if (pending < 0) return;
    brightness_hw_pending = -1;
    backlight_request_normal_percent(pending);
}
void brightness_hw_apply_timer_cb(lv_timer_t * timer) {
    (void) timer;
    brightness_hw_apply_pending();
    if (brightness_hw_apply_timer && !brightness_drag_active && brightness_hw_pending < 0)
        lv_timer_pause(brightness_hw_apply_timer);
}
void quick_drawer_brightness_changed_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = (lv_obj_t *) lv_event_get_target(e);
    int32_t percent = lv_slider_get_value(slider);

    if (code == LV_EVENT_PRESSED) {
        brightness_drag_active = true;
        if (brightness_hw_apply_timer) {
            lv_timer_reset(brightness_hw_apply_timer);
            lv_timer_resume(brightness_hw_apply_timer);
        }
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        brightness_hw_pending = (int) percent;
        if (quick_drawer_brightness_label)
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d", (int) percent);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        brightness_drag_active = false;
        brightness_hw_pending = (int) percent;
        brightness_hw_apply_pending();
        if (brightness_hw_apply_timer) lv_timer_pause(brightness_hw_apply_timer);
        if (quick_drawer_brightness_label)
            lv_label_set_text_fmt(quick_drawer_brightness_label, "%d", (int) percent);
        current_settings.brightness_percent = (int) percent;
        settings_save_async(&current_settings);
        quick_drawer_mark_snapshot_dirty(); /* one rebuild, now that the label has settled at its final value */
    }
}

/* Builds the caption and state labels of an expanded-row toggle at row_y
 * inside the expansion box. State text and color are left to the caller. */
void quick_drawer_make_toggle_labels(int slot, int32_t x, int32_t row_y,
                                            const char * name_text) {
    lv_obj_t * name = lv_label_create(quick_drawer_expansion_box);
    quick_drawer_toggle_name[slot] = name;
    lv_label_set_text(name, name_text);
    lv_obj_add_style(name, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(name, fallback_font_ui_12(), 0);
    lv_obj_set_width(name, QUICK_DRAWER_TOGGLE_CELL_W);
    quick_drawer_size_toggle_caption(name);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_height(name, 2 * lv_font_get_line_height(&lv_font_montserrat_12));
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
#endif
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(name, x, row_y + QUICK_DRAWER_TOGGLE_ICON_PX + QUICK_DRAWER_TOGGLE_CAPTION_GAP);

    lv_obj_t * state = lv_label_create(quick_drawer_expansion_box);
    quick_drawer_toggle_state[slot] = state;
    lv_obj_set_width(state, QUICK_DRAWER_TOGGLE_CELL_W);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_height(state, lv_font_get_line_height(&lv_font_montserrat_12));
    lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
#endif
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(state, fallback_font_ui_12(), 0);
    lv_obj_set_pos(state, x,
#if defined(BOARD_TEMPOTEC_V1)
                   row_y + QUICK_DRAWER_TOGGLE_ICON_PX + BOARD_SCALE_PY(5) +
                       2 * lv_font_get_line_height(&lv_font_montserrat_12) + BOARD_SCALE_PY(5));
#else
                   quick_drawer_toggle_state_y(row_y));
#endif
}
typedef struct {
    lv_obj_t * icon;
    lv_obj_t * name;
    lv_obj_t * state;
    int id;
} quick_drawer_layout_item_t;
bool quick_drawer_customization_enabled(void) {
    return ui_customization_effective().drawer_enabled;
}
static void quick_drawer_place_widget(lv_obj_t * obj, lv_obj_t * parent,
                                      int32_t x, int32_t y, bool visible) {
    if (!obj) return;
    if (lv_obj_get_parent(obj) != parent) lv_obj_set_parent(obj, parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_flag(obj, LV_OBJ_FLAG_HIDDEN, !visible);
}

/* Repositions existing objects in semantic order. The first row remains
 * visible when the drawer is collapsed; two revealed rows hold four more each. */
_Static_assert(UI_DRAWER_COUNT + PLUGIN_MAX_QUICK_TOGGLES <= UI_ORDER_MAX,
               "Quick Drawer order cannot hold every plugin toggle");
void quick_drawer_reflow_customization(void) {
    if (!quick_drawer || !quick_drawer_expansion_box) return;

    ui_customization_t customization = ui_customization_effective();
    quick_drawer_reset_expansion();

    quick_drawer_layout_item_t items[QUICK_DRAWER_TOGGLE_SLOTS];
    int count = 0;
    for (unsigned i = 0; i < customization.drawer.count && i < UI_ORDER_MAX && count < UI_DRAWER_MAX_TILES; ++i) {
        unsigned id = customization.drawer.ids[i];
        if (id < QD_TOGGLE_COUNT) {
            if (quick_drawer_toggle_icon[id] && quick_drawer_toggle_name[id] &&
                quick_drawer_toggle_state[id]) {
                items[count++] = (quick_drawer_layout_item_t){
                    quick_drawer_toggle_icon[id], quick_drawer_toggle_name[id],
                    quick_drawer_toggle_state[id], (int)id
                };
            }
        } else if (id >= UI_DRAWER_COUNT) {
            /* The final native order key marks where new plugins go. */
            int index = QD_TOGGLE_COUNT + (int)(id - UI_DRAWER_COUNT);
            int state_index = QD_TOGGLE_COUNT + (int)(id - UI_DRAWER_COUNT);
            if (id - UI_DRAWER_COUNT < (unsigned)quick_drawer_plugin_toggle_count &&
                quick_drawer_toggle_icon[index] && quick_drawer_toggle_name[index] &&
                quick_drawer_toggle_state[state_index]) {
                items[count++] = (quick_drawer_layout_item_t){
                    quick_drawer_toggle_icon[index], quick_drawer_toggle_name[index],
                    quick_drawer_toggle_state[state_index], index
                };
            }
        }
    }

#if defined(BOARD_TEMPOTEC_V1)
    int expansion_rows = count > 4 ? (count - 4 + 3) / 4 : 0;
    quick_drawer_expansion_full = quick_drawer_expansion_visible_height(
        QUICK_DRAWER_TOGGLE_ROW_PITCH * expansion_rows);
#else
    int expansion_rows = count <= 4 ? 0 : count <= 8 ? 1 : 2;
    quick_drawer_expansion_full = QUICK_DRAWER_TOGGLE_ROW_PITCH * expansion_rows;
#endif
    if (quick_drawer_expansion_handle) {
        lv_obj_set_flag(quick_drawer_expansion_handle, LV_OBJ_FLAG_HIDDEN, !expansion_rows);
    }

    for (int i = 0; i < count; ++i) {
#if defined(BOARD_TEMPOTEC_V1)
        int row = i < 4 ? 0 : 1 + (i - 4) / 4;
        int column = i < 4 ? i : (i - 4) % 4;
        int row_count = i < 4 ? (count < 4 ? count : 4)
                              : (count - 4 - (row - 1) * 4 < 4
                                 ? count - 4 - (row - 1) * 4 : 4);
        int32_t x = quick_drawer_toggle_column_x(column, row_count);
        /* Expanded captions are single-line, so later rows can sit closer. */
        int32_t y = row == 0 ? QUICK_DRAWER_ROW1_TOP
                             : (row - 1) * (QUICK_DRAWER_TOGGLE_ROW_PITCH - BOARD_SCALE_PY(60));
#else
        int row = i / 4, column = i % 4;
        int row_count = count - row * 4 < 4 ? count - row * 4 : 4;
        int32_t x = quick_drawer_toggle_column_x(column, row_count);
        int32_t y = row == 0 ? QUICK_DRAWER_ROW1_TOP :
                    row == 1 ? 0 : QUICK_DRAWER_TOGGLE_ROW_PITCH;
#endif
        lv_obj_t * parent = row == 0 ? quick_drawer : quick_drawer_expansion_box;
        int32_t caption_y = y + QUICK_DRAWER_TOGGLE_ICON_PX + QUICK_DRAWER_TOGGLE_CAPTION_GAP;
        int32_t state_y = quick_drawer_toggle_state_y(y);
#if defined(BOARD_TEMPOTEC_V1)
        int32_t compact_label_w = QUICK_DRAWER_TOGGLE_CELL_W - BOARD_SCALE_PX(10);
        int32_t label_inset = (QUICK_DRAWER_TOGGLE_CELL_W - compact_label_w) / 2;
        lv_obj_set_style_text_font(items[i].name, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_style_text_letter_space(items[i].name, -1, 0);
        lv_obj_set_width(items[i].name, row > 0 ? compact_label_w : QUICK_DRAWER_TOGGLE_CELL_W);
        /* One scrolling caption and, in expanded rows, a close status line
         * without reserving an empty second caption line. */
        int32_t caption_h = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
        lv_obj_set_height(items[i].name, caption_h);
        row_label_enable_marquee(items[i].name);
        if (row > 0) state_y = caption_y + caption_h + BOARD_SCALE_PY(5);
        lv_obj_set_width(items[i].state, compact_label_w);
        lv_obj_set_height(items[i].state, lv_font_get_line_height(&lv_font_montserrat_12));
        lv_label_set_long_mode(items[i].state, LV_LABEL_LONG_DOT);
        int32_t label_x = x + label_inset;
#endif
#if defined(BOARD_TEMPOTEC_V1)
        int32_t icon_x = x + (QUICK_DRAWER_TOGGLE_CELL_W - QUICK_DRAWER_TOGGLE_ICON_PX) / 2;
        quick_drawer_place_widget(items[i].icon, parent, icon_x, y, true);
        quick_drawer_place_widget(items[i].name, parent, row > 0 ? label_x : x, caption_y, true);
        quick_drawer_place_widget(items[i].state, parent, label_x, state_y, true);
#else
        quick_drawer_place_widget(items[i].icon, parent, x, y, true);
        quick_drawer_place_widget(items[i].name, parent, x, caption_y, true);
        quick_drawer_place_widget(items[i].state, parent, x, state_y, true);
#endif
    }
    /* Keep omitted toggles hidden even if a timer or a state refresh updates
     * their image or caption later. */
    for (int i = 0; i < QD_TOGGLE_COUNT + quick_drawer_plugin_toggle_count; ++i) {
        bool visible = false;
        for (int j = 0; j < count; ++j)
            if (items[j].id == i) { visible = true; break; }
        if (!visible) {
            lv_obj_t * parent = i < 4 ? quick_drawer : quick_drawer_expansion_box;
            quick_drawer_place_widget(quick_drawer_toggle_icon[i], parent, 0, 0, false);
            quick_drawer_place_widget(quick_drawer_toggle_name[i], parent, 0, 0, false);
            int state_index = i;
            if (state_index < QUICK_DRAWER_TOGGLE_SLOTS)
                quick_drawer_place_widget(quick_drawer_toggle_state[state_index], parent, 0, 0, false);
        }
    }
#if defined(BOARD_TEMPOTEC_V1)
    /* Resolve the new order before measuring icon centers for captions. */
    lv_obj_update_layout(quick_drawer);
    quick_drawer_pack_primary_captions();
#endif

    lv_obj_set_height(quick_drawer_expansion_box, 0);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_scroll_to_y(quick_drawer_expansion_box, 0, LV_ANIM_OFF);
#endif
    quick_drawer_toggle_force_src = true;
    gui_shell_refresh_quick_drawer_expansion_toggles();
    quick_drawer_toggle_force_src = false;
    if (quick_drawer_eq_button) {
        lv_obj_set_flag(quick_drawer_eq_button, LV_OBJ_FLAG_HIDDEN, !customization.drawer_eq_visible);
    }
    gui_shell_refresh_quick_drawer_volume_visibility();
    quick_drawer_mark_snapshot_dirty();
}
void gui_shell_apply_customization(void) {
    if (!quick_drawer) return;
    if (!quick_drawer_customization_enabled() && quick_drawer_open) {
        quick_drawer_customization_reflow_pending = true;
        close_quick_drawer();
        return;
    }
    if (quick_drawer_open || quick_drawer_bitmap_motion || quick_drawer_direct_motion) {
        quick_drawer_customization_reflow_pending = true;
        if (quick_drawer_open) close_quick_drawer();
        return;
    }
    quick_drawer_customization_reflow_pending = false;
    quick_drawer_reflow_customization();
}
void quick_drawer_brightness_teardown_flush(void) {
    if (quick_drawer_brightness_track && lv_slider_is_dragged(quick_drawer_brightness_track)) {
        int percent = (int) lv_slider_get_value(quick_drawer_brightness_track);
        brightness_hw_pending = percent;
        current_settings.brightness_percent = percent;
        settings_save(&current_settings);
    }
    brightness_hw_apply_pending();
    brightness_drag_active = false;
    if (brightness_hw_apply_timer) {
        lv_timer_delete(brightness_hw_apply_timer);
        brightness_hw_apply_timer = NULL;
    }
    brightness_hw_pending = -1;
}
void quick_drawer_volume_teardown_flush(void) {
    /* hw_volume_coalesce_teardown() can't re-read a live slider itself (it
     * doesn't know which widget owns the drag) -- caller-side step, same
     * as gui_player_teardown()'s own equivalent for the volume popup, done
     * here while quick_drawer_volume_track still exists (just below). */
    if (quick_drawer_volume_hv.drag_active && quick_drawer_volume_track)
        hw_volume_coalesce_drag_update(&quick_drawer_volume_hv, (int) lv_slider_get_value(quick_drawer_volume_track));
    hw_volume_coalesce_teardown(&quick_drawer_volume_hv);
}
void quick_drawer_teardown_widgets(void) {
    if (quick_drawer) {
        lv_obj_delete(quick_drawer);
        quick_drawer = NULL;
    }
    quick_drawer_brightness_icon = NULL;
#if defined(BOARD_TEMPOTEC_V1)
    quick_drawer_brightness_container = NULL;
#endif
    /* Every widget the gui_shell_update_quick_drawer_*() entry points can
     * reach: callers during a reload must find NULL, not freed objects. */
    quick_drawer_play_btn = NULL;
    quick_drawer_brightness_track = NULL;
    quick_drawer_brightness_label = NULL;
    quick_drawer_wifi_icon = NULL;
    quick_drawer_bt_icon = NULL;
    quick_drawer_sleep_icon = NULL;
    quick_drawer_crossfade_icon = NULL;
    quick_drawer_title_label = NULL;
    quick_drawer_artist_label = NULL;
    quick_drawer_album_label = NULL;
    quick_drawer_format_label = NULL;
    quick_drawer_cover_img = NULL; /* child of quick_drawer -- already deleted by the delete above */
    quick_drawer_cover_frame = NULL;
    quick_drawer_volume_container = NULL;
    quick_drawer_volume_icon = NULL;
    quick_drawer_eq_button = NULL;
    quick_drawer_volume_track = NULL;
    quick_drawer_volume_label = NULL;
    for (int i = 0; i < QUICK_DRAWER_TOGGLE_SLOTS; i++) quick_drawer_toggle_state[i] = NULL;
    for (int i = 0; i < QUICK_DRAWER_TOGGLE_SLOTS; i++) {
        quick_drawer_toggle_icon[i] = NULL;
        quick_drawer_toggle_name[i] = NULL;
    }
    quick_drawer_customization_reflow_pending = false;
    /* All children of quick_drawer, already deleted with it just above --
     * cleared so a rebuild (gui_reload.c) cannot act on a dangling one, and
     * so the expansion state machine no-ops until it is rebuilt. */
    quick_drawer_expansion_box = NULL;
    quick_drawer_expansion_handle = NULL;
    quick_drawer_card = NULL;
    quick_drawer_airplay_icon = NULL;
    quick_drawer_bt_dac_icon = NULL;
    quick_drawer_dlna_icon = NULL;
    quick_drawer_gapless_icon = NULL;
    quick_drawer_rc_icon = NULL;
    quick_drawer_car_mode_icon = NULL;
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; i++) quick_drawer_plugin_icon[i] = NULL;
    quick_drawer_plugin_toggle_count = 0;
    quick_drawer_motion_teardown_state();
    gui_shell_gestures_reset_expansion_flags();
    asset_decoded_image_close(&quick_drawer_brightness_image);
    close_quick_drawer_toggle_images();
}
void gui_shell_refresh_static_assets(void) {
    if (!quick_drawer) return;
    /* Theme/plugin asset files may have changed in place, so discard source
     * snapshots before rebuilding the accent variants. */
    close_quick_drawer_toggle_images();
    asset_decoded_image_close(&quick_drawer_brightness_image);
    const void * brightness = asset_decoded_image_open(&quick_drawer_brightness_image, "pull_down/blk.png")
                            ? asset_decoded_image_source(&quick_drawer_brightness_image) : NULL;
    if (quick_drawer_brightness_icon)
        lv_image_set_src(quick_drawer_brightness_icon,
                         brightness ? brightness : asset_path("pull_down/blk.png"));
    /* The toggle icons are decoded copies too, so a changed asset
     * on disk only reaches them through a re-decode -- and that re-decode
     * frees the buffers the widgets currently point at, so this must also
     * re-point them, which is exactly what this does. */
    gui_shell_refresh_quick_drawer_toggle_accent();
    quick_drawer_mark_snapshot_dirty();
}
void gui_shell_update_quick_drawer_track(const char * title, const char * artist,
                                         const char * album) {
    if (quick_drawer_title_label) {
        const char * want_title = title ? title : TR("No track loaded");
        const char * want_artist = artist ? artist : "";
        const char * want_album = album ? album : "";
        /* Only touch a label whose text actually changed: lv_label_set_text()
         * restarts LV_LABEL_LONG_SCROLL_CIRCULAR from the beginning, so
         * re-setting identical text would keep resetting the marquee (and
         * its 2s wait) instead of letting it scroll. Same guard gui_player.c
         * already applies to its own now-playing labels. */
        bool changed = false;
        if (!ui_label_text_matches(quick_drawer_title_label, want_title)) {
            lv_label_set_text(quick_drawer_title_label, want_title);
            changed = true;
        }
        if (!ui_label_text_matches(quick_drawer_artist_label, want_artist)) {
            lv_label_set_text(quick_drawer_artist_label, want_artist);
            changed = true;
        }
        if (quick_drawer_album_label && !ui_label_text_matches(quick_drawer_album_label, want_album)) {
            lv_label_set_text(quick_drawer_album_label, want_album);
            changed = true;
        }
        if (changed) quick_drawer_mark_snapshot_dirty();
    }
    gui_shell_refresh_quick_drawer_cover();
}
void gui_shell_update_quick_drawer_format(const char * text) {
    if (!quick_drawer_format_label) return;
    const char * want = text ? text : "";
    if (ui_label_text_matches(quick_drawer_format_label, want)) return;
    lv_label_set_text(quick_drawer_format_label, want);
    quick_drawer_mark_snapshot_dirty();
}
void gui_shell_refresh_quick_drawer_cover(void) {
    if (!quick_drawer_cover_img) return;
    const lv_image_dsc_t * frost = gui_player_get_current_reflection_dsc();
    if (frost && frost->data) {
        lv_image_set_src(quick_drawer_cover_img, frost);
        lv_obj_remove_flag(quick_drawer_cover_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* Drop the old descriptor source before its RGB565 buffer can be
         * released. Hiding alone leaves LVGL holding that source pointer. */
        lv_image_set_src(quick_drawer_cover_img, NULL);
        lv_obj_add_flag(quick_drawer_cover_img, LV_OBJ_FLAG_HIDDEN);
    }
    quick_drawer_fit_cover();
    quick_drawer_mark_snapshot_dirty();
}
void gui_shell_update_quick_drawer_play_state(bool is_playing) {
    if (quick_drawer_play_btn) {
        lv_image_set_src(quick_drawer_play_btn, gui_player_play_btn_image_src(is_playing));
        quick_drawer_mark_snapshot_dirty();
    }
}
void gui_shell_refresh_quick_drawer_toggle_accent(void) {
    /* Reloaded descriptors can reuse an old allocation's address. Rebind
     * every live tile after reload, even when its pointer compares equal. */
    quick_drawer_toggle_force_src = true;
    load_quick_drawer_toggle_images();
    /* Re-point every widget at the buffers just decoded -- load_...() freed
     * the previous ones, so anything still holding those would be reading
     * released memory on its next draw. Same reason refresh_play_btn_icon()
     * re-sets its own widgets after reloading. State comes from the same
     * authoritative reads build_quick_drawer() uses for the captions. */
    if (quick_drawer_wifi_icon)
        quick_drawer_apply_toggle_image(QD_TOGGLE_WIFI, quick_drawer_wifi_icon, gui_shell_wifi_effective_enabled());
    /* Cached state: a live bt_control_is_powered() query runs bluetoothctl
     * and can block this UI-thread path, which "Match album art" reaches on
     * track changes. */
    if (quick_drawer_bt_icon)
        quick_drawer_apply_toggle_image(QD_TOGGLE_BT, quick_drawer_bt_icon, bt_is_powered_cached);
    if (quick_drawer_sleep_icon)
        quick_drawer_apply_toggle_image(QD_TOGGLE_SLEEP, quick_drawer_sleep_icon, sleep_timer_active);
    if (quick_drawer_crossfade_icon)
        quick_drawer_apply_toggle_image(QD_TOGGLE_CROSSFADE, quick_drawer_crossfade_icon, current_settings.crossfade_enabled);
    /* The expanded rows are subject to the exact same hazard -- load_...()
     * re-decoded their "on" buffers too (including every plugin tile's), so
     * any of them currently showing an on-state would otherwise still point
     * at freed memory. Off-state icons that needed rounding are decoded
     * copies too, so every tile is re-pointed, whatever its state. */
    gui_shell_refresh_quick_drawer_expansion_toggles();
    quick_drawer_toggle_force_src = false;
    quick_drawer_mark_snapshot_dirty();
}
