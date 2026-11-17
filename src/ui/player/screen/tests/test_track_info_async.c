#include "../../../widgets/cover_flow/cover_flow_internal.h"
#include "src/ui/player/lyrics/player_inline_lyrics.h"
#include "src/ui/shell/gui_shell.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "src/ui/widgets/controls/star_rating.h"
#include "src/ui/widgets/controls/star_rating_icons.h"
#include "src/ui/library/browse/gui_group_songs_internal.h"
#include "src/ui/widgets/controls/gui_rating_popup.h"
#include "src/ui/library/browse/gui_album_header_internal.h"
#include "src/ui/application/gui.h"
#include "src/ui/player/screen/gui_track_info.h"
#include "src/ui/player/screen/gui_player_internal.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/ui/library/browse/gui_library_internal.h"
#include "src/ui/library/library_az_index.h"
#include "src/library/metadata/db/album_rating.h"
#include "src/library/metadata/db/metadata_db_internal.h"
#include "src/library/remote/remote_state.h"
#include "src/ui/appearance/gui_theme.h"
#include "src/ui/player/artwork/player_backdrop.h"
#include "src/ui/player/layout/player_layouts.h"
#include "src/library/playlists/rating_writer.h"
#include "src/ui/player/queue/player_queue_model.h"
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static atomic_bool all_rating_fixture, all_rating_hold, all_rating_fail;
static atomic_int all_rating_reads, all_rating_variant, all_rating_failures;
static compact_list_click_cb_t all_click, all_menu;
static int all_menu_rank = -1;
static int all_playing_row = -1;
void __real_compact_list_set_now_playing(lv_obj_t *, int);
void __wrap_compact_list_set_now_playing(lv_obj_t *list, int row) {
    if (list == all_songs_list) all_playing_row = row;
    __real_compact_list_set_now_playing(list, row);
}
static int snapshot_refs, snapshot_opens;
static bool snapshot_wrong_count;
static bool has_text(lv_obj_t *obj, const char *text);
static atomic_int favorite_reads, favorite_resolves;
static atomic_bool hold_favorites, sidecar_favorites;
static bool current_track;
static const char * current_path = "./music/current.wav";
static pthread_t ui_thread;
static atomic_int rating_reads;
static atomic_bool hold_popup, popup_entered;
static const char *popup_song = "./music/popup-rated.wav";
int __real_rating_writer_stars(const char *path);
int __wrap_rating_writer_stars(const char * path) {
    if (path && !strcmp(path, popup_song)) return __real_rating_writer_stars(path);
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(path && path[0]);
    if (strstr(path, "Popup album")) {
        atomic_store(&popup_entered, true);
        while (atomic_load(&hold_popup)) usleep(1000);
    }
    atomic_fetch_add(&rating_reads, 1);
    return 3;
}
static atomic_int list_fixture;
static atomic_int song_loads;
static atomic_bool hold_song_loads;
static atomic_int held_song_loads;
static int album_item_count, playback_count, source_count;
static const int list_stars[] = {5, 5, 4, 3, 2, 1};

static void load_list_albums(rated_album_t **out, int *count) {
    *count = atomic_load(&list_fixture) == 2 ? 2 : 6;
    *out = calloc((size_t)*count, sizeof(**out));
    assert(*out);
    for (int i = 0; i < *count; i++) {
        char names[80];
        snprintf(names, sizeof(names), "Filter artist\tFilter album %d", i);
        char *copy = strdup(names);
        assert(copy);
        copy[strlen("Filter artist")] = '\0';
        (*out)[i] = (rated_album_t){copy, copy + sizeof("Filter artist"),
            *count == 2 ? 1 : list_stars[i]};
    }
}

void __real_metadata_db_load_favorite_songs(char ***, int *, int **);
void __real_metadata_db_load_rated_albums(rated_album_t **, int *);
void __wrap_metadata_db_load_favorite_songs(char ***paths, int *count, int **stars) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    song_loads++;
    if (atomic_load(&list_fixture) == 3) {
        __real_metadata_db_load_favorite_songs(paths, count, stars);
        return;
    }
    *count = atomic_load(&list_fixture) == 2 ? 0 : 4;
    *paths = calloc((size_t)(*count + 1), sizeof(**paths));
    assert(*paths && stars);
    *stars = calloc((size_t)(*count + 1), sizeof(**stars));
    assert(*stars);
    for (int i = 0; i < *count; i++) {
        char path[80];
        snprintf(path, sizeof(path), "./music/filter-%d.wav", i);
        (*paths)[i] = strdup(path);
        (*stars)[i] = *count == 2 ? 1 : list_stars[i];
    }
    /* Hold after reading, so the result is a snapshot of the old data. */
    if (atomic_load(&hold_song_loads)) {
        atomic_fetch_add(&held_song_loads, 1);
        while (atomic_load(&hold_song_loads)) usleep(1000);
    }
}

static int section_values[2][3], section_count[2];
static compact_list_click_cb_t album_click, album_menu;
static int album_opened, album_menu_opened;

lv_obj_t *__real_build_compact_list_screen(const char *, lv_event_cb_t,
    const compact_list_item_t *, int, compact_list_click_cb_t, compact_list_click_cb_t,
    lv_obj_t **, lv_obj_t **, int32_t, bool, lv_style_t *);
lv_obj_t *__wrap_build_compact_list_screen(const char *title, lv_event_cb_t back,
    const compact_list_item_t *items, int count, compact_list_click_cb_t click,
    compact_list_click_cb_t menu, lv_obj_t **list, lv_obj_t **label,
    int32_t width, bool playing, lv_style_t *style) {
    if (!strcmp(title, "All songs")) { all_click = click; all_menu = menu; }
    if (!strcmp(title, "Top rated albums")) { album_click = click; album_menu = menu; }
    return __real_build_compact_list_screen(title, back, items, count, click, menu,
                                          list, label, width, playing, style);
}
void __wrap_gui_library_open_album(const char *name, const char *artist) {
    assert(!strcmp(artist, "Filter artist"));
    assert(sscanf(name, "Filter album %d", &album_opened) == 1);
}
void __wrap_gui_library_open_album_menu(const char *name, const char *artist) {
    assert(!strcmp(artist, "Filter artist"));
    assert(sscanf(name, "Filter album %d", &album_menu_opened) == 1);
}

void __real_compact_list_set_items(lv_obj_t *, const compact_list_item_t *, int);
void __wrap_compact_list_set_items(lv_obj_t *list, const compact_list_item_t *items, int count) {
    if ((lv_obj_get_name(list) && !strcmp(lv_obj_get_name(list), "top_rated_albums"))) {
        album_item_count = 0;
        for (int i = 0; i < count; i++)
            if (!items[i].is_section_header) album_item_count++;
    }
    int kind = (lv_obj_get_name(list) && !strcmp(lv_obj_get_name(list), "top_rated_albums")) ? 1 : 0;
    if (kind || list == group_songs_list) {
        section_count[kind] = 0;
        for (int i = 0; i < count; i++) {
            if (!items[i].section_stars) continue;
            assert(items[i].is_section_header && section_count[kind] < 3);
            section_values[kind][section_count[kind]++] = items[i].section_stars;
        }
    }
    __real_compact_list_set_items(list, items, count);
}

void __wrap_on_file_selected(char **paths, int count, int selected) {
    assert(selected >= 0 && selected < count);
    playback_count = count;
    for (int i = 0; i < count; i++) {
        char expected[80];
        snprintf(expected, sizeof(expected), "./music/filter-%d.wav", i);
        assert(!strcmp(paths[i], expected));
        free(paths[i]);
    }
    free(paths);
}

void __wrap_set_player_source_group_songs_direct(const group_song_entry_t *entries,
                                                int count, const char *title, int selected) {
    assert(entries && selected >= 0 && selected < count);
    assert(!strcmp(title, "Top rated") || (getenv("COMPAS_TEST_MOST_PLAYED") && !strcmp(title, "Most played")));
    source_count = count;
}

static void load_test_albums(rated_album_t **out, int *count) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    atomic_fetch_add(&favorite_reads, 1);
    if (atomic_load(&list_fixture) == 3) {
        __real_metadata_db_load_rated_albums(out, count);
        return;
    }
    if (atomic_load(&list_fixture)) { load_list_albums(out, count); return; }
    if (atomic_load(&sidecar_favorites)) {
        int32_t rating = 0;
        remote_state_get("album://Favorite artist\tWorker album", &rating, NULL, NULL);
        if (!rating) { *out = NULL; *count = 0; return; }
    }
    *out = calloc(1, sizeof(**out));
    char *names = strdup("Favorite artist\tWorker album");
    assert(*out && names);
    names[strlen("Favorite artist")] = '\0';
    (*out)[0] = (rated_album_t){names, names + sizeof("Favorite artist"), 4};
    *count = 1;
}
void __wrap_metadata_db_load_rated_albums(rated_album_t **out, int *count) {
    load_test_albums(out, count);
    while (atomic_load(&hold_favorites)) usleep(1000);
}
bool __wrap_album_group_resolve(const char *album, const char *artist, group_row_t *out) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    atomic_fetch_add(&favorite_resolves, 1);
    snprintf(out->name, sizeof(out->name), "%s", album);
    snprintf(out->album_artist, sizeof(out->album_artist), "%s", artist);
    return true;
}
static void pump_ui(void) {
    lv_tick_inc(25);
    lv_timer_handler();
    usleep(1000);
}
static int visible_text_count(lv_obj_t *obj, const char *text) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;
    int count = lv_obj_check_type(obj, &lv_label_class) &&
                !strcmp(lv_label_get_text(obj), text);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        count += visible_text_count(lv_obj_get_child(obj, i), text);
    return count;
}
static void check_favorite_empty(void) {
    lv_obj_t *screen = lv_screen_active();
    assert(visible_text_count(screen, "No albums rated 3 stars or more yet.") == 1);
    assert(visible_text_count(screen, "No items") == 0);
    assert(visible_text_count(screen, "There are no entries in this view.") == 0);
    assert(visible_text_count(screen, "Worker album") == 0);
}
static void check_favorite_empty_results(void) {
    atomic_store(&sidecar_favorites, true);
    int reads = atomic_load(&favorite_reads);
    show_favorite_albums();
    for (int i = 0; i < 1000 && atomic_load(&favorite_reads) == reads; i++) pump_ui();
    assert(atomic_load(&favorite_reads) > reads);
    for (int i = 0; i < 100; i++) pump_ui();
    check_favorite_empty();
    assert(remote_state_set_rating("album://Favorite artist\tWorker album", 4));
    for (int i = 0; i < 2000 && !visible_text_count(lv_screen_active(), "Worker album"); i++) pump_ui();
    assert(visible_text_count(lv_screen_active(), "Worker album") == 1);
    assert(visible_text_count(lv_screen_active(), "No albums rated 3 stars or more yet.") == 0);
    assert(remote_state_set_rating("album://Favorite artist\tWorker album", 0));
    for (int i = 0; i < 2000 && visible_text_count(lv_screen_active(), "Worker album"); i++) pump_ui();
    check_favorite_empty();
    nav_pop_immediate();
    favorite_albums_teardown();
}
static void check_favorite_albums(void) {
    atomic_store(&hold_favorites, true);
    show_favorite_albums();
    for (int i = 0; i < 1000 && !atomic_load(&favorite_reads); i++) pump_ui();
    assert(atomic_load(&favorite_reads) == 1);
    assert(!has_text(lv_screen_active(), "Worker album"));
    nav_pop_immediate();
    favorite_albums_teardown();
    show_favorite_albums();
    atomic_store(&hold_favorites, false);
    for (int i = 0; i < 2000 && !has_text(lv_screen_active(), "Worker album"); i++) pump_ui();
    assert(has_text(lv_screen_active(), "Worker album"));
    assert(atomic_load(&favorite_reads) == 2);
    assert(atomic_load(&favorite_resolves) == 2);
    nav_pop_immediate();
    favorite_albums_teardown();
}
bool __wrap_gui_player_has_active_track(void) { return current_track; }
const char * __wrap_gui_player_get_current_track_path(void) { return current_path; }

