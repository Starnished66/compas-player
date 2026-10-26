#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/ui/application/gui.h"
#include "../../src/ui/library/browse/gui_library.h"
#include "../../src/ui/player/screen/gui_player.h"
#include "../../src/ui/shell/gui_notifications.h"
#include "../../src/ui/widgets/lists/compact_list.h"
#include "../../src/ui/widgets/builders/screen_builders.h"
#include "../../src/ui/appearance/gui_theme.h"
#include "../../src/ui/text/rendering/gui_text_input.h"
#include "../../src/ui/text/fonts/ui_fonts.h"
#include "../../src/ui/text/rendering/fallback_font.h"
#include "../../src/library/metadata/db/metadata_db.h"
#include "../../src/ui/library/browse/gui_library.c"
#include "src/ui/library/library_search.c"
#include "src/ui/library/library_az_index.c"

static lv_display_t *test_display;
static void flush_ready(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    LV_UNUSED(a); LV_UNUSED(p); lv_display_flush_ready(d);
}

static const char *memory_names[] = {"Beta", "Delta Song", "Gamma"};
static int memory_count = 3;
static const char *memory_name(int index) {
    assert(index >= 0 && index < memory_count);
    return memory_names[index];
}

static void *captured_arg;
static void *(*captured_worker)(void *);
static unsigned creates, joins;
static bool capture_thread_calls;
static bool capture_active_items;
static bool use_real_t9_release;
static bool active_items_cleared;
static const char *last_active_label;
static int64_t attached_identities[8];
static uint64_t attached_artwork_keys[8];
static const char *attached_labels[8];
static int attached_count;
static void *must_clear_before_free;
static bool freed_labels_safely;
static unsigned refresh_spy_calls;
static search_binding_t *refresh_probe_binding;
static bool refresh_expect_active, refresh_expect_results;
static int refresh_expected_count, refresh_expected_row;
static const char *refresh_expected_label;
static struct { int op; lv_obj_t *list; } thumbnail_events[16];
static size_t thumbnail_event_count;
enum { THUMB_BEGIN = 1, THUMB_END = 2 };
static void refresh_spy(void) {
    refresh_spy_calls++;
    if (!refresh_probe_binding) return;
    assert(refresh_probe_binding->active == refresh_expect_active);
    assert((refresh_probe_binding->filtered_indices != NULL) == refresh_expect_results);
    if (!refresh_expect_results) {
        assert(refresh_probe_binding->filtered_count == 0);
        assert(refresh_probe_binding->filtered_labels == NULL);
        return;
    }
    assert(refresh_probe_binding->filtered_count == refresh_expected_count);
    assert(refresh_probe_binding->filtered_indices[0] == refresh_expected_row);
    if (refresh_expected_label)
        assert(strcmp(refresh_probe_binding->filtered_labels[0], refresh_expected_label) == 0);
}
static lv_obj_t *no_az_strip(lv_obj_t *screen) { (void)screen; return NULL; }
static void record_thumbnail_event(int op, lv_obj_t *list) {
    assert(thumbnail_event_count < sizeof(thumbnail_events) / sizeof(thumbnail_events[0]));
    thumbnail_events[thumbnail_event_count].op = op;
    thumbnail_events[thumbnail_event_count].list = list;
    thumbnail_event_count++;
}

int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int __real_pthread_join(pthread_t, void **);
void __real_t9_keypad_release(void);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start)(void *), void *arg) {
    if (!capture_thread_calls) return __real_pthread_create(thread, attr, start, arg);
    creates++;
    captured_worker = start;
    captured_arg = arg;
    *thread = (pthread_t)0x1234;
    return 0;
}
int __wrap_pthread_join(pthread_t thread, void **result) {
    if (capture_thread_calls) {
        assert(thread == (pthread_t)0x1234);
        joins++;
        if (result) *result = NULL;
        return 0;
    }
    return __real_pthread_join(thread, result);
}

void __real_compact_list_set_items(lv_obj_t *, const compact_list_item_t *, int);
void __wrap_compact_list_set_items(lv_obj_t *list, const compact_list_item_t *items, int count) {
    if (capture_active_items) {
        if (!items || count == 0) {
            active_items_cleared = true;
            last_active_label = NULL;
        } else {
            active_items_cleared = false;
            last_active_label = items[0].label;
            attached_count = count < 8 ? count : 8;
            for (int i = 0; i < attached_count; i++) {
                attached_identities[i] = items[i].identity;
                attached_artwork_keys[i] = items[i].artwork_key;
                attached_labels[i] = items[i].label;
            }
        }
    }
    __real_compact_list_set_items(list, items, count);
}
void __real_free(void *);
void __wrap_free(void *ptr) {
    if (ptr && ptr == must_clear_before_free) {
        assert(active_items_cleared);
        assert(last_active_label == NULL);
        freed_labels_safely = true;
        must_clear_before_free = NULL;
    }
    __real_free(ptr);
}

