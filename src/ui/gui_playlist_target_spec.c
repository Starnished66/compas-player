#include "gui_playlist_target.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static gui_playlist_target_snapshot_t target(void) {
    gui_playlist_target_snapshot_t snapshot;
    gui_playlist_target_copy(&snapshot);
    return snapshot;
}

static void test_song_and_folder_begin(void) {
    gui_playlist_target_clear_folder();
    uint64_t generation = target().generation;
    assert(gui_playlist_target_begin_folder("/music/folder") == generation + 1);
    gui_playlist_target_snapshot_t snapshot = target();
    assert(snapshot.kind == GUI_PLAYLIST_TARGET_FOLDER);
    assert(strcmp(snapshot.folder_path, "/music/folder") == 0);
    assert(snapshot.song_path[0] == '\0');

    assert(gui_playlist_target_begin_song("/music/song.flac") == generation + 2);
    snapshot = target();
    assert(snapshot.kind == GUI_PLAYLIST_TARGET_SONG);
    assert(strcmp(snapshot.song_path, "/music/song.flac") == 0);
    assert(snapshot.folder_path[0] == '\0');
}

static void test_folder_clear(void) {
    uint64_t generation = gui_playlist_target_begin_folder("/music/complete");
    gui_playlist_target_clear_folder();
    gui_playlist_target_snapshot_t snapshot = target();
    assert(snapshot.generation == generation + 1 && snapshot.kind == GUI_PLAYLIST_TARGET_NONE);
}

static void test_song_create_success_transition(void) {
    uint64_t generation = gui_playlist_target_begin_song("/music/song.flac");
    gui_playlist_target_snapshot_t snapshot = target();
    /* Song and standalone create success clear the folder slot and advance
     * the shared generation while preserving any song path. */
    gui_playlist_target_clear_folder();
    snapshot = target();
    assert(snapshot.generation == generation + 1);
    assert(snapshot.kind == GUI_PLAYLIST_TARGET_SONG);
    assert(strcmp(snapshot.song_path, "/music/song.flac") == 0);
}

static void test_standalone(void) {
    uint64_t standalone_generation = gui_playlist_target_begin_standalone();
    gui_playlist_target_snapshot_t snapshot = target();
    assert(snapshot.generation == standalone_generation);
    assert(snapshot.kind == GUI_PLAYLIST_TARGET_NONE);
    assert(snapshot.song_path[0] == '\0' && snapshot.folder_path[0] == '\0');

}

static void test_teardown_folder_only_and_always_bumps(void) {
    uint64_t generation = gui_playlist_target_begin_folder("/music/teardown");
    gui_playlist_target_clear_folder();
    gui_playlist_target_snapshot_t snapshot = target();
    assert(snapshot.generation == generation + 1 && snapshot.kind == GUI_PLAYLIST_TARGET_NONE);
    assert(snapshot.folder_path[0] == '\0');

    generation = gui_playlist_target_begin_song("/music/retained.flac");
    gui_playlist_target_clear_folder();
    snapshot = target();
    assert(snapshot.generation == generation + 1 && snapshot.kind == GUI_PLAYLIST_TARGET_SONG);
    assert(strcmp(snapshot.song_path, "/music/retained.flac") == 0);
    generation = snapshot.generation;
    gui_playlist_target_clear_folder();
    snapshot = target();
    assert(snapshot.generation == generation + 1 && snapshot.kind == GUI_PLAYLIST_TARGET_SONG);
    assert(strcmp(snapshot.song_path, "/music/retained.flac") == 0);
}

int main(void) {
    test_song_and_folder_begin();
    test_folder_clear();
    test_song_create_success_transition();
    test_standalone();
    test_teardown_folder_only_and_always_bumps();
    puts("GUI playlist target spec passed");
    return 0;
}
