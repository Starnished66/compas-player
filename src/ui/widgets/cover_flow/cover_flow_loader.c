#include "cover_flow_loader.h"
#include "../../library/browse/gui_album_cover.h"
#include "../../../core/process/worker_thread.h"
#include "../../../core/ui/display/ui_wake.h"
#include <stdatomic.h>
#include <stdlib.h>

extern void install_thread_crash_altstack(void) __attribute__((weak));

#define LOADER_STACK (512 * 1024)

enum { RUNNING, DONE, ABANDONED };

struct cover_flow_loader {
    cover_flow_loader_result_t result;
    int64_t song_id;
    int face_px, rows;
    atomic_bool cancel;
    atomic_int state;
};

static bool cancelled(void * user) {
    return atomic_load_explicit(&((cover_flow_loader_t *) user)->cancel, memory_order_relaxed);
}

static void release(cover_flow_loader_t * l) {
    free(l->result.pixels);
    free(l);
}

/* Decodes, then grows the square to the caller's height. */
static void * worker_main(void * arg) {
    cover_flow_loader_t * l = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    if (install_thread_crash_altstack) install_thread_crash_altstack();
    uint16_t * px = gui_album_cover_decode(l->song_id, l->face_px, l->result.allow_generate, cancelled, l);
    l->result.pixels = px ? realloc(px, (size_t) l->face_px * (size_t) l->rows * 2) : NULL;
    if (px && !l->result.pixels) free(px);
    if (atomic_exchange(&l->state, DONE) == ABANDONED) release(l);
    else ui_wake_notify();
    return NULL;
}

cover_flow_loader_t * cover_flow_loader_start(int item, int64_t song_id, bool allow_generate, int face_px, int rows) {
    cover_flow_loader_t * l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->result = (cover_flow_loader_result_t) { .item = item, .allow_generate = allow_generate };
    l->song_id = song_id;
    l->face_px = face_px;
    l->rows = rows;
    pthread_t thread;
    if (worker_thread_create(&thread, LOADER_STACK, worker_main, l) != 0) {
        free(l);
        return NULL;
    }
    (void) pthread_detach(thread);
    return l;
}

bool cover_flow_loader_take(cover_flow_loader_t ** l, cover_flow_loader_result_t * out) {
    if (atomic_load(&(*l)->state) != DONE) return false;
    *out = (*l)->result;
    free(*l);
    *l = NULL;
    return true;
}

void cover_flow_loader_drop(cover_flow_loader_t ** l) {
    if (!*l) return;
    atomic_store_explicit(&(*l)->cancel, true, memory_order_relaxed);
    if (atomic_exchange(&(*l)->state, ABANDONED) == DONE) release(*l);
    *l = NULL;
}