int __wrap_metadata_db_search_names(metadata_db_az_kind_t kind, const char *needle,
                                    int max_rows, metadata_db_search_hit_t *out) {
    assert(kind == METADATA_DB_AZ_ALL_SONGS);
    assert(max_rows >= 1);
    if (strcmp(needle, "first") == 0) {
        snprintf(out[0].label, sizeof(out[0].label), "%s", "First result");
        out[0].offset = 7;
        return 1;
    }
    if (strcmp(needle, "latest") == 0) {
        snprintf(out[0].label, sizeof(out[0].label), "%s", "Latest result");
        out[0].offset = 19;
        return 1;
    }
    if (strcmp(needle, "recent") == 0) {
        snprintf(out[0].label, sizeof(out[0].label), "%s", "Alpha from source 4");
        out[0].offset = 4;
        snprintf(out[1].label, sizeof(out[1].label), "%s", "Omega from source 9");
        out[1].offset = 9;
        return 2;
    }
    assert(!"unexpected worker query");
    return 0;
}
int __wrap_metadata_db_get_songs_filtered_page(const char *a, const char *b, const char *c,
                                                const char *d, const char *e, int offset,
                                                int count, song_row_t *rows) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)count;
    if (rows && offset == 4) {
        rows[0].id = 401;
        snprintf(rows[0].path, sizeof(rows[0].path), "%s", "/recency/later.flac");
        return 1;
    }
    if (rows && offset == 9) {
        rows[0].id = 902;
        snprintf(rows[0].path, sizeof(rows[0].path), "%s", "/recency/earlier.flac");
        return 1;
    }
    return 0;
}
uint64_t __wrap_song_album_thumbnail_key(const song_row_t *row) {
    return row ? (uint64_t)row->id + 0x100000U : 0;
}
int64_t __wrap_metadata_db_get_song_recency_offset(const char *path) {
    if (path && strcmp(path, "/recency/later.flac") == 0) return 90;
    if (path && strcmp(path, "/recency/earlier.flac") == 0) return 10;
    return -1;
}
int64_t __wrap_metadata_db_get_song_count(void) { return 42; }
void __wrap_metadata_db_get_group_counts(int *artists, int *album_artists, int *albums) {
    if (artists) *artists = 0;
    if (album_artists) *album_artists = 0;
    if (albums) *albums = 0;
}
int64_t __wrap_metadata_db_get_song_recency_offset(const char *path);
uint64_t __wrap_song_album_thumbnail_key(const song_row_t *row);
static int restored_total;
static compact_list_fetch_page_cb_t restored_fetch;
void __wrap_compact_list_set_paged_provider(lv_obj_t *list, compact_list_fetch_page_cb_t fetch,
                                            void *ctx, int total) {
    (void)list; (void)ctx;
    restored_fetch = fetch;
    restored_total = total;
}
void __wrap_refresh_now_playing_indicators(void) { }
void __wrap_t9_keypad_release(void) { if (use_real_t9_release) __real_t9_keypad_release(); }
void __real_library_thumbnails_begin_view(lv_obj_t *);
void __real_library_thumbnails_end_view(lv_obj_t *);
void __wrap_library_thumbnails_begin_view(lv_obj_t *list) {
    record_thumbnail_event(THUMB_BEGIN, list);
    __real_library_thumbnails_begin_view(list);
}
void __wrap_library_thumbnails_end_view(lv_obj_t *list) {
    record_thumbnail_event(THUMB_END, list);
    __real_library_thumbnails_end_view(list);
}

static int test_fetch_page(void *ctx, int offset, int count, compact_list_page_row_t out[]) {
    (void)ctx; (void)offset; (void)count; (void)out;
    return 0;
}

static void register_test_search(search_binding_id_t id, lv_obj_t *screen, lv_obj_t *list,
                                 library_search_name_of_t name_of, const int *count_ptr,
                                 bool overlay, bool db_backed, metadata_db_az_kind_t kind,
                                 compact_list_fetch_page_cb_t restore) {
    register_search(id, screen, list, name_of, count_ptr, overlay, db_backed, kind, restore);
}