static atomic_bool hold_probe, entered;
bool __real_audio_probe_file_format(const char *, audio_current_format_info_t *);
bool __wrap_audio_probe_file_format(const char *path, audio_current_format_info_t *out) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(!strstr(path, "#compas-cue="));
    atomic_store(&entered, true);
    while (atomic_load(&hold_probe)) usleep(1000);
    return __real_audio_probe_file_format(path, out);
}
static bool has_text(lv_obj_t *obj, const char *text) {
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        if (has_text(lv_obj_get_child(obj, i), text)) return true;
    return false;
}
static void wait_for(const char *text) {
    for (int i = 0; i < 2000 && !has_text(gui_track_info_get_screen(), text); i++) {
        gui_track_info_poll(); usleep(1000);
    }
    assert(has_text(gui_track_info_get_screen(), text));
}
static void check_current_rating(int stars) {
    nav_pop_immediate();
    current_track = true;
    gui_track_info_context_t context = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(context.path, "./music/current.wav");
    gui_track_info_set_current(&context);
    int reads = atomic_load(&rating_reads);
    gui_player_set_rating_path(current_path, 2);
    gui_track_info_open();
    wait_for("2/5");
    gui_player_set_rating_path(current_path, 5);
    gui_track_info_poll();
    assert(has_text(gui_track_info_get_screen(), "5/5"));
    assert(!has_text(gui_track_info_get_screen(), "2/5"));
    for (int i = 0; i < 100; i++) { gui_track_info_poll(); usleep(1000); }
    assert(atomic_load(&rating_reads) == reads + 1);
    gui_player_set_rating_path(current_path, stars);
    gui_track_info_poll();
    assert(has_text(gui_track_info_get_screen(), "Rating") == (stars > 0));
    if (stars > 0) assert(has_text(gui_track_info_get_screen(), "4/5"));
    /* Playback advances while Information stays on this song. */
    current_track = false;
    show_now_playing_rating(1);
    gui_track_info_poll();
    assert(has_text(gui_track_info_get_screen(), "Rating") == (stars > 0));
    if (stars > 0) assert(has_text(gui_track_info_get_screen(), "4/5"));
    assert(!has_text(gui_track_info_get_screen(), "3/5"));
    assert(!has_text(gui_track_info_get_screen(), "1/5"));
    assert(atomic_load(&rating_reads) == reads + 1);
}

static void check_rating_during_probe(int stars) {
    nav_pop_immediate();
    current_track = true;
    current_path = "./music/selected.wav";
    gui_track_info_context_t context = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(context.path, current_path);
    gui_track_info_set_current(&context);
    show_now_playing_rating(3);
    atomic_store(&hold_probe, true);
    atomic_store(&entered, false);
    int reads = atomic_load(&rating_reads);
    gui_track_info_open();
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    assert(atomic_load(&rating_reads) == reads + 1);
    gui_player_set_rating_path(current_path, stars);
    atomic_store(&hold_probe, false);
    wait_for("16-bit / 48 kHz");
    /* Keep the edited rating after playback advances. */
    current_track = false;
    current_path = "./music/current.wav";
    show_now_playing_rating(1);
    gui_track_info_poll();
    assert(has_text(gui_track_info_get_screen(), "Rating") == (stars > 0));
    if (stars > 0) assert(has_text(gui_track_info_get_screen(), "4/5"));
    assert(!has_text(gui_track_info_get_screen(), "3/5"));
    assert(!has_text(gui_track_info_get_screen(), "1/5"));
    assert(atomic_load(&rating_reads) == reads + 1);
}

static void check_placeholder_rating(void) {
    nav_pop_immediate();
    current_track = true;
    current_path = "./music/current.wav";
    gui_track_info_context_t selected = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(selected.path, "./music/selected.wav");
    gui_track_info_open_for_context(&selected);
    wait_for("16-bit / 48 kHz");
    assert(has_text(gui_track_info_get_screen(), "3/5"));
    int reads = atomic_load(&rating_reads);
    /* Playback enters B, then leaves before its rating loads. */
    current_path = selected.path;
    show_now_playing_rating(0);
    gui_track_info_poll();
    current_path = "./music/current.wav";
    show_now_playing_rating(1);
    gui_track_info_poll();
    assert(has_text(gui_track_info_get_screen(), "3/5"));
    assert(!has_text(gui_track_info_get_screen(), "1/5"));
    assert(atomic_load(&rating_reads) == reads);
}

static void check_stale_completion(gui_track_info_context_t * selected) {
    /* A late result after leaving must not replace the playing context. */
    nav_pop_immediate();
    atomic_store(&hold_probe, true); atomic_store(&entered, false);
    gui_track_info_open_for_context(selected);
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    nav_pop_immediate();
    atomic_store(&hold_probe, false);
    gui_track_info_open();
    for (int i = 0; i < 100; i++) { gui_track_info_poll(); usleep(1000); }
    assert(!has_text(gui_track_info_get_screen(), "16-bit / 48 kHz"));
}

static lv_obj_t *find_full_rating(lv_obj_t *obj);

static void check_rating_clearance(lv_obj_t *obj, lv_obj_t *hit, const lv_area_t *area) {
    if (obj == hit || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_area_t other, overlap;
    lv_obj_get_coords(obj, &other);
    const char *name = lv_obj_get_name(obj);
    bool cover = name && (!strcmp(name, "cover_card") || !strcmp(name, "cover_img"));
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) && !cover &&
        lv_obj_get_child_count(obj) == 0) {
        if (lv_obj_check_type(obj, &lv_arc_class)) {
            for (int32_t y = area->y1; y <= area->y2; y++)
                for (int32_t x = area->x1; x <= area->x2; x++) {
                    lv_point_t point = {x, y};
                    assert(!lv_obj_hit_test(obj, &point));
                }
            return;
        }
        if (lv_area_intersect(&overlap, area, &other))
            fprintf(stderr, "Rating overlaps %s at %d,%d-%d,%d\n", name ? name : "hit",
                    (int)other.x1, (int)other.y1, (int)other.x2, (int)other.y2);
        assert(!lv_area_intersect(&overlap, area, &other));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        check_rating_clearance(lv_obj_get_child(obj, i), hit, area);
}

void __real_register_swipe_dead_zone(lv_obj_t *obj);
void __wrap_register_swipe_dead_zone(lv_obj_t *obj) {
    const char *name = lv_obj_get_name(obj);
    assert(!name || strcmp(name, "cover_card"));
    __real_register_swipe_dead_zone(obj);
}

static bool checking_swipe;
static int swipe_step;
static int shell_gestures;
static void observe_shell_gesture(lv_event_t *e) {
    (void)e;
    shell_gestures++;
}
bool __real_gui_player_transport_button(int direction);
bool __wrap_gui_player_transport_button(int direction) {
    if (!checking_swipe) return __real_gui_player_transport_button(direction);
    swipe_step += direction;
    return true;
}

static lv_indev_data_t rating_pointer;

static void read_rating_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    *data = rating_pointer;
}

static void check_cover_swipe(lv_indev_t *pointer, lv_point_t start, int step) {
    checking_swipe = true;
    swipe_step = 0;
    shell_gestures = 0;
    assert(!point_in_swipe_dead_zone(start));
    rating_pointer.point = start;
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    for (int i = 0; i < 6; i++) {
        rating_pointer.point.x -= step * 12;
        lv_indev_read(pointer);
    }
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    assert(swipe_step == 0);
    assert(shell_gestures == 1);
    assert(!lv_obj_has_state(gui_player_rating_control(), LV_STATE_PRESSED));
    /* Left gestures retain HEAD's release-click behavior. Right gestures
     * are consumed by shell navigation before release. */
    if (step < 0) {
        assert(lv_screen_active() != gui_player_get_screen());
        assert(!find_full_rating(lv_layer_top()));
        assert(!player_inline_lyrics_is_open());
    }
    gui_rating_popup_teardown();
    player_inline_lyrics_set_open(false, false);
    lv_screen_load(gui_player_get_screen());
    checking_swipe = false;
}

static void check_cover_lyrics(lv_indev_t *pointer, lv_obj_t *hit, lv_point_t point) {
    bool enabled = current_settings.lyrics_enabled;
    current_settings.lyrics_enabled = true;
    rating_pointer.point = point;
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    bool supports_lyrics = player_layouts_get_interactions()->lyrics_enabled;
    assert(player_inline_lyrics_is_open() == supports_lyrics);
    player_inline_lyrics_set_open(true, false);
    assert(lv_obj_has_flag(hit, LV_OBJ_FLAG_HIDDEN) == supports_lyrics);
    assert(!find_full_rating(lv_layer_top()));
    player_inline_lyrics_set_open(false, false);
    assert(!lv_obj_has_flag(hit, LV_OBJ_FLAG_HIDDEN));
    current_settings.lyrics_enabled = enabled;
}

/* Finds an exposed point whose touch target is `want` (cover image or bare card). */
static bool find_cover_point(lv_obj_t *screen, lv_obj_t *want, lv_point_t *out) {
    lv_area_t area;
    lv_obj_get_coords(lv_obj_find_by_name(screen, "cover_card"), &area);
    for (int32_t y = area.y1 + 8; y < area.y2; y += 8)
        for (int32_t x = area.x1 + 8; x < area.x2; x += 8) {
            lv_point_t point = {x, y};
            if (lv_indev_search_obj(screen, &point) == want &&
                !lv_indev_search_obj(lv_layer_top(), &point) &&
                !lv_indev_search_obj(lv_layer_sys(), &point)) { *out = point; return true; }
        }
    return false;
}

/* As before the rating work, the cover image opens lyrics and bare card space does not. */
static void check_cover_taps(lv_obj_t *screen, lv_indev_t *pointer, lv_obj_t *hit) {
    lv_point_t point;
    bool image = find_cover_point(screen, lv_obj_find_by_name(screen, "cover_img"), &point);
    if (image) check_cover_lyrics(pointer, hit, point);
    bool card = find_cover_point(screen, lv_obj_find_by_name(screen, "cover_card"), &point);
    if (!card) return;
    bool enabled = current_settings.lyrics_enabled;
    current_settings.lyrics_enabled = true;
    rating_pointer.point = point;
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    assert(!player_inline_lyrics_is_open());
    current_settings.lyrics_enabled = enabled;
}

static void tap_rating(lv_indev_t *pointer) {
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
}

