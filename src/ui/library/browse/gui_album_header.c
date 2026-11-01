#include "gui_album_header_internal.h"
#include "gui_album_facts.h"
#include "gui_album_cover.h"
#include "../library_thumbnails.h"
#include "../../widgets/lists/lv_idempotent.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../assets/assets.h"
#include "../../../library/metadata/durations/duration_lookup.h"
#include "../../../core/system/runtime/board_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>


static char dur_marker;

static void load_record(void) {
    free(A.track);
    A.track = NULL;
    A.have_record = gui_album_facts_lookup(A.key, A.count, &A.year, A.genre, sizeof(A.genre), &A.track);
}

/* ---- Widgets ------------------------------------------------------------ */
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

static void add_chip(const char * text, bool gold) {
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    int32_t line = lv_font_get_line_height(font), padx = BOARD_SCALE_PX(12), gap = BOARD_SCALE_PX(6);
    lv_color_t amber = lv_color_hex(accent_palette[11]);
    lv_obj_t * chip = plain(A.chips);
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
        lv_image_set_src(icon, asset_path(A.hires_icon));
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

/* Hides a chip whose data is not known; never shows a partial total. The chips
 * are only rebuilt when what they show changes, so a probe result that adds no
 * chip costs nothing. The column's height needs no measuring: it is content
 * sized with the cover as its minimum (see gui_album_header_build()), so LVGL's
 * normal layout pass grows it when wrapping chips need more room. */
static bool update_chips(void) {
    if (!A.chips) return false;
    char format[sizeof(A.chip_format)] = "", total_text[sizeof(A.chip_total)] = "";
    bool hires_chip = false;
    if (album_settled()) {
        /* A failed track is known but unplayable: the chips cover the playable ones. */
        bool mixed = false, hires = true;
        double total = 0;
        int playable = 0, first = -1;
        for (int i = 0; i < A.count; i++) {
            if (A.state[i] != 1) continue;
            if (first < 0) first = i;
            playable++;
            total += A.dur[i].duration_ms / 1000.0;
            if (!duration_info_is_hires(&A.dur[i])) hires = false;
            if (A.dur[i].codec != A.dur[first].codec) mixed = true;
        }
        snprintf(format, sizeof(format), "%s",
                 playable ? (mixed ? TR("Mixed") : audio_codec_name((audio_codec_t) A.dur[first].codec)) : "");
        hires_chip = playable && hires && A.hires_icon[0];
        if (playable && total > 0.0) {
            int minutes = (int) ((total + 30.0) / 60.0);
            if (minutes < 1) minutes = 1;
            if (minutes >= 60) snprintf(total_text, sizeof(total_text), TR("%d hr %d min"), minutes / 60, minutes % 60);
            else snprintf(total_text, sizeof(total_text), TR("%d min"), minutes);
        }
    }
    if (hires_chip == A.chip_hires && !strcmp(format, A.chip_format) && !strcmp(total_text, A.chip_total)) return false;
    snprintf(A.chip_format, sizeof(A.chip_format), "%s", format);
    snprintf(A.chip_total, sizeof(A.chip_total), "%s", total_text);
    A.chip_hires = hires_chip;
    lv_obj_clean(A.chips);
    if (format[0]) add_chip(format, false);
    if (hires_chip) add_chip(TR("Hi-Res"), true);
    if (total_text[0]) add_chip(total_text, false);
    return true;
}

void gui_album_header_build(lv_obj_t * list, const gui_album_header_params_t * p) {
    int count = p->entries ? p->count : 0;
    uint64_t key = gui_album_facts_seed(count);
    for (int i = 0; i < count; i++) key = gui_album_facts_add(key, p->entries[i].path);
    if (key != A.key || count != A.count || !A.paths) album_state_reset(key, p->entries, count);
    if (!A.have_record && count > 0 && A.paths) load_record();
    A.hires_icon[0] = 0;
    if (count > 0 && p->quality_asset) snprintf(A.hires_icon, sizeof(A.hires_icon), "%s", p->quality_asset(p->entries[0].path));
    A.cover_song_id = 0; A.cover_key = 0;
    for (int i = 0; i < count; i++)
        if (p->entries[i].song_id > 0 && p->entries[i].artwork_key != 0) {
            A.cover_song_id = p->entries[i].song_id; A.cover_key = p->entries[i].artwork_key; break;
        }

    if (A.cover_song_id > 0) gui_album_cover_request(A.cover_song_id, BOARD_SCALE_PX(170));
    int32_t row_w = LIST_ROW_WIDTH, cs = BOARD_SCALE_PX(170), gap = BOARD_SCALE_PX(18);
    A.cover_px = cs;
    A.info_w = row_w - cs - gap;
    A.root = plain(list);
    lv_obj_set_size(A.root, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(A.root, gap, 0);
    lv_obj_set_style_pad_bottom(A.root, BOARD_SCALE_PX(8), 0);
    lv_obj_set_flex_flow(A.root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * top = plain(A.root);
    lv_obj_set_size(top, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(top, gap, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t * cover = plain(top);
    lv_obj_set_size(cover, cs, cs);
    lv_obj_set_style_radius(cover, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_add_style(cover, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    A.cover_img = lv_image_create(cover);
    lv_obj_remove_flag(A.cover_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(A.cover_img, LV_OBJ_FLAG_HIDDEN);

    int32_t col_gap = BOARD_SCALE_PX(4);
    A.info = plain(top);
    lv_obj_set_width(A.info, A.info_w);
    /* At least the cover's height, growing when the content needs it (small
     * panels, large fonts, wrapping chips); LVGL resolves it in its own pass. */
    lv_obj_set_height(A.info, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(A.info, A.cover_px, 0);
    lv_obj_set_style_pad_row(A.info, col_gap, 0);
    lv_obj_set_flex_flow(A.info, LV_FLEX_FLOW_COLUMN);
    text_label(A.info, p->album && p->album[0] ? p->album : TR("Unknown album"),
                                  GUI_FONT_ROLE_TITLE, &style_theme_text_primary, A.info_w, 2);
    if (p->album_artist && p->album_artist[0]) {
        text_label(A.info, p->album_artist, GUI_FONT_ROLE_ROW, gui_theme_accent_style(), A.info_w, 1);
    }
    char meta[160] = "";
    if (A.year > 0) snprintf(meta, sizeof(meta), "%d", A.year);
    if (A.genre[0]) {
        size_t len = strlen(meta);
        snprintf(meta + len, sizeof(meta) - len, "%s%s", len ? " \xC2\xB7 " : "", A.genre);
    }
    if (meta[0]) {
        text_label(A.info, meta, GUI_FONT_ROLE_BODY, &style_theme_text_muted, A.info_w, 1);
    }
    lv_obj_t * spacer = plain(A.info);
    lv_obj_set_size(spacer, 1, 0);
    lv_obj_set_flex_grow(spacer, 1);
    A.chips = plain(A.info);
    lv_obj_set_size(A.chips, A.info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(A.chips, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_row(A.chips, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(A.chips, LV_FLEX_FLOW_ROW_WRAP);
    A.chip_format[0] = A.chip_total[0] = 0;
    A.chip_hires = false;
    update_chips();

    if (p->show_buttons) {
        gui_play_buttons_build(A.root, row_w, p->play_cb, NULL, p->shuffle_cb, NULL);
    }
    A.dirty = false;
}

void gui_album_header_release_cover(void) { gui_album_cover_release(); }

void gui_album_header_forget(void) {
    A.root = A.info = A.chips = A.cover_img = NULL;
}

/* ---- Cover --------------------------------------------------------------- */
/* Sets the cover image's size and alignment, touching LVGL only when they change:
 * STRETCH makes LVGL lay the screen out on every assignment. */
static void fit_cover(lv_image_align_t align) {
    if (lv_obj_get_style_width(A.cover_img, LV_PART_MAIN) != A.cover_px ||
        lv_obj_get_style_height(A.cover_img, LV_PART_MAIN) != A.cover_px)
        lv_obj_set_size(A.cover_img, A.cover_px, A.cover_px);
    if (lv_image_get_inner_align(A.cover_img) != align) lv_image_set_inner_align(A.cover_img, align);
    lv_idem_align(A.cover_img, LV_ALIGN_CENTER, 0, 0);
}

/* The sharp cover replaces the thumbnail once it is decoded. It is decoded at
 * exactly the cover's size, so it is drawn as it is (centred, no scaling). */
static bool show_sharp_cover(void) {
    const lv_image_dsc_t * dsc = A.cover_img && A.cover_song_id > 0 ? gui_album_cover_get(A.cover_song_id) : NULL;
    if (!dsc) return false;
    bool exact = dsc->header.w == A.cover_px && dsc->header.h == A.cover_px;
    if (lv_image_get_src(A.cover_img) != dsc || lv_obj_has_flag(A.cover_img, LV_OBJ_FLAG_HIDDEN))
        lv_image_set_src(A.cover_img, dsc);
    fit_cover(exact ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(A.cover_img, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void gui_album_header_refresh_cover(lv_obj_t * list) {
    if (!A.root || !A.cover_img || A.cover_song_id <= 0 || show_sharp_cover()) return;
    /* The shared decorator pads a row for a thumbnail; this header lays out
     * its own children, so undo that and let the image fill the cover box. */
    library_thumbnails_decorate_cover_image(list, A.root, A.cover_img, A.cover_song_id, A.cover_key);
    fit_cover(LV_IMAGE_ALIGN_STRETCH);
}

bool gui_album_header_detach_cover(const lv_image_dsc_t * dsc) {
    if (!A.cover_img || lv_image_get_src(A.cover_img) != dsc) return false;
    lv_image_set_src(A.cover_img, NULL);
    lv_obj_add_flag(A.cover_img, LV_OBJ_FLAG_HIDDEN);
    return true;
}

/* ---- Rows ---------------------------------------------------------------- */
const char * gui_album_header_row_title(const char * title, char * buf, size_t size) {
    const char * newline = title ? strchr(title, '\n') : NULL;
    size_t len = newline ? (size_t) (newline - title) : (title ? strlen(title) : 0);
    if (len >= size) len = size - 1;
    if (title) memcpy(buf, title, len);
    buf[len] = 0;
    return buf;
}

static void set_duration_text(lv_obj_t * label, unsigned seconds) {
    char text[16];
    snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    lv_label_set_text(label, text);
}

void gui_album_header_decorate_row(lv_obj_t * row, int index) {
    if (!row || lv_obj_get_child_count(row) < 2) return;
    const lv_font_t * row_font = gui_theme_font(GUI_FONT_ROLE_ROW);
    const lv_font_t * body = gui_theme_font(GUI_FONT_ROLE_BODY);
    int32_t title_h = row_label_bounded_height(row_font);
    int32_t h = BOARD_SCALE_PX(76);
    if (h < title_h + 2 * BOARD_SCALE_PX(8)) h = title_h + 2 * BOARD_SCALE_PX(8);
    int32_t pad = BOARD_SCALE_PX(14), num_w = BOARD_SCALE_PX(40), badge = BOARD_SCALE_PX(40);
    int32_t dur_w = text_width("88:88", body), dur_right = pad + badge + BOARD_SCALE_PX(6);
    int32_t title_x = pad + num_w + BOARD_SCALE_PX(8);
    lv_obj_set_size(row, LIST_ROW_WIDTH, h);
    lv_obj_t * title = lv_obj_get_child(row, 0);
    lv_obj_set_pos(title, title_x, (h - title_h) / 2);
    lv_obj_set_width(title, LIST_ROW_WIDTH - title_x - dur_right - dur_w - BOARD_SCALE_PX(8));

    char number[12];
    int shown = A.track && index >= 0 && index < A.count && A.track[index] > 0 ? A.track[index] : index + 1;
    snprintf(number, sizeof(number), "%d", shown);
    lv_obj_t * n = lv_label_create(row);
    lv_label_set_text(n, number);
    lv_obj_add_style(n, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(n, body, 0);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(n, num_w);
    lv_obj_set_pos(n, pad, (h - lv_font_get_line_height(body)) / 2);
    lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * d = lv_label_create(row);
    lv_obj_add_style(d, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(d, body, 0);
    lv_obj_set_user_data(d, &dur_marker);
    lv_obj_align(d, LV_ALIGN_RIGHT_MID, -dur_right, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    if (A.state && index >= 0 && index < A.count && A.state[index] == 1)
        set_duration_text(d, (unsigned) (A.dur[index].duration_ms / 1000.0 + 0.5));
    else lv_label_set_text(d, "");
}

void gui_album_header_row_set_duration(lv_obj_t * row, unsigned seconds) {
    uint32_t n = row ? lv_obj_get_child_count(row) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * child = lv_obj_get_child(row, (int32_t) i);
        if (lv_obj_get_user_data(child) == &dur_marker) { set_duration_text(child, seconds); return; }
    }
}

/* ---- Polling and shutdown ------------------------------------------------ */
bool gui_album_header_poll(bool visible, bool allowed) {
    if (gui_album_cover_poll(visible) && A.root) show_sharp_cover();
    album_probe_poll(allowed);
    if (!A.dirty || !A.root) return false;
    A.dirty = false;
    return update_chips();
}

bool gui_album_header_busy(void) { return album_probe_busy() || gui_album_cover_busy(); }

void gui_album_header_shutdown(int64_t deadline_ms) {
    gui_album_cover_shutdown();
    album_probe_shutdown(deadline_ms);
}