static void test_memory_backed_remap_and_close(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="before"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);
    assert(!library_ui_live); /* Subsonic can register before gui_library_init(). */
    register_test_search(SEARCH_BINDING_SUBSONIC_ARTISTS, screen, list, memory_name,
                         &memory_count, false, false, METADATA_DB_AZ_ARTIST, NULL);
    search_binding_t *b = &search_bindings[SEARCH_BINDING_SUBSONIC_ARTISTS];
    assert(b->screen == screen && b->list == list && b->name_of == memory_name);
    b->active = true;
    search_apply_filter(b, "delta");
    assert(b->filtered_count == 1);
    assert(search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, 0) == 1);
    assert(b->filtered_indices[0] == 1);
    search_close_internal(b, true);
    assert(!b->active && b->filtered_indices == NULL && b->filtered_labels == NULL);
    assert(search_remap_index(SEARCH_BINDING_SUBSONIC_ARTISTS, 2) == 2);
    assert(!library_search_showing_results(SEARCH_BINDING_SUBSONIC_ARTISTS));

    /* An empty filtered result still means search mode was applied, even
     * after its binding becomes inactive; the bounded map API rejects rows. */
    search_apply_filter(b, "no such name");
    assert(library_search_showing_results(SEARCH_BINDING_SUBSONIC_ARTISTS));
    assert(!library_search_is_active(SEARCH_BINDING_SUBSONIC_ARTISTS));
    assert(!library_search_is_active_on_list(list));
    int source_row = 123;
    assert(!library_search_try_map_filtered_row(SEARCH_BINDING_SUBSONIC_ARTISTS, 0, &source_row));
    assert(source_row == 123);
    assert(library_search_find_filtered_row(SEARCH_BINDING_SUBSONIC_ARTISTS, 1) == -1);
    search_apply_filter(b, "");
    assert(!library_search_showing_results(SEARCH_BINDING_SUBSONIC_ARTISTS));
    b->screen = NULL;
    b->list = NULL;
    lv_obj_delete(screen);
}

static void test_public_registration_copies_config(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="source"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);
    library_search_config_t config = {
        .screen=screen,.list=list,.name_of=memory_name,.count_ptr=&memory_count,
        .is_overlay_list=false,.db_backed=false,.db_kind=METADATA_DB_AZ_ARTIST
    };
    library_search_register(SEARCH_BINDING_SUBSONIC_ALBUMS, &config);
    config.screen = NULL;
    config.list = NULL;
    config.name_of = NULL;
    config.count_ptr = NULL;
    search_binding_t *b = &search_bindings[SEARCH_BINDING_SUBSONIC_ALBUMS];
    assert(b->screen == screen && b->list == list);
    assert(b->name_of == memory_name && b->count_ptr == &memory_count);
    b->screen = b->list = NULL;
    lv_obj_delete(screen);
}

