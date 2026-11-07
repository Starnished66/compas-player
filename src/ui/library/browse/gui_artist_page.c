#include "gui_artist_page.h"
#include "../../assets/gui_image_dsc.h"
#include "gui_artist_data.h"
#include "../library_thumbnails.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../widgets/lists/compact_list.h"
#include "../../assets/assets.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../../core/persistence/settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILES ARTIST_PAGE_MOSAIC_ALBUMS
#define TILE_BYTES ((size_t) ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX * 2)

typedef struct {
    lv_obj_t * list, * root;
    gui_artist_page_params_t params;
    char name[128];
    gui_artist_revisions_t rev;   /* what the header currently shows */
    uint32_t request_tick, artist_request_tick, artist_commits;
    uint16_t * artist_pixels;
    bool artist_ready, artist_missing, artist_enabled, artist_checked;
    bool songs_long_fired;
    /* The header's widgets live as long as the list: later facts only change
     * text and visibility. */
    lv_obj_t * name_label, * counts_label, * genre_label, * chips, * year_chip, * year_label, * total_chip, * total_label;
    lv_obj_t * box;               /* the mosaic square */
    char counts_text[160], genre_text[128], year_text[32], total_text[48];
    bool has_buttons;
    int32_t header_height;        /* what compact_list_set_header() was last told */
    /* Mosaic tiles own a copy of the pixels, so an LRU eviction cannot
     * leave them dangling. */
    lv_obj_t * tile_img[TILES];
    uint16_t * tile_pixels[TILES];
    lv_image_dsc_t tile_dsc[TILES];
    uint64_t tile_key[TILES];
    int64_t tile_song[TILES];
    uint8_t tile_state[TILES]; /* 0 waiting, 1 shown, 2 no cover */
    uint8_t tile_shown[TILES]; /* what the image holds: 0 nothing, 1 placeholder, 2 the pixels */
    int tile_album[TILES];     /* which of the albums (0..3) a tile shows */
    int tile_count;            /* 0, 1 or 4 tiles in use */
    int tile_laid_out;         /* the count the tile geometry was set for */
    bool tiles_due;            /* the tile set changed: look the cache up once */
    uint32_t thumb_commits;    /* library_thumbnails_commit_count() at the last look */
    int32_t cover_px;
    lv_obj_t * songs_all_label;   /* "All N songs >" on the Songs row (NULL without songs) */
    int32_t songs_all_w;          /* room for it beside the row's title */
} page_t;
/* Local Music and Subsonic may both retain their screens on the back stack. */
static page_t pages[2];

static page_t * page_for(lv_obj_t * list, bool create) {
    if (!list) return NULL;
    for (int i = 0; i < 2; i++) if (pages[i].list == list) return &pages[i];
    for (int i = 0; create && i < 2; i++) if (!pages[i].list) return &pages[i];
    return NULL;
}

static lv_obj_t * plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Sets the text of a fixed-width label of at most max_lines lines (ending in
 * "..." beyond that) and fits its height to it. Does nothing when unchanged. */
static void fit_label(lv_obj_t * label, const char * text, gui_font_role_t role, int32_t width, int max_lines) {
    const char * old = lv_label_get_text(label);
    if (old && !strcmp(old, text)) return;
    const lv_font_t * font = gui_theme_font(role);
    lv_label_set_text(label, text);
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int32_t line = lv_font_get_line_height(font);
    int32_t height = size.y < line ? line : size.y;
    if (height > line * max_lines) height = line * max_lines;
    lv_obj_set_size(label, width, height);
}

static lv_obj_t * text_label(lv_obj_t * parent, const char * text, gui_font_role_t role,
                             lv_style_t * style, int32_t width, int max_lines) {
    lv_obj_t * label = lv_label_create(parent);
    lv_obj_add_style(label, style, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, ""); /* so fit_label() sizes it for `text` */
    fit_label(label, text, role, width, max_lines);
    return label;
}

