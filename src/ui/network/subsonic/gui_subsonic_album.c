#include "gui_subsonic_album.h"
#include "gui_subsonic_album_cover.h"
#include "gui_subsonic_internal.h"
#include "../../appearance/gui_theme.h"
#include "../../library/browse/gui_album_view.h"
#include "../../library/browse/gui_library_internal.h"
#include "../../player/screen/gui_player.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/gui_play_bar.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../widgets/lists/compact_list.h"
#include "../../widgets/builders/screen_builders_internal.h"
#include "../../../core/system/runtime/board_config.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* The page on screen. The server and cover id are copied: the caches that
 * the caller borrowed them from may be replaced while the page stays. */
static lv_obj_t * list;
static gui_album_view_t view;
static subsonic_server_t server;
static char cover_id[GUI_SUBSONIC_COVER_ID_MAX];
static bool active;
static lv_obj_t * bar; /* the Play / Shuffle bar above a compact songs list */

lv_obj_t * gui_subsonic_album_list_build(lv_obj_t * screen) {
    list = build_song_list_body(screen);
    lv_obj_set_style_pad_hor(list, GUI_LIST_SIDE_MARGIN, 0);
    screen_builders_configure_content_scrollbar(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    return list;
}

bool gui_subsonic_album_fits(int count) { return count > 0 && count <= GUI_SUBSONIC_ALBUM_MAX_TRACKS; }

/* ---- Facts of the album --------------------------------------------------- */
typedef struct { char format[16]; bool hires; double seconds; } facts_t;

/* The common suffix upper-cased, "Mixed" when the tracks differ, nothing when unknown. */
static void facts_of(const subsonic_song_t * songs, int count, facts_t * f) {
    memset(f, 0, sizeof(*f));
    bool mixed = false;
    f->hires = true;
    for (int i = 0; i < count; i++) {
        const subsonic_song_t * s = &songs[i];
        f->seconds += s->duration_seconds;
        if (strcasecmp(s->suffix, songs[0].suffix) != 0) mixed = true;
        if (s->bit_depth <= 16 && s->sample_rate <= 48000) f->hires = false;
    }
    if (mixed) snprintf(f->format, sizeof(f->format), "%s", TR("Mixed"));
    else for (size_t i = 0; songs[0].suffix[i] && i + 1 < sizeof(f->format); i++)
        f->format[i] = (char) toupper((unsigned char) songs[0].suffix[i]);
}


/* ---- Play ------------------------------------------------------------------ */
static void play_from(int index, bool shuffle) {
    if (!gui_subsonic_wifi_connected_guard()) return;
    gui_player_set_play_mode(collection_play_mode(shuffle ? PLAY_MODE_SHUFFLE : PLAY_MODE_SEQUENTIAL));
    gui_subsonic_play_from(index);
}

static void play_cb(lv_event_t * e) {
    (void) e;
    play_from(0, false);
}

static int shuffle_count; /* tracks on the page: where Shuffle may start */

static void shuffle_cb(lv_event_t * e) {
    (void) e;
    if (shuffle_count <= 0) return;
    play_from((int) (((unsigned) time(NULL) ^ lv_tick_get()) % (unsigned) shuffle_count), true);
}

static void row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_subsonic_play_from((int) (intptr_t) lv_event_get_user_data(e));
}

/* ---- Rows ------------------------------------------------------------------ */
static int shown_number(const subsonic_song_t * songs, int i) { return songs[i].track > 0 ? songs[i].track : i + 1; }

static bool multi_disc(const subsonic_song_t * songs, int count) {
    for (int i = 1; i < count; i++) if (songs[i].disc != songs[0].disc) return true;
    return false;
}

static void add_row(const subsonic_song_t * songs, int i) {
    lv_obj_t * row = build_music_list_row(list, songs[i].title, NULL, BOARD_SCALE_PX(70));
    gui_album_view_row_badge(row, gui_song_quality_asset(songs[i].suffix));
    gui_album_view_decorate_row(row, shown_number(songs, i), view.num_w,
                                songs[i].duration_seconds > 0 ? songs[i].duration_seconds : -1);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
}

