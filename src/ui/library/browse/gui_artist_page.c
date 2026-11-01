#include "gui_artist_page.h"
#include "gui_artist_data.h"
#include "../library_thumbnails.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../widgets/lists/compact_list.h"
#include "../../assets/assets.h"
#include "../../../core/system/runtime/board_config.h"
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
    uint32_t request_tick;
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
static page_t P;

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
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        P.songs_long_fired = false; /* a long press that ended in a drag sends no CLICKED */
        if (P.params.on_songs_press) P.params.on_songs_press(true);
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (P.params.on_songs_press) P.params.on_songs_press(false);
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (P.params.on_songs_press) P.params.on_songs_press(false);
        P.songs_long_fired = true;
        if (P.params.on_menu) P.params.on_menu();
    } else if (code == LV_EVENT_CLICKED) {
        if (P.songs_long_fired) { P.songs_long_fired = false; return; }
        if (P.params.on_songs) P.params.on_songs();
    }
}

/* ---- Mosaic -------------------------------------------------------------- */
/* One album: a single cover. Two or three: the covers repeated so the square
 * stays full. Four or more: the first four. */
static void plan_tiles(const gui_artist_summary_t * s) {
    int n = s->ready ? s->mosaic_count : 0;
    static const int two[4] = { 0, 1, 1, 0 }, three[4] = { 0, 1, 2, 0 }, four[4] = { 0, 1, 2, 3 };
    const int * map = n >= 4 ? four : n == 3 ? three : two;
    P.tile_count = n <= 0 ? 0 : n == 1 ? 1 : 4;
    for (int i = 0; i < P.tile_count; i++) {
        int a = n == 1 ? 0 : map[i];
        if (P.tile_key[i] != s->mosaic_key[a] || P.tile_song[i] != s->mosaic_song_id[a]) {
            P.tile_key[i] = s->mosaic_key[a];
            P.tile_song[i] = s->mosaic_song_id[a];
            P.tile_state[i] = 0;
            P.tile_shown[i] = 0;
            P.tiles_due = true;
        }
        P.tile_album[i] = a;
    }
}

