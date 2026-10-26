#include "../../application/gui.h"
#include "../browse/gui_folder_actions.h"
#include "gui_playlist_target.h"
#include "gui_playlists.h"
#include "../../text/rendering/gui_text_input.h"
#include "../../../library/playlists/playlist_files.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static text_entry_done_cb_t pending_done;
static void *pending_data;
static int errors, inserts, pops, folder_jobs, open_rows;
static char last_error[128], folder_path[PATH_MAX], folder_name[201];
static bool folder_create;
static char borrowed_name[128], borrowed_path[PATH_MAX], borrowed_song[PATH_MAX];
static int borrowed_count;
static void fixture_text_entry(const char *title, const char *initial, bool password, bool numeric,
                               text_entry_done_cb_t cb, void *data) {
    (void)title;
    (void)initial;
    (void)password;
    (void)numeric;
    pending_done = cb;
    pending_data = data;
}
static void fixture_error(const char *s) {
    errors++;
    snprintf(last_error, sizeof(last_error), "%s", s);
}
static void fixture_info(const char *s) { (void)s; }
static void fixture_pop(void) { pops++; }
static void fixture_insert(const char *p) {
    (void)p;
    inserts++;
}
static void fixture_remove(int slot) { (void)slot; }
static int fixture_depth(void) { return 1; }
static lv_obj_t *fixture_screen_at(int i) {
    (void)i;
    return NULL;
}
static void fixture_load_playlists(char ***paths, int *count) {
    *paths = NULL;
    *count = 0;
}
static bool fixture_folder_job(bool create, const char *source, const char *destination,
                               const char *name, uint64_t generation, lv_obj_t *screen) {
    (void)destination;
    (void)generation;
    (void)screen;
    folder_jobs++;
    folder_create = create;
    snprintf(folder_path, sizeof(folder_path), "%s", source);
    snprintf(folder_name, sizeof(folder_name), "%s", name ? name : "");
    return true;
}
static const char *fixture_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void fixture_open_row(gui_playlist_row_kind_t kind, const char *name, const char *path,
                             char **songs, int count) {
    assert(kind == GUI_PLAYLIST_ROW_SAVED_M3U);
    open_rows++;
    snprintf(borrowed_name, sizeof(borrowed_name), "%s", name);
    snprintf(borrowed_path, sizeof(borrowed_path), "%s", path);
    borrowed_count = count;
    if (count)
        snprintf(borrowed_song, sizeof(borrowed_song), "%s", songs[0]);
    assert(name && path && count == 1 && songs && songs[0]);
}
#define show_text_entry fixture_text_entry
#define show_error_toast fixture_error
#define show_info_toast fixture_info
#define nav_pop fixture_pop
#define nav_remove_stack_slot fixture_remove
#define gui_navigation_get_depth fixture_depth
#define gui_navigation_get_screen_at fixture_screen_at
#define metadata_db_playlist_insert_one fixture_insert
#define metadata_db_load_all_playlists fixture_load_playlists
#define gui_folder_actions_start_playlist_job fixture_folder_job
#define basename_of fixture_basename
#include "gui_playlists.c"
#include "gui_playlist_target.c"