static void check_rating_edge(lv_obj_t *screen, lv_obj_t *hit, const lv_area_t *area, bool on_cover) {
    lv_obj_t *saved = lv_screen_active();
    int animation_scale = current_settings.animation_scale;
    current_settings.animation_scale = 0;
    lv_screen_load(screen);
    lv_obj_update_layout(screen);
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_rating_pointer);
    /* Off the cover the area may reach the shell's top-edge drawer pull, which
     * takes presses there: tap the lower left corner instead of mid-height. */
    rating_pointer.point = (lv_point_t){area->x1 + 1, on_cover ? (area->y1 + area->y2) / 2 : area->y2 - 1};
    assert(lv_indev_search_obj(screen, &rating_pointer.point) == hit);
    if (on_cover) {
        lv_obj_t *cover = lv_obj_find_by_name(screen, "cover_img");
        assert(!lv_obj_hit_test(cover, &rating_pointer.point));
        assert(!lv_obj_hit_test(lv_obj_find_by_name(screen, "cover_card"), &rating_pointer.point));
        assert(!point_in_swipe_dead_zone(rating_pointer.point));
    }
    current_track = true;
    show_now_playing_rating(2);
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    assert(lv_obj_has_state(gui_player_rating_control(), LV_STATE_PRESSED));
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    /* A tap rates 5 without the popup; a second tap clears the 5. */
    assert(star_rating_get(gui_player_rating_control()) == 5);
    assert(!find_full_rating(lv_layer_top()));
    assert(!player_inline_lyrics_is_open());
    assert(!lv_obj_has_state(gui_player_rating_control(), LV_STATE_PRESSED));
    tap_rating(pointer);
    assert(star_rating_get(gui_player_rating_control()) == 0);
    /* Holding opens the popup and leaves the rating alone. */
    rating_pointer.state = LV_INDEV_STATE_PRESSED;
    for (int i = 0; i < 40 && !find_full_rating(lv_layer_top()); i++) {
        lv_tick_inc(25);
        usleep(25000); /* the host tick may follow the real clock */
        lv_indev_read(pointer);
    }
    rating_pointer.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    assert(find_full_rating(lv_layer_top()));
    assert(star_rating_get(gui_player_rating_control()) == 0);
    gui_rating_popup_teardown();
    if (on_cover) {
        lv_obj_add_event_cb(screen, observe_shell_gesture, LV_EVENT_GESTURE, NULL);
        check_cover_swipe(pointer, rating_pointer.point, 1);
        lv_area_t card;
        lv_obj_get_coords(lv_obj_find_by_name(screen, "cover_card"), &card);
        lv_point_t middle = {(card.x1 + card.x2) / 2, (card.y1 + card.y2) / 2};
        check_cover_swipe(pointer, middle, 1);
        check_cover_swipe(pointer, middle, -1);
        lv_obj_remove_event_cb(screen, observe_shell_gesture);
        check_cover_taps(screen, pointer, hit);
    }
    current_track = false;
    lv_indev_delete(pointer);
    lv_screen_load(saved);
    current_settings.animation_scale = animation_scale;
}

static void check_rating_roles(lv_obj_t *screen, const lv_area_t *area) {
    const char *names[] = {"quality_pill", "song_count", "lyrics_toggle", "controls_row"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        lv_obj_t *obj = lv_obj_find_by_name(screen, names[i]);
        if (!obj) continue;
        lv_area_t other, overlap;
        lv_obj_get_coords(obj, &other);
        assert(!lv_area_intersect(&overlap, area, &other));
    }
}

/* The compact control: star image, then the count chip holding its label. */
static lv_obj_t *rating_chip(lv_obj_t *control) { return lv_obj_get_child(control, 1); }
static lv_obj_t *rating_digit(lv_obj_t *control) { return lv_obj_get_child(rating_chip(control), 0); }

static void check_inside_clip(lv_obj_t *cover, const lv_area_t *area) {
    lv_area_t card;
    lv_obj_get_coords(cover, &card);
    int32_t radius = lv_obj_get_style_radius(cover, 0);
    lv_point_t corners[] = {{area->x1, area->y1}, {area->x2, area->y1},
                            {area->x1, area->y2}, {area->x2, area->y2}};
    for (unsigned i = 0; i < 4; i++)
        assert(lv_area_is_point_on(&card, &corners[i], radius));
}

static void check_rating_geometry(lv_obj_t *screen, lv_obj_t *control) {
    lv_obj_t *hit = lv_obj_find_by_name(screen, "rating_hit");
    lv_area_t area, card, visible;
    assert(hit);
    lv_obj_get_coords(hit, &area);
    lv_obj_get_coords(lv_obj_find_by_name(screen, "cover_card"), &card);
    lv_obj_get_coords(control, &visible);
    int32_t size = LV_MAX(48, BOARD_SCALE_PX(72));
    assert(lv_area_get_width(&area) == size && lv_area_get_height(&area) == size);
    assert(area.x1 >= card.x1 && area.y1 >= card.y1 && area.x2 <= card.x2 && area.y2 <= card.y2);
    assert(visible.x1 >= area.x1 && visible.y1 >= area.y1 && visible.x2 <= area.x2 && visible.y2 <= area.y2);
    assert(abs(area.x1 + area.x2 - visible.x1 - visible.x2) <= 1);
    assert(abs(area.y1 + area.y2 - visible.y1 - visible.y2) <= 1);
    assert(lv_obj_get_width(lv_obj_get_child(control, 0)) == LV_MAX(32, BOARD_SCALE_PX(48)));
    assert(lv_obj_get_style_text_font(rating_digit(control), 0) == &app_font_16);
    check_inside_clip(lv_obj_find_by_name(screen, "cover_card"), &area);
    check_inside_clip(lv_obj_find_by_name(screen, "cover_card"), &visible);
    check_rating_clearance(screen, hit, &area);
    check_rating_roles(screen, &area);
    check_rating_edge(screen, hit, &area, true);
    printf("Player star %d, hit %d at %d,%d inside cover %d,%d-%d,%d\n",
           LV_MAX(32, BOARD_SCALE_PX(48)), (int)size, (int)area.x1, (int)area.y1,
           (int)card.x1, (int)card.y1, (int)card.x2, (int)card.y2);
}

static bool rating_anchor_on_cover(lv_obj_t *screen, lv_obj_t *anchor) {
    lv_obj_t *card = lv_obj_find_by_name(screen, "cover_card");
    for (lv_obj_t *p = lv_obj_get_parent(anchor); p; p = lv_obj_get_parent(p))
        if (p == card) return true;
    return false;
}

/* The layout's anchor stays where the plugin put it; COMPAS_TEST_ANCHOR is
 * "x y w h parent_name" read from the XML when those are plain numbers. */
static void check_anchor_kept(lv_obj_t *anchor) {
    const char *spec = getenv("COMPAS_TEST_ANCHOR");
    if (!spec || !*spec) return;
    int x, y, w, h;
    char parent[64] = "";
    int fields = sscanf(spec, "%d %d %d %d %63s", &x, &y, &w, &h, parent);
    assert(fields >= 4);
    assert(lv_obj_get_x(anchor) == x && lv_obj_get_y(anchor) == y);
    assert(lv_obj_get_width(anchor) == w && lv_obj_get_height(anchor) == h);
    if (fields == 5) assert(!strcmp(lv_obj_get_name(lv_obj_get_parent(anchor)), parent));
}

/* The count chip may overlap the star but never cover most of it. */
static void check_chip_leaves_star(lv_obj_t *control) {
    lv_area_t star, chip, overlap;
    lv_obj_get_coords(lv_obj_get_child(control, 0), &star);
    lv_obj_get_coords(rating_chip(control), &chip);
    if (lv_obj_has_flag(rating_chip(control), LV_OBJ_FLAG_HIDDEN)) return;
    if (!lv_area_intersect(&overlap, &star, &chip)) return;
    assert(lv_area_get_size(&overlap) * 2 < lv_area_get_size(&star));
}

/* Every size a layout can ask for, rated: the star stays visible next to its chip. */
static void check_small_star_chips(void) {
    static const int32_t sizes[] = {16, 18, 20, 24, 28, 32, 40, 48};
    lv_obj_t *screen = lv_obj_create(NULL);
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        lv_obj_t *control = star_rating_create(screen, sizes[i], true);
        star_rating_set(control, 5);
        lv_obj_update_layout(screen);
        assert(!lv_obj_has_flag(rating_chip(control), LV_OBJ_FLAG_HIDDEN));
        check_chip_leaves_star(control);
        lv_obj_delete(control);
    }
    lv_obj_delete(screen);
}

static void check_rating_geometry_off_cover(lv_obj_t *screen, lv_obj_t *anchor, lv_obj_t *control) {
    lv_obj_t *hit = lv_obj_find_by_name(screen, "rating_hit");
    lv_area_t area, bounds, visible, box;
    assert(hit);
    assert(lv_obj_has_flag(anchor, LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    lv_obj_get_coords(hit, &area);
    lv_obj_get_coords(screen, &bounds);
    lv_obj_get_coords(control, &visible);
    lv_obj_get_coords(anchor, &box);
    assert(abs(box.x1 + box.x2 - visible.x1 - visible.x2) <= 1);
    assert(abs(box.y1 + box.y2 - visible.y1 - visible.y2) <= 1);
    lv_obj_t *icon = lv_obj_find_by_name(screen, "favorite_icon");
    int32_t star = icon ? lv_obj_get_width(icon) : lv_obj_get_height(anchor);
    assert(lv_obj_get_width(lv_obj_get_child(control, 0)) == star);
    assert(area.x1 >= bounds.x1 && area.y1 >= bounds.y1 && area.x2 <= bounds.x2 && area.y2 <= bounds.y2);
    assert(visible.x1 >= area.x1 && visible.y1 >= area.y1 && visible.x2 <= area.x2 && visible.y2 <= area.y2);
    int32_t anchor_side = LV_MIN(lv_obj_get_width(anchor), lv_obj_get_height(anchor));
    assert(lv_area_get_width(&area) <= LV_MAX(LV_MAX(48, BOARD_SCALE_PX(72)), anchor_side));
    assert(lv_area_get_width(&area) >= anchor_side);
    check_chip_leaves_star(control);
    bool edge = area.x1 == bounds.x1 || area.y1 == bounds.y1 || area.x2 == bounds.x2 || area.y2 == bounds.y2;
    if (!edge) {
        assert(abs(area.x1 + area.x2 - visible.x1 - visible.x2) <= 1);
        assert(abs(area.y1 + area.y2 - visible.y1 - visible.y2) <= 1);
    }
    /* Shrinking stops at the anchor's size or the control's, whichever is larger: a layout that overlaps its own
     * anchor with another target (OrbitPlayer's album line, HibysPlayer's
     * quality line at 320x480) cannot be cleared there; larger areas must be. */
    if (lv_area_get_width(&area) > LV_MAX(anchor_side, lv_area_get_width(&visible))) {
        check_rating_clearance(screen, hit, &area);
        check_rating_roles(screen, &area);
    }
    check_rating_edge(screen, hit, &area, false);
    printf("Player star %d, hit %d at %d,%d beside the cover\n", (int)star,
           (int)lv_area_get_width(&area), (int)area.x1, (int)area.y1);
}

static void check_player_rating(bool plugin) {
    lv_obj_t * screen = gui_player_get_screen();
    lv_obj_t * anchor = lv_obj_find_by_name(screen, "favorite_circle");
    lv_obj_t * control = gui_player_rating_control();
    assert(anchor && control && lv_obj_get_parent(control) == anchor);
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "favorite_icon"), LV_OBJ_FLAG_HIDDEN));
    for (int stars = 0; stars <= 5; stars++) {
        show_now_playing_rating(stars);
        assert(star_rating_get(control) == stars);
        assert(lv_obj_has_flag(rating_chip(control), LV_OBJ_FLAG_HIDDEN) == (stars == 0));
        if (stars) assert(lv_label_get_text(rating_digit(control))[0] == '0' + stars);
    }
    lv_obj_update_layout(screen);
    const char *xml = getenv("COMPAS_TEST_PLAYER_XML");
    bool on_cover = rating_anchor_on_cover(screen, anchor);
    /* These two plugins put the anchor beside the title; the control must follow it. */
    if (plugin && xml && (strstr(xml, "/HibysPlayer/") || strstr(xml, "/GalleryPlayer/"))) assert(!on_cover);
    if (on_cover) {
        check_rating_geometry(screen, control);
        assert(lv_obj_get_width(control) <= lv_obj_get_width(anchor));
        assert(lv_obj_get_height(control) <= lv_obj_get_height(anchor));
    } else {
        if (plugin) check_anchor_kept(anchor);
        check_rating_geometry_off_cover(screen, anchor, control);
    }
    assert(!lv_obj_has_flag(control, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_width(control) > 0 && lv_obj_get_height(control) > 0);
    lv_obj_t *transport = lv_obj_find_by_name(screen, "order_btn");
    lv_color_t color = lv_obj_get_style_image_recolor(transport, 0);
    lv_style_value_t text;
    assert(lv_style_get_prop(gui_player_backdrop_text_style(false), LV_STYLE_TEXT_COLOR, &text) == LV_STYLE_RES_FOUND);
    if (lv_obj_get_style_image_recolor_opa(transport, 0) != LV_OPA_COVER) color = text.color;
    lv_obj_set_style_text_color(anchor, lv_color_hex(0x123456), 0);
    assert(!lv_color_eq(color, lv_obj_get_style_text_color(anchor, 0)));
    assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(control, 0), 0), color));
    show_now_playing_rating(0);
    assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(control, 0), 0), color));
    assert(lv_obj_get_style_image_recolor_opa(lv_obj_get_child(control, 0), 0) == LV_OPA_COVER);
    show_now_playing_rating(5);
    assert(lv_color_eq(lv_obj_get_style_bg_color(rating_chip(control), 0), color));
    show_now_playing_rating(0);
}

