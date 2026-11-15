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
static char storage_path[1024];
static atomic_bool block_discovery, discovery_entered;

lv_timer_t * lv_timer_create(lv_timer_cb_t cb, uint32_t period, void * data) {
    (void)cb; (void)period; (void)data;
    return (lv_timer_t *)1;
}
size_t storage_roots_list(storage_root_t * roots, size_t max) {
    assert(max > 0);
    assert(strlen(storage_path) < sizeof(roots[0].path));
    strcpy(roots[0].path, storage_path);
    return 1;
}
bool storage_roots_find_path(const storage_root_t * roots, size_t count, const char * path, size_t * index) {
    if (count && path_below(path, roots[0].path)) { *index = 0; return true; }
    return false;
}
static void finish_discovery(void) {
    for (int i = 0; i < 5000 && (discovery_active || discovery_queued); ++i) {
        poll_discovery();
        usleep(1000);
    }
    assert(!discovery_active && !discovery_queued);
}

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
    atomic_store(&discovery_entered, true);
    while (atomic_load(&block_discovery)) usleep(1000);
    *count = 1;
    char ** paths = calloc(1, sizeof(*paths));
    paths[0] = strdup(current_path);
    return paths;
}
metadata_db_album_sort_t requested_group_album_sort(metadata_db_group_kind_t kind) { (void)kind; return METADATA_DB_ALBUM_SORT_YEAR; }
char ** gui_plugin_get_next_album_tracks_sorted(const char * artist, const char * album, metadata_db_album_sort_t sort, int * count) {
    (void)artist; (void)album;
    (void)sort;
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
/* Cue stand-ins: any reference resolves to a three-track sheet. */
bool cue_reference_is(const char * reference) { return reference && strstr(reference, "#compas-cue="); }
bool cue_reference_parse(const char * reference, char * source, size_t size, unsigned * ordinal) {
    const char * mark = strstr(reference, "#compas-cue=");
    if (!mark || (size_t)(mark - reference) >= size) return false;
    memcpy(source, reference, (size_t)(mark - reference));
    source[mark - reference] = '\0';
    *ordinal = (unsigned)atoi(mark + strlen("#compas-cue="));
    return true;
}
bool cue_reference_resolve(const char * reference, cue_sheet_t * sheet, unsigned * ordinal) {
    char source[1024];
    memset(sheet, 0, sizeof(*sheet));
    sheet->track_count = 3;
    return cue_reference_parse(reference, source, sizeof(source), ordinal);
}
void cue_sheet_free(cue_sheet_t * sheet) { (void)sheet; }
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
    snprintf(storage_path, sizeof(storage_path), "%s", root);
    char a[1024], b[1024], empty[1024], path[1024];
    snprintf(a, sizeof(a), "%s/a", root); assert(!mkdir(a, 0700));
    snprintf(b, sizeof(b), "%s/B", root); assert(!mkdir(b, 0700));
    snprintf(empty, sizeof(empty), "%s/aa-empty", root); assert(!mkdir(empty, 0700));
    touch_file(root, "a/z.FLAC"); touch_file(root, "a/A.mp3");
    touch_file(root, "B/Z.mp3"); touch_file(root, "B/a.flac");
    snprintf(current_path, sizeof(current_path), "%s/a/A.mp3", root);
    int count;
    char ** tracks = next_folder_tracks(current_path, root, &count);
    assert(!tracks && !count); /* Must be final file in current folder. */
    snprintf(current_path, sizeof(current_path), "%s/a/z.FLAC", root);
    tracks = next_folder_tracks(current_path, root, &count);
    assert(count == 2 && strstr(tracks[0], "/B/a.flac"));
    gui_plugin_free_string_array(tracks, count);

    /* Nested Artist/Album folders climb into the next artist; the music root is a hard ceiling. */
    char music[1024], outside[1024];
    snprintf(music, sizeof(music), "%s/music", root); assert(!mkdir(music, 0700));
    snprintf(outside, sizeof(outside), "%s/zz-outside", root); assert(!mkdir(outside, 0700));
    const char * dirs[] = { "music/Artist1", "music/Artist1/Album", "music/Artist2", "music/Artist2/Album1",
                            "music/Artist2/Album2", "music/Artist3", "music/Artist3/Empty", "music/Artist3/Sub",
                            "music/Artist3/Sub/Deep", "music/Artist4", "music/Artist4/Album" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]); assert(!mkdir(path, 0700));
    }
    const char * nested_files[] = { "music/Artist1/Album/cue.flac", "music/Artist2/Album1/x.flac",
                                    "music/Artist2/Album2/y.flac", "music/Artist3/Sub/Deep/d.flac",
                                    "music/Artist4/Album/last.flac", "zz-outside/o.flac", "music/root.flac" };
    for (size_t i = 0; i < sizeof(nested_files) / sizeof(nested_files[0]); ++i) touch_file(root, nested_files[i]);
    snprintf(path, sizeof(path), "%s/music/Artist1/Album/cue.flac#compas-cue=2", root);
    assert(!next_folder_tracks(path, music, &count)); /* Not the sheet's last track. */
    snprintf(path, sizeof(path), "%s/music/Artist1/Album/cue.flac#compas-cue=3", root);
    tracks = next_folder_tracks(path, music, &count);
    assert(count == 1 && strstr(tracks[0], "/Artist2/Album1/x.flac")); /* Cue reference as last track. */
    gui_plugin_free_string_array(tracks, count);
    snprintf(path, sizeof(path), "%s/music/Artist2/Album1/x.flac", root);
    tracks = next_folder_tracks(path, music, &count);
    assert(count == 1 && strstr(tracks[0], "/Artist2/Album2/y.flac"));
    gui_plugin_free_string_array(tracks, count);
    snprintf(path, sizeof(path), "%s/music/Artist2/Album2/y.flac", root);
    tracks = next_folder_tracks(path, music, &count);
    assert(count == 1 && strstr(tracks[0], "/Artist3/Sub/Deep/d.flac")); /* Skips empty, goes deep. */
    gui_plugin_free_string_array(tracks, count);
    snprintf(path, sizeof(path), "%s/music/Artist3/Sub/Deep/d.flac", root);
    tracks = next_folder_tracks(path, music, &count);
    assert(count == 1 && strstr(tracks[0], "/Artist4/Album/last.flac"));
    gui_plugin_free_string_array(tracks, count);
    snprintf(path, sizeof(path), "%s/music/Artist4/Album/last.flac", root);
    assert(!next_folder_tracks(path, music, &count) && !count); /* End of library; never past the root. */
    snprintf(path, sizeof(path), "%s/music/root.flac", root);
    assert(!next_folder_tracks(path, music, &count) && !count); /* Files directly in the root stay put. */
    snprintf(path, sizeof(path), "%s/music/Artist4/Album/last.flac", root);
    tracks = next_folder_tracks(path, root, &count); /* Under a wider root the climb continues. */
    assert(count == 1 && strstr(tracks[0], "/zz-outside/o.flac"));
    gui_plugin_free_string_array(tracks, count);
    /* The live containing root also allows a USB-style tree to cross artists. */
    snprintf(storage_path, sizeof(storage_path), "%s", music);
    snprintf(current_path, sizeof(current_path), "%s/music/Artist1/Album/cue.flac", root);
    current_settings.native_play_through_folders = true;
    gui_play_through_track_started(NULL, NULL);
    finish_discovery();
    assert(candidate_count == 1 && strstr(candidates[0], "/Artist2/Album1/x.flac"));
    clear_candidates();
    snprintf(storage_path, sizeof(storage_path), "%s", root);
    for (size_t i = 0; i < sizeof(nested_files) / sizeof(nested_files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, nested_files[i]); assert(!unlink(path));
    }
    for (size_t i = sizeof(dirs) / sizeof(dirs[0]); i-- > 0;) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]); assert(!rmdir(path));
    }
    assert(!rmdir(music)); assert(!rmdir(outside));

    current_settings.native_play_through_folders = true;
    current_settings.native_play_through_albums = true;
    album_next_count = 1001;
    gui_play_through_track_started("Artist", "Album");
    finish_discovery();
    assert(candidate_count == 1001 && strstr(candidates[0], "next-album")); /* Album wins. */
    playing = true;
    continuation_tick(NULL); continuation_tick(NULL);
    assert(candidate_count == 1001 && !launches); /* Early completion remains armed. */
    playing = false;
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
    /* Stale completion after a stop or setting change cannot rearm playback. */
    current_settings.native_play_through_albums = true;
    atomic_store(&block_discovery, true);
    atomic_store(&discovery_entered, false);
    gui_play_through_track_started("Artist", "Album");
    while (!atomic_load(&discovery_entered)) usleep(1000);
    gui_play_through_stopped();
    atomic_store(&block_discovery, false);
    finish_discovery();
    assert(!candidates);
    atomic_store(&block_discovery, true);
    atomic_store(&discovery_entered, false);
    gui_play_through_track_started("Artist", "Album");
    while (!atomic_load(&discovery_entered)) usleep(1000);
    snprintf(current_path, sizeof(current_path), "/changed/track.flac");
    atomic_store(&block_discovery, false);
    finish_discovery();
    assert(!candidates);
    gui_play_through_track_started("Artist", "Album");
    current_settings.native_play_through_albums = false;
    finish_discovery();
    assert(!candidates);
    /* A queue that ends before discovery completes continues from the timer. */
    current_settings.native_play_through_albums = true;
    atomic_store(&block_discovery, true);
    gui_play_through_track_started("Artist", "Album");
    gui_play_through_queue_exhausted(1);
    assert(launches == 4);
    atomic_store(&block_discovery, false);
    finish_discovery();
    continuation_tick(NULL); continuation_tick(NULL);
    assert(launches == 5);
    gui_play_through_stopped();
    mode = "shuffle";
    gui_play_through_track_started("Artist", "Album"); assert(!candidates);
    clear_candidates();
    const char * files[] = { "a/z.FLAC", "a/A.mp3", "B/Z.mp3", "B/a.flac" };
    for (int i = 0; i < 4; ++i) { snprintf(path, sizeof(path), "%s/%s", root, files[i]); assert(!unlink(path)); }
    assert(!rmdir(a)); assert(!rmdir(b)); assert(!rmdir(empty)); assert(!rmdir(root));
    puts("Play Through state, lexical folder order, album priority, modes, stop and 1001-track batches passed");
    return 0;
}
