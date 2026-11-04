#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../../assets/cover_card_preview.h"
#include "../../plugins/gui_plugins.h"
#include "../../shell/gui_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Native row builders: plugin icons, text rows, section headers, cover cards,
 * slider cards and the wide list row. */

#ifdef HOST_BUILD
  #define MUSIC_ROOT_DIR "./music"
#else
  /* SD card mount point -- each file that needs it defines its own copy
   * rather than sharing a header, same convention gui.c/audio.c/
   * plugin_manager.c already use. Needed here for pill_row_apply_icon()'s
   * own relative-icon-path resolution -- see its own comment. */
  #define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

/* An image scaled to `target_px` on its longest edge, centred in a non-
 * interactive wrapper sized to the scaled image so layout math sees the real
 * on-screen size (lv_image_set_scale() alone leaves the widget bounds alone).
 * Taps fall through to the wrapper's parent; the image is the wrapper's only
 * child. `fallback_native_px` is the assumed square source size when the
 * decoder cannot read its header (a missing/corrupt file just renders at that
 * size rather than failing the row). *out_h receives the wrapper's height. */
lv_obj_t * scaled_icon_create(lv_obj_t * parent, const char * src, int32_t target_px,
                              int32_t fallback_native_px, int32_t * out_h) {
    lv_obj_t * wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * img = lv_image_create(wrap);
    lv_image_set_src(img, src);

    lv_image_header_t header;
    int32_t scale = fallback_native_px > 0 ? (int32_t) (((int64_t) target_px * 256) / fallback_native_px) : 256;
    int32_t native_w = fallback_native_px, native_h = fallback_native_px;
    if (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK) {
        int32_t native_max = header.w > header.h ? header.w : header.h;
        if (native_max > 0) scale = (int32_t) (((int64_t) target_px * 256) / native_max);
        native_w = header.w;
        native_h = header.h;
    }
    lv_image_set_scale(img, scale);
    int32_t target_h = (int32_t) (((int64_t) native_h * scale) / 256);
    lv_obj_set_size(wrap, (int32_t) (((int64_t) native_w * scale) / 256), target_h);
    lv_obj_center(img);
    if (out_h) *out_h = target_h;
    return wrap;
}

void pill_row_apply_icon(lv_obj_t * row, lv_obj_t * label, const char * icon_path, int32_t icon_px,
                          lv_align_t align, int32_t x, int32_t y) {
    if (!icon_path) return;

    /* Relative icon paths are resolved against <sd_root>/.plugins/ (the plugin directory),
     * while absolute paths starting with '/' are preserved verbatim.
     * A path that does not fit is skipped; a truncated path could open a different file. */
    char resolved[600];
    int resolved_len;
    if (icon_path[0] == '/') {
        resolved_len = snprintf(resolved, sizeof(resolved), "%s", icon_path);
    } else {
        resolved_len = snprintf(resolved, sizeof(resolved), "%s/.plugins/%s", MUSIC_ROOT_DIR, icon_path);
    }
    if (resolved_len < 0 || (size_t)resolved_len >= sizeof(resolved)) return;

    /* "S:" is LVGL's POSIX-filesystem driver letter (lv_conf.h). Opens
     * resolved as an absolute filesystem path. Two extra bytes hold that prefix. */
    char src[sizeof(resolved) + 2];
    int src_len = snprintf(src, sizeof(src), "S:%s", resolved);
    if (src_len < 0 || (size_t)src_len >= sizeof(src)) return;

    /* A plugin icon is cosmetic: an unreadable header renders it at icon_px
     * (scale 256 = LV_SCALE_NONE) rather than failing the whole row. */
    lv_obj_t * img_wrap = scaled_icon_create(row, src, icon_px, icon_px, NULL);
    lv_obj_align(img_wrap, align, x, y);

    /* Re-align, not re-inset -- label may already be non-default-aligned
     * (e.g. a caller that already moved it for its own reasons); this always
     * wins as the final word on the label's position, matching every other
     * call site's own single lv_obj_align() call. icon_px (not target_w) is
     * the reserved gap -- icon_px is always the longer edge, so this never
     * underestimates the icon's real width even for a non-square source. */
    lv_obj_align(label, align, x + icon_px + 12, y);
}