static void check_transport_tint(void) {
    lv_obj_t *source = lv_obj_find_by_name(gui_player_get_screen(), "order_btn");
    lv_obj_t *control = gui_player_rating_control();
    lv_color_t tint = lv_color_hex(0x82c6ef);
    lv_obj_set_style_image_recolor(source, tint, 0);
    lv_obj_set_style_image_recolor_opa(source, LV_OPA_COVER, 0);
    /* Backdrop changes refresh the whole transport style. */
    lv_obj_refresh_style(source, LV_PART_MAIN, LV_STYLE_PROP_ANY);
    for (int stars = 0; stars <= 5; stars++) {
        show_now_playing_rating(stars);
        assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(control, 0), 0), tint));
        if (stars) assert(lv_color_eq(lv_obj_get_style_bg_color(rating_chip(control), 0), tint));
        assert(lv_obj_get_style_image_recolor_opa(lv_obj_get_child(control, 0), 0) == LV_OPA_COVER);
    }
    lv_obj_remove_local_style_prop(source, LV_STYLE_IMAGE_RECOLOR, 0);
    lv_obj_remove_local_style_prop(source, LV_STYLE_IMAGE_RECOLOR_OPA, 0);
    lv_obj_refresh_style(source, LV_PART_MAIN, LV_STYLE_PROP_ANY);
    show_now_playing_rating(0);
}

