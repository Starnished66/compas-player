/* Exercise the actual continuation state machine with a synchronous player
 * stand-in, including its track-started/stop callbacks during a new launch. */
#include "src/ui/settings/gui_play_through.c"
#include <assert.h>
#include <unistd.h>

player_settings_t current_settings;
static const char * mode = "sequential";
static char current_path[1024];
static bool playing, paused;
static int launches, launch_count;
static int album_next_count;
static char first_launched[1024];

const char * gui_plugin_get_play_mode(void) { return mode; }
const char * gui_plugin_get_current_track_path(void) { return current_path; }
bool audio_is_playing(void) { return playing; }
bool audio_is_paused(void) { return paused; }
const char * i18n_tr(const char * text) { return text; }
void show_info_toast(const char * text) { (void)text; }
void gui_plugin_free_string_array(char ** paths, int count) {
    for (int i = 0; i < count; ++i) free(paths[i]);
    free(paths);
}
static char ** path_array(int count, const char * prefix) {
    char ** paths = calloc((size_t)count, sizeof(*paths));
    for (int i = 0; i < count; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "%s/%04d.flac", prefix, i);
        paths[i] = strdup(path);
    }
    return paths;
}
char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * count) {
    (void)artist; (void)album;
    *count = 1;
    char ** paths = calloc(1, sizeof(*paths));
    paths[0] = strdup(current_path);
    return paths;
}
char ** gui_plugin_get_next_album_tracks(const char * artist, const char * album, int * count) {
    (void)artist; (void)album;
    *count = album_next_count;
    return *count ? path_array(*count, "/next-album") : NULL;
}
void gui_plugin_play_paths(const char * const * paths, int count, int start, bool open) {
    (void)start; (void)open;
    launches++;
    launch_count = count;
    snprintf(first_launched, sizeof(first_launched), "%s", paths[0]);
    snprintf(current_path, sizeof(current_path), "%s", paths[0]);
    gui_play_through_stopped();
    gui_play_through_track_started("Artist", "Album");
}
static void touch_file(const char * root, const char * tail) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", root, tail);
    FILE * file = fopen(path, "w");
    assert(file);
    fclose(file);
}
int main(void) {
    char root[] = "/tmp/compas-play-through-XXXXXX";
    assert(mkdtemp(root));
    char a[1024], b[1024], empty[1024];
    snprintf(a, sizeof(a), "%s/a", root); assert(!mkdir(a, 0700));
    snprintf(b, sizeof(b), "%s/B", root); assert(!mkdir(b, 0700));
    snprintf(empty, sizeof(empty), "%s/aa-empty", root); assert(!mkdir(empty, 0700));
    touch_file(root, "a/z.FLAC"); touch_file(root, "a/A.mp3");
    touch_file(root, "B/Z.mp3"); touch_file(root, "B/a.flac");
    snprintf(current_path, sizeof(current_path), "%s/a/A.mp3", root);
    int count;
    char ** tracks = next_folder_tracks(current_path, &count);
    assert(!tracks && !count); /* Must be final file in current folder. */
    snprintf(current_path, sizeof(current_path), "%s/a/z.FLAC", root);
    tracks = next_folder_tracks(current_path, &count);
    assert(count == 2 && strstr(tracks[0], "/B/a.flac"));
    gui_plugin_free_string_array(tracks, count);

    current_settings.native_play_through_folders = true;
    current_settings.native_play_through_albums = true;
    album_next_count = 1001;
    gui_play_through_track_started("Artist", "Album");
    assert(candidate_count == 1001 && strstr(candidates[0], "next-album")); /* Album wins. */
    mode = "shuffle";
    gui_play_through_queue_exhausted(1);
    assert(!launches);
    mode = "repeat_all";
    continuation_tick(NULL); continuation_tick(NULL);
    assert(!launches);
    mode = "sequential";
    gui_play_through_queue_exhausted(-1); assert(!launches);
    gui_play_through_queue_exhausted(1);
    assert(launches == 1 && launch_count == 500 && candidate_index == 500);
    gui_play_through_queue_exhausted(1);
    assert(launches == 2 && launch_count == 500 && candidate_index == 1000);
    assert(strstr(first_launched, "0500.flac"));
    /* Final batch can arm the following album; disable that lookup here. */
    current_settings.native_play_through_albums = false;
    current_settings.native_play_through_folders = false;
    gui_play_through_queue_exhausted(1);
    assert(launches == 3 && launch_count == 1 && !candidates);
    assert(strstr(first_launched, "1000.flac"));

    candidates = path_array(2, "/test"); candidate_count = 2;
    gui_play_through_stopped();
    gui_play_through_queue_exhausted(1);
    assert(launches == 3 && !candidates);
    candidates = path_array(2, "/test"); candidate_count = 2;
    playing = true; continuation_tick(NULL); continuation_tick(NULL); assert(launches == 3);
    playing = false; paused = true; continuation_tick(NULL); continuation_tick(NULL); assert(launches == 3);
    paused = false; continuation_tick(NULL); assert(launches == 3);
    gui_play_through_resumed(); continuation_tick(NULL); assert(launches == 3);
    continuation_tick(NULL); assert(launches == 4);
    mode = "shuffle";
    gui_play_through_track_started("Artist", "Album"); assert(!candidates);
    clear_candidates();
    char path[1024];
    const char * files[] = { "a/z.FLAC", "a/A.mp3", "B/Z.mp3", "B/a.flac" };
    for (int i = 0; i < 4; ++i) { snprintf(path, sizeof(path), "%s/%s", root, files[i]); assert(!unlink(path)); }
    assert(!rmdir(a)); assert(!rmdir(b)); assert(!rmdir(empty)); assert(!rmdir(root));
    puts("Play Through state, lexical folder order, album priority, modes, stop and 1001-track batches passed");
    return 0;
}