static void test_refresh_callback_call_points(lv_obj_t *root) {
    lv_obj_t *memory_screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="memory source"}};
    lv_obj_t *memory_list = build_compact_list_widget(memory_screen, initial, 1, NULL, NULL,
                                                       LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(memory_screen && memory_list);
    library_search_config_t memory_config = {
        .screen=memory_screen,.list=memory_list,.name_of=memory_name,
        .count_ptr=&memory_count,.is_overlay_list=false,.db_backed=false,
        .db_kind=METADATA_DB_AZ_ARTIST,.az_strip_for_screen=no_az_strip,
        .refresh_now_playing=refresh_spy
    };
    library_search_register(SEARCH_BINDING_SUBSONIC_ALBUMS, &memory_config);
    search_binding_t *memory = &search_bindings[SEARCH_BINDING_SUBSONIC_ALBUMS];
    refresh_probe_binding = memory;
    memory->active = true;
    refresh_spy_calls = 0;
    refresh_expect_active = true;
    refresh_expect_results = true;
    refresh_expected_count = 1;
    refresh_expected_row = 1;
    refresh_expected_label = NULL;
    search_apply_filter(memory, "delta");
    assert(refresh_spy_calls == 1); /* memory result applied */
    refresh_probe_binding = NULL;
    free(memory->filtered_indices);
    memory->filtered_indices = NULL;
    memory->filtered_count = 0;
    memory->screen = memory->list = NULL;
    lv_obj_delete(memory_screen);

    lv_obj_t *db_screen = lv_obj_create(root);
    lv_obj_t *db_list = build_compact_list_widget(db_screen, initial, 1, NULL, NULL,
                                                  LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(db_screen && db_list);
    lv_screen_load(db_screen);
    library_search_config_t db_config = {
        .screen=db_screen,.list=db_list,.is_overlay_list=false,.db_backed=true,
        .db_kind=METADATA_DB_AZ_ALL_SONGS,.restore_fetch_page=test_fetch_page,
        .az_strip_for_screen=no_az_strip,.refresh_now_playing=refresh_spy
    };
    library_search_register(SEARCH_BINDING_ALL_SONGS, &db_config);
    search_binding_t *db = &search_bindings[SEARCH_BINDING_ALL_SONGS];
    refresh_probe_binding = db;
    db->active = true;
    capture_thread_calls = true;
    search_job_active = false;
    search_job_pending_valid = false;
    atomic_store(&search_job_done_flag, false);
    refresh_spy_calls = 1;
    refresh_expect_active = true;
    refresh_expect_results = false;
    refresh_expected_label = NULL;
    search_apply_filter(db, "");
    assert(refresh_spy_calls == 2); /* empty DB query */
    search_apply_filter(db, "latest");
    assert(captured_worker && captured_arg && library_search_has_active_job());
    captured_worker(captured_arg);
    refresh_expect_active = true;
    refresh_expect_results = true;
    refresh_expected_count = 1;
    refresh_expected_row = 19;
    refresh_expected_label = "Latest result";
    poll_search_job();
    assert(refresh_spy_calls == 3); /* completed DB result applied */
    refresh_expect_active = false;
    refresh_expect_results = false;
    refresh_expected_label = NULL;
    search_close(db);
    assert(refresh_spy_calls == 4); /* close refresh */
    refresh_probe_binding = NULL;
    capture_thread_calls = false;
    db->screen = db->list = NULL;
    lv_screen_load(root);
    lv_obj_delete(db_screen);
}

static void test_real_t9_search_dismiss_reopen_and_files_overlay(lv_obj_t *root) {
    assert(gui_text_input_init());

    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="initial"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);

    register_test_search(SEARCH_BINDING_SUBSONIC_ARTISTS, screen, list, memory_name,
                         &memory_count, false, false, METADATA_DB_AZ_ARTIST, NULL);
    search_binding_t *memory = &search_bindings[SEARCH_BINDING_SUBSONIC_ARTISTS];
    lv_obj_t *bar = memory->search_bar;
    lv_screen_load(screen);
    search_open(memory);
    assert(t9_keypad_is_inline_active());
    assert(strcmp(t9_keypad_get_text(), "") == 0);
    lv_obj_t *textarea = lv_obj_get_child(bar, (int32_t)lv_obj_get_child_count(bar) - 1);
    assert(textarea);
    lv_textarea_set_text(textarea, "delta");
    t9_keypad_dismiss_only();
    assert(!t9_keypad_is_inline_active());
    assert(strcmp(t9_keypad_get_text(), "delta") == 0);
    assert(memory->filtered_count == 1 && memory->filtered_indices[0] == 1);
    lv_obj_send_event(textarea, LV_EVENT_CLICKED, NULL);
    assert(t9_keypad_is_inline_active());
    assert(strcmp(t9_keypad_get_text(), "delta") == 0);
    use_real_t9_release = true;
    search_close_internal(memory, true);
    use_real_t9_release = false;
    assert(!t9_keypad_is_inline_active());
    assert(lv_obj_get_parent(textarea) == gui_text_input_get_screen());
    assert(strcmp(t9_keypad_get_text(), "delta") == 0);
    memory->screen = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);

    /* Exercise the actual Files search-open/close path with a real overlay,
     * options control, and the canonical screen-to-A-Z binding lookup. */
    screen = lv_obj_create(root);
    lv_obj_t *options = lv_obj_create(screen);
    lv_obj_t *strip = lv_label_create(screen);
    lv_obj_t *popup = lv_obj_create(screen);
    lv_obj_t *popup_label = lv_label_create(popup);
    list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                     LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && options && strip && popup && popup_label && list);
    files_screen = screen;
    files_search_list = list;
    files_options_btn = options;
    files_title_label = lv_label_create(screen);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    az_index_bindings[0] = (az_index_binding_t){screen,list,strip,popup,popup_label,
                                                METADATA_DB_AZ_ALL_SONGS};
    az_index_registered_count = 1;
    register_test_search(SEARCH_BINDING_FILES, screen, list, NULL, NULL, true, true,
                         METADATA_DB_AZ_ALL_SONGS, test_fetch_page);
    search_binding_t *files = &search_bindings[SEARCH_BINDING_FILES];
    bar = files->search_bar;
    lv_screen_load(screen);
    lv_obj_update_layout(list);
    search_open(files);
    assert(files->active && !lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(options, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(bar, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_height(list) == t9_keypad_get_grid_y() - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    use_real_t9_release = true;
    search_close_internal(files, true);
    use_real_t9_release = false;
    assert(!files->active && lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(options, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN));
    files->screen = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);
    files_screen = files_search_list = files_options_btn = files_title_label = NULL;
    files->list = NULL;
    az_index_registered_count = 0;
    gui_text_input_teardown();
}

static void test_group_songs_basename_fallback(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="group source"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    lv_obj_t *search_list = build_compact_list_widget(screen, NULL, 0, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list && search_list);
    static group_song_entry_t entries[] = {
        {.path="/Music/fallback-only-name.flac",.title="Tagged alternate",.song_id=3101,.artwork_key=0xbeef},
        {.path="/Music/unrelated.flac",.title="Other title",.song_id=3102,.artwork_key=0xcafe}
    };
    group_songs_entries = entries;
    group_songs_count = 2;
    group_songs_screen = screen;
    group_songs_list = list;
    group_songs_search_list = search_list;
    register_test_search(SEARCH_BINDING_GROUP_SONGS, screen, search_list,
                         group_songs_search_name_of, &group_songs_count, true, false,
                         METADATA_DB_AZ_ALL_SONGS, NULL);
    search_binding_t *b = &search_bindings[SEARCH_BINDING_GROUP_SONGS];
    b->active = true;
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_t *preferred_action = lv_obj_create(screen);
    assert(title && preferred_action);
    library_search_update_header(SEARCH_BINDING_GROUP_SONGS, title, preferred_action, true);
    assert(lv_obj_has_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN));
    b->active = false;
    library_search_update_header(SEARCH_BINDING_GROUP_SONGS, title, preferred_action, false);
    assert(!lv_obj_has_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN));
    b->active = true;
    assert(gui_text_input_init());
    lv_obj_add_flag(search_list, LV_OBJ_FLAG_HIDDEN);
    thumbnail_event_count = 0;
    lv_screen_load(screen);
    search_open(b);
    assert(b->active && !lv_obj_has_flag(search_list, LV_OBJ_FLAG_HIDDEN));
    assert(thumbnail_event_count == 1);
    assert(thumbnail_events[0].op == THUMB_BEGIN && thumbnail_events[0].list == search_list);
    capture_active_items = true;
    search_apply_filter(b, "fallback-only-name");
    assert(b->filtered_count == 1 && b->filtered_indices[0] == 0);
    assert(attached_count == 1 && attached_identities[0] == 3101);
    assert(attached_artwork_keys[0] == 0xbeef);
    assert(strcmp(attached_labels[0], "Tagged alternate") == 0);
    use_real_t9_release = true;
    search_close_internal(b, true);
    use_real_t9_release = false;
    assert(!t9_keypad_is_inline_active());
    assert(lv_obj_has_flag(search_list, LV_OBJ_FLAG_HIDDEN));
    assert(thumbnail_event_count == 3);
    assert(thumbnail_events[1].op == THUMB_END && thumbnail_events[1].list == search_list);
    assert(thumbnail_events[2].op == THUMB_BEGIN && thumbnail_events[2].list == list);
    b->active = false;
    capture_active_items = false;
    group_songs_entries = NULL;
    group_songs_count = 0;
    group_songs_screen = group_songs_list = group_songs_search_list = NULL;
    b->screen = b->list = NULL;
    lv_screen_load(root);
    gui_text_input_teardown();
    lv_obj_delete(screen);
}