/* Gives each tile its square for the current tile count; only when it changed. */
static void layout_tiles(void) {
    if (P.tile_laid_out == P.tile_count) return;
    P.tile_laid_out = P.tile_count;
    int32_t cs = P.cover_px, half = cs / 2;
    for (int i = 0; i < TILES; i++) {
        if (i >= P.tile_count) { lv_obj_add_flag(P.tile_img[i], LV_OBJ_FLAG_HIDDEN); continue; }
        int32_t x = P.tile_count == 1 ? 0 : (i % 2) * half, y = P.tile_count == 1 ? 0 : (i / 2) * half;
        int32_t w = P.tile_count == 1 ? cs : (i % 2 ? cs - half : half);
        int32_t h = P.tile_count == 1 ? cs : (i / 2 ? cs - half : half);
        lv_obj_set_pos(P.tile_img[i], x, y);
        lv_obj_set_size(P.tile_img[i], w, h);
        lv_obj_remove_flag(P.tile_img[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* Assigns the tile's source only when what it shows actually changes. */
static void show_tile(int i) {
    if (!P.tile_img[i] || i >= P.tile_count) return;
    uint8_t want = P.tile_state[i] == 1 && P.tile_pixels[i] ? 2 : 1;
    if (P.tile_shown[i] == want) return;
    P.tile_shown[i] = want;
    if (want == 2) lv_image_set_src(P.tile_img[i], &P.tile_dsc[i]);
    else lv_image_set_src(P.tile_img[i], asset_path("touch_list/list_default_album.png"));
}

static void create_mosaic(lv_obj_t * parent, int32_t cs) {
    P.cover_px = cs;
    lv_obj_t * box = plain(parent);
    P.box = box;
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
        P.tile_img[i] = img;
        P.tile_shown[i] = 0;
    }
    P.tile_laid_out = 0;
}

static bool fill_tiles(lv_obj_t * list) {
    bool waiting = false;
    for (int i = 0; i < P.tile_count; i++) {
        if (P.tile_state[i] != 0 || !P.tile_key[i]) continue;
        if (!P.tile_pixels[i]) P.tile_pixels[i] = malloc(TILE_BYTES);
        bool no_art = false;
        if (P.tile_pixels[i] && library_thumbnails_copy_album(P.tile_key[i], P.tile_pixels[i], &no_art)) {
            P.tile_dsc[i] = (lv_image_dsc_t) { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .w = ALBUM_THUMBNAIL_PX, .h = ALBUM_THUMBNAIL_PX, .stride = ALBUM_THUMBNAIL_PX * 2 },
                .data_size = (uint32_t) TILE_BYTES, .data = (const uint8_t *) P.tile_pixels[i] };
            P.tile_state[i] = 1;
            show_tile(i);
            lv_obj_invalidate(P.tile_img[i]);
        } else if (no_art) {
            P.tile_state[i] = 2;
        } else {
            waiting = true;
        }
    }
    if (waiting && lv_tick_elaps(P.request_tick) >= 400) {
        P.request_tick = lv_tick_get();
        for (int i = 0; i < P.tile_count; i++)
            if (P.tile_state[i] == 0 && P.tile_key[i]) library_thumbnails_request_album(list, P.tile_song[i], P.tile_key[i]);
    }
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

static void apply_params(void);

/* Brings the header's text, chips and mosaic in line with `s`. Only what
 * changed is touched; returns true when something that can move the layout did. */
static bool apply_summary(const gui_artist_summary_t * s) {
    int32_t info_w = LIST_ROW_WIDTH_WIDE - P.cover_px - BOARD_SCALE_PX(18);
    char genre[128], year[32], total[48];
    summary_texts(s, genre, sizeof(genre), year, sizeof(year), total, sizeof(total));
    bool layout = false;
    if (strcmp(genre, P.genre_text)) {
        snprintf(P.genre_text, sizeof(P.genre_text), "%s", genre);
        if (genre[0]) fit_label(P.genre_label, genre, GUI_FONT_ROLE_BODY, info_w, 1);
        set_hidden(P.genre_label, !genre[0]);
        layout = true;
    }
    if (strcmp(year, P.year_text)) {
        snprintf(P.year_text, sizeof(P.year_text), "%s", year);
        if (year[0]) lv_label_set_text(P.year_label, year);
        set_hidden(P.year_chip, !year[0]);
        layout = true;
    }
    if (strcmp(total, P.total_text)) {
        snprintf(P.total_text, sizeof(P.total_text), "%s", total);
        if (total[0]) lv_label_set_text(P.total_label, total);
        set_hidden(P.total_chip, !total[0]);
        layout = true;
    }
    if (layout) set_hidden(P.chips, !P.year_text[0] && !P.total_text[0]);
    plan_tiles(s);
    layout_tiles();
    for (int i = 0; i < P.tile_count; i++) show_tile(i);
    return layout;
}

/* The header's height can only change with the text and chips; tell the list
 * once it has been laid out again. */
static void relayout(void) {
    lv_obj_update_layout(P.root);
    int32_t height = lv_obj_get_height(P.root) + BOARD_SCALE_PX(8);
    if (height != P.header_height) {
        P.header_height = height;
        compact_list_set_header(P.list, P.root, height);
    }
}

static void create_header(void) {
    int32_t row_w = LIST_ROW_WIDTH_WIDE, cs = BOARD_SCALE_PX(170), gap = BOARD_SCALE_PX(18);
    int32_t info_w = row_w - cs - gap;
    P.root = plain(P.list);
    lv_obj_set_size(P.root, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(P.root, BOARD_SCALE_PX(14), 0);
    lv_obj_set_flex_flow(P.root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * top = plain(P.root);
    lv_obj_set_size(top, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(top, gap, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    create_mosaic(top, cs);

    lv_obj_t * info = plain(top);
    lv_obj_set_size(info, info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(info, BOARD_SCALE_PX(4), 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    P.name_label = text_label(info, P.name, GUI_FONT_ROLE_TITLE, &style_theme_text_primary, info_w, 2);
    P.counts_label = text_label(info, "", GUI_FONT_ROLE_BODY, gui_theme_accent_style(), info_w, 2);
    P.genre_label = text_label(info, "", GUI_FONT_ROLE_BODY, &style_theme_text_muted, info_w, 1);
    lv_obj_add_flag(P.genre_label, LV_OBJ_FLAG_HIDDEN);
    P.chips = plain(info);
    lv_obj_set_size(P.chips, info_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(P.chips, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_row(P.chips, BOARD_SCALE_PX(6), 0);
    lv_obj_set_flex_flow(P.chips, LV_FLEX_FLOW_ROW_WRAP);
    P.year_chip = chip(P.chips, &P.year_label);
    P.total_chip = chip(P.chips, &P.total_label);
    lv_obj_add_flag(P.year_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.total_chip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(P.chips, LV_OBJ_FLAG_HIDDEN);

    P.has_buttons = P.params.songs > 0;
    if (P.has_buttons) {
        gui_play_buttons_build(P.root, row_w, button_click_cb, P.params.on_play, button_click_cb, P.params.on_shuffle);

        const lv_font_t * row_font = gui_theme_font(GUI_FONT_ROLE_ROW);
        int32_t rh = lv_font_get_line_height(row_font) + 2 * BOARD_SCALE_PX(16), pad = BOARD_SCALE_PX(16);
        lv_obj_t * row = plain(P.root);
        lv_obj_add_style(row, &pill_row_bg_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_set_size(row, row_w, rh);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, songs_row_cb, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_t * icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_LIST);
        lv_obj_add_style(icon, gui_theme_accent_style(), 0);
        lv_obj_set_style_text_font(icon, row_font, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, pad, 0);
        lv_obj_t * title = lv_label_create(row);
        lv_label_set_text(title, TR("Songs"));
        lv_obj_add_style(title, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(title, row_font, 0);
        int32_t title_x = pad + lv_font_get_line_height(row_font) + BOARD_SCALE_PX(12);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, title_x, 0);
        lv_point_t title_size;
        lv_text_get_size(&title_size, TR("Songs"), row_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        P.songs_all_w = row_w - pad - (title_x + title_size.x + BOARD_SCALE_PX(12));
        P.songs_all_label = lv_label_create(row);
        lv_obj_add_style(P.songs_all_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(P.songs_all_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_text_align(P.songs_all_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(P.songs_all_label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(P.songs_all_label, P.songs_all_w > 0 ? P.songs_all_w : 0);
        lv_obj_align(P.songs_all_label, LV_ALIGN_RIGHT_MID, -pad, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(P.songs_all_label, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t * section = text_label(P.root, TR("Albums"), GUI_FONT_ROLE_BODY, &style_theme_text_muted, row_w, 1);
    lv_obj_set_style_pad_left(section, BOARD_SCALE_PX(8), 0);
    lv_obj_set_width(section, row_w);
}

/* Name and counts: known at once, from the opening list row. */
static void apply_params(void) {
    int32_t info_w = LIST_ROW_WIDTH_WIDE - P.cover_px - BOARD_SCALE_PX(18);
    char text[160], part[64];
    fit_label(P.name_label, P.name, GUI_FONT_ROLE_TITLE, info_w, 2);
    snprintf(text, sizeof(text), TR_N("%d album", "%d albums", P.params.albums), P.params.albums);
    snprintf(part, sizeof(part), TR_N("%d song", "%d songs", P.params.songs), P.params.songs);
    size_t len = strlen(text);
    snprintf(text + len, sizeof(text) - len, " \xC2\xB7 %s", part);
    fit_label(P.counts_label, text, GUI_FONT_ROLE_BODY, info_w, 2);
    if (P.songs_all_label) {
        /* Long translations collide with the row's title: fall back to the bare count. */
        snprintf(text, sizeof(text), TR_N("All %d song", "All %d songs", P.params.songs), P.params.songs);
        len = strlen(text);
        snprintf(text + len, sizeof(text) - len, "  " LV_SYMBOL_RIGHT);
        lv_point_t size;
        lv_text_get_size(&size, text, gui_theme_font(GUI_FONT_ROLE_BODY), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x > P.songs_all_w)
            snprintf(text, sizeof(text), "%d  " LV_SYMBOL_RIGHT, P.params.songs);
        lv_label_set_text(P.songs_all_label, text);
    }
}

void gui_artist_page_build(lv_obj_t * list, const gui_artist_page_params_t * params) {
    if (!list || !params) return;
    if (P.root && P.list != list) P.root = NULL;
    bool reuse = P.root && (P.params.songs > 0) == (params->songs > 0);
    if (P.root && !reuse) { lv_obj_delete(P.root); P.root = NULL; }
    P.list = list;
    P.params = *params;
    snprintf(P.name, sizeof(P.name), "%s", params->name ? params->name : "");
    P.params.name = P.name;
    for (int i = 0; i < TILES; i++) { P.tile_key[i] = 0; P.tile_state[i] = 0; P.tile_shown[i] = 0; }
    P.tile_count = 0;
    P.tiles_due = true;
    P.request_tick = 0;
    if (!reuse) {
        P.genre_text[0] = P.year_text[0] = P.total_text[0] = 0;
        P.header_height = -1;
        P.songs_all_label = NULL;
        create_header();
    }
    /* Facts and revisions are read together, so a publication between the two
     * can never leave the header marked current without showing it. */
    gui_artist_summary_t s;
    gui_artist_data_snapshot(&s, &P.rev);
    apply_params();
    apply_summary(&s);
    relayout();
}

void gui_artist_page_forget(void) {
    P.root = NULL; P.list = NULL;
    P.name_label = P.counts_label = P.genre_label = P.chips = P.year_chip = P.year_label = NULL;
    P.total_chip = P.total_label = P.box = P.songs_all_label = NULL;
    for (int i = 0; i < TILES; i++) P.tile_img[i] = NULL;
}

bool gui_artist_page_poll(lv_obj_t * list, bool visible) {
    if (!P.root || P.list != list || !visible) return false;
    gui_artist_summary_t s;
    gui_artist_revisions_t rev;
    gui_artist_data_snapshot(&s, &rev);
    bool refetch = rev.albums != P.rev.albums;
    if (rev.summary != P.rev.summary || rev.duration != P.rev.duration) {
        if (apply_summary(&s)) relayout();
    }
    P.rev = rev;
    /* Each look scans the thumbnail cache per tile: only when a tile is waiting
     * and something new landed, the set changed, or the re-request is due. */
    bool tile_waiting = false;
    for (int i = 0; i < P.tile_count; i++)
        if (P.tile_state[i] == 0 && P.tile_key[i]) tile_waiting = true;
    uint32_t commits = library_thumbnails_commit_count();
    if (tile_waiting && (P.tiles_due || commits != P.thumb_commits || lv_tick_elaps(P.request_tick) >= 400)) {
        P.tiles_due = false;
        P.thumb_commits = commits;
        (void) fill_tiles(list);
    }
    return refetch;
}

void gui_artist_page_release(void) {
    for (int i = 0; i < TILES; i++) {
        free(P.tile_pixels[i]);
        P.tile_pixels[i] = NULL;
        P.tile_state[i] = 0;
        P.tile_shown[i] = 0;
        P.tile_key[i] = 0;
        P.tile_img[i] = NULL;
    }
}
