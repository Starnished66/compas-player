#include "src/ui/application/gui.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/ui/library/library_search.h"
#include "src/ui/text/rendering/gui_text_input.h"
#include "src/ui/widgets/builders/screen_builders.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
static pthread_t ui_thread;
static atomic_bool hold_read, entered;
static int count = 2;
static int batch_calls, shown_count;
static bool replaced;
static int expected_batch_count = 1;
static char extra_paths[201][64];
static lv_obj_t *album_list_for_restore;
static int album_restore_count_argument = -1, album_provider_restore_count = -1;
static int album_group_count = 4;
static const char *name(int index) { return index == 0 ? "Needle cached" : "Needle missing"; }
static library_search_group_item_t item(int index) {
    library_search_group_item_t result = {
        .path = index > 1 ? extra_paths[index] : index == 0 ? "./music/cached.flac" :
            replaced ? "./music/replaced.flac" : "./music/needle.flac",
        .song_id = 41 + index, .artwork_key = 100 + index,
    };
    if (index == 0) strcpy(result.subtitle, "Cached artist and album");
    return result;
}
static lv_obj_t *az(lv_obj_t *screen) { (void)screen; return NULL; }
static void refresh(void) {}
bool __wrap_metadata_db_get_song_by_path(const char *path, song_row_t *song) {
    (void)path; (void)song;
    assert(!"group search must use the batch API");
    return false;
}
void __wrap_metadata_db_get_songs_by_paths(const char *const *paths, int n, song_row_t *songs) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(n == expected_batch_count); /* Cached rows never enter the request. */
    for (int i = 1; i < n; i++) assert(!strcmp(paths[i], extra_paths[i + 1]));
    bool replacement = !strcmp(paths[0], "./music/replaced.flac");
    assert(replacement || !strcmp(paths[0], "./music/needle.flac"));
    batch_calls++;
    atomic_store(&entered, true);
    while (atomic_load(&hold_read)) usleep(1000);
    memset(songs, 0, (size_t)n * sizeof(*songs));
    for (int i = 0; i < n; i++) {
        songs[i].id = 42 + i;
        strcpy(songs[i].tags.title, "Database title must not replace the label");
        strcpy(songs[i].tags.artist, replacement ? "Replacement artist" : "Uncached artist");
        strcpy(songs[i].tags.album, "Uncached album");
    }
}
void __real_compact_list_set_items(lv_obj_t *, const compact_list_item_t *, int);
void __wrap_compact_list_set_items(lv_obj_t *list, const compact_list_item_t *items, int n) {
    shown_count = n;
    for (int i = 0; i < n; i++) {
        assert(items[i].identity >= 41 && items[i].identity < 241);
        assert(items[i].artwork_key == (uint64_t)(items[i].identity + 59));
        assert(items[i].artwork_name == NULL);
    }
    __real_compact_list_set_items(list, items, n);
}
void __real_compact_list_set_paged_provider(lv_obj_t *, compact_list_fetch_page_cb_t, void *, int);
void __wrap_compact_list_set_paged_provider(lv_obj_t *list, compact_list_fetch_page_cb_t fetch,
                                            void *ctx, int total) {
    if (list == album_list_for_restore) album_provider_restore_count = total;
    __real_compact_list_set_paged_provider(list, fetch, ctx, total);
}
void __wrap_metadata_db_get_group_counts(int *artists, int *album_artists, int *albums) {
    if (artists) *artists = 2;
    if (album_artists) *album_artists = 3;
    if (albums) *albums = album_group_count;
}
int __wrap_gui_library_albums_paged_total(int normal_count) {
    album_restore_count_argument = normal_count;
    return normal_count + 2; /* mock one 5-star and one unrated header */
}
static int albums_restore_page(void *ctx, int offset, int count, compact_list_page_row_t rows[]) {
    (void)ctx; (void)offset; (void)count; (void)rows;
    return 0;
}
static bool has_text(lv_obj_t *obj, const char *text) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        if (has_text(lv_obj_get_child(obj, i), text)) return true;
    return false;
}
static lv_obj_t *textarea(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_textarea_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = textarea(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}
static void check_album_search_restore(void) {
    /* Closing a DB-backed Albums search restores displayed row count, which
     * includes the rating section headers in its paged provider total. */
    lv_obj_t *album_screen = lv_obj_create(NULL);
    album_list_for_restore = build_compact_list_widget(album_screen, NULL, 0, NULL, NULL,
                                                       LIST_ROW_WIDTH, false, NULL);
    library_search_config_t albums_config = {
        .screen = album_screen, .list = album_list_for_restore, .is_overlay_list = false,
        .db_backed = true, .db_kind = METADATA_DB_AZ_ALBUM,
        .restore_fetch_page = albums_restore_page,
        .az_strip_for_screen = az, .refresh_now_playing = refresh,
    };
    library_search_register(SEARCH_BINDING_ALBUMS, &albums_config);
    nav_push(album_screen);
    library_search_open(SEARCH_BINDING_ALBUMS);
    album_restore_count_argument = album_provider_restore_count = -1;
    assert(search_close_if_active_for_screen(album_screen));
    assert(album_restore_count_argument == album_group_count);
    assert(album_provider_restore_count == album_group_count + 2);
}

static void initialize_test_ui(void) {
    lv_init();
    assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings);
    audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    ui_thread = pthread_self();
}