static void test_latest_pending_and_label_lifetime(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="paged"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);
    lv_screen_load(screen);
    register_test_search(SEARCH_BINDING_ALL_SONGS, screen, list, NULL, NULL, false, true,
                         METADATA_DB_AZ_ALL_SONGS, test_fetch_page);
    search_binding_t *b = &search_bindings[SEARCH_BINDING_ALL_SONGS];
    b->active = true;
    search_catalog_epoch = 0;
    search_job_active = false;
    search_job_pending_valid = false;
    atomic_store(&search_job_done_flag, false);
    creates = joins = 0;
    capture_thread_calls = true;
    capture_active_items = true;

    search_apply_filter(b, "first");
    assert(search_job_active && creates == 1 && captured_worker && captured_arg);
    void *first_arg = captured_arg;
    void *(*first_worker)(void *) = captured_worker;
    search_apply_filter(b, "older pending");
    search_apply_filter(b, "latest");
    assert(search_job_pending_valid);
    assert(strcmp(search_job_pending_request.query, "latest") == 0);

    first_worker(first_arg);
    poll_search_job();
    assert(joins == 1 && search_job_active && creates == 2);
    assert(captured_worker && captured_arg);
    assert(strcmp(search_job_pending_request.query, "latest") == 0);
    captured_worker(captured_arg);
    poll_search_job();
    assert(joins == 2 && !search_job_active && !search_job_pending_valid);
    assert(b->filtered_count == 1 && b->filtered_indices[0] == 19);
    assert(strcmp(b->filtered_labels[0], "Latest result") == 0);
    assert(last_active_label == b->filtered_labels[0]);

    active_items_cleared = false;
    must_clear_before_free = b->filtered_labels;
    freed_labels_safely = false;
    search_close_internal(b, true);
    assert(freed_labels_safely && must_clear_before_free == NULL);
    assert(active_items_cleared && last_active_label == NULL);
    assert(restored_fetch == test_fetch_page && restored_total == 43);

    /* A catalog generation change must reject a worker result even when its
     * binding remains active and on screen. */
    b->active = true;
    search_apply_filter(b, "first");
    void *stale_arg = captured_arg;
    void *(*stale_worker)(void *) = captured_worker;
    library_search_invalidate_catalog();
    stale_worker(stale_arg);
    poll_search_job();
    assert(joins == 3 && b->filtered_labels == NULL && b->filtered_indices == NULL);

    /* Closing while a query is in flight leaves its eventual result inert. */
    b->active = true;
    search_apply_filter(b, "first");
    void *closed_arg = captured_arg;
    void *(*closed_worker)(void *) = captured_worker;
    search_close_internal(b, true);
    closed_worker(closed_arg);
    poll_search_job();
    assert(joins == 4 && !search_job_active && b->filtered_labels == NULL);
    capture_active_items = false;
    capture_thread_calls = false;
    b->screen = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);
}