static void add_rows(const subsonic_song_t * songs, int count) {
    bool discs = multi_disc(songs, count);
    for (int i = 0; i < count; i++) {
        if (discs && (i == 0 || songs[i].disc != songs[i - 1].disc)) {
            char header[32];
            snprintf(header, sizeof(header), TR("Disc %d"), songs[i].disc > 0 ? songs[i].disc : 1);
            add_section_header(list, header);
        }
        add_row(songs, i);
    }
}

/* ---- Header and cover ------------------------------------------------------- */
static void build_header(const gui_subsonic_album_info_t * info, const subsonic_song_t * songs, int count) {
    facts_t facts;
    facts_of(songs, count, &facts);
    char meta[160], total[48];
    gui_album_view_meta_text(info->year, info->genre, meta, sizeof(meta));
    gui_album_view_total_text(facts.seconds, total, sizeof(total));
    gui_album_view_params_t params = {
        .album = info->title, .artist = info->artist, .meta = meta, .hires_icon = gui_song_quality_asset(songs[0].suffix),
        .show_buttons = true, .play_cb = play_cb, .shuffle_cb = shuffle_cb,
    };
    gui_album_view_build(&view, list, &params);
    gui_album_view_set_chips(&view, facts.format, facts.hires, total);
}

static void show_cover(void) {
    const lv_image_dsc_t * dsc = view.cover_img ? gui_subsonic_album_cover_get(cover_id) : NULL;
    if (dsc) gui_album_view_show_cover(&view, dsc);
}

static void request_cover(void) {
    if (!active || !cover_id[0] || lv_screen_active() != lv_obj_get_screen(list)) return;
    gui_subsonic_album_cover_request(&server, cover_id, (int) view.cover_px);
    show_cover();
}

/* ---- Page ------------------------------------------------------------------- */
void gui_subsonic_album_show(const gui_subsonic_album_info_t * info, const subsonic_song_t * songs, int count,
                             bool keep_scroll) {
    int32_t scroll_y = active && keep_scroll ? lv_obj_get_scroll_y(list) : 0;
    memset(&view, 0, sizeof(view)); /* the widgets go with the clean below */
    lv_obj_clean(list);
    server = *info->server;
    snprintf(cover_id, sizeof(cover_id), "%s", info->cover_id ? info->cover_id : "");
    shuffle_count = count;
    active = true;
    lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);
    build_header(info, songs, count);
    int widest = 1;
    for (int i = 0; i < count; i++) if (shown_number(songs, i) > widest) widest = shown_number(songs, i);
    view.num_w = gui_album_view_number_width(widest);
    add_rows(songs, count);
    request_cover();
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, scroll_y, LV_ANIM_OFF);
}

void gui_subsonic_album_clear(void) {
    if (!list) return;
    gui_subsonic_album_cover_release();
    memset(&view, 0, sizeof(view));
    lv_obj_clean(list);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    active = false;
    cover_id[0] = 0;
    shuffle_count = 0;
}

static void screen_left(void) {
    if (!active) return;
    gui_album_view_detach_cover(&view, NULL);
    gui_subsonic_album_cover_release();
}

void gui_subsonic_album_screen_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SCREEN_UNLOADED) screen_left();
    else if (code == LV_EVENT_SCREEN_LOADED) request_cover();
}

void gui_subsonic_album_teardown(void) {
    gui_subsonic_album_clear();
    list = bar = NULL;
}

void gui_subsonic_songs_bar_set(lv_obj_t * compact, int count) {
    if (bar) {
        compact_list_set_header(compact, NULL, 0);
        lv_obj_delete(bar);
        bar = NULL;
    }
    shuffle_count = count;
    if (count <= 0) return;
    char text[48];
    snprintf(text, sizeof(text), TR_N("%d track", "%d tracks", count), count);
    bar = gui_play_bar_build(compact, LIST_ROW_WIDTH_WIDE, text, play_cb, shuffle_cb);
    lv_obj_update_layout(bar);
    compact_list_set_header(compact, bar, lv_obj_get_height(bar) + BOARD_SCALE_PX(8));
}

void gui_subsonic_album_poll(void) {
    if (gui_subsonic_album_cover_poll() && active) show_cover();
}
