#include "gui_album_header.h"
#include "gui_album_facts.h"
#include "gui_album_cover.h"
#include "../library_thumbnails.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../assets/assets.h"
#include "../../../audio/probe/track_probe.h"
#include "../../../core/system/runtime/board_config.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---- Per-album UI state (UI thread only) -------------------------------- */
typedef struct {
    uint64_t key;
    int count;
    char ** paths;      /* own copies: a probe batch outlives the entry array */
    uint8_t * state;    /* 0 unknown, 1 probed, 2 failed */
    double * secs;
    uint8_t * codec;
    uint8_t * hires;    /* DSD, above 48 kHz or above 16 bit */
    int * track;
    bool have_record, dirty;
    int year;
    char genre[128], hires_icon[96];
    lv_obj_t * root, * info, * chips, * cover_img;
    int cover_px, info_w;
    bool info_fixed;
    int64_t cover_song_id;
    uint64_t cover_key;
} album_t;
static album_t A;

static track_probe_job_t * job;
static uint64_t job_key;
static int * batch;
static int batch_n, start_fails;
static bool job_cancelled;
static uint32_t retry_tick;
static char dur_marker;

static void state_free(void) {
    for (int i = 0; A.paths && i < A.count; i++) free(A.paths[i]);
    free(A.paths); free(A.state); free(A.secs); free(A.codec); free(A.hires); free(A.track);
    memset(&A, 0, sizeof(A));
}

static void state_reset(uint64_t key, const group_song_entry_t * entries, int count) {
    state_free();
    A.key = key;
    A.paths = calloc((size_t) count + 1, sizeof(char *));
    A.state = calloc((size_t) count + 1, 1);
    A.secs = calloc((size_t) count + 1, sizeof(double));
    A.codec = calloc((size_t) count + 1, 1);
    A.hires = calloc((size_t) count + 1, 1);
    if (!A.paths || !A.state || !A.secs || !A.codec || !A.hires) { state_free(); return; }
    A.count = count;
    for (int i = 0; i < count; i++) A.paths[i] = strdup(entries[i].path ? entries[i].path : "");
    start_fails = 0;
}

static void load_record(void) {
    free(A.track);
    A.track = NULL;
    A.have_record = gui_album_facts_lookup(A.key, A.count, &A.year, A.genre, sizeof(A.genre), &A.track);
}

static bool settled(void) {
    if (!A.state || A.count <= 0) return false;
    for (int i = 0; i < A.count; i++) if (A.state[i] == 0) return false;
    return true;
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

static int32_t chip_height(void) {
    return lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) + 2 * BOARD_SCALE_PX(5) + 2;
}