static void test_catalog_and_binding_invalidation_intents(lv_obj_t *root) {
    lv_obj_t *sub_screen = lv_obj_create(root);
    compact_list_item_t sub_initial[] = {{.label="Subsonic source"}};
    lv_obj_t *sub_list = build_compact_list_widget(sub_screen, sub_initial, 1, NULL, NULL,
                                                   LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(sub_screen && sub_list);
    register_test_search(SEARCH_BINDING_SUBSONIC_ARTISTS, sub_screen, sub_list,
                         memory_name, &memory_count, false, false,
                         METADATA_DB_AZ_ARTIST, NULL);
    search_binding_t *sub = &search_bindings[SEARCH_BINDING_SUBSONIC_ARTISTS];
    sub->active = true;
    search_apply_filter(sub, "delta");
    assert(sub->filtered_count == 1 && sub->filtered_indices[0] == 1);

    lv_obj_t *db_screen = lv_obj_create(root);
    compact_list_item_t db_initial[] = {{.label="DB source"}};
    lv_obj_t *db_list = build_compact_list_widget(db_screen, db_initial, 1, NULL, NULL,
                                                  LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(db_screen && db_list);
    lv_screen_load(db_screen);
    register_test_search(SEARCH_BINDING_ALL_SONGS, db_screen, db_list, NULL, NULL,
                         false, true, METADATA_DB_AZ_ALL_SONGS, test_fetch_page);
    search_binding_t *db = &search_bindings[SEARCH_BINDING_ALL_SONGS];
    db->active = true;
    capture_active_items = true;
    capture_thread_calls = true;
    creates = joins = 0;
    search_job_active = false;
    search_job_pending_valid = false;
    atomic_store(&search_job_done_flag, false);
    search_apply_filter(db, "latest");
    assert(captured_worker && captured_arg && library_search_has_active_job());
    captured_worker(captured_arg);
    poll_search_job();
    assert(db->filtered_labels && library_search_showing_results(SEARCH_BINDING_ALL_SONGS));

    uint64_t old_epoch = search_catalog_epoch;
    restored_fetch = NULL;
    library_search_invalidate_catalog();
    assert(search_catalog_epoch == old_epoch + 1);
    assert(!search_job_pending_valid);
    library_search_invalidate_bindings();
    assert(!library_search_is_active(SEARCH_BINDING_ALL_SONGS));
    assert(db->filtered_labels == NULL && !library_search_showing_results(SEARCH_BINDING_ALL_SONGS));
    assert(restored_fetch == NULL); /* publication close(false) leaves rearm to the GUI */
    assert(library_search_is_active(SEARCH_BINDING_SUBSONIC_ARTISTS));
    assert(sub->filtered_count == 1 && sub->filtered_indices[0] == 1);
    search_close(sub);
    assert(!library_search_is_active(SEARCH_BINDING_SUBSONIC_ARTISTS));
    capture_active_items = false;
    capture_thread_calls = false;
    db->screen = db->list = NULL;
    sub->screen = sub->list = NULL;
    lv_screen_load(root);
    lv_obj_delete(db_screen);
    lv_obj_delete(sub_screen);
}

static void test_quiesce_and_shutdown_join_contracts(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="source"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);
    lv_screen_load(screen);
    register_test_search(SEARCH_BINDING_ALL_SONGS, screen, list, NULL, NULL,
                         false, true, METADATA_DB_AZ_ALL_SONGS, test_fetch_page);
    search_binding_t *b = &search_bindings[SEARCH_BINDING_ALL_SONGS];
    b->active = true;
    capture_thread_calls = true;
    creates = joins = 0;
    search_job_active = false;
    search_job_pending_valid = false;
    atomic_store(&search_job_done_flag, false);

    search_apply_filter(b, "first");
    assert(library_search_has_active_job());
    search_apply_filter(b, "latest");
    assert(search_job_pending_valid);
    captured_worker(captured_arg);
    library_search_quiesce_for_ui_reload();
    assert(joins == 1 && !library_search_has_active_job());
    assert(!search_job_pending_valid && !atomic_load(&search_job_done_flag));
    assert(search_job_for_binding == NULL && search_job_pending_binding == NULL);
    assert(library_search_is_active(SEARCH_BINDING_ALL_SONGS));

    search_apply_filter(b, "first");
    assert(library_search_has_active_job());
    search_apply_filter(b, "latest");
    assert(search_job_pending_valid);
    captured_worker(captured_arg);
    search_binding_t *saved_job_binding = search_job_for_binding;
    search_binding_t *saved_pending_binding = search_job_pending_binding;
    library_search_join_for_shutdown();
    assert(joins == 2 && !library_search_has_active_job());
    assert(search_job_pending_valid && atomic_load(&search_job_done_flag));
    assert(search_job_for_binding == saved_job_binding && saved_job_binding == b);
    assert(search_job_pending_binding == saved_pending_binding && saved_pending_binding == b);
    assert(library_search_is_active(SEARCH_BINDING_ALL_SONGS));

    /* The fake worker is done; restore a quiescent fixture for later cases. */
    search_job_pending_valid = false;
    search_job_for_binding = NULL;
    search_job_pending_binding = NULL;
    atomic_store(&search_job_done_flag, false);
    capture_thread_calls = false;
    search_close(b);
    b->screen = b->list = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);
}