static lv_obj_t * chip(lv_obj_t * parent, lv_obj_t ** label_out) {
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    lv_obj_t * c = plain(parent);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(c, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_ver(c, BOARD_SCALE_PX(5), 0);
    lv_obj_add_style(c, &style_theme_card_bg, 0);
    lv_obj_t * label = lv_label_create(c);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    *label_out = label;
    return c;
}

static void button_click_cb(lv_event_t * e) {
    void (*fn)(void) = lv_event_get_user_data(e);
    if (fn && lv_event_get_code(e) == LV_EVENT_CLICKED) fn();
}

static void songs_row_cb(lv_event_t * e) {
    page_t * p = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if (p->params.on_songs_press) p->params.on_songs_press(true);
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (p->params.on_songs_press) p->params.on_songs_press(false);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (p->params.on_songs_press) p->params.on_songs_press(false);
        p->songs_long_fired = true;
        if (p->params.on_menu) p->params.on_menu();
    } else if (code == LV_EVENT_CLICKED) {
        if (p->songs_long_fired) { p->songs_long_fired = false; return; }
        if (p->params.on_songs) p->params.on_songs();
    }
}

/* ---- Mosaic -------------------------------------------------------------- */
/* One album: a single cover. Two or three: the covers repeated so the square
 * stays full. Four or more: the first four. An artist photo counts as the
 * first picture, ahead of the albums. */
static void plan_tiles(page_t * p, const gui_artist_summary_t * s) {
    int n = s->ready ? s->mosaic_count : 0;
    int photo = p->artist_ready && p->artist_enabled && n > 0;
    int m = n + photo;
    static const int two[4] = { 0, 1, 1, 0 }, three[4] = { 0, 1, 2, 0 }, four[4] = { 0, 1, 2, 3 };
    const int * map = m >= 4 ? four : m == 3 ? three : two;
    p->tile_count = m <= 0 ? 0 : m == 1 ? 1 : 4;
    for (int i = 0; i < p->tile_count; i++) {
        int a = (m == 1 ? 0 : map[i]) - photo; /* -1 is the photo */
        uint64_t key = a < 0 ? UINT64_MAX : s->mosaic_key[a];
        int64_t song = a < 0 ? 0 : s->mosaic_song_id[a];
        if (p->tile_key[i] != key || p->tile_song[i] != song) {
            p->tile_key[i] = key;
            p->tile_song[i] = song;
            p->tile_state[i] = key ? 0 : 2;
            p->tiles_due = true;
        }
        p->tile_album[i] = a;
    }
}