static void check_plugin_rating(void) {
    const char *path = getenv("COMPAS_TEST_PLAYER_XML");
    if (!path) return;
    assert(player_layouts_session_select_xml("test.rating", "Rating test", path, true));
    assert(gui_player_rebuild_layout(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    assert(lv_obj_find_by_name(gui_player_get_screen(), "order_btn"));
    assert(!strcmp(player_layouts_effective_id(), "test.rating"));
    check_player_rating(true);
    check_transport_tint();
    printf("Rating layout passed: %s\n", path);
    assert(player_layouts_session_clear_selection());
    assert(gui_player_rebuild_layout(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
}

static void check_minimal_rating(void) {
    const char *path = getenv("COMPAS_TEST_MINIMAL_XML");
    assert(path);
    assert(player_layouts_session_select_xml("test.minimal", "Minimal rating", path, true));
    assert(gui_player_rebuild_layout(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    assert(!strcmp(player_layouts_effective_id(), "test.minimal"));
    lv_obj_t *screen = gui_player_get_screen();
    lv_obj_t *anchor = lv_obj_find_by_name(screen, "favorite_circle");
    lv_obj_t *control = gui_player_rating_control();
    assert(anchor && control && lv_obj_get_parent(control) == anchor);
    assert(!lv_obj_find_by_name(screen, "transport_color"));
    assert(!lv_obj_find_by_name(screen, "order_btn"));
    lv_obj_t *title = lv_obj_find_by_name(screen, "title");
    assert(title);
    lv_style_t *style = gui_player_backdrop_text_style(false);
    lv_style_value_t text;
    assert(lv_style_get_prop(style, LV_STYLE_TEXT_COLOR, &text) == LV_STYLE_RES_FOUND);
    lv_color_t colors[] = { text.color, lv_color_hex(lv_color_to_u32(text.color) ^ 0xFFFFFF) };
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        /* Refresh the bound title after a backdrop contrast change. */
        lv_style_set_text_color(style, colors[i]);
        lv_obj_refresh_style(title, LV_PART_MAIN, LV_STYLE_PROP_ANY);
        for (int stars = 0; stars <= 5; stars++) {
            show_now_playing_rating(stars);
            assert(lv_color_eq(lv_obj_get_style_image_recolor(lv_obj_get_child(control, 0), 0), colors[i]));
            assert(lv_obj_get_style_image_recolor_opa(lv_obj_get_child(control, 0), 0) == LV_OPA_COVER);
            if (!stars) continue;
            assert(lv_color_eq(lv_obj_get_style_bg_color(rating_chip(control), 0), colors[i]));
            assert(lv_obj_get_style_bg_opa(rating_chip(control), 0) == LV_OPA_COVER);
        }
    }
    lv_style_set_text_color(style, text.color);
    lv_obj_refresh_style(title, LV_PART_MAIN, LV_STYLE_PROP_ANY);
    show_now_playing_rating(0);
    assert(player_layouts_session_clear_selection());
    assert(gui_player_rebuild_layout(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
}

static void *record_test_album(void *data) {
    (void)data;
    song_row_t song = {0};
    strcpy(song.path, "./music/selected.wav");
    gui_album_header_record(&song, 1, "Album artist", "Test album");
    return NULL;
}

static void check_loading_album_rating(void) {
    lv_obj_t *list = lv_obj_create(NULL);
    lv_obj_set_width(list, BOARD_SCREEN_WIDTH);
    gui_album_header_params_t params = {.album = "Test album", .album_artist = "Album artist"};
    gui_album_header_build(list, &params);
    lv_obj_t *control = lv_obj_get_child(A.view.info, 2);
    lv_obj_send_event(lv_obj_get_child(control, 3), LV_EVENT_CLICKED, NULL);
    gui_album_header_poll(false, false);
    assert(star_rating_get(control) == 4);
    gui_album_header_forget();
    lv_obj_delete(list);
}

static void check_album_rating(void) {
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, record_test_album, NULL));
    pthread_join(worker, NULL);
    int reads = atomic_load(&rating_reads);
    lv_obj_t *list = lv_obj_create(NULL);
    lv_obj_set_width(list, BOARD_SCREEN_WIDTH);
    group_song_entry_t entry = {.path = "./music/selected.wav"};
    gui_album_header_params_t params = {
        .album = "Test album", .album_artist = "Album artist", .entries = &entry, .count = 1,
    };
    gui_album_header_build(list, &params);
    lv_obj_t *control = lv_obj_get_child(A.view.info, 2);
    assert(star_rating_get(control) == 4);
    lv_obj_update_layout(list);
    assert(lv_obj_get_width(control) <= lv_obj_get_width(A.view.info));
    gui_album_header_note_rating("Other artist", "Test album", 1);
    assert(star_rating_get(control) == 4);
    lv_obj_send_event(lv_obj_get_child(control, 4), LV_EVENT_CLICKED, NULL);
    assert(star_rating_get(control) == 5);
    gui_album_header_poll(false, false);
    assert(star_rating_get(control) == 5);
    lv_obj_send_event(lv_obj_get_child(control, 4), LV_EVENT_CLICKED, NULL);
    gui_album_header_poll(false, false);
    assert(star_rating_get(control) == 0);
    assert(atomic_load(&rating_reads) == reads);
    gui_album_header_forget();
    lv_obj_delete(list);
}

static lv_obj_t *find_full_rating(lv_obj_t *obj) {
    bool stars = lv_obj_get_child_count(obj) == 5;
    for (uint32_t i = 0; stars && i < 5; i++)
        stars = lv_obj_check_type(lv_obj_get_child(obj, i), &lv_image_class);
    if (stars) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_full_rating(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static void check_popup_colors(lv_obj_t *control, int stars) {
    lv_style_value_t text;
    assert(lv_style_get_prop(&style_theme_text_primary, LV_STYLE_TEXT_COLOR, &text) == LV_STYLE_RES_FOUND);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *image = lv_obj_get_child(control, i);
        lv_color_t color = i < stars ? lv_color_hex(gui_theme_accent_rgb()) : text.color;
        assert(lv_color_eq(lv_obj_get_style_image_recolor(image, 0), color));
        assert(lv_obj_get_style_image_recolor_opa(image, 0) == LV_OPA_COVER);
    }
}

static void check_song_popup(void) {
    current_track = true;
    assert(strcmp(popup_song, current_path));
    assert(rating_writer_write_now(popup_song, 5));
    gui_player_open_rating_popup(popup_song);
    lv_obj_t *control = find_full_rating(lv_layer_top());
    assert(control && star_rating_get(control) == 5);
    check_popup_colors(control, 5);
    assert(!has_text(lv_layer_top(), "Clear rating"));
    assert(has_text(lv_layer_top(), "Cancel"));
    lv_obj_update_layout(control);
    assert(lv_obj_get_width(control) <= lv_obj_get_content_width(lv_obj_get_parent(control)));
    int32_t px = lv_obj_get_width(lv_obj_get_child(control, 0));
    assert(px >= BOARD_SCALE_PX(48) && px <= BOARD_SCALE_PX(64));
    assert(lv_obj_get_y(lv_obj_get_child(control, 0)) == lv_obj_get_y(lv_obj_get_child(control, 4)));
    printf("Popup star size: %d px\n", (int)px);
    lv_obj_send_event(lv_obj_get_child(control, 4), LV_EVENT_CLICKED, NULL);
    assert(__real_rating_writer_stars(popup_song) == 0);
    gui_player_open_rating_popup(popup_song);
    assert(star_rating_get(control) == 0);
    lv_obj_send_event(lv_obj_get_child(control, 2), LV_EVENT_CLICKED, NULL);
    assert(__real_rating_writer_stars(popup_song) == 3);
    gui_player_open_rating_popup(popup_song);
    assert(star_rating_get(control) == 3);
    check_popup_colors(control, 3);
    assert(now_playing_stars == 0);
    current_track = false;
    gui_rating_popup_teardown();
}

static void check_popup_accent(void) {
    uint32_t saved_accent = gui_theme_accent_rgb();
    gui_rating_popup_open("Accent test", 3, NULL, NULL);
    lv_obj_t *control = find_full_rating(lv_layer_top());
    assert(control && star_rating_get(control) == 3);
    check_popup_colors(control, 3);
    lv_obj_send_event(lv_obj_get_child(control, 2), LV_EVENT_CLICKED, NULL);
    uint32_t changed_accent = saved_accent ^ 0xffffff;
    gui_theme_apply_accent(changed_accent);
    assert(gui_theme_accent_rgb() == changed_accent);
    gui_rating_popup_open("Accent test", 3, NULL, NULL);
    assert(find_full_rating(lv_layer_top()) == control);
    assert(star_rating_get(control) == 3);
    check_popup_colors(control, 3);
    gui_rating_popup_teardown();
    gui_theme_apply_accent(saved_accent);
}

static void check_popup_rating(void) {
    atomic_store(&hold_popup, true);
    gui_rating_popup_open("Rate album", 0, NULL, NULL);
    gui_rating_popup_load_stars("album://Artist\tPopup album");
    for (int i = 0; i < 1000 && !atomic_load(&popup_entered); i++) pump_ui();
    assert(atomic_load(&popup_entered));
    gui_rating_popup_open("Replacement", 5, NULL, NULL);
    atomic_store(&hold_popup, false);
    for (int i = 0; i < 100; i++) pump_ui();
    lv_obj_t *control = find_full_rating(lv_layer_top());
    assert(control && star_rating_get(control) == 5);
    gui_rating_popup_open("Rate album", 0, NULL, NULL);
    gui_rating_popup_load_stars("album://Artist\tPopup album");
    for (int i = 0; i < 1000 && star_rating_get(control) != 3; i++) pump_ui();
    assert(star_rating_get(control) == 3);
    gui_rating_popup_load_stars("album://Artist\tPopup album");
    gui_rating_popup_teardown();
    for (int i = 0; i < 100; i++) pump_ui();
}

static lv_obj_t *visible_label(lv_obj_t *obj, const char *text) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = visible_label(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static void tap_play_pill(const char *text) {
    lv_obj_t *label = visible_label(lv_screen_active(), text);
    assert(label);
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, NULL);
}

static void check_list_count(bool songs, int count) {
    if (!songs) { assert(album_item_count == count); return; }
    assert(group_songs_count == count);
    int rows = 0;
    for (int i = 0; i < group_songs_compact_row_count(); i++)
        if (group_songs_compact_row_source(i) >= 0) rows++;
    assert(rows == count);
    char text[32];
    snprintf(text, sizeof(text), "%d tracks", count);
    if (count) assert(visible_text_count(lv_screen_active(), text) == 1);
}

static void check_section_control(lv_obj_t *control) {
    int stars = star_rating_get(control);
    assert(stars >= 1 && stars <= 5);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *image = lv_obj_get_child(control, i);
        assert(lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) == (i >= stars));
        if (i < stars) {
            assert(lv_image_get_src(image) == &star_rating_icon_on);
            assert(lv_obj_get_style_image_recolor_opa(image, 0) == LV_OPA_COVER);
            assert(lv_color_eq(lv_obj_get_style_image_recolor(image, 0),
                               lv_color_hex(gui_theme_accent_rgb())));
        }
        assert(!lv_obj_has_flag(image, LV_OBJ_FLAG_CLICKABLE));
    }
}

static int check_section_widgets(lv_obj_t *obj) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return 0;
    int count = 0;
    const char *name = lv_obj_get_name(obj);
    if (name && !strcmp(name, "rating_section")) {
        check_section_control(obj);
        count++;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        count += check_section_widgets(lv_obj_get_child(obj, i));
    return count;
}

static void check_list_empty(bool songs) {
    check_list_count(songs, 0);
    assert(visible_text_count(lv_screen_active(), songs
        ? "No songs rated 3 stars or more yet." : "No albums rated 3 stars or more yet.") == 1);
    assert(visible_text_count(lv_screen_active(), "No items") == 0);
}

static void wait_rating_count(bool songs, int count) {
    for (int i = 0; i < 3000; i++) {
        pump_ui();
        if ((songs ? group_songs_count : album_item_count) == count) break;
    }
    lv_obj_update_layout(lv_screen_active());
    check_list_count(songs, count);
    int headers = check_section_widgets(lv_screen_active());
    assert(count ? headers > 0 : headers == 0);
}

static void open_rated_songs(void) {
    library_playlist_open_row(GUI_PLAYLIST_ROW_FAVORITES, "Top rated", NULL, NULL, 0);
    wait_rating_count(true, atomic_load(&list_fixture) == 2 ? 0 : 4);
}

static void check_rated_list_accent(void) {
    uint32_t saved_accent = gui_theme_accent_rgb();
    uint32_t saved_cover = 0;
    bool had_cover = gui_theme_cover_accent(&saved_cover);
    atomic_store(&list_fixture, 1);
    open_rated_songs();
    album_item_count = -1;
    show_favorite_albums();
    wait_rating_count(false, 4);
    uint32_t changed_accent = saved_accent ^ 0xffffff;
    gui_theme_apply_accent(changed_accent);
    assert(gui_theme_accent_rgb() == changed_accent);
    assert(check_section_widgets(group_songs_list) > 0);
    assert(check_section_widgets(lv_screen_active()) > 0);
    gui_theme_set_cover_accent(true, changed_accent);
    gui_theme_set_accent_dynamic(true);
    gui_theme_set_cover_accent(true, saved_accent);
    assert(gui_theme_accent_rgb() == saved_accent);
    assert(check_section_widgets(group_songs_list) > 0);
    assert(check_section_widgets(lv_screen_active()) > 0);
    nav_pop_immediate();
    assert(check_section_widgets(lv_screen_active()) > 0);
    nav_pop_immediate();
    favorite_albums_teardown();
    gui_theme_apply_accent(saved_accent);
    gui_theme_set_cover_accent(had_cover, saved_cover);
    atomic_store(&list_fixture, 0);
}

/* A database refresh during a pending load must not be lost to that older snapshot. */
static void check_refresh_during_load(void) {
    atomic_store(&list_fixture, 1);
    open_rated_songs();
    atomic_store(&held_song_loads, 0);
    atomic_store(&hold_song_loads, true);
    refresh_group_songs_from_database();
    for (int i = 0; i < 2000 && !atomic_load(&held_song_loads); i++) pump_ui();
    assert(atomic_load(&held_song_loads) == 1);
    atomic_store(&list_fixture, 2);
    refresh_group_songs_from_database();
    atomic_store(&hold_song_loads, false);
    wait_rating_count(true, 0);
    check_list_empty(true);
    nav_pop_immediate();
    atomic_store(&list_fixture, 0);
}

static void check_three_sections(bool songs) {
    int kind = songs ? 0 : 1;
    assert(section_count[kind] == 3);
    for (int i = 0; i < 3; i++) assert(section_values[kind][i] == 5 - i);
}

static void check_album_mapping(void) {
    const int rows[] = {1, 2, 4, 6};
    for (int i = 0; i < 4; i++) {
        album_opened = album_menu_opened = -1;
        album_click(rows[i]);
        album_menu(rows[i]);
        assert(album_opened == i && album_menu_opened == i);
    }
    album_opened = album_menu_opened = -1;
    album_click(0); album_menu(3); album_click(5);
    assert(album_opened == -1 && album_menu_opened == -1);
}

static void check_top_songs(void) {
    atomic_store(&list_fixture, 1);
    open_rated_songs();
    check_three_sections(true);
    assert(group_songs_compact_row_count() == 7);
    const int sources[] = {-1, 0, 1, -1, 2, -1, 3};
    for (int i = 0; i < 7; i++) assert(group_songs_compact_row_source(i) == sources[i]);
    tap_play_pill(LV_SYMBOL_PLAY "  Play");
    assert(playback_count == 4 && source_count == 4);
    tap_play_pill(LV_SYMBOL_SHUFFLE "  Shuffle");
    assert(playback_count == 4 && source_count == 4);
    nav_pop_immediate();
    atomic_store(&list_fixture, 2);
    open_rated_songs();
    check_list_empty(true);
    nav_pop_immediate();
    atomic_store(&list_fixture, 0);
}

static void check_top_albums(void) {
    atomic_store(&list_fixture, 1);
    album_item_count = -1;
    show_favorite_albums();
    wait_rating_count(false, 4);
    check_three_sections(false);
    check_album_mapping();
    int reads = atomic_load(&favorite_reads);
    atomic_store(&hold_favorites, true);
    assert(remote_state_set_rating("album://Filter artist\tRevision", 1));
    for (int i = 0; i < 2000 && atomic_load(&favorite_reads) == reads; i++) pump_ui();
    assert(atomic_load(&favorite_reads) > reads);
    atomic_store(&list_fixture, 2);
    lv_obj_send_event(lv_screen_active(), LV_EVENT_SCREEN_LOAD_START, NULL);
    atomic_store(&hold_favorites, false);
    wait_rating_count(false, 0);
    check_list_empty(false);
    nav_pop_immediate();
    favorite_albums_teardown();
    atomic_store(&list_fixture, 0);
}

static void seed_live_ratings(void) {
    rated_album_t *albums = NULL;
    int count = 0;
    __real_metadata_db_load_rated_albums(&albums, &count);
    for (int i = 0; i < count; i++) {
        char key[ALBUM_RATING_KEY_SIZE];
        assert(album_rating_key(albums[i].album_artist, albums[i].album, key, sizeof(key)));
        assert(rating_writer_write_now(key, 0));
    }
    metadata_db_free_rated_albums(albums, count);
    metadata_db_begin_targeted_update();
    cached_tags_t tags = {0};
    for (int i = 0; i < 3; i++) {
        char path[80];
        snprintf(path, sizeof(path), "./music/live-%d.wav", i);
        metadata_db_put(path, 1, 1, &tags);
    }
    assert(metadata_db_end_update());
}

static void check_live_rating_refresh(bool songs) {
    char paths[3][ALBUM_RATING_KEY_SIZE];
    for (int i = 0; i < 3; i++) {
        snprintf(paths[i], sizeof(paths[i]), songs ? "./music/live-%d.wav"
                 : "album://Live artist\tLive album %d", i);
        assert(rating_writer_write_now(paths[i], 5));
    }
    if (songs) library_playlist_open_row(GUI_PLAYLIST_ROW_FAVORITES, "Favorites", NULL, NULL, 0);
    else { album_item_count = -1; show_favorite_albums(); }
    wait_rating_count(songs, 3);
    unsigned revision = remote_state_revision();
    rating_writer_submit(paths[0], 3);
    for (int i = 0; i < 500; i++) pump_ui();
    wait_rating_count(songs, 3);
    assert(remote_state_revision() != revision);
    int kind = songs ? 0 : 1;
    assert(section_count[kind] == 2);
    assert(section_values[kind][0] == 5 && section_values[kind][1] == 3);
    if (songs) {
        assert(!strcmp(group_songs_entries[2].path, paths[0]));
        assert(group_songs_rating_section(2) == 3);
    }
    rating_writer_submit(paths[0], 2);
    wait_rating_count(songs, 2);
    assert(rating_writer_write_now(paths[0], 3));
    wait_rating_count(songs, 3);
    if (songs) assert(!strcmp(group_songs_entries[2].path, paths[0]));
    nav_pop_immediate();
    if (!songs) favorite_albums_teardown();
}

static void check_live_ratings(void) {
    atomic_store(&list_fixture, 3);
    seed_live_ratings();
    check_live_rating_refresh(true);
    check_live_rating_refresh(false);
    atomic_store(&list_fixture, 0);
}

metadata_db_snapshot_t *__real_metadata_db_snapshot_open(bool);
metadata_db_snapshot_t *__wrap_metadata_db_snapshot_open(bool recency) {
    metadata_db_snapshot_t *snapshot = __real_metadata_db_snapshot_open(recency);
    if (atomic_load(&all_rating_fixture) && snapshot) {
        assert(pthread_equal(ui_thread, pthread_self()));
        snapshot_refs++;
        snapshot_opens++;
    }
    return snapshot;
}
int __real_metadata_db_snapshot_count(const metadata_db_snapshot_t *);
int __wrap_metadata_db_snapshot_count(const metadata_db_snapshot_t *snapshot) {
    int count = __real_metadata_db_snapshot_count(snapshot);
    return snapshot_wrong_count ? count + 1 : count;
}
metadata_db_snapshot_t *__real_metadata_db_snapshot_retain(metadata_db_snapshot_t *);
metadata_db_snapshot_t *__wrap_metadata_db_snapshot_retain(metadata_db_snapshot_t *snapshot) {
    if (atomic_load(&all_rating_fixture) && snapshot) snapshot_refs++;
    return __real_metadata_db_snapshot_retain(snapshot);
}
void __real_metadata_db_snapshot_close(metadata_db_snapshot_t *);
void __wrap_metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot) {
    if (atomic_load(&all_rating_fixture) && snapshot) {
        assert(snapshot_refs > 0);
        snapshot_refs--;
    }
    __real_metadata_db_snapshot_close(snapshot);
}

bool __real_metadata_db_load_rating_order(int **, int *, int *, int32_t *);
bool __wrap_metadata_db_load_rating_order(int **ranks, int sections[6], int *count, int32_t *generation) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    if (!atomic_load(&all_rating_fixture))
        return __real_metadata_db_load_rating_order(ranks, sections, count, generation);
    int stars[] = {5,0,3,5,1,0};
    if (atomic_load(&all_rating_variant)) stars[2] = 0;
    *generation = metadata_db_get_generation();
    *count = 6;
    *ranks = malloc(6 * sizeof(int));
    memset(sections, 0, 6 * sizeof(int));
    for (int star = 5, pos = 0; star >= 0; star--)
        for (int rank = 0; rank < 6; rank++)
            if (stars[rank] == star) { (*ranks)[pos++] = rank; sections[star]++; }
    atomic_fetch_add(&all_rating_reads, 1);
    while (atomic_load(&all_rating_hold)) usleep(1000);
    bool failed = atomic_load(&all_rating_fail);
    if (failed) atomic_fetch_add(&all_rating_failures, 1);
    return !failed;
}

static atomic_bool all_rating_db_busy;
static atomic_int az_generation_offset;
bool __real_metadata_db_try_get_generation(int32_t *);
bool __wrap_metadata_db_try_get_generation(int32_t *generation) {
    bool ok = !atomic_load(&all_rating_db_busy) && __real_metadata_db_try_get_generation(generation);
    if (ok) *generation += atomic_load(&az_generation_offset);
    return ok;
}

int64_t __real_metadata_db_get_song_count(void);
int64_t __wrap_metadata_db_get_song_count(void) {
    return atomic_load(&all_rating_fixture) ? 6 : __real_metadata_db_get_song_count();
}

int __real_metadata_db_get_songs_at_title_ranks(const int *, int, song_row_t *);
int __wrap_metadata_db_get_songs_at_title_ranks(const int *ranks, int count, song_row_t *rows) {
    if (!atomic_load(&all_rating_fixture)) return __real_metadata_db_get_songs_at_title_ranks(ranks, count, rows);
    for (int i = 0; i < count; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        snprintf(rows[i].tags.title, sizeof(rows[i].tags.title), "Rating song %d", ranks[i]);
        snprintf(rows[i].path, sizeof(rows[i].path), "./music/rating-%d.wav", ranks[i]);
    }
    return count;
}

int __real_metadata_db_get_songs_filtered_page(const char *, const char *, const char *,
                                                const char *, const char *, int, int, song_row_t *);
int __wrap_metadata_db_get_songs_filtered_page(const char *query, const char *artist,
        const char *album_artist, const char *album, const char *genre, int offset, int count, song_row_t *rows) {
    if (!atomic_load(&all_rating_fixture))
        return __real_metadata_db_get_songs_filtered_page(query, artist, album_artist, album, genre, offset, count, rows);
    int ranks[6];
    if (offset < 0 || offset >= 6) return 0;
    if (count > 6 - offset) count = 6 - offset;
    for (int i = 0; i < count; i++) ranks[i] = offset + i;
    return __wrap_metadata_db_get_songs_at_title_ranks(ranks, count, rows);
}
void __real_open_song_context_menu(const char *);
void __wrap_open_song_context_menu(const char *path) {
    if (!atomic_load(&all_rating_fixture)) { __real_open_song_context_menu(path); return; }
    assert(sscanf(path, "./music/rating-%d.wav", &all_menu_rank) == 1);
}

static void wait_all_rating(int total) {
    for (int i = 0; i < 2000; i++) {
        pump_ui();
        if (all_songs_rating_active() && all_songs_paged_total() == total) return;
    }
    assert(false && "All songs order did not land");
}

static void all_rating_revision(int value) {
    assert(remote_state_set_rating("album://All songs test\tRevision", value));
}

static void check_all_rating_refresh(void) {
    atomic_store(&all_rating_hold, true);
    int reads = atomic_load(&all_rating_reads);
    all_rating_revision(1);
    for (int i = 0; i < 2000 && atomic_load(&all_rating_reads) == reads; i++) pump_ui();
    assert(atomic_load(&all_rating_reads) > reads);
    atomic_store(&all_rating_variant, 1);
    all_rating_revision(2);
    atomic_store(&all_rating_hold, false);
    wait_all_rating(9);
    compact_list_page_row_t rows[10];
    assert(all_songs_fetch_page(NULL, 0, 10, rows) == 9);
    for (int i = 0; i < 9; i++) assert(rows[i].section_stars != 3);
}

/* A busy database when the load completes delays the landing, not the result. */
static void check_all_rating_busy_landing(void) {
    int reads = atomic_load(&all_rating_reads);
    atomic_store(&all_rating_variant, 0);
    atomic_store(&all_rating_db_busy, true);
    all_rating_revision(3);
    for (int i = 0; i < 2000 && atomic_load(&all_rating_reads) == reads; i++) pump_ui();
    for (int i = 0; i < 50; i++) pump_ui();
    assert(atomic_load(&all_rating_reads) == reads + 1 && all_songs_paged_total() == 9);
    atomic_store(&all_rating_db_busy, false);
    wait_all_rating(10);
    assert(atomic_load(&all_rating_reads) == reads + 1);
    atomic_store(&all_rating_variant, 1);
    all_rating_revision(4);
    wait_all_rating(9);
}

bool metadata_db_query_try_lock(void);
void metadata_db_query_unlock(void);
static atomic_bool db_holder_locked, db_holder_release;
static void *hold_database(void *unused) {
    (void)unused;
    while (!metadata_db_query_try_lock()) usleep(1000);
    atomic_store(&db_holder_locked, true);
    while (!atomic_load(&db_holder_release)) usleep(1000);
    metadata_db_query_unlock();
    return NULL;
}

/* Another thread holding the database must not stall scheduling on the UI. */
static void check_all_rating_schedule_busy(void) {
    pthread_t holder;
    atomic_store(&db_holder_locked, false);
    atomic_store(&db_holder_release, false);
    assert(!pthread_create(&holder, NULL, hold_database, NULL));
    while (!atomic_load(&db_holder_locked)) usleep(1000);
    int reads = atomic_load(&all_rating_reads);
    all_songs_rating_start();
    usleep(20000);
    assert(atomic_load(&all_rating_reads) == reads);
    atomic_store(&db_holder_release, true);
    pthread_join(holder, NULL);
    all_songs_rating_start();
    for (int i = 0; i < 2000 && atomic_load(&all_rating_reads) == reads; i++) pump_ui();
    assert(atomic_load(&all_rating_reads) == reads + 1);
    wait_all_rating(9);
}

static void check_all_rating_rows(lv_obj_t *screen) {
    assert(!library_az_index_list_allowed(all_songs_list));
    compact_list_page_row_t rows[10];
    assert(all_songs_fetch_page(NULL, 0, 10, rows) == 10);
    const int ranks[] = {-1,0,3,-1,2,-1,4,-1,1,5};
    const int stars[] = {5,0,0,3,0,1,0,0,0,0};
    for (int i = 0; i < 10; i++) {
        assert(all_songs_rating_rank_at(i, NULL) == ranks[i]);
        assert(rows[i].is_section_header == (ranks[i] < 0));
        assert(rows[i].section_stars == stars[i]);
    }
    assert(!strcmp(rows[7].label, "Unrated"));
    lv_obj_update_layout(screen);
    compact_list_refresh_visible(all_songs_list);
    for (int i = 0; i < 2000 && !check_section_widgets(all_songs_list); i++) pump_ui();
    assert(check_section_widgets(all_songs_list) > 0);
    assert(all_songs_rating_display_row(2) == 4);
    assert(songs_title_fetch_page(NULL, 0, 10, rows) == 6);
    for (int i = 0; i < 6; i++) assert(!rows[i].is_section_header);
}

static void check_queue_paths(const int ranks[6]) {
    assert(player_queue_model_count() == 6);
    for (int i = 0; i < 6; i++) {
        char path[80];
        snprintf(path, sizeof(path), "./music/rating-%d.wav", ranks[i]);
        assert(!strcmp(player_queue_model_path_at(i), path));
    }
}

static void check_all_rating_playback(void) {
    const int expected[] = {0,3,2,4,1,5};
    free_playlist();
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;
    int opens = snapshot_opens;
    all_click(0);
    assert(snapshot_opens == opens);
    set_player_source_all_songs(0);
    all_click(2);
    check_queue_paths(expected);
    assert(player_queue_model_current() == 1 && snapshot_refs == 1);
    assert(player_source_kind == PLAYER_SOURCE_NONE && current_settings.last_source_kind == 0);
    for (int i = 2; i < 6; i++) {
        gui_player_step_manual(1);
        assert(player_queue_model_current() == i);
        check_queue_paths(expected);
    }
    for (int i = 4; i >= 0; i--) {
        gui_player_step_manual(-1);
        assert(player_queue_model_current() == i);
        check_queue_paths(expected);
    }
    playlist_start_source = PLAY_ALL_SOURCE_ALL_SONGS;
    playlist_start_selected(false);
    assert(player_queue_model_current() == 0);
    playlist_start_selected(true);
    assert(current_settings.play_mode == PLAY_MODE_SHUFFLE);
    check_queue_paths(expected);
    all_menu_rank = -1;
    all_menu(0);
    assert(all_menu_rank == -1);
    all_menu(6);
    assert(all_menu_rank == 4);
    free_playlist();
    assert(snapshot_refs == 0);
}

static void check_all_rating_rejection(void) {
    for (int stale = 0; stale < 2; stale++) {
        int *ranks = malloc(6 * sizeof(*ranks));
        for (int i = 0; i < 6; i++) ranks[i] = i;
        snapshot_wrong_count = !stale;
        on_file_selected_lazy_ranks(ranks, 6, 0, metadata_db_snapshot_open(false),
                                    metadata_db_get_generation() + stale);
        snapshot_wrong_count = false;
        assert(snapshot_refs == 0 && player_queue_model_count() == 0);
    }
}

static void check_all_rating_fallback(void) {
    const int expected[] = {0,1,2,3,4,5};
    assert(!all_songs_rating_active());
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;
    all_click(2);
    assert(player_queue_model_current() == 2);
    check_queue_paths(expected);
    all_menu_rank = -1;
    all_menu(3);
    assert(all_menu_rank == 3);
    playlist_start_source = PLAY_ALL_SOURCE_ALL_SONGS;
    playlist_start_selected(false);
    assert(player_queue_model_current() == 0);
    check_queue_paths(expected);
    playlist_start_selected(true);
    assert(current_settings.play_mode == PLAY_MODE_SHUFFLE);
    check_queue_paths(expected);
    assert(player_source_kind == PLAYER_SOURCE_ALL_SONGS);
    free_playlist();
    assert(snapshot_refs == 0);
    lv_screen_load(all_songs_screen);
}

static void seed_all_rating_songs(void) {
    free_playlist();
    metadata_db_begin_update();
    for (int i = 0; i < 6; i++) {
        cached_tags_t tags = {0};
        char path[80];
        snprintf(path, sizeof(path), "./music/rating-%d.wav", i);
        snprintf(tags.title, sizeof(tags.title), "Rating song %d", i);
        metadata_db_put(path, 1, 1, &tags);
    }
    assert(metadata_db_end_update());
}

static lv_obj_t *find_search_entry(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_textarea_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *entry = find_search_entry(lv_obj_get_child(obj, i));
        if (entry) return entry;
    }
    return NULL;
}

static void check_all_rating_search(lv_obj_t *screen) {
    library_search_open(SEARCH_BINDING_ALL_SONGS);
    lv_obj_t *entry = find_search_entry(screen);
    assert(entry);
    lv_textarea_set_text(entry, "Rating song 2");
    int rank = -1;
    for (int i = 0; i < 2000; i++) {
        pump_ui();
        poll_search_job();
        if (library_search_try_map_filtered_row(SEARCH_BINDING_ALL_SONGS, 0, &rank)) break;
    }
    assert(rank == 2 && library_search_showing_results(SEARCH_BINDING_ALL_SONGS));
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;
    all_click(0);
    const int expected[] = {0,1,2,3,4,5};
    check_queue_paths(expected);
    assert(player_queue_model_current() == 2 && player_source_kind == PLAYER_SOURCE_ALL_SONGS);
    lv_screen_load(screen);
    check_all_rating_refresh();
    assert(library_search_is_active(SEARCH_BINDING_ALL_SONGS));
    assert(search_remap_index(SEARCH_BINDING_ALL_SONGS, 0) == 2);
    assert(search_close_if_active_for_screen(screen));
    assert(all_songs_paged_total() == 9);
    compact_list_page_row_t rows[10];
    assert(all_songs_fetch_page(NULL, 0, 10, rows) == 9 && rows[0].is_section_header);
    free_playlist();
    assert(snapshot_refs == 0);
}

static lv_obj_t *find_sort_option(lv_obj_t *obj, const char *text) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_2) && has_text(obj, text)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_sort_option(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool cached_row_zero(int index, compact_list_page_row_t *row, void *ctx) {
    if (index == 0) *(const char **)ctx = row->label;
    return false;
}

/* Releases the held database after a delay, recording that it had to. */
static atomic_bool db_release_forced;
static void *release_database_later(void *unused) {
    (void)unused;
    usleep(2000000);
    if (!atomic_load(&db_holder_release)) atomic_store(&db_release_forced, true);
    atomic_store(&db_holder_release, true);
    return NULL;
}

/* Opening the All songs sort menu must not wait on a busy database. */
static void check_all_songs_menu_busy(lv_obj_t *button) {
    pthread_t holder, releaser;
    atomic_store(&db_holder_locked, false);
    atomic_store(&db_holder_release, false);
    atomic_store(&db_release_forced, false);
    assert(!pthread_create(&holder, NULL, hold_database, NULL));
    while (!atomic_load(&db_holder_locked)) usleep(1000);
    assert(!pthread_create(&releaser, NULL, release_database_later, NULL));
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    assert(!atomic_load(&db_release_forced));
    assert(find_sort_option(lv_layer_top(), "Rating"));
    atomic_store(&db_holder_release, true);
    pthread_join(holder, NULL);
    pthread_join(releaser, NULL);
    assert(gui_option_menu_close());
}

static void select_all_songs_sort(lv_obj_t *button, int current, int selected) {
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    assert(current_settings.all_songs_sort_mode == (current == 2));
    assert(current_settings.all_songs_sort_reverse == (current == 1));
    assert(has_text(lv_layer_top(), "All songs"));
    lv_obj_t *options[] = {
        find_sort_option(lv_layer_top(), "Name (A–Z)"),
        find_sort_option(lv_layer_top(), "Name (Z–A)"),
        find_sort_option(lv_layer_top(), "Rating")
    };
    assert(options[0] && options[1] && options[2]);
    assert(lv_obj_get_parent(options[0]) == lv_obj_get_parent(options[2]));
    assert(lv_obj_get_parent(options[0]) == lv_obj_get_parent(options[1]));
    int count = 0;
    lv_obj_t *popup = lv_obj_get_parent(options[0]);
    for (uint32_t i = 0; i < lv_obj_get_child_count(popup); i++)
        if (lv_obj_has_flag(lv_obj_get_child(popup, i), LV_OBJ_FLAG_USER_2)) count++;
    assert(count == 3);
    for (int i = 0; i < 3; i++)
        assert(lv_obj_has_flag(lv_obj_get_child(options[i], 1), LV_OBJ_FLAG_HIDDEN) == (i != current));
    lv_obj_send_event(options[selected], LV_EVENT_CLICKED, NULL);
    assert(!find_sort_option(lv_layer_top(), "Rating"));
}

static atomic_bool az_hold_build, az_build_entered, az_release_build, az_build_exited;
static pthread_t az_ui_thread;
bool __real_metadata_db_build_az_table(metadata_db_az_kind_t, bool, int *, int32_t *, bool *);
bool __wrap_metadata_db_build_az_table(metadata_db_az_kind_t kind, bool reverse, int *jump,
                                      int32_t *generation, bool *available) {
    assert(!pthread_equal(pthread_self(), az_ui_thread));
    bool valid = __real_metadata_db_build_az_table(kind, reverse, jump, generation, available);
    *generation += atomic_load(&az_generation_offset);
    if (kind == METADATA_DB_AZ_ALL_SONGS && atomic_load(&az_hold_build)) {
        /* A rejected completion must never supply a row to the replacement. */
        for (int i = 0; i < 27; i++) jump[i] = 999;
        atomic_store(&az_build_entered, true);
        while (!atomic_load(&az_release_build)) usleep(1000);
        atomic_store(&az_build_exited, true);
    }
    return valid;
}

static atomic_bool az_guard_held, az_guard_release;
static void *hold_az_guard(void *unused) {
    (void)unused;
    METADATA_DB_GUARD;
    atomic_store(&az_guard_held, true);
    while (!atomic_load(&az_guard_release)) usleep(1000);
    return NULL;
}

static lv_indev_data_t strip_pointer;
static int strip_target = -1;
void __real_compact_list_scroll_to_index(lv_obj_t *, int);
void __wrap_compact_list_scroll_to_index(lv_obj_t *list, int index) {
    if (list == all_songs_list) strip_target = index;
    __real_compact_list_scroll_to_index(list, index);
}
static void read_strip_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    *data = strip_pointer;
}
static void check_reverse_strip(lv_obj_t *strip) {
    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_strip_pointer);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(all_songs_screen);
    lv_area_t area;
    lv_obj_get_coords(strip, &area);
    /* Z falls forward to R, the only populated letter in this fixture. */
    strip_pointer = (lv_indev_data_t){ .point = { (area.x1 + area.x2) / 2, area.y1 + 1 },
                                      .state = LV_INDEV_STATE_PRESSED };
    library_az_index_reset_drag();
    strip_target = -1;
    lv_indev_read(pointer);
    /* Warm the asynchronously prepared table before testing the ready path. */
    for (int i = 0; i < 2000 && strip_target < 0; i++) {
        pump_ui();
        library_az_index_poll(NULL);
    }
    library_az_index_poll(NULL);
    assert(strip_target == 0);
    assert(has_text(all_songs_screen, "Z"));
    /* A completed cache must also refuse a busy generation check immediately. */
    pthread_t holder;
    atomic_store(&az_guard_held, false);
    atomic_store(&az_guard_release, false);
    assert(!pthread_create(&holder, NULL, hold_az_guard, NULL));
    while (!atomic_load(&az_guard_held)) usleep(1000);
    library_az_index_reset_drag();
    strip_target = -1;
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    alarm(5);
    library_az_index_poll(NULL);
    clock_gettime(CLOCK_MONOTONIC, &after);
    alarm(0);
    assert((after.tv_sec - before.tv_sec) * 1000000000LL +
           after.tv_nsec - before.tv_nsec < 100000000LL);
    assert(strip_target == -1);
    library_az_index_rearm(all_songs_list);
    library_az_index_poll(NULL);
    assert(strip_target == -1);
    atomic_store(&az_guard_release, true);
    pthread_join(holder, NULL);
    for (int i = 0; i < 2000 && strip_target < 0; i++) {
        pump_ui();
        library_az_index_poll(NULL);
    }
    assert(strip_target == 0);

    /* Hold a worker result across a generation change. The next worker is
     * held too, so a stale landing cannot be hidden by a fast replacement. */
    atomic_store(&az_build_exited, false);
    atomic_store(&az_hold_build, true);
    atomic_store(&az_build_entered, false);
    atomic_store(&az_release_build, false);
    library_az_index_set_reverse(all_songs_screen, true);
    for (int i = 0; i < 2000 && !atomic_load(&az_build_entered); i++) pump_ui();
    assert(atomic_load(&az_build_entered));
    atomic_fetch_add(&az_generation_offset, 1);
    strip_target = -1;
    library_az_index_poll(NULL);
    assert(strip_target == -1);
    atomic_store(&az_release_build, true);
    /* A busy landing check retains ownership until a later timer tick. */
    atomic_store(&all_rating_db_busy, true);
    for (int i = 0; i < 2000 && !atomic_load(&az_build_exited); i++) pump_ui();
    assert(atomic_load(&az_build_exited));
    atomic_store(&az_build_entered, false);
    atomic_store(&az_release_build, false);
    atomic_store(&all_rating_db_busy, false);
    for (int i = 0; i < 2000 && !atomic_load(&az_build_entered); i++) pump_ui();
    assert(atomic_load(&az_build_entered));
    strip_target = -1;
    library_az_index_poll(NULL);
    assert(strip_target == -1);

    /* Remove the binding while its worker owns a result, then reuse its
     * slot. The lifetime token must reject that completion. */
    library_az_index_unregister(all_songs_screen);
    lv_obj_delete(strip);
    library_az_index_register(all_songs_screen, all_songs_list, METADATA_DB_AZ_ALL_SONGS);
    library_az_index_set_reverse(all_songs_screen, true);
    strip = library_az_index_strip_for_screen(all_songs_screen);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
    /* A new strip has no coordinates until layout; the press is judged once. */
    lv_obj_update_layout(all_songs_screen);
    atomic_store(&az_hold_build, false);
    atomic_store(&az_release_build, true);
    for (int i = 0; i < 2000 && strip_target < 0; i++) {
        pump_ui();
        library_az_index_poll(NULL);
    }
    assert(strip_target == 0);
    library_az_index_reset_drag();
    lv_indev_delete(pointer);
    atomic_store(&az_generation_offset, 0);
}