static void flush_ready(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    LV_UNUSED(a);
    LV_UNUSED(p);
    lv_display_flush_ready(d);
}
static lv_obj_t *event_row(lv_event_cb_t cb, void *data) {
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    assert(o);
    lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, data);
    return o;
}
static void send_click(lv_obj_t *o) {
    assert(lv_obj_send_event(o, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
}
static gui_playlist_target_snapshot_t target(void) {
    gui_playlist_target_snapshot_t t;
    gui_playlist_target_copy(&t);
    return t;
}
static void path_suffix(char *out, size_t capacity, const char *root, const char *suffix) {
    size_t root_len = strlen(root), suffix_len = strlen(suffix);
    assert(root_len + suffix_len < capacity);
    memcpy(out, root, root_len);
    memcpy(out + root_len, suffix, suffix_len + 1);
}

int main(void) {
    char wd[] = "./playlist-module-XXXXXX";
    assert(mkdtemp(wd));
    assert(chdir(wd) == 0);
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/Playlists", 0700) == 0);
    FILE *f = fopen("music/song.flac", "wb");
    assert(f);
    fputs("song", f);
    fclose(f);
    char root[PATH_MAX], song[PATH_MAX], list[PATH_MAX], created[PATH_MAX], stale[PATH_MAX];
    assert(getcwd(root, sizeof(root)));
    path_suffix(song, sizeof(song), root, "/music/song.flac");
    lv_init();
    lv_display_t *display = lv_display_create(320, 240);
    assert(display);
    static uint8_t buf[320 * 16 * 4];
    lv_display_set_buffers(display, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);
    lv_obj_t *screen = lv_obj_create(NULL);
    assert(screen);
    lv_screen_load(screen);
    add_to_playlist_screen = screen;
    add_to_playlist_list = lv_obj_create(screen);
    assert(add_to_playlist_list);
    gui_playlists_init(fixture_open_row, NULL, NULL);

    gui_playlist_target_begin_song(song);
    playlist_name_target_mode = PLAYLIST_NAME_TARGET_SONG;
    playlist_name_target_generation = target().generation;
    lv_obj_t *newrow = event_row(new_playlist_row_cb, NULL);
    send_click(newrow);
    assert(pending_done == new_playlist_name_done_cb);
    path_suffix(list, sizeof(list), root, "/music/Playlists/Actual.m3u");
    assert(playlist_files_write_new("./music/Playlists", "Actual", (const char *[]){song}, 1, list,
                                    sizeof(list)));
    lv_obj_t *existing = event_row(existing_playlist_row_cb, list);
    send_click(existing);
    assert(errors == 1 && strstr(last_error, "already added") && pops == 1);

    send_click(newrow);
    assert(pending_done);
    pending_done("Created", pending_data);
    path_suffix(created, sizeof(created), root, "/music/Playlists/Created.m3u");
    assert(access(created, F_OK) == 0 && inserts == 1 && pops == 2);
    gui_playlist_target_snapshot_t t = target();
    assert(t.kind == GUI_PLAYLIST_TARGET_SONG && !strcmp(t.song_path, song));

    send_click(newrow);
    assert(pending_done);
    gui_playlist_target_begin_song("/music/replaced.flac");
    path_suffix(stale, sizeof(stale), root, "/music/Playlists/Stale.m3u");
    int old_errors = errors;
    pending_done("Stale", pending_data);
    assert(access(stale, F_OK) != 0 && errors == old_errors + 1 && strstr(last_error, "expired"));

    gui_playlist_target_begin_folder(wd);
    playlist_name_target_mode = PLAYLIST_NAME_TARGET_FOLDER;
    playlist_name_target_generation = target().generation;
    send_click(newrow);
    assert(pending_done);
    pending_done("Folder List", pending_data);
    assert(folder_jobs == 1 && folder_create && !strcmp(folder_path, wd) &&
           !strcmp(folder_name, "Folder List"));

    /* Exercise the module's synchronous saved-M3U callback contract. */
    playlists_m3u_paths = calloc(1, sizeof(*playlists_m3u_paths));
    assert(playlists_m3u_paths);
    playlists_m3u_paths[0] = strdup(list);
    assert(playlists_m3u_paths[0]);
    playlists_m3u_count = 1;
    lv_obj_t *saved = event_row(playlist_row_click_cb, (void *)(intptr_t)4);
    send_click(saved);
    assert(open_rows == 1 && borrowed_count == 1 && !strcmp(borrowed_song, song) &&
           !strcmp(borrowed_path, list) && !strcmp(borrowed_name, "Actual.m3u"));

    free(playlists_m3u_paths[0]);
    free(playlists_m3u_paths);
    assert(playlist_files_delete(list));
    assert(playlist_files_delete(created));
    assert(unlink("music/song.flac") == 0);
    assert(rmdir("music/Playlists") == 0);
    assert(rmdir("music") == 0);
    assert(chdir("..") == 0);
    assert(rmdir(wd) == 0);
    puts("actual gui_playlists module: song target/create=PASS duplicate=PASS "
         "stale name=PASS folder create request=PASS saved callback borrowed "
         "lifetime=PASS");
    return 0;
}