const lv_font_t * pill_row_resolve_text_size(const char * text_size) {
    if (!text_size) return gui_theme_font(GUI_FONT_ROLE_BODY);
    if (strcmp(text_size, "small") == 0) return &app_font_16;
    if (strcmp(text_size, "medium") == 0) return &app_font_22;
    if (strcmp(text_size, "large") == 0) return &app_font_28;
    /* 8x16 monospace bitmap font (lv_conf.h's LV_FONT_UNSCII_16) -- ASCII-only,
     * see that flag's own comment. Plugin rows only (plugin.set_home_layout()'s
     * text_size = "mono", PLUGINS.md). */
    if (strcmp(text_size, "mono") == 0) return &lv_font_unscii_16;
    return gui_theme_font(GUI_FONT_ROLE_BODY);
}

/* Play All rows can be pooled labels or ordinary container rows. Align to
 * the physical card in both cases: LVGL's child alignment origin excludes
 * the parent's padding, so cancel the horizontal inset and asymmetric
 * vertical text padding used by shared list-row labels. */
lv_obj_t * decorate_play_all_row(lv_obj_t * row) {
    if (!row) return NULL;

    lv_obj_t * circle = lv_obj_create(row);
    if (!circle) return NULL;
    lv_obj_set_size(circle, BOARD_SCALE_PX(44), BOARD_SCALE_PX(44));
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(circle, 0, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    lv_obj_add_style(circle, gui_theme_accent_style(), 0);
    lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(circle, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_t * play = lv_label_create(circle);
    if (play) {
        lv_label_set_text(play, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(play, lv_color_white(), 0);
        const lv_font_t * play_font = gui_theme_font(GUI_FONT_ROLE_BODY);
        lv_obj_set_style_text_font(play, play_font, 0);
        lv_obj_align(play, LV_ALIGN_CENTER, 0, 0);
        /* The play glyph's advance box is centered by the label, but the
         * ink is a right-pointing triangle whose centroid sits one third
         * across its glyph box. Center that ink using the active font's
         * actual bearing and bounds (including any fallback font). */
        lv_font_glyph_dsc_t play_glyph;
        if (lv_font_get_glyph_dsc(play_font, &play_glyph, 0xF04B, 0) && play_glyph.box_w > 0) {
            int32_t advance_center = play_glyph.adv_w / 2;
            int32_t ink_center = play_glyph.ofs_x + play_glyph.box_w / 3;
            lv_obj_set_style_translate_x(play, advance_center - ink_center, 0);
        }
        lv_obj_remove_flag(play, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    int32_t pad_left = 0;
    int32_t pad_top = 0;
    int32_t pad_bottom = 0;
    if (lv_obj_check_type(row, &lv_label_class)) {
        pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
        pad_top = lv_obj_get_style_pad_top(row, LV_PART_MAIN);
        pad_bottom = lv_obj_get_style_pad_bottom(row, LV_PART_MAIN);
    }
    lv_obj_align(circle, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(28) - pad_left,
                 (pad_top - pad_bottom) / -2);
    return circle;
}

/* A native full-width text row: the shared row surface and one scrolling label
 * of `label_w`. */
static lv_obj_t * native_text_row(lv_obj_t * parent, const char * text, const lv_font_t * font,
                                  int32_t label_w) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, ui_list_row_width(), ui_list_row_height());
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, GUI_TEXT_INSET, 0);
    configure_scrolling_row_label(label, label_w);
    return row;
}

static lv_obj_t * pill_row_clickable(lv_obj_t * row, lv_event_cb_t on_click, void * user_data) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    if (on_click) lv_obj_add_event_cb(row, on_click, LV_EVENT_CLICKED, user_data);
    return row;
}

lv_obj_t * add_pill_row_base(lv_obj_t * parent, const char * label_text) {
    lv_obj_t * row = native_text_row(parent, label_text, gui_theme_font(GUI_FONT_ROLE_BODY),
                                     ui_list_row_width() - 136);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    return row;
}

lv_obj_t * add_pill_summary_label(lv_obj_t * row) {
    lv_obj_update_layout(row);
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), -BOARD_SCALE_PX(16));
    lv_obj_t * label = lv_label_create(row);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_width(label, lv_obj_get_width(row) - BOARD_SCALE_PX(84));
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(24), BOARD_SCALE_PX(20));
    int32_t needed = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) +
                     lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + BOARD_SCALE_PX(48);
    if (lv_obj_get_height(row) < needed) lv_obj_set_height(row, needed);
    return label;
}