static void test_recently_added_offsets_and_artwork_alignment(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    compact_list_item_t initial[] = {{.label="paged"}};
    lv_obj_t *list = build_compact_list_widget(screen, initial, 1, NULL, NULL,
                                               LIST_ROW_WIDTH_WIDE, false, NULL);
    assert(screen && list);
    lv_screen_load(screen);
    register_test_search(SEARCH_BINDING_RECENTLY_ADDED, screen, list, NULL, NULL, false, true,
                         METADATA_DB_AZ_ALL_SONGS, test_fetch_page);
    search_binding_t *b = &search_bindings[SEARCH_BINDING_RECENTLY_ADDED];
    b->active = true;
    capture_thread_calls = true;
    capture_active_items = true;
    creates = joins = 0;
    search_job_active = false;
    search_job_pending_valid = false;
    atomic_store(&search_job_done_flag, false);
    search_apply_filter(b, "recent");
    assert(search_job_active && creates == 1 && captured_worker && captured_arg);
    captured_worker(captured_arg);
    poll_search_job();
    assert(joins == 1 && !search_job_active);
    assert(b->filtered_count == 2);
    assert(b->filtered_indices[0] == 10 && b->filtered_indices[1] == 90);
    assert(search_job_result_identity[0] == 902 && search_job_result_identity[1] == 401);
    assert(search_job_result_artwork_key[0] == 0x100000U + 902);
    assert(search_job_result_artwork_key[1] == 0x100000U + 401);
    assert(attached_count == 2);
    assert(attached_identities[0] == 902 && attached_identities[1] == 401);
    assert(attached_artwork_keys[0] == 0x100000U + 902);
    assert(attached_artwork_keys[1] == 0x100000U + 401);
    assert(strcmp(attached_labels[0], "Omega from source 9") == 0);
    assert(strcmp(attached_labels[1], "Alpha from source 4") == 0);
    capture_active_items = false;
    capture_thread_calls = false;
    search_close_internal(b, true);
    b->screen = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);
}

/* Drive the production A-Z poller from a real LVGL pointer device. The
 * metadata table may be empty in this host fixture; these cases characterize
 * ownership, popup lifetime, and timer state, which do not depend on catalog
 * contents. */