static void check_reverse_search(const int expected[]) {
    compact_list_page_row_t rows[1];
    library_search_open(SEARCH_BINDING_ALL_SONGS);
    lv_obj_t *entry = find_search_entry(all_songs_screen);
    assert(entry);
    lv_textarea_set_text(entry, "Rating song 2");
    int rank = -1;
    for (int i = 0; i < 2000; i++) {
        pump_ui();
        poll_search_job();
        if (library_search_try_map_filtered_row(SEARCH_BINDING_ALL_SONGS, 0, &rank)) break;
    }
    assert(rank == 2 && search_remap_index(SEARCH_BINDING_ALL_SONGS, 0) == 2);
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;
    all_click(0);
    check_queue_paths(expected);
    assert(player_queue_model_current() == 3);
    all_menu(0);
    assert(all_menu_rank == 2);
    lv_screen_load(all_songs_screen);
    assert(search_close_if_active_for_screen(all_songs_screen));
    assert(all_songs_fetch_page(NULL, 0, 1, rows) == 1);
    assert(!strcmp(rows[0].label, "Rating song 5"));
    lv_obj_t *strip = library_az_index_strip_for_screen(all_songs_screen);
    assert(strip);
    assert(!strncmp(lv_label_get_text(strip), "Z\nY\nX", 5));
}