lv_obj_t * add_pill_toggle_row(lv_obj_t * parent, const char * label_text, bool checked, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);

    /* Non-clickable switch visual; the row itself handles the click event. */
    lv_obj_t * toggle_sw = lv_switch_create(row);
    lv_obj_remove_flag(toggle_sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(toggle_sw, gui_theme_accent_style(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
    lv_obj_align(toggle_sw, LV_ALIGN_RIGHT_MID, -20, 0);

    return pill_row_clickable(row, on_click, NULL);
}

lv_obj_t * add_pill_chevron_row(lv_obj_t * parent, const char * label_text, lv_event_cb_t on_click) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    build_navigation_chevron(row, BOARD_SCALE_PX(20), false, 0);

    return pill_row_clickable(row, on_click, NULL);
}

lv_obj_t * add_pill_option_row(lv_obj_t * parent, const char * label_text, bool selected,
                              lv_event_cb_t on_click, void * user_data) {
    lv_obj_t * row = add_pill_row_base(parent, label_text);
    if (selected) {
        lv_obj_add_style(row, gui_theme_accent_outline_style(), 0);
        lv_obj_set_style_border_width(row, BOARD_SCALE_PX(3), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_t * text = lv_obj_get_child(row, 0);
        configure_scrolling_row_label(text, pill_row_default_width() - GUI_TEXT_INSET - BOARD_SCALE_PX(64));
        lv_obj_t * check = lv_label_create(row);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_obj_add_style(check, gui_theme_accent_style(), 0);
        lv_obj_set_style_text_font(check, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-20), 0);
        lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
    }
    return pill_row_clickable(row, on_click, user_data);
}

lv_obj_t * build_check_box(lv_obj_t * parent, bool checked) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, BOARD_SCALE_PX(24), BOARD_SCALE_PX(24));
    lv_obj_set_style_radius(box, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_border_width(box, BOARD_SCALE_PX(2), 0);
    lv_obj_set_style_border_color(box, accent_lv_color(), 0);
    lv_obj_set_style_bg_opa(box, checked ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(box, accent_lv_color(), 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (checked) {
        lv_obj_t * check = lv_label_create(box);
        lv_label_set_text(check, LV_SYMBOL_OK);
        lv_color_t check_color = lv_color_brightness(accent_lv_color()) > 160
            ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white();
        lv_obj_set_style_text_color(check, check_color, 0);
        lv_obj_center(check);
        lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    return box;
}

lv_obj_t * add_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
#if defined(BOARD_TEMPOTEC_V1)
    /* Section captions can be longer than the entire narrow display. Give
     * them the list's content width so LVGL wraps instead of centering a
     * source-sized label beyond both edges of the viewport. */
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_right(label, BOARD_SCALE_PX(24), 0);
#else
    lv_obj_set_style_pad_top(label, 12, 0);
    lv_obj_set_style_pad_left(label, 24, 0);
#endif
    return label;
}

/* Muted explanatory paragraph that wraps to the full list width. */
lv_obj_t * add_wrapped_section_header(lv_obj_t * parent, const char * text) {
    lv_obj_t * header = add_section_header(parent, text);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_style_pad_right(header, BOARD_SCALE_PX(24), 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_WRAP);
    return header;
}

void configure_cover_card_grid(lv_obj_t * list, int columns) {
    if (!list || columns < 1) return;
    int32_t side = BOARD_SCALE_PX(16), gap = BOARD_SCALE_PX(14);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(list, side, 0);
    lv_obj_set_style_pad_right(list, side, 0);
    lv_obj_set_style_pad_column(list, gap, 0);
    lv_obj_set_style_pad_row(list, gap, 0);
}

typedef struct {
    lv_obj_t * cover;
    lv_point_t press_point;
    bool tracking;
    bool cancelled;
} cover_card_event_ctx_t;

/* LVGL suppresses CLICKED once scrolling owns the pointer. Track the
 * original touch point too, so small drags and press-lost transitions do
 * not select a card. The caller's callback keeps its original user data. */
static void cover_card_guard_event_cb(lv_event_t * e) {
    cover_card_event_ctx_t * ctx = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(ctx);
        return;
    }
    if (!ctx) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if (indev) lv_indev_get_point(indev, &ctx->press_point);
        ctx->tracking = indev != NULL;
        ctx->cancelled = indev && lv_indev_get_scroll_obj(indev) != NULL;
        lv_obj_set_style_border_width(ctx->cover, BOARD_SCALE_PX(2), 0);
    } else if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED) {
        lv_indev_t * indev = lv_event_get_indev(e);
        if (ctx->tracking && indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            if (LV_ABS(point.x - ctx->press_point.x) > BOARD_SCALE_PX(8) ||
                LV_ABS(point.y - ctx->press_point.y) > BOARD_SCALE_PX(8) ||
                lv_indev_get_scroll_obj(indev))
                ctx->cancelled = true;
        }
        if (code == LV_EVENT_RELEASED) {
            ctx->tracking = false;
            lv_obj_set_style_border_width(ctx->cover, 0, 0);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        ctx->tracking = false;
        ctx->cancelled = true;
        lv_obj_set_style_border_width(ctx->cover, 0, 0);
    } else if (code == LV_EVENT_CLICKED) {
        if (ctx->cancelled) lv_event_stop_processing(e);
        ctx->tracking = false;
        ctx->cancelled = false;
    }
}