/* Returns the width the chip takes, for the wrap check. */
static int32_t add_chip(const char * text, bool gold) {
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
    int32_t width = text_width(text, font) + 2 * padx + 2;
    if (gold) {
        lv_obj_set_style_border_color(chip, amber, 0);
        lv_obj_set_style_bg_color(chip, amber, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
        lv_obj_t * icon = lv_image_create(chip);
        lv_image_set_src(icon, asset_path(A.hires_icon));
        lv_obj_set_size(icon, line + 2, line + 2);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        width += line + 2 + gap;
    } else {
        lv_obj_add_style(chip, &style_theme_card_bg, 0);
    }
    lv_obj_t * label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    if (gold) lv_obj_set_style_text_color(label, amber, 0);
    else lv_obj_add_style(label, &style_theme_text_primary, 0);
    return width;
}

/* Hides a chip whose data is not known; never shows a partial total. */
static void update_chips(void) {
    if (!A.chips) return;
    lv_obj_clean(A.chips);
    int32_t used = 0, gap = BOARD_SCALE_PX(8), shown = 0;
    if (settled()) {
        bool all_ok = true, mixed = false, hires = true;
        double total = 0;
        for (int i = 0; i < A.count; i++) {
            if (A.state[i] != 1) { all_ok = false; continue; }
            total += A.secs[i];
            if (!A.hires[i]) hires = false;
            if (A.codec[i] != A.codec[0]) mixed = true;
        }
        const char * format = all_ok ? (mixed ? TR("Mixed") : audio_codec_name((audio_codec_t) A.codec[0])) : "";
        if (format[0]) { used += add_chip(format, false); shown++; }
        if (all_ok && hires && A.hires_icon[0]) { used += add_chip(TR("Hi-Res"), true); shown++; }
        if (all_ok && total > 0.0) {
            char text[48];
            int minutes = (int) ((total + 30.0) / 60.0);
            if (minutes < 1) minutes = 1;
            if (minutes >= 60) snprintf(text, sizeof(text), TR("%d hr %d min"), minutes / 60, minutes % 60);
            else snprintf(text, sizeof(text), TR("%d min"), minutes);
            used += add_chip(text, false); shown++;
        }
    }
    used += shown > 1 ? (shown - 1) * gap : 0;
    /* Chips that would wrap push the text column taller than the cover. */
    lv_obj_set_height(A.info, A.info_fixed && used <= A.info_w ? A.cover_px : LV_SIZE_CONTENT);
}

static lv_obj_t * pill_button(lv_obj_t * parent, int32_t w, int32_t h, const char * text, bool filled,
                              lv_event_cb_t cb) {
    lv_obj_t * button = plain(parent);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
    if (filled) {
        lv_obj_add_style(button, gui_theme_accent_style(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, lv_color_brightness(accent_lv_color()) > 160
            ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white(), 0);
    } else {
        lv_obj_add_style(button, &style_theme_card_bg, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, BOARD_SCALE_PX(2), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
    }
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    return button;
}

void gui_album_header_build(lv_obj_t * list, const gui_album_header_params_t * p) {
    int count = p->entries ? p->count : 0;
    uint64_t key = gui_album_facts_seed(count);
    for (int i = 0; i < count; i++) key = gui_album_facts_add(key, p->entries[i].path);
    if (key != A.key || count != A.count || !A.paths) state_reset(key, p->entries, count);
    if (!A.have_record && count > 0) load_record();
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
    lv_obj_set_style_pad_row(A.info, col_gap, 0);
    lv_obj_set_flex_flow(A.info, LV_FLEX_FLOW_COLUMN);
    int32_t used = chip_height();
    lv_obj_t * title = text_label(A.info, p->album && p->album[0] ? p->album : TR("Unknown album"),
                                  GUI_FONT_ROLE_TITLE, &style_theme_text_primary, A.info_w, 2);
    used += lv_obj_get_height(title) + col_gap;
    if (p->album_artist && p->album_artist[0]) {
        lv_obj_t * artist = text_label(A.info, p->album_artist, GUI_FONT_ROLE_ROW, gui_theme_accent_style(), A.info_w, 1);
        used += lv_obj_get_height(artist) + col_gap;
    }
    char meta[160] = "";
    if (A.year > 0) snprintf(meta, sizeof(meta), "%d", A.year);
    if (A.genre[0]) {
        size_t len = strlen(meta);
        snprintf(meta + len, sizeof(meta) - len, "%s%s", len ? " \xC2\xB7 " : "", A.genre);
    }
    if (meta[0]) {
        lv_obj_t * m = text_label(A.info, meta, GUI_FONT_ROLE_BODY, &style_theme_text_muted, A.info_w, 1);
        used += lv_obj_get_height(m) + col_gap;
    }
    lv_obj_t * spacer = plain(A.info);
    lv_obj_set_size(spacer, 1, 0);
    lv_obj_set_flex_grow(spacer, 1);
    A.chips = plain(A.info);
    lv_obj_set_size(A.chips, A.info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(A.chips, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_row(A.chips, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(A.chips, LV_FLEX_FLOW_ROW_WRAP);
    A.info_fixed = used <= cs;
    update_chips();

    if (p->show_buttons) {
        int32_t bgap = BOARD_SCALE_PX(12), bw = (row_w - bgap) / 2;
        int32_t bh = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW)) + BOARD_SCALE_PX(20);
        if (bh < BOARD_SCALE_PX(56)) bh = BOARD_SCALE_PX(56);
        lv_obj_t * buttons = plain(A.root);
        lv_obj_set_size(buttons, row_w, bh);
        lv_obj_set_style_pad_column(buttons, bgap, 0);
        lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
        char play[64], shuffle[64];
        snprintf(play, sizeof(play), LV_SYMBOL_PLAY "  %s", TR("Play"));
        snprintf(shuffle, sizeof(shuffle), LV_SYMBOL_SHUFFLE "  %s", TR("Shuffle"));
        pill_button(buttons, bw, bh, play, true, p->play_cb);
        pill_button(buttons, bw, bh, shuffle, false, p->shuffle_cb);
    }
    A.dirty = false;
}

void gui_album_header_release_cover(void) { gui_album_cover_release(); }

void gui_album_header_forget(void) {
    A.root = A.info = A.chips = A.cover_img = NULL;
}

/* ---- Cover --------------------------------------------------------------- */
/* The sharp cover replaces the 72 px thumbnail once it is decoded. */
static bool show_sharp_cover(void) {
    const lv_image_dsc_t * dsc = A.cover_img && A.cover_song_id > 0 ? gui_album_cover_get(A.cover_song_id) : NULL;
    if (!dsc) return false;
    lv_image_set_src(A.cover_img, dsc);
    lv_obj_set_size(A.cover_img, A.cover_px, A.cover_px);
    lv_image_set_inner_align(A.cover_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_align(A.cover_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(A.cover_img, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void gui_album_header_refresh_cover(lv_obj_t * list) {
    if (!A.root || !A.cover_img || A.cover_song_id <= 0 || show_sharp_cover()) return;
    /* The shared decorator pads a row for a thumbnail; this header lays out
     * its own children, so undo that and let the image fill the cover box. */
    library_thumbnails_decorate_group_row(list, A.root, A.cover_img, 0, 0, A.cover_song_id, A.cover_key);
    lv_obj_set_style_pad_left(A.root, 0, 0);
    lv_obj_set_size(A.cover_img, A.cover_px, A.cover_px);
    lv_image_set_inner_align(A.cover_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_align(A.cover_img, LV_ALIGN_CENTER, 0, 0);
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
        set_duration_text(d, (unsigned) (A.secs[index] + 0.5));
    else lv_label_set_text(d, "");
}

void gui_album_header_row_set_duration(lv_obj_t * row, unsigned seconds) {
    uint32_t n = row ? lv_obj_get_child_count(row) : 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * child = lv_obj_get_child(row, (int32_t) i);
        if (lv_obj_get_user_data(child) == &dur_marker) { set_duration_text(child, seconds); return; }
    }
}

/* ---- Probing every track -------------------------------------------------- */
static bool duration_ok(const audio_current_format_info_t * info) {
    return info->valid && info->duration_seconds > 0.0 && info->duration_seconds <= UINT_MAX - 1.0;
}

static void store(int index, const audio_current_format_info_t * info) {
    A.state[index] = 1;
    A.hires[index] = info->is_dsd || info->codec == AUDIO_CODEC_DSD ||
                     info->source_sample_rate > 48000 || info->source_bit_depth > 16;
    A.secs[index] = info->duration_seconds;
    A.codec[index] = (uint8_t) info->codec;
    A.dirty = true;
}

void gui_album_header_note_probe(int index, const audio_current_format_info_t * info) {
    if (!A.state || index < 0 || index >= A.count || !info || !duration_ok(info)) return;
    store(index, info);
}

static void free_job(void) {
    track_probe_job_destroy(job);
    job = NULL;
    free(batch);
    batch = NULL;
    batch_n = 0;
}

void gui_album_header_poll(bool visible, bool allowed) {
    if (gui_album_cover_poll(visible) && A.root) show_sharp_cover();
    if (job) {
        bool current = job_key == A.key && A.state;
        if ((!allowed || !current) && !job_cancelled) { track_probe_job_cancel(job); job_cancelled = true; }
        bool done = track_probe_job_done(job), drained = false;
        for (int budget = 0; budget < 8; budget++) {
            size_t index;
            audio_current_format_info_t info;
            if (!track_probe_job_next(job, &index, &info)) { drained = true; break; }
            if (!current || (int) index >= batch_n || batch[index] >= A.count) continue;
            if (duration_ok(&info)) store(batch[index], &info);
            /* A cancelled job may publish empty results for skipped files. */
            else if (!job_cancelled && A.state[batch[index]] != 1) { A.state[batch[index]] = 2; A.dirty = true; }
        }
        if (done && drained) free_job();
    }
    if (!job && allowed && A.state && A.count > 0 &&
        (start_fails == 0 || lv_tick_elaps(retry_tick) >= 1000)) {
        const char * paths[TRACK_PROBE_MAX_TRACKS];
        int * picked = malloc(sizeof(int) * TRACK_PROBE_MAX_TRACKS);
        int n = 0;
        for (int i = 0; picked && i < A.count && n < TRACK_PROBE_MAX_TRACKS; i++) {
            if (A.state[i] != 0) continue;
            if (strlen(A.paths[i]) >= TRACK_PROBE_MAX_PATH_BYTES) { A.state[i] = 2; A.dirty = true; continue; }
            paths[n] = A.paths[i];
            picked[n++] = i;
        }
        if (n > 0) {
            job = track_probe_job_start(paths, (size_t) n);
            if (job) { batch = picked; batch_n = n; job_key = A.key; job_cancelled = false; start_fails = 0; }
            else {
                free(picked);
                retry_tick = lv_tick_get();
                /* Give up after three refusals so the chips can settle. */
                if (++start_fails >= 3)
                    for (int i = 0; i < A.count; i++) if (A.state[i] == 0) { A.state[i] = 2; A.dirty = true; }
            }
        } else free(picked);
    }
    if (A.dirty && A.root) { A.dirty = false; update_chips(); }
}

void gui_album_header_shutdown(void) {
    gui_album_cover_shutdown();
    if (job) track_probe_job_cancel_and_destroy(job);
    job = NULL;
    free(batch);
    batch = NULL;
    batch_n = 0;
    state_free();
}