static void check_all_songs_reverse(lv_obj_t *sort) {
    select_all_songs_sort(sort, 0, 1);
    settings_save_drain();
    player_settings_t saved;
    assert(settings_load(&saved) && saved.all_songs_sort_mode == 0 && saved.all_songs_sort_reverse);
    compact_list_page_row_t rows[10];
    assert(all_songs_fetch_page(NULL, 0, 10, rows) == 6);
    for (int i = 0; i < 6; i++) {
        char title[40];
        snprintf(title, sizeof(title), "Rating song %d", 5 - i);
        assert(!strcmp(rows[i].label, title));
    }
    assert(all_songs_fetch_page(NULL, 5, 10, rows) == 1);
    assert(!strcmp(rows[0].label, "Rating song 0"));
    assert(all_songs_fetch_page(NULL, 6, 1, rows) == 0);
    assert(songs_title_fetch_page(NULL, 0, 1, rows) == 1);
    assert(!strcmp(rows[0].label, "Rating song 0"));
    const int expected[] = {5,4,3,2,1,0};
    current_settings.play_mode = PLAY_MODE_SEQUENTIAL;
    all_click(0);
    check_queue_paths(expected);
    assert(player_queue_model_current() == 0);
    assert(player_source_kind == PLAYER_SOURCE_NONE && current_settings.last_source_kind == 0);
    gui_player_step_manual(1);
    assert(player_queue_model_current() == 1);
    check_queue_paths(expected);
    all_menu(0);
    assert(all_menu_rank == 5);
    all_menu(5);
    assert(all_menu_rank == 0);
    playlist_start_source = PLAY_ALL_SOURCE_ALL_SONGS;
    playlist_start_selected(false);
    assert(player_queue_model_current() == 0);
    check_queue_paths(expected);
    playlist_start_selected(true);
    assert(current_settings.play_mode == PLAY_MODE_SHUFFLE);
    check_queue_paths(expected);
    bool seen[6] = {0};
    for (int i = 0; i < 6; i++) {
        int pos = player_queue_model_current();
        assert(pos >= 0 && pos < 6 && !seen[pos]);
        seen[pos] = true;
        if (i < 5) gui_player_step_manual(1);
    }
    lv_screen_load(all_songs_screen);
    assert(library_az_index_list_allowed(all_songs_list));
    lv_obj_t *strip = library_az_index_strip_for_screen(all_songs_screen);
    assert(strip && !strncmp(lv_label_get_text(strip), "Z\nY\nX", 5));
    int jump[27];
    metadata_db_get_az_table_sorted(METADATA_DB_AZ_ALL_SONGS, true, jump);
    assert(jump['R' - 'A'] == 0);
    check_reverse_strip(strip);
    snprintf(now_playing_path, sizeof(now_playing_path), "./music/rating-2.wav");
    all_playing_row = -1;
    refresh_now_playing_indicators();
    for (int i = 0; i < 2000 && all_playing_row != 3; i++) {
        pump_ui();
        gui_library_poll_now_playing_indicators();
    }
    assert(all_playing_row == 3);
    check_reverse_search(expected);
    free_playlist();
    assert(snapshot_refs == 0);
    snapshot_opens = 0;
    select_all_songs_sort(sort, 1, 2);
    assert(!current_settings.all_songs_sort_reverse);
    /* While the rating order loads, the rows shown are the A-Z fallback that
     * taps and Play use, not the Z-A rows that were on screen. */
    const char *first = NULL;
    for (int i = 0; i < 2000 && !compact_list_rederive_cached(all_songs_list, cached_row_zero, &first); i++)
        pump_ui();
    assert(first && !strcmp(first, "Rating song 0"));
}

