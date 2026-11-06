#include "gui_subsonic_genres.h"
#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include <stdio.h>
#include <stdlib.h>

#define GENRE_SUBTITLE_SIZE 24
typedef char genre_subtitle_t[GENRE_SUBTITLE_SIZE];

static lv_obj_t * genres_screen;
static lv_obj_t * genres_list;
static lv_obj_t * genres_title;
static subsonic_genre_t * genres;
static genre_subtitle_t * subtitles;
static int genres_count;
static void (*pick_cb)(const char * name);

static void genre_click_cb(int index) {
    if (index >= 0 && index < genres_count && pick_cb) pick_cb(genres[index].name);
}

static void genres_free(void) {
    compact_list_set_items(genres_list, NULL, 0); /* the rows borrow the names */
    free(genres);
    free(subtitles);
    genres = NULL;
    subtitles = NULL;
    genres_count = 0;
}

void gui_subsonic_genres_init(void (*pick)(const char * name)) {
    pick_cb = pick;
    genres_screen = build_compact_list_screen(TR("Genres"), generic_back_cb, NULL, 0, genre_click_cb, NULL,
                                              &genres_list, &genres_title, LIST_ROW_WIDTH_WIDE, false, NULL);
    finalize_screen_navigation(genres_screen);
}

bool gui_subsonic_genres_show(subsonic_genre_t * fresh, int count) {
    int n = count > 0 ? count : 1;
    compact_list_item_t * items = malloc(sizeof(*items) * (size_t) n);
    genre_subtitle_t * texts = malloc(sizeof(*texts) * (size_t) n);
    if (!items || !texts) {
        free(items);
        free(texts);
        free(fresh);
        return false;
    }
    genres_free();
    genres = fresh;
    subtitles = texts;
    genres_count = count;
    for (int i = 0; i < count; i++) {
        snprintf(texts[i], sizeof(texts[i]), TR_N("%d album", "%d albums", fresh[i].album_count),
                 fresh[i].album_count);
        items[i] = (compact_list_item_t){ .label = fresh[i].name, .subtitle = texts[i],
                                          .trailing_asset = "navigation/chevron-right.png" };
    }
    compact_list_set_items(genres_list, items, count);
    free(items);
    nav_push(genres_screen);
    return true;
}

void gui_subsonic_genres_teardown(void) {
    genres_free();
    if (genres_screen) lv_obj_delete(genres_screen);
    genres_screen = NULL;
}
