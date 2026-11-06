#include "gui_album_view.h"
#include "../../widgets/lists/lv_idempotent.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../assets/assets.h"
#include "../../../core/system/runtime/board_config.h"
#include <stdio.h>
#include <string.h>

static char dur_marker;

static lv_obj_t * plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Fixed-width label of at most max_lines lines, ending in "..." beyond that. */
static lv_obj_t * text_label(lv_obj_t * parent, const char * text, gui_font_role_t role,
                             lv_style_t * style, int32_t width, int max_lines) {
    const lv_font_t * font = gui_theme_font(role);
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, style, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int32_t line = lv_font_get_line_height(font);
    int32_t height = size.y < line ? line : size.y;
    if (height > line * max_lines) height = line * max_lines;
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, width, height);
    return label;
}

static int32_t text_width(const char * text, const lv_font_t * font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Wide enough for the album's longest number on one line, measured as 8s (the widest digit). */
int32_t gui_album_view_number_width(int widest) {
    char digits[12];
    int len = snprintf(digits, sizeof(digits), "%d", widest < 1 ? 1 : widest);
    memset(digits, '8', (size_t) len);
    int32_t w = text_width(digits, gui_theme_font(GUI_FONT_ROLE_BODY));
    return w > BOARD_SCALE_PX(40) ? w : BOARD_SCALE_PX(40);
}

static void add_chip(gui_album_view_t * v, const char * text, bool gold) {
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t line = lv_font_get_line_height(font), padx = BOARD_SCALE_PX(12), gap = BOARD_SCALE_PX(6);
    lv_color_t amber = lv_color_hex(accent_palette[11]);
    lv_obj_t * chip = plain(v->chips);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(chip, padx, 0);
    lv_obj_set_style_pad_ver(chip, BOARD_SCALE_PX(5), 0);
    lv_obj_set_style_pad_column(chip, gap, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (gold) {
        lv_obj_set_style_border_color(chip, amber, 0);
        lv_obj_set_style_bg_color(chip, amber, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
        lv_obj_t * icon = lv_image_create(chip);
        lv_image_set_src(icon, asset_path(v->hires_icon));
        lv_obj_set_size(icon, line + 2, line + 2);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_style(chip, &style_theme_card_bg, 0);
    }
    lv_obj_t * label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    if (gold) lv_obj_set_style_text_color(label, amber, 0);
    else lv_obj_add_style(label, &style_theme_text_primary, 0);
}

void gui_album_view_set_chips(gui_album_view_t * v, const char * format, bool hires, const char * total) {
    if (!v->chips) return;
    lv_obj_clean(v->chips);
    if (format[0]) add_chip(v, format, false);
    if (hires) add_chip(v, TR("Hi-Res"), true);
    if (total[0]) add_chip(v, total, false);
}

void gui_album_view_total_text(double seconds, char * out, size_t size) {
    out[0] = 0;
    if (seconds <= 0.0) return;
    int minutes = (int) ((seconds + 30.0) / 60.0);
    if (minutes < 1) minutes = 1;
    if (minutes >= 60) snprintf(out, size, TR("%d hr %d min"), minutes / 60, minutes % 60);
    else snprintf(out, size, TR("%d min"), minutes);
}

void gui_album_view_meta_text(int year, const char * genre, char * out, size_t size) {
    out[0] = 0;
    if (year > 0) snprintf(out, size, "%d", year);
    if (genre && genre[0]) {
        size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%s", len ? " \xC2\xB7 " : "", genre);
    }
}

static lv_obj_t * build_cover(gui_album_view_t * v, lv_obj_t * top) {
    lv_obj_t * cover = plain(top);
    lv_obj_set_size(cover, v->cover_px, v->cover_px);
    lv_obj_set_style_radius(cover, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_add_style(cover, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    v->cover_img = lv_image_create(cover);
    lv_obj_remove_flag(v->cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN);
    return cover;
}

/* Title, artist and meta lines, then the empty chip row at the bottom. */
static void build_info(gui_album_view_t * v, lv_obj_t * top, const gui_album_view_params_t * p) {
    v->info = plain(top);
    lv_obj_set_width(v->info, v->info_w);
    /* At least the cover's height, growing when the content needs it (small
     * panels, large fonts, wrapping chips); LVGL resolves it in its own pass. */
    lv_obj_set_height(v->info, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(v->info, v->cover_px, 0);
    lv_obj_set_style_pad_row(v->info, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(v->info, LV_FLEX_FLOW_COLUMN);
    text_label(v->info, p->album && p->album[0] ? p->album : TR("Unknown album"),
               GUI_FONT_ROLE_TITLE, &style_theme_text_primary, v->info_w, 2);
    if (p->artist && p->artist[0])
        text_label(v->info, p->artist, GUI_FONT_ROLE_ROW, gui_theme_accent_style(), v->info_w, 1);
    if (p->meta && p->meta[0])
        text_label(v->info, p->meta, GUI_FONT_ROLE_BODY, &style_theme_text_muted, v->info_w, 1);
    lv_obj_t * spacer = plain(v->info);
    lv_obj_set_size(spacer, 1, 0);
    lv_obj_set_flex_grow(spacer, 1);
    v->chips = plain(v->info);
    lv_obj_set_size(v->chips, v->info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(v->chips, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_row(v->chips, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(v->chips, LV_FLEX_FLOW_ROW_WRAP);
}

void gui_album_view_build(gui_album_view_t * v, lv_obj_t * list, const gui_album_view_params_t * p) {
    int32_t row_w = LIST_ROW_WIDTH, gap = BOARD_SCALE_PX(18);
    v->cover_px = BOARD_SCALE_PX(170);
    v->info_w = row_w - v->cover_px - gap;
    snprintf(v->hires_icon, sizeof(v->hires_icon), "%s", p->hires_icon ? p->hires_icon : "");
    v->root = plain(list);
    lv_obj_set_size(v->root, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(v->root, gap, 0);
    lv_obj_set_style_pad_bottom(v->root, BOARD_SCALE_PX(8), 0);
    lv_obj_set_flex_flow(v->root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * top = plain(v->root);
    lv_obj_set_size(top, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(top, gap, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    build_cover(v, top);
    build_info(v, top, p);
    if (p->show_buttons) gui_play_buttons_build(v->root, row_w, p->play_cb, NULL, p->shuffle_cb, NULL);
}

void gui_album_view_fit_cover(gui_album_view_t * v, lv_image_align_t align) {
    if (lv_obj_get_style_width(v->cover_img, LV_PART_MAIN) != v->cover_px ||
        lv_obj_get_style_height(v->cover_img, LV_PART_MAIN) != v->cover_px)
        lv_obj_set_size(v->cover_img, v->cover_px, v->cover_px);
    if (lv_image_get_inner_align(v->cover_img) != align) lv_image_set_inner_align(v->cover_img, align);
    lv_idem_align(v->cover_img, LV_ALIGN_CENTER, 0, 0);
}

void gui_album_view_show_cover(gui_album_view_t * v, const lv_image_dsc_t * dsc) {
    bool exact = dsc->header.w == v->cover_px && dsc->header.h == v->cover_px;
    if (lv_image_get_src(v->cover_img) != dsc || lv_obj_has_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN))
        lv_image_set_src(v->cover_img, dsc);
    gui_album_view_fit_cover(v, exact ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN);
}

bool gui_album_view_detach_cover(gui_album_view_t * v, const lv_image_dsc_t * dsc) {
    if (!v->cover_img || (dsc && lv_image_get_src(v->cover_img) != dsc)) return false;
    lv_image_set_src(v->cover_img, NULL);
    lv_obj_add_flag(v->cover_img, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* ---- Rows ---------------------------------------------------------------- */
static void set_duration_text(lv_obj_t * label, unsigned seconds) {
    char text[16];
    snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    lv_label_set_text(label, text);
}

static lv_obj_t * row_label(lv_obj_t * row, const char * text, int32_t w, int32_t h, int32_t pad) {
    const lv_font_t * body = gui_theme_font(GUI_FONT_ROLE_BODY);
    lv_obj_t * n = lv_label_create(row);
    lv_label_set_text(n, text);
    lv_obj_add_style(n, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(n, body, 0);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(n, w);
    lv_obj_set_pos(n, pad, (h - lv_font_get_line_height(body)) / 2);
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
    return n;
}

void gui_album_view_decorate_row(lv_obj_t * row, int number, int num_w, int seconds) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    const lv_font_t * row_font = gui_theme_font(GUI_FONT_ROLE_ROW);
    const lv_font_t * body = gui_theme_font(GUI_FONT_ROLE_BODY);
    int32_t title_h = row_label_bounded_height(row_font);
    int32_t h = BOARD_SCALE_PX(76);
    if (h < title_h + 2 * BOARD_SCALE_PX(8)) h = title_h + 2 * BOARD_SCALE_PX(8);
    int32_t pad = BOARD_SCALE_PX(14), badge = BOARD_SCALE_PX(40);
    if (num_w <= 0) num_w = BOARD_SCALE_PX(40);
    int32_t dur_w = text_width("88:88", body), dur_right = pad + badge + BOARD_SCALE_PX(6);
    int32_t title_x = pad + num_w + BOARD_SCALE_PX(8);
    lv_obj_set_size(row, LIST_ROW_WIDTH, h);
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_set_pos(title, title_x, (h - title_h) / 2);
    lv_obj_set_width(title, LIST_ROW_WIDTH - title_x - dur_right - dur_w - BOARD_SCALE_PX(8));

    char text[12];
    snprintf(text, sizeof(text), "%d", number);
    row_label(row, text, num_w, h, pad);

    lv_obj_t * d = lv_label_create(row);
    lv_obj_add_style(d, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(d, body, 0);
    lv_obj_set_user_data(d, &dur_marker);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -dur_right, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    if (seconds >= 0) set_duration_text(d, (unsigned) seconds);
    else lv_label_set_text(d, "");
}

void gui_album_view_row_set_duration(lv_obj_t * row, unsigned seconds) {
    uint32_t n = row ? lv_obj_get_child_count(row) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * child = lv_obj_get_child(row, (int32_t) i);
        if (lv_obj_get_user_data(child) == &dur_marker) { set_duration_text(child, seconds); return; }
    }
}

void gui_album_view_row_badge(lv_obj_t * row, const char * asset) {
    lv_obj_t * quality = lv_image_create(row);
    lv_image_set_src(quality, asset_path(asset));
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_size(quality, BOARD_SCALE_PX(34), BOARD_SCALE_PX(34));
    lv_image_set_inner_align(quality, LV_IMAGE_ALIGN_CONTAIN);
#endif
    /* Child alignment is relative to the label's padded content
     * box. Cancel the 70px text reserve so the badge is physically
     * 14px from the card edge (same rule as compact-list rows). */
    lv_obj_align(quality, LV_ALIGN_RIGHT_MID, BOARD_SCALE_PX(-14), 0);
    lv_obj_remove_flag(quality, LV_OBJ_FLAG_CLICKABLE);
}