int main(void) {
    initialize_test_ui();
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *list = build_compact_list_widget(screen, NULL, 0, NULL, NULL, LIST_ROW_WIDTH, false, NULL);
    library_search_config_t config = {
        .screen = screen, .list = list, .name_of = name, .count_ptr = &count,
        .is_overlay_list = true, .group_item = item, .az_strip_for_screen = az,
        .refresh_now_playing = refresh, .restore_thumbnail_list = list,
    };
    library_search_register(SEARCH_BINDING_GROUP_SONGS, &config);
    nav_push(screen);
    library_search_open(SEARCH_BINDING_GROUP_SONGS);
    lv_obj_t *entry = textarea(screen);
    assert(entry);
    atomic_store(&hold_read, true);
    lv_textarea_set_text(entry, "Needle");
    assert(shown_count == 2);
    assert(has_text(list, "Needle cached"));
    assert(has_text(list, "Needle missing"));
    assert(has_text(list, "Cached artist and album"));
    for (int i = 0; i < 1000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    poll_search_job();
    assert(!has_text(list, "Uncached artist"));
    atomic_store(&hold_read, false);
    for (int i = 0; i < 1000 && library_search_has_active_job(); i++) {
        poll_search_job(); usleep(1000);
    }
    assert(!library_search_has_active_job());
    assert(batch_calls == 1);
    assert(has_text(list, "Uncached artist"));
    assert(has_text(list, "Uncached album"));
    assert(has_text(list, "Needle missing"));
    assert(!has_text(list, "Database title"));

    /* A newer keystroke wins even when it needs no worker. */
    atomic_store(&entered, false);
    atomic_store(&hold_read, true);
    lv_textarea_set_text(entry, "missing");
    assert(shown_count == 1 && search_remap_index(SEARCH_BINDING_GROUP_SONGS, 0) == 1);
    for (int i = 0; i < 1000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    lv_textarea_set_text(entry, "cached");
    assert(shown_count == 1 && search_remap_index(SEARCH_BINDING_GROUP_SONGS, 0) == 0);
    atomic_store(&hold_read, false);
    for (int i = 0; i < 1000 && library_search_has_active_job(); i++) {
        poll_search_job(); usleep(1000);
    }
    assert(!library_search_has_active_job());
    assert(!has_text(list, "Uncached artist"));
    assert(has_text(list, "Cached artist and album"));

    /* Closing/reopening for a replaced group rejects the old batch. */
    atomic_store(&entered, false);
    atomic_store(&hold_read, true);
    lv_textarea_set_text(entry, "missing");
    for (int i = 0; i < 1000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    assert(search_close_if_active_for_screen(screen));
    replaced = true;
    library_search_open(SEARCH_BINDING_GROUP_SONGS);
    lv_textarea_set_text(entry, "missing");
    assert(shown_count == 1 && has_text(list, "Needle missing"));
    atomic_store(&hold_read, false);
    for (int i = 0; i < 1000 && library_search_has_active_job(); i++) {
        poll_search_job(); usleep(1000);
        assert(!has_text(list, "Uncached artist"));
    }
    assert(!library_search_has_active_job());
    assert(has_text(list, "Replacement artist"));
    assert(batch_calls == 4);
    count = 201;
    expected_batch_count = 199;
    for (int i = 2; i < count; i++) snprintf(extra_paths[i], sizeof(extra_paths[i]), "./music/track%d.flac", i);
    atomic_store(&entered, false);
    atomic_store(&hold_read, true);
    lv_textarea_set_text(entry, "Needle");
    assert(shown_count == 200);
    assert(search_remap_index(SEARCH_BINDING_GROUP_SONGS, 199) == 199);
    for (int i = 0; i < 1000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    atomic_store(&hold_read, false);
    for (int i = 0; i < 1000 && library_search_has_active_job(); i++) {
        poll_search_job(); usleep(1000);
    }
    assert(!library_search_has_active_job());
    assert(batch_calls == 5);
    assert(has_text(list, "Replacement artist"));
    lv_textarea_set_text(entry, "no matches");
    assert(shown_count == 0 && !library_search_has_active_job());
    lv_textarea_set_text(entry, "");
    assert(shown_count == 0 && !library_search_has_active_job());
    assert(!library_search_showing_results(SEARCH_BINDING_GROUP_SONGS));
    assert(search_close_if_active_for_screen(screen));

    check_album_search_restore();
    puts("Group search shows rows immediately, batches missing subtitles and rejects stale fills");
}