static void check_all_songs_by_rating(void) {
    seed_all_rating_songs();
    atomic_store(&all_rating_fixture, true);
    atomic_store(&all_rating_hold, true);
    current_settings.all_songs_sort_mode = 0;
    lv_obj_t *screen = all_songs_screen;
    lv_screen_load(screen);
    lv_obj_t *sort = lv_obj_find_by_name(screen, "all_songs_sort_btn");
    assert(sort && all_click && all_menu);
    check_all_songs_menu_busy(sort);
    check_all_songs_reverse(sort);
    assert(current_settings.all_songs_sort_mode == 1);
    settings_save_drain();
    player_settings_t saved;
    assert(settings_load(&saved) && saved.all_songs_sort_mode == 1);
    for (int i = 0; i < 2000 && !atomic_load(&all_rating_reads); i++) pump_ui();
    assert(atomic_load(&all_rating_reads) && snapshot_opens == 0);
    current_settings.all_songs_sort_reverse = true;
    check_all_rating_fallback();
    current_settings.all_songs_sort_reverse = false;
    atomic_store(&all_rating_fail, true);
    atomic_store(&all_rating_hold, false);
    for (int i = 0; i < 2000 && !atomic_load(&all_rating_failures); i++) pump_ui();
    assert(atomic_load(&all_rating_failures));
    for (int i = 0; i < 50; i++) pump_ui();
    check_all_rating_fallback();
    atomic_store(&all_rating_fail, false);
    wait_all_rating(10);
    assert(lv_obj_get_scroll_y(all_songs_list) == 0);
    check_all_rating_rows(screen);
    check_all_rating_playback();
    check_all_rating_rejection();
    lv_screen_load(screen);
    check_all_rating_search(screen);
    check_all_rating_busy_landing();
    int reads = atomic_load(&all_rating_reads);
    refresh_library_screens_after_reload(false);
    lv_screen_load(all_songs_screen);
    wait_all_rating(9);
    assert(atomic_load(&all_rating_reads) > reads);
    check_all_rating_schedule_busy();
    sort = lv_obj_find_by_name(all_songs_screen, "all_songs_sort_btn");
    select_all_songs_sort(sort, 2, 0);
    settings_save_drain();
    assert(settings_load(&saved) && saved.all_songs_sort_mode == 0);
    assert(current_settings.all_songs_sort_mode == 0 && all_songs_paged_total() == 6);
    assert(!current_settings.all_songs_sort_reverse && !saved.all_songs_sort_reverse);
    compact_list_page_row_t rows[6];
    assert(all_songs_fetch_page(NULL, 0, 6, rows) == 6);
    assert(!strcmp(rows[0].label, "Rating song 0") && !strcmp(rows[5].label, "Rating song 5"));
    select_all_songs_sort(sort, 0, 0);
    assert(library_az_index_list_allowed(all_songs_list));
    atomic_store(&all_rating_fixture, false);
    all_songs_rearm();
}

static atomic_bool most_fixture, most_hold, most_entered;
static atomic_int most_completed;
void __real_metadata_db_load_top_played_songs(int limit, char ***paths, int *count);
void __wrap_metadata_db_load_top_played_songs(int limit, char ***paths, int *count) {
    if (!atomic_load(&most_fixture)) { __real_metadata_db_load_top_played_songs(limit, paths, count); return; }
    bool ui = pthread_equal(az_ui_thread, pthread_self());
    atomic_store(&most_entered, true);
    while (!ui && atomic_load(&most_hold)) usleep(1000);
    *count = 2; *paths = calloc(2, sizeof(char *)); assert(*paths);
    (*paths)[0] = strdup("./music/filter-0.wav"); (*paths)[1] = strdup("./music/filter-1.wav");
    atomic_fetch_add(&most_completed, 1);
}
static void wait_most_entered(void) {
    for (int i = 0; i < 2000 && !atomic_load(&most_entered); ++i) pump_ui();
    assert(atomic_load(&most_entered));
}
static void check_most_played_refresh_pending(void) {
    int completed = atomic_load(&most_completed);
    atomic_store(&most_hold, true); atomic_store(&most_entered, false);
    library_playlist_open_row(GUI_PLAYLIST_ROW_MOST_PLAYED, NULL, NULL, NULL, 0);
    wait_most_entered();
    refresh_group_songs_from_database();
    refresh_group_songs_from_database();
    atomic_store(&most_hold, false);
    for (int i = 0; i < 2000 && atomic_load(&most_completed) == completed; i++) usleep(1000);
    assert(atomic_load(&most_completed) == completed + 1);
    atomic_store(&most_hold, true); atomic_store(&most_entered, false);
    wait_most_entered(); /* the stale completion must launch exactly one replacement */
    assert(group_songs_count == 0);
    atomic_store(&most_hold, false);
    for (int i = 0; i < 2000 && group_songs_count != 2; i++) pump_ui();
    assert(group_songs_count == 2 && atomic_load(&most_completed) == completed + 2);
}

static void check_most_played_async(void) {
    atomic_store(&most_fixture, true); atomic_store(&most_hold, true);
    library_playlist_open_row(GUI_PLAYLIST_ROW_MOST_PLAYED, NULL, NULL, NULL, 0);
    wait_most_entered();
    assert(group_songs_count == 0);
    lv_obj_t *covered = lv_obj_create(NULL);
    lv_screen_load(covered);
    atomic_store(&most_hold, false);
    for (int i = 0; i < 2000 && group_songs_count != 2; ++i) pump_ui();
    assert(group_songs_count == 2 && lv_screen_active() == covered);
    atomic_store(&most_hold, true); atomic_store(&most_entered, false);
    refresh_group_songs_from_database(); /* must return before the held worker */
    wait_most_entered();
    show_group_songs("Other collection", NULL, 0);
    atomic_store(&most_hold, false);
    for (int i = 0; i < 300; ++i) pump_ui();
    assert(atomic_load(&most_completed) == 2 && group_songs_count == 0);
    atomic_store(&most_hold, true); atomic_store(&most_entered, false);
    library_playlist_open_row(GUI_PLAYLIST_ROW_MOST_PLAYED, NULL, NULL, NULL, 0);
    wait_most_entered();
    play_remote_control_song("./music/filter-0.wav", "@most_played", "", "", "");
    assert(playback_count == 2); /* remote scope matches the synchronous sibling collections */
    atomic_store(&most_hold, false);
    for (int i = 0; i < 2000 && group_songs_count != 2; ++i) pump_ui();
    assert(group_songs_count == 2); /* a concurrent remote play must not consume the view result */
    check_most_played_refresh_pending();
    puts("Most Played async loading, covered result, stale view, refresh retry and concurrent remote play passed");
}

static void geometry_select(int index) { (void)index; }
static void check_cover_flow_geometry(void) {
    cover_flow_item_t item = { .title = "Album", .subtitle = "Artist", .song_id = -1 };
    cover_flow_params_t params = { .title = "Albums", .items = &item, .count = 1, .az_index = true, .on_select = geometry_select };
    assert(cover_flow_show(&params));
    assert(cf_instance);
    lv_obj_update_layout(cf_instance->screen);
    lv_area_t title, subtitle;
    lv_obj_get_coords(cf_instance->title_label, &title);
    lv_obj_get_coords(cf_instance->sub_label, &subtitle);
    assert(title.y2 < subtitle.y1);
    assert(subtitle.y2 < cf_instance->g.strip_y);
    assert(cf_instance->g.strip_y + cf_instance->g.strip_h <=
           BOARD_SCREEN_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    assert(cf_instance->g.cover_y >= cf_instance->g.area_top);
    puts("Cover Flow actual LVGL label/strip geometry passed");
}

static void check_information_metadata(void) {
    gui_track_info_context_t playing = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(playing.path, "./music/current.wav");
    gui_track_info_set_current(&playing);
    gui_track_info_context_t selected = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(selected.path, "./music/selected.wav");
    atomic_store(&hold_probe, true);
    gui_track_info_open_for_context(&selected);
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    gui_track_info_poll(); /* Must return even while the file worker is held. */
    assert(!has_text(gui_track_info_get_screen(), "16-bit / 48 kHz"));
    atomic_store(&hold_probe, false);
    wait_for("16-bit / 48 kHz");
    assert(has_text(gui_track_info_get_screen(), "3/5"));
    assert(has_text(gui_track_info_get_screen(), "Stereo (2 channels)"));
    assert(has_text(gui_track_info_get_screen(), "~1536 kbps"));
    assert(has_text(gui_track_info_get_screen(), "0:02"));
    nav_pop_immediate();
    strcpy(selected.path, "./music/selected.cue#compas-cue=2");
    gui_track_info_open_for_context(&selected);
    wait_for("16-bit / 48 kHz");
    assert(has_text(gui_track_info_get_screen(), "wav"));
    assert(has_text(gui_track_info_get_screen(), "375.0 KB"));
    assert(has_text(gui_track_info_get_screen(), "0:01"));
    assert(has_text(gui_track_info_get_screen(), "~1536 kbps"));
    check_stale_completion(&selected);
    check_current_rating(0);
    check_current_rating(4);
    check_rating_during_probe(4);
    check_rating_during_probe(0);
    check_placeholder_rating();
}

int compas_original_main(int argc, char **argv);
int main(int argc, char **argv) {
    az_ui_thread = pthread_self();
    if (argc > 1 && (!strcmp(argv[1], "--metadata-artwork-helper") ||
                     !strcmp(argv[1], "--metadata-scan-worker")))
        return compas_original_main(argc, argv);
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    if (getenv("COMPAS_TEST_MOST_PLAYED")) { check_most_played_async(); return 0; }
    if (getenv("COMPAS_TEST_COVER_GEOMETRY")) { check_cover_flow_geometry(); return 0; }
    check_small_star_chips();
    check_player_rating(false);
    check_transport_tint();
    check_plugin_rating();
    if (getenv("COMPAS_TEST_PLAYER_XML")) return 0;
    check_minimal_rating();
    check_song_popup();
    check_popup_accent();
    ui_thread = pthread_self();
    check_loading_album_rating();
    check_album_rating();
    check_favorite_albums();
    check_favorite_empty_results();
    check_rated_list_accent();
    check_top_songs();
    check_refresh_during_load();
    check_top_albums();
    check_live_ratings();
    check_all_songs_by_rating();
    check_popup_rating();
    check_information_metadata();
    puts("Information metadata, ratings, CUE facts and stale completion passed");
}
