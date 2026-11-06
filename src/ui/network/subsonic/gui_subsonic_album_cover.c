#include "gui_subsonic_album_cover.h"
#include "../../assets/gui_image_dsc.h"
#include "../../shell/gui_shell.h"
#include "../../../core/process/worker_notify.h"
#include "../../../core/process/worker_thread.h"
#include "../../../library/remote/subsonic/subsonic_covers.h"
#include "../../../network/http/client/http_client.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRY_CHECK_MS 2000U /* how often a cover that failed offline looks for Wi-Fi */
#define JOB_STACK (256 * 1024)

/* A job owns its thread and cancel token. Whoever finishes second frees it:
 * the UI when it collects a finished job, the job when the UI let it go. */
typedef struct {
    http_cancel_token_t cancel;
    subsonic_server_t server;
    char id[GUI_SUBSONIC_COVER_ID_MAX];
    int px;
    bool net;   /* Wi-Fi was up when it started */
    bool done;  /* guarded by `lock` */
    uint16_t * pixels;
} job_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static job_t * current; /* guarded by `lock` */

/* UI thread only. */
static subsonic_server_t want_server;
static char want_id[GUI_SUBSONIC_COVER_ID_MAX], ready_id[GUI_SUBSONIC_COVER_ID_MAX], failed_id[GUI_SUBSONIC_COVER_ID_MAX];
static int want_px;
static bool failed_offline;
static uint32_t checked_at; /* when a cover that failed offline last looked for Wi-Fi */
static uint16_t * ready_pixels;
static lv_image_dsc_t ready_dsc;

static void job_free(job_t * j) {
    http_cancel_token_destroy(&j->cancel);
    free(j->pixels);
    free(j);
}

static bool decode_cancelled(void * user) { return http_cancel_token_is_cancelled(&((job_t *) user)->cancel); }

static void job_load(job_t * j) {
    uint8_t * data = NULL;
    size_t n = 0;
    if (!subsonic_cover_large_bytes(&j->server, j->id, j->net, &j->cancel, &data, &n)) return;
    j->pixels = subsonic_cover_decode_large(&j->server, j->id, data, n, j->px, decode_cancelled, j);
}

static void * job_main(void * arg) {
    job_t * j = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    job_load(j);
    pthread_mutex_lock(&lock);
    bool mine = current == j;
    if (mine) j->done = true;
    pthread_mutex_unlock(&lock);
    if (mine) worker_notify_ui();
    else job_free(j);
    return NULL;
}

/* Lets go of the running job without waiting for it. */
static void job_abandon(void) {
    pthread_mutex_lock(&lock);
    job_t * j = current;
    current = NULL;
    if (j && j->done) { pthread_mutex_unlock(&lock); job_free(j); return; }
    if (j) http_cancel_token_cancel(&j->cancel);
    pthread_mutex_unlock(&lock);
}

static void job_start(void) {
    job_t * j = calloc(1, sizeof(*j));
    if (!j) return;
    http_cancel_token_init(&j->cancel);
    j->server = want_server;
    snprintf(j->id, sizeof(j->id), "%s", want_id);
    j->px = want_px;
    j->net = gui_shell_wifi_connected();
    pthread_t thread;
    pthread_mutex_lock(&lock);
    current = j;
    bool ok = worker_thread_create(&thread, JOB_STACK, job_main, j) == 0;
    if (!ok) current = NULL;
    pthread_mutex_unlock(&lock);
    if (ok) pthread_detach(thread);
    else job_free(j);
}

static void drop_ready(void) {
    free(ready_pixels);
    ready_pixels = NULL;
    ready_id[0] = 0;
}

static void install(job_t * j) {
    drop_ready();
    ready_pixels = j->pixels;
    j->pixels = NULL;
    snprintf(ready_id, sizeof(ready_id), "%s", j->id);
    gui_image_dsc_init_rgb565(&ready_dsc, ready_pixels, j->px);
}

/* Takes the finished job, if there is one. */
static job_t * job_take(void) {
    pthread_mutex_lock(&lock);
    job_t * j = current && current->done ? current : NULL;
    if (j) current = NULL;
    pthread_mutex_unlock(&lock);
    return j;
}

static bool job_running(void) {
    pthread_mutex_lock(&lock);
    bool running = current != NULL;
    pthread_mutex_unlock(&lock);
    return running;
}

void gui_subsonic_album_cover_request(const subsonic_server_t * server, const char * cover_id, int px) {
    if (!cover_id || !cover_id[0] || px <= 0) return;
    bool same = !strcmp(cover_id, want_id) && px == want_px;
    if (!same) {
        job_abandon();
        failed_id[0] = 0;
        if (strcmp(cover_id, ready_id)) drop_ready();
    }
    want_server = *server;
    snprintf(want_id, sizeof(want_id), "%s", cover_id);
    want_px = px;
    if (!job_running() && strcmp(want_id, ready_id) && strcmp(want_id, failed_id)) job_start();
}

bool gui_subsonic_album_cover_poll(void) {
    bool ready = false;
    job_t * j = job_take();
    if (j) {
        if (j->pixels && !strcmp(j->id, want_id) && j->px == want_px) { install(j); ready = true; }
        else if (!j->pixels && !strcmp(j->id, want_id)) {
            snprintf(failed_id, sizeof(failed_id), "%s", j->id);
            failed_offline = !j->net || !gui_shell_wifi_connected(); /* offline at the start or now */
            checked_at = lv_tick_get();
        }
        job_free(j);
    }
    if (failed_id[0] && failed_offline && lv_tick_elaps(checked_at) >= RETRY_CHECK_MS) {
        checked_at = lv_tick_get();
        if (gui_shell_wifi_connected()) failed_id[0] = 0; /* Wi-Fi is back: ask again */
    }
    if (want_id[0] && !job_running() && strcmp(want_id, ready_id) && strcmp(want_id, failed_id)) job_start();
    return ready;
}

const lv_image_dsc_t * gui_subsonic_album_cover_get(const char * cover_id) {
    return ready_pixels && cover_id && !strcmp(cover_id, ready_id) ? &ready_dsc : NULL;
}

void gui_subsonic_album_cover_release(void) {
    job_abandon();
    want_id[0] = failed_id[0] = 0;
    drop_ready();
}