lv_obj_t * add_cover_card(lv_obj_t * list, const char * label,
                          const char * resolved_image_src, int columns,
                          lv_event_cb_t on_click, void * user_data) {
    if (!list || columns < 1) return NULL;
    /* Resolve percentage widths before sizing cards in an inset container,
     * such as Quick Setup; full-screen grids retain the same geometry. */
    lv_obj_update_layout(list);
    int32_t gap = lv_obj_get_style_pad_column(list, 0);
    int32_t card_w = (lv_obj_get_content_width(list) - (columns - 1) * gap) / columns;
    if (card_w < 1) return NULL;
    int32_t cover_h = card_w * 3 / 2;
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);

    lv_obj_t * card = lv_obj_create(list);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, card_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(6), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * cover = lv_obj_create(card);
    lv_obj_remove_style_all(cover);
    lv_obj_set_size(cover, card_w, cover_h);
    lv_obj_add_style(cover, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cover, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_border_color(cover, lv_color_white(), 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (resolved_image_src && resolved_image_src[0]) {
        lv_obj_t * image = lv_image_create(cover);
        lv_image_header_t header;
        if (cover_card_preview_set(image, resolved_image_src, card_w, cover_h)) {
            lv_obj_center(image);
        } else if (lv_image_decoder_get_info(resolved_image_src, &header) == LV_RESULT_OK &&
                   header.w > 0 && header.h > 0) {
            lv_image_set_src(image, resolved_image_src);
            int64_t scale_w = (int64_t) card_w * 256 / header.w;
            int64_t scale_h = (int64_t) cover_h * 256 / header.h;
            lv_image_set_scale(image, (uint32_t) (scale_w < scale_h ? scale_w : scale_h));
            lv_obj_center(image);
        } else {
            lv_obj_delete(image);
        }
    }
    if (lv_obj_get_child_count(cover) == 0) {
        lv_obj_t * placeholder = lv_label_create(cover);
        lv_label_set_text(placeholder, label);
        lv_obj_add_style(placeholder, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(placeholder, font, 0);
        lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(placeholder, card_w - BOARD_SCALE_PX(16));
        lv_label_set_long_mode(placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_center(placeholder);
    }

    lv_obj_t * title = lv_label_create(card);
    lv_label_set_text(title, label);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(title, card_w, lv_font_get_line_height(font) * 2);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    if (on_click) {
        cover_card_event_ctx_t * ctx = lv_malloc(sizeof(*ctx));
        if (ctx) {
            *ctx = (cover_card_event_ctx_t) { .cover = cover };
            lv_obj_add_event_cb(card, cover_card_guard_event_cb, LV_EVENT_ALL, ctx);
            lv_obj_add_event_cb(card, on_click, LV_EVENT_CLICKED, user_data);
        } else {
            lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return card;
}

int find_nearest_step_index(const int * steps, int count, int value) {
    if (!steps || count <= 0) return 0;
    int best = 0;
    int best_diff = abs(value - steps[0]);
    for (int i = 1; i < count; i++) {
        int diff = abs(value - steps[i]);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

int32_t ui_row_label_width(lv_obj_t * row, int32_t row_w, int32_t label_x, int32_t right_space) {
    const int32_t border = lv_obj_get_style_border_width(row, LV_PART_MAIN);
    const int32_t pad_left = lv_obj_get_style_pad_left(row, LV_PART_MAIN);
    return row_w - (border + pad_left) - label_x - right_space;
}

/* Measured on the label's own (shaped) text, as drawn. */
bool ui_row_fit_label(lv_obj_t * row, lv_obj_t * label, const lv_font_t * font,
                      int32_t row_w, int32_t label_x, int32_t label_w) {
    const int32_t row_h = ui_list_row_height();
    const int32_t line_h = lv_font_get_line_height(font);
    lv_point_t text_size;
    lv_text_get_size(&text_size, lv_label_get_text(label), font, 0, 0, label_w, LV_TEXT_FLAG_NONE);
    const bool tall = text_size.y > line_h;
    lv_label_set_long_mode(label, tall ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    if (!tall) {
        lv_obj_set_size(row, row_w, row_h);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, label_x, 0);
        return false;
    }
    /* Symmetric padding keeps the text off the card borders and puts its
     * first line where a one-line row centres it. */
    const int32_t border = lv_obj_get_style_border_width(row, LV_PART_MAIN);
    lv_obj_set_size(row, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, row_h, 0);
    lv_obj_set_style_pad_ver(row, (row_h - line_h) / 2 - border, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, label_x, 0);
    return true;
}

/* Native wide rows shared by saved playlists and CUE tracks. */
lv_obj_t * build_wide_list_row(lv_obj_t * parent, const char * label_text, bool chevron) {
    lv_obj_t * row = native_text_row(parent, label_text, &LIST_ROW_FONT,
                                     LIST_ROW_WIDTH_WIDE - LIST_ROW_LABEL_INSET - BOARD_SCALE_PX(60));
    if (chevron) {
        build_navigation_chevron(row, GUI_TEXT_INSET, false, 0);
    }
    return row;
}

void ui_clear_flag_on_press_cb(lv_event_t * e) {
    bool * flag = (bool *) lv_event_get_user_data(e);
    if (flag && lv_event_get_code(e) == LV_EVENT_PRESSED) *flag = false;
}
