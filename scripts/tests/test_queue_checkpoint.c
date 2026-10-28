#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "src/ui/player/queue/player_queue_model.h"
#include "src/ui/player/queue/player_queue_storage.h"
#include "src/library/playlists/playlist_files.h"
#include "src/library/remote/remote_track.h"
#include "src/library/browser/files/file_ops.h"
#include "src/hardware/usb/usb_mode_control.h"

/* Stubs for the dependencies the queue model and storage never exercise here. */
bool remote_track_path_is_remote(const char * p) { (void) p; return false; }
void file_browser_index_close(file_browser_index_t * i) { (void) i; }
bool file_browser_index_playable_path_at(const file_browser_index_t * i, unsigned n, char * o, size_t s) { (void) i; (void) n; (void) o; (void) s; return false; }
bool file_browser_index_retain(const file_browser_index_t * a, file_browser_index_t ** b) { (void) a; (void) b; return false; }
void metadata_db_snapshot_close(metadata_db_snapshot_t * s) { (void) s; }
bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t * s, int r, char * o, size_t n) { (void) s; (void) r; (void) o; (void) n; return false; }
metadata_db_snapshot_t * metadata_db_snapshot_retain(metadata_db_snapshot_t * s) { return s; }
bool file_op_external_begin(void) { return true; }
void file_op_external_end(void) {}
bool usb_mode_control_storage_write_begin(void) { return true; }
void usb_mode_control_storage_write_end(void) {}
bool playlist_files_write_new_stream_at(int d, const char * n, int c, playlist_files_path_provider p, void * x, char * l, size_t s) { (void) d; (void) n; (void) c; (void) p; (void) x; (void) l; (void) s; return false; }

static void install(int count, int mode) {
    char ** paths = calloc((size_t) count, sizeof(char *));
    for (int i = 0; i < count; i++) { char b[64]; snprintf(b, sizeof b, "/m/%d.flac", i); paths[i] = strdup(b); }
    int * order = NULL;
    if (mode == PLAY_MODE_SHUFFLE) { order = malloc(sizeof(int) * (size_t) count); for (int i = 0; i < count; i++) order[i] = i; }
    player_queue_model_clear_begin();
    player_queue_model_clear_finish();
    player_queue_model_install_take((player_queue_install_t){
        .paths = paths, .count = count, .current = 0, .shuffle_order = order, .shuffle_pos = order ? 0 : -1,
        .next_insert_index = -1, .queue_sd_dirfd = -1 });
}

static uint64_t layout_of(int mode) {
    queue_resume_header_t h; uint64_t l = 0;
    assert(player_queue_model_checkpoint_header(mode, 1.0, &h, &l));
    return l;
}

int main(void) {
    const int mode = PLAY_MODE_SEQUENTIAL;

    /* Header and layout hash are stable for an unchanged queue. */
    install(6, mode);
    uint64_t rev = player_queue_model_revision();
    uint64_t l0 = layout_of(mode);
    assert(layout_of(mode) == l0 && player_queue_model_revision() == rev);

    /* Each mutator bumps the revision on its own, with no arming call. */
    const char * extra = "/m/extra.flac";
    assert(player_queue_model_try_add_many(&extra, 1, mode, NULL));
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    assert(player_queue_model_remove_pending(0));
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    assert(player_queue_model_edit_displayed(rev, 3, 2, mode) == PLAYER_QUEUE_EDIT_OK);
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    int physical = -1;
    assert(player_queue_model_restart_displayed(2, mode, &physical));
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    assert(player_queue_model_delete_current(&physical));
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    assert(player_queue_model_clear_to_current(mode));
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    player_queue_model_clear_begin();
    assert(player_queue_model_revision() > rev); rev = player_queue_model_revision();
    player_queue_model_clear_finish();
    assert(player_queue_model_revision() > rev);

    /* A shuffle-mode toggle changes the layout hash even with the same revision. */
    install(5, PLAY_MODE_SHUFFLE);
    uint64_t shuffled = layout_of(PLAY_MODE_SHUFFLE);
    assert(shuffled != layout_of(mode));

    /* A failed header-only job leaves the fallback flag set for the next
     * flush, even though the worker was joined before the reap. */
    char tmpl[] = "/tmp/compas-qc-XXXXXX";
    assert(mkdtemp(tmpl));
    int dirfd = open(tmpl, O_RDONLY | O_DIRECTORY);
    queue_resume_header_t h = { .count = 3, .current = 0, .position = 1.0 };
    assert(player_queue_storage_checkpoint_start_header(h, dirfd, "queue.bin"));
    player_queue_storage_checkpoint_join();
    assert(!player_queue_storage_checkpoint_failed());
    assert(player_queue_storage_checkpoint_take_header_fallback());
    assert(!player_queue_storage_checkpoint_take_header_fallback());
    char cmd[160]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void) !system(cmd);
    puts("queue checkpoint tests passed");
    return 0;
}