/* Gives each tile its square for the current tile count; only when it changed. */
static void layout_tiles(page_t * p) {
    if (p->tile_laid_out == p->tile_count) return;
    p->tile_laid_out = p->tile_count;
    int32_t cs = p->cover_px, half = cs / 2;
    for (int i = 0; i < TILES; i++) {
        if (i >= p->tile_count) { lv_obj_add_flag(p->tile_img[i], LV_OBJ_FLAG_HIDDEN); continue; }
        int32_t x = p->tile_count == 1 ? 0 : (i % 2) * half, y = p->tile_count == 1 ? 0 : (i / 2) * half;
        int32_t w = p->tile_count == 1 ? cs : (i % 2 ? cs - half : half);
        int32_t h = p->tile_count == 1 ? cs : (i / 2 ? cs - half : half);
        lv_obj_set_pos(p->tile_img[i], x, y);
        lv_obj_set_size(p->tile_img[i], w, h);
        lv_obj_remove_flag(p->tile_img[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Assigns the tile's source only when what it shows actually changes. */
static void show_tile(page_t * p, int i) {
    if (!p->tile_img[i] || i >= p->tile_count) return;
    if (p->tile_state[i] == 0 && p->tile_shown[i] == 2) return;
    uint8_t want = p->tile_state[i] == 1 && p->tile_pixels[i] ? 2 : 1;
    if (p->tile_shown[i] == want) return;
    p->tile_shown[i] = want;
    if (want == 2) lv_image_set_src(p->tile_img[i], &p->tile_dsc[i]);
    else lv_image_set_src(p->tile_img[i], asset_path("touch_list/list_default_album.png"));
}

static void create_mosaic(page_t * p, lv_obj_t * parent, int32_t cs) {
    p->cover_px = cs;
    lv_obj_t * box = plain(parent);
    p->box = box;
    lv_obj_set_size(box, cs, cs);
    lv_obj_set_style_radius(box, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_add_style(box, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    for (int i = 0; i < TILES; i++) {
        lv_obj_t * img = lv_image_create(box);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
        p->tile_img[i] = img;
        p->tile_shown[i] = 0;
    }
    p->tile_laid_out = 0;
}

static bool fill_tiles(page_t * p, lv_obj_t * list) {
    bool waiting = false;
    for (int i = 0; i < p->tile_count; i++) {
        if (p->tile_state[i] != 0 || !p->tile_key[i]) continue;
        if (!p->tile_pixels[i]) p->tile_pixels[i] = malloc(TILE_BYTES);
        bool no_art = false;
        if (p->tile_album[i] < 0 && p->tile_pixels[i]) {
            memcpy(p->tile_pixels[i], p->artist_pixels, TILE_BYTES);
            gui_image_dsc_init_rgb565(&p->tile_dsc[i], p->tile_pixels[i], ALBUM_THUMBNAIL_PX);
            p->tile_state[i] = 1;
            p->tile_shown[i] = 0;
            show_tile(p, i);
        } else if (p->tile_pixels[i] && (p->params.copy_tile ? p->params.copy_tile(p->params.source, p->tile_album[i], p->tile_pixels[i], &no_art) :
            library_thumbnails_copy_album(p->tile_key[i], p->tile_pixels[i], &no_art))) {
            gui_image_dsc_init_rgb565(&p->tile_dsc[i], p->tile_pixels[i], ALBUM_THUMBNAIL_PX);
            p->tile_state[i] = 1;
            p->tile_shown[i] = 0;
            show_tile(p, i);
            lv_obj_invalidate(p->tile_img[i]);
        } else if (no_art) {
            p->tile_state[i] = 2;
            show_tile(p, i);
        } else {
            waiting = true;
        }
    }
    if (!p->params.copy_tile && waiting && lv_tick_elaps(p->request_tick) >= 400) {
        p->request_tick = lv_tick_get();
        for (int i = 0; i < p->tile_count; i++)
            if (p->tile_state[i] == 0 && p->tile_key[i] && p->tile_album[i] >= 0) library_thumbnails_request_album(list, p->tile_song[i], p->tile_key[i]);
    }
    if (p->params.copy_tile) p->request_tick = lv_tick_get();
    return waiting;
}

/* ---- Header -------------------------------------------------------------- */
static void format_total(char * text, size_t size, double seconds) {
    int minutes = (int) ((seconds + 30.0) / 60.0);
    if (minutes < 1) minutes = 1;
    if (minutes >= 60) snprintf(text, size, TR("%d hr %d min"), minutes / 60, minutes % 60);
    else snprintf(text, size, TR("%d min"), minutes);
}

/* Text of a chip or label as it should be shown, from the facts. */
static void summary_texts(const gui_artist_summary_t * s, char * genre, size_t genre_size,
                          char * year, size_t year_size, char * total, size_t total_size) {
    genre[0] = year[0] = total[0] = 0;
    if (!s->ready) return;
    snprintf(genre, genre_size, "%s", s->genre);
    if (s->year_min > 0) {
        if (s->year_max > s->year_min) snprintf(year, year_size, "%d\xE2\x80\x93%d", s->year_min, s->year_max);
        else snprintf(year, year_size, "%d", s->year_min);
    }
    if (s->duration_known && s->total_seconds > 0.0) format_total(total, total_size, s->total_seconds);
}

static void set_hidden(lv_obj_t * o, bool hidden) {
    if (hidden == lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void apply_params(page_t * p);

/* Brings the header's text, chips and mosaic in line with `s`. Only what
 * changed is touched; returns true when something that can move the layout did. */
static bool apply_summary(page_t * p, const gui_artist_summary_t * s) {
    int32_t info_w = LIST_ROW_WIDTH_WIDE - p->cover_px - BOARD_SCALE_PX(18);
    char genre[128], year[32], total[48];
    summary_texts(s, genre, sizeof(genre), year, sizeof(year), total, sizeof(total));
    bool layout = false;
    if (s->ready && p->params.counts_from_summary && p->params.songs != s->songs) {
        p->params.songs = s->songs;
        apply_params(p);
        layout = true;
    }
    if (strcmp(genre, p->genre_text)) {
        snprintf(p->genre_text, sizeof(p->genre_text), "%s", genre);
        if (genre[0]) fit_label(p->genre_label, genre, GUI_FONT_ROLE_BODY, info_w, 1);
        set_hidden(p->genre_label, !genre[0]);
        layout = true;
    }
    if (strcmp(year, p->year_text)) {
        snprintf(p->year_text, sizeof(p->year_text), "%s", year);
        if (year[0]) lv_label_set_text(p->year_label, year);
        set_hidden(p->year_chip, !year[0]);
        layout = true;
    }
    if (strcmp(total, p->total_text)) {
        snprintf(p->total_text, sizeof(p->total_text), "%s", total);
        if (total[0]) lv_label_set_text(p->total_label, total);
        set_hidden(p->total_chip, !total[0]);
        layout = true;
    }
    if (layout) set_hidden(p->chips, !p->year_text[0] && !p->total_text[0]);
    plan_tiles(p, s);
    layout_tiles(p);
    for (int i = 0; i < p->tile_count; i++) show_tile(p, i);
    return layout;
}

/* The header's height can only change with the text and chips; tell the list
 * once it has been laid out again. */
static void relayout(page_t * p) {
    lv_obj_update_layout(p->root);
    int32_t height = lv_obj_get_height(p->root) + BOARD_SCALE_PX(8);
    if (height != p->header_height) {
        p->header_height = height;
        compact_list_set_header(p->list, p->root, height);
    }
}

static void create_info(page_t * p, lv_obj_t * top, int32_t info_w) {
    lv_obj_t * info = plain(top);
    lv_obj_set_size(info, info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    p->name_label = text_label(info, p->name, GUI_FONT_ROLE_TITLE, &style_theme_text_primary, info_w, 2);
    p->counts_label = text_label(info, "", GUI_FONT_ROLE_BODY, gui_theme_accent_style(), info_w, 2);
    p->genre_label = text_label(info, "", GUI_FONT_ROLE_BODY, &style_theme_text_muted, info_w, 1);
    lv_obj_add_flag(p->genre_label, LV_OBJ_FLAG_HIDDEN);
    p->chips = plain(info);
    lv_obj_set_size(p->chips, info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(p->chips, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_row(p->chips, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(p->chips, LV_FLEX_FLOW_ROW_WRAP);
    p->year_chip = chip(p->chips, &p->year_label);
    p->total_chip = chip(p->chips, &p->total_label);
    lv_obj_add_flag(p->year_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->total_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->chips, LV_OBJ_FLAG_HIDDEN);
}

static void create_songs_labels(page_t * p, lv_obj_t * row, int32_t row_w, int32_t pad, const lv_font_t * row_font) {
    lv_obj_t * title = lv_label_create(row);
    lv_label_set_text(title, TR("Songs"));
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, row_font, 0);
    int32_t title_x = pad + lv_font_get_line_height(row_font) + BOARD_SCALE_PX(12);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, title_x, 0);
    lv_point_t title_size;
    lv_text_get_size(&title_size, TR("Songs"), row_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    p->songs_all_w = row_w - pad - (title_x + title_size.x + BOARD_SCALE_PX(12));
    p->songs_all_label = lv_label_create(row);
    lv_obj_add_style(p->songs_all_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(p->songs_all_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_align(p->songs_all_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(p->songs_all_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(p->songs_all_label, p->songs_all_w > 0 ? p->songs_all_w : 0);
    lv_obj_align(p->songs_all_label, LV_ALIGN_RIGHT_MID, -pad, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p->songs_all_label, LV_OBJ_FLAG_CLICKABLE);
}

static void create_songs_row(page_t * p, int32_t row_w) {
    const lv_font_t * row_font = gui_theme_font(GUI_FONT_ROLE_ROW);
    int32_t rh = lv_font_get_line_height(row_font) + 2 * BOARD_SCALE_PX(16), pad = BOARD_SCALE_PX(16);
    lv_obj_t * row = plain(p->root);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_size(row, row_w, rh);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_LONG_PRESSED, p);
    lv_obj_add_event_cb(row, ui_clear_flag_on_press_cb, LV_EVENT_PRESSED, &p->songs_long_fired);
    lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_PRESSED, p);
    lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_PRESS_LOST, p);
    lv_obj_t * icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_LIST);
    lv_obj_add_style(icon, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(icon, row_font, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, pad, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    create_songs_labels(p, row, row_w, pad, row_font);
}

static void create_header(page_t * p) {
    int32_t row_w = LIST_ROW_WIDTH_WIDE, cs = BOARD_SCALE_PX(170), gap = BOARD_SCALE_PX(18);
    int32_t info_w = row_w - cs - gap;
    p->root = plain(p->list);
    lv_obj_set_size(p->root, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(p->root, BOARD_SCALE_PX(14), 0);
    lv_obj_set_flex_flow(p->root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * top = plain(p->root);
    lv_obj_set_size(top, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(top, gap, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_mosaic(p, top, cs);

    create_info(p, top, info_w);

    p->has_buttons = p->params.songs > 0;
    if (p->has_buttons) {
        gui_play_buttons_build(p->root, row_w, button_click_cb, p->params.on_play, button_click_cb, p->params.on_shuffle);

        create_songs_row(p, row_w);
    }
    lv_obj_t * section = text_label(p->root, TR("Albums"), GUI_FONT_ROLE_BODY, &style_theme_text_muted, row_w, 1);
    lv_obj_set_style_pad_left(section, BOARD_SCALE_PX(8), 0);
    lv_obj_set_width(section, row_w);
}

/* Name and counts: known at once, from the opening list row. */
static void apply_params(page_t * p) {
    int32_t info_w = LIST_ROW_WIDTH_WIDE - p->cover_px - BOARD_SCALE_PX(18);
    char text[160], part[64];
    fit_label(p->name_label, p->name, GUI_FONT_ROLE_TITLE, info_w, 2);
    snprintf(text, sizeof(text), TR_N("%d album", "%d albums", p->params.albums), p->params.albums);
    snprintf(part, sizeof(part), TR_N("%d song", "%d songs", p->params.songs), p->params.songs);
    size_t len = strlen(text);
    snprintf(text + len, sizeof(text) - len, " \xC2\xB7 %s", part);
    fit_label(p->counts_label, text, GUI_FONT_ROLE_BODY, info_w, 2);
    if (p->songs_all_label) {
        /* Long translations collide with the row's title: fall back to the bare count. */
        snprintf(text, sizeof(text), TR_N("All %d song", "All %d songs", p->params.songs), p->params.songs);
        len = strlen(text);
        snprintf(text + len, sizeof(text) - len, "  " LV_SYMBOL_RIGHT);
        lv_point_t size;
        lv_text_get_size(&size, text, gui_theme_font(GUI_FONT_ROLE_BODY), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > p->songs_all_w)
            snprintf(text, sizeof(text), "%d  " LV_SYMBOL_RIGHT, p->params.songs);
        lv_label_set_text(p->songs_all_label, text);
    }
}

void gui_artist_page_build(lv_obj_t * list, const gui_artist_page_params_t * params) {
    if (!list || !params) return;
    page_t * p = page_for(list, true);
    if (!p) return;
    bool reuse = p->root && (p->params.songs > 0) == (params->songs > 0);
    if (p->root && !reuse) { lv_obj_delete(p->root); p->root = NULL; }
    p->list = list;
    p->params = *params;
    snprintf(p->name, sizeof(p->name), "%s", params->name ? params->name : "");
    p->params.name = p->name;
    for (int i = 0; i < TILES; i++) { p->tile_key[i] = 0; p->tile_state[i] = 0; p->tile_shown[i] = 0; }
    p->tile_count = 0;
    p->tiles_due = true;
    p->request_tick = p->artist_request_tick = 0;
    p->artist_ready = p->artist_missing = p->artist_checked = false;
    p->artist_enabled = current_settings.show_artist_images;
    if (!reuse) {
        p->genre_text[0] = p->year_text[0] = p->total_text[0] = 0;
        p->header_height = -1;
        p->songs_all_label = NULL;
        create_header(p);
    }
    /* Facts and revisions are read together, so a publication between the two
     * can never leave the header marked current without showing it. */
    gui_artist_summary_t s;
    if (p->params.snapshot) p->params.snapshot(p->params.source, &s, &p->rev);
    else gui_artist_data_snapshot(&s, &p->rev);
    apply_params(p);
    apply_summary(p, &s);
    relayout(p);
}

void gui_artist_page_forget(lv_obj_t * list) {
    page_t * p = page_for(list, false);
    if (!p) return;
    if (p->root) lv_obj_delete(p->root);
    p->root = NULL; p->list = NULL;
    p->name_label = p->counts_label = p->genre_label = p->chips = p->year_chip = p->year_label = NULL;
    p->total_chip = p->total_label = p->box = p->songs_all_label = NULL;
    for (int i = 0; i < TILES; i++) p->tile_img[i] = NULL;
}

bool gui_artist_page_poll(lv_obj_t * list, bool visible) {
    page_t * p = page_for(list, false);
    if (!p) return false;
    if (!p->root || p->list != list || !visible) return false;
    gui_artist_summary_t s;
    gui_artist_revisions_t rev;
    if (p->params.snapshot) p->params.snapshot(p->params.source, &s, &rev);
    else gui_artist_data_snapshot(&s, &rev);
    bool refetch = rev.albums != p->rev.albums;
    if (rev.summary != p->rev.summary || rev.duration != p->rev.duration) {
        if (apply_summary(p, &s)) relayout(p);
    }
    bool enabled = current_settings.show_artist_images;
    bool photo_pending = enabled && !p->artist_ready && !p->artist_missing &&
                         (p->params.copy_artist || s.artist_key);
    bool photo_due = !p->artist_checked || library_thumbnails_commit_count() != p->artist_commits ||
                     lv_tick_elaps(p->artist_request_tick) >= 400;
    if (photo_pending && photo_due) {
        p->artist_checked = true;
        p->artist_request_tick = lv_tick_get();
        p->artist_commits = library_thumbnails_commit_count();
        if (!p->artist_pixels) p->artist_pixels = malloc(TILE_BYTES);
        bool no_art = false;
        bool ready = p->artist_pixels && (p->params.copy_artist ?
            p->params.copy_artist(p->params.source, p->artist_pixels, &no_art) :
            library_thumbnails_copy_artist_photo(s.artist_key, p->artist_pixels, &no_art));
        if (ready) p->artist_ready = true;
        else if (no_art) p->artist_missing = true;
        else if (!p->params.copy_artist)
            library_thumbnails_request_artist_photo(list, s.artist_song_id, s.artist_key,
                                                    s.artist_group_kind, p->name);
    }
    if (p->artist_enabled != enabled || (p->artist_ready && photo_pending)) {
        bool hide_photo = p->artist_enabled && !enabled && p->tile_count > 0 && p->tile_album[0] < 0;
        if (hide_photo) p->tile_shown[0] = 0;
        p->artist_enabled = enabled;
        plan_tiles(p, &s);
        layout_tiles(p);
        for (int i = 0; i < p->tile_count; i++) show_tile(p, i);
    }
    p->rev = rev;
    /* Each look scans the thumbnail cache per tile: only when a tile is waiting
     * and something new landed, the set changed, or the re-request is due. */
    bool tile_waiting = false;
    for (int i = 0; i < p->tile_count; i++)
        if (p->tile_state[i] == 0 && p->tile_key[i]) tile_waiting = true;
    uint32_t commits = library_thumbnails_commit_count();
    if (tile_waiting && (p->tiles_due || commits != p->thumb_commits || lv_tick_elaps(p->request_tick) >= 400)) {
        p->tiles_due = false;
        p->thumb_commits = commits;
        (void) fill_tiles(p, list);
    }
    return refetch;
}

void gui_artist_page_release(lv_obj_t * list) {
    page_t * p = page_for(list, false);
    if (!p) return;
    free(p->artist_pixels);
    p->artist_pixels = NULL;
    p->artist_ready = p->artist_missing = false;
    for (int i = 0; i < TILES; i++) {
        free(p->tile_pixels[i]);
        p->tile_pixels[i] = NULL;
        p->tile_state[i] = 0;
        p->tile_shown[i] = 0;
        p->tile_key[i] = 0;
        p->tile_img[i] = NULL;
    }
}

/* "1999 · 8 tracks". */
void gui_artist_page_album_subtitle(char * sub, size_t room, int year, int tracks) {
    sub[0] = 0;
    if (year > 0) snprintf(sub, room, "%d", year);
    if (tracks > 0) {
        size_t len = strlen(sub);
        snprintf(sub + len, room - len, "%s", len ? " \xC2\xB7 " : "");
        len = strlen(sub);
        snprintf(sub + len, room - len, TR_N("%d track", "%d tracks", tracks), tracks);
    }
}