static lv_indev_t *az_test_pointer;
static lv_indev_state_t az_test_state;
static lv_point_t az_test_point;
static bool az_test_allow_strip = true;
static bool az_test_strip_allowed(lv_obj_t *list) {
    (void)list;
    return az_test_allow_strip;
}
static void az_test_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    data->state = az_test_state;
    data->point = az_test_point;
}
static void az_test_sample(lv_indev_state_t state, int x, int y) {
    az_test_state = state;
    az_test_point.x = x;
    az_test_point.y = y;
    lv_indev_read(az_test_pointer);
}
static void test_az_drag_press_ownership_and_teardown(lv_obj_t *root) {
    lv_obj_t *screen = lv_obj_create(root);
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_t *strip = lv_obj_create(screen);
    lv_obj_t *popup = lv_obj_create(screen);
    lv_obj_t *label = lv_label_create(popup);
    assert(screen && list && strip && popup && label);
    lv_obj_set_pos(strip, 440, 100);
    lv_obj_set_size(strip, 24, 540);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, 430, 700);
    lv_obj_set_pos(popup, 400, 250);
    lv_obj_set_size(popup, 36, 36);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
    az_index_bindings[0] = (az_index_binding_t){screen,list,strip,popup,label,
                                                METADATA_DB_AZ_ALL_SONGS};
    az_index_registered_count = 1;
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL, NULL);
    lv_screen_load(screen);
    az_test_pointer = lv_indev_create();
    assert(az_test_pointer);
    lv_indev_set_type(az_test_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(az_test_pointer, az_test_read);
    lv_indev_set_mode(az_test_pointer, LV_INDEV_MODE_EVENT);
    lv_indev_set_display(az_test_pointer, test_display);
    library_az_index_init(find_pointer_indev, az_index_strip_allowed);
    assert(az_index_drag_timer);
    lv_area_t test_strip_area;
    lv_obj_update_layout(screen);
    lv_obj_get_coords(strip, &test_strip_area);
    int strip_x = (test_strip_area.x1 + test_strip_area.x2) / 2;
    int strip_y = (test_strip_area.y1 + test_strip_area.y2) / 2;

    /* A swipe that starts outside the visible strip cannot acquire it later. */
    az_test_sample(LV_INDEV_STATE_PRESSED, 300, strip_y);
    poll_az_index_drag(az_index_drag_timer);
    az_test_sample(LV_INDEV_STATE_PRESSED, strip_x, strip_y);
    poll_az_index_drag(az_index_drag_timer);
    assert(!az_index_dragging);
    assert(lv_obj_has_flag(popup, LV_OBJ_FLAG_HIDDEN));
    az_test_sample(LV_INDEV_STATE_RELEASED, strip_x, strip_y);
    poll_az_index_drag(az_index_drag_timer);

    /* A fresh press beginning on the strip owns the drag and opens the bubble;
     * release closes it and pauses the poll timer. */
    gui_library_resume_fast_timers();
    az_test_sample(LV_INDEV_STATE_PRESSED, strip_x, strip_y);
    assert(lv_indev_get_state(az_test_pointer) == LV_INDEV_STATE_PRESSED);
    assert(lv_screen_active() == screen);
    assert(find_pointer_indev() == az_test_pointer);
    poll_az_index_drag(az_index_drag_timer);
    assert(az_index_dragging);
    assert(!lv_obj_has_flag(popup, LV_OBJ_FLAG_HIDDEN));
    az_test_sample(LV_INDEV_STATE_RELEASED, strip_x, strip_y);
    poll_az_index_drag(az_index_drag_timer);
    assert(!az_index_dragging);
    assert(lv_obj_has_flag(popup, LV_OBJ_FLAG_HIDDEN));
    assert(lv_timer_get_paused(az_index_drag_timer));

    /* Reset during an owned press hides the popup and pauses polling. The
     * held pointer cannot be reconsidered until the next press edge resumes. */
    gui_library_resume_fast_timers();
    az_test_sample(LV_INDEV_STATE_PRESSED, strip_x, strip_y);
    poll_az_index_drag(az_index_drag_timer);
    assert(az_index_dragging);
    gui_library_reset_drag_state();
    assert(!az_index_dragging && lv_timer_get_paused(az_index_drag_timer));
    assert(lv_obj_has_flag(popup, LV_OBJ_FLAG_HIDDEN));
    az_test_sample(LV_INDEV_STATE_PRESSED, strip_x, strip_y);
    assert(lv_timer_get_paused(az_index_drag_timer));
    az_test_sample(LV_INDEV_STATE_RELEASED, strip_x, strip_y);
    gui_library_resume_fast_timers();
    az_test_sample(LV_INDEV_STATE_PRESSED, strip_x, strip_y);
    poll_az_index_drag(az_index_drag_timer);
    assert(az_index_dragging);

    /* Scroll visibility owns a separate timer and reset removes its binding. */
    lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
    assert(az_index_visibility_timer && az_index_visibility_binding);
    library_az_index_reset_bindings();
    assert(!az_index_visibility_timer && !az_index_visibility_binding);
    /* Exercise the GUI policy hook used to suppress the strip while albums
     * are sorted by a non-name key. The test controls the policy result so it
     * does not need to initialize a catalog database. */
    az_index_registered_count = 1;
    az_test_allow_strip = false;
    library_az_index_init(find_pointer_indev, az_test_strip_allowed);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
    assert(lv_obj_has_flag(strip, LV_OBJ_FLAG_HIDDEN));
    assert(!az_index_visibility_timer && !az_index_visibility_binding);
    az_test_allow_strip = true;
    library_az_index_init(find_pointer_indev, az_index_strip_allowed);
    library_az_index_reset_bindings();
    gui_library_reset_drag_state();
    lv_timer_delete(az_index_drag_timer);
    az_index_drag_timer = NULL;
    az_test_sample(LV_INDEV_STATE_RELEASED, strip_x, strip_y);
    lv_indev_delete(az_test_pointer);
    az_test_pointer = NULL;
    lv_screen_load(root);
    lv_obj_delete(screen);
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    test_display = lv_display_create(480, 800);
    assert(test_display);
    static uint8_t draw_buffer[480 * 12 * 4];
    lv_display_set_buffers(test_display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(test_display, flush_ready);
    gui_theme_init();
    screen_builders_init_list_row_style();
    lv_obj_t *root = lv_obj_create(NULL);
    assert(root);
    lv_screen_load(root);
    test_memory_backed_remap_and_close(root);
    test_public_registration_copies_config(root);
    test_refresh_callback_call_points(root);
    test_latest_pending_and_label_lifetime(root);
    test_catalog_and_binding_invalidation_intents(root);
    test_quiesce_and_shutdown_join_contracts(root);
    test_recently_added_offsets_and_artwork_alignment(root);
    test_az_drag_press_ownership_and_teardown(root);
    test_group_songs_basename_fallback(root);
    test_real_t9_search_dismiss_reopen_and_files_overlay(root);
    lv_screen_load(root);
    lv_obj_delete(root);
    puts("library_search component integration (wrapper, lifetimes, remap, worker, T9 and overlays): PASS");
    return 0;
}
