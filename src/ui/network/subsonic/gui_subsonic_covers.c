#include "gui_subsonic_covers.h"
#include "../../assets/gui_image_dsc.h"
#include "gui_subsonic_artist.h"
#include "gui_subsonic_cover_flow.h"
#include "../../library/library_thumbnails.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_shell.h"
#include "../../widgets/lists/compact_list.h"
#include "../../../core/process/worker_notify.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../../library/remote/subsonic/subsonic_covers.h"
#include "../../../network/http/client/http_client.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COVER_PX BOARD_LIBRARY_THUMB_PX
#define COVER_LRU_SIZE 48
#define COVER_QUEUE_MAX 12
#define COVER_RESULT_MAX 8
#define COVER_MEMO_MAX 512
#define COVER_LISTS_MAX 4
#define COVER_RETRY_MS 30000U /* a failed cover is asked for again after this */
#define COVER_ID_MAX 256 /* the longest id a list holds (255) plus the NUL */

typedef enum { COVER_OK, COVER_NO_ART, COVER_FAILED } cover_status_t;
typedef struct { uint64_t key; uint32_t gen; bool net; char id[COVER_ID_MAX]; } cover_request_t; /* net: Wi-Fi was up when asked */
typedef struct { uint64_t key; uint32_t gen; cover_status_t status; uint16_t * pixels; } cover_result_t;

/* Shared with the worker; everything below is guarded by `lock`. The queue is a
 * stack: the newest request (what the reader is looking at) is served first. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
/* A worker owns its thread and cancel token. Stopping one only unlinks it from
 * `current` and cancels it: it notices, drops its work, frees itself and exits,
 * so the UI never waits for a download. Only `current` may publish results. */
typedef struct { pthread_t thread; http_cancel_token_t cancel; } cover_worker_t;
static cover_worker_t * current;
static int retiring; /* retired workers still running: at most one thread is ever alive */
static subsonic_server_t server;
static cover_request_t queue[COVER_QUEUE_MAX];
static int queue_n;
static uint64_t in_flight;
static uint32_t in_flight_gen;
static cover_result_t results[COVER_RESULT_MAX];
static int result_n;
static uint32_t generation; /* bumped by reset: older requests and results are dropped */

/* UI thread only. */
typedef struct { uint64_t key; uint16_t * pixels; lv_image_dsc_t dsc; uint32_t used; } cover_entry_t;
static cover_entry_t lru[COVER_LRU_SIZE];
static uint32_t use_clock;
static struct { uint64_t key; bool no_art; uint32_t at; } memo[COVER_MEMO_MAX]; /* key 0 = empty */
static int memo_next;
static lv_obj_t * lists[COVER_LISTS_MAX];
static char server_key[SUBSONIC_CACHE_KEY_SIZE];
static bool released; /* decoded covers were freed: no requests until the screen is back */

/* ---- Worker ---- */

static cover_status_t cover_load(const subsonic_server_t * srv, const cover_request_t * req,
                                 http_cancel_token_t * cancel, uint16_t ** pixels) {
    if (subsonic_cover_thumb_load(srv, req->id, COVER_PX, pixels)) return COVER_OK;
    if (!req->net) return COVER_FAILED; /* retried once Wi-Fi is back */
    bool no_art = false;
    if (subsonic_cover_thumb_fetch(srv, req->id, COVER_PX, cancel, pixels, &no_art)) return COVER_OK;
    return no_art ? COVER_NO_ART : COVER_FAILED;
}

/* Called with the lock held; it is released around the slow work. */
static void worker_serve(cover_worker_t * me, cover_request_t req) {
    in_flight = req.key;
    in_flight_gen = req.gen;
    subsonic_server_t srv = server;
    pthread_mutex_unlock(&lock);
    cover_result_t result = { .key = req.key, .gen = req.gen };
    result.status = cover_load(&srv, &req, &me->cancel, &result.pixels);
    pthread_mutex_lock(&lock);
    if (current != me || req.gen != generation) {
        if (current == me) in_flight = 0;
        free(result.pixels);
        return;
    }
    in_flight = 0;
    results[result_n++] = result;
    pthread_mutex_unlock(&lock);
    worker_notify_ui();
    pthread_mutex_lock(&lock);
}

static bool worker_start_locked(void);

static void * worker_main(void * arg) {
    cover_worker_t * me = arg;
    pthread_mutex_lock(&lock);
    while (current == me) {
        if (queue_n == 0 || result_n == COVER_RESULT_MAX) {
            pthread_cond_wait(&wake, &lock);
            continue;
        }
        cover_request_t req = queue[--queue_n];
        if (req.gen == generation) worker_serve(me, req);
    }
    retiring--;
    if (!current && queue_n > 0) (void) worker_start_locked(); /* requests that waited for this exit */
    pthread_mutex_unlock(&lock);
    http_cancel_token_destroy(&me->cancel);
    free(me);
    return NULL;
}

static bool worker_start_locked(void) {
    if (current) return true;
    cover_worker_t * w = calloc(1, sizeof(*w));
    if (!w) return false;
    http_cancel_token_init(&w->cancel);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        http_cancel_token_destroy(&w->cancel);
        free(w);
        return false;
    }
    pthread_detach(w->thread);
    current = w;
    return true;
}

/* Never blocks: the old worker finishes or aborts on its own. */
static void worker_stop(void) {
    pthread_mutex_lock(&lock);
    cover_worker_t * old = current;
    current = NULL;
    queue_n = 0;
    in_flight = 0;
    for (int i = 0; i < result_n; i++) free(results[i].pixels);
    result_n = 0;
    if (old) {
        retiring++;
        http_cancel_token_cancel(&old->cancel);
    }
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
}

static bool request_pending_locked(uint64_t key) {
    /* Work from before a reset is dropped when it ends: it must not stand in for a new request. */
    if (key == in_flight && in_flight_gen == generation) return true;
    for (int i = 0; i < queue_n; i++) if (queue[i].key == key) return true;
    for (int i = 0; i < result_n; i++) if (results[i].key == key && results[i].gen == generation) return true;
    return false;
}

static void cover_request(uint64_t key, const char * id) {
    bool net = gui_shell_wifi_connected(); /* UI thread: the worker never asks the radio */
    if (strlen(id) >= COVER_ID_MAX) return; /* never truncated: the id must match its key */
    pthread_mutex_lock(&lock);
    if (!request_pending_locked(key)) {
        if (queue_n == COVER_QUEUE_MAX) { /* drop the oldest: it is the furthest from the reader */
            memmove(queue, queue + 1, sizeof(queue[0]) * (COVER_QUEUE_MAX - 1));
            queue_n--;
        }
        cover_request_t * req = &queue[queue_n++];
        req->key = key;
        req->gen = generation;
        req->net = net;
        snprintf(req->id, sizeof(req->id), "%s", id);
        if (!current && retiring == 0) (void) worker_start_locked();
        pthread_cond_broadcast(&wake);
    }
    pthread_mutex_unlock(&lock);
}

/* ---- Session memo: covers not asked for again ---- */

/* A failure stops counting after COVER_RETRY_MS; no_art lasts the session. */
static bool memo_has(uint64_t key) {
    for (int i = 0; i < COVER_MEMO_MAX; i++) {
        if (memo[i].key != key) continue;
        if (memo[i].no_art || lv_tick_elaps(memo[i].at) < COVER_RETRY_MS) return true;
        memo[i].key = 0;
    }
    return false;
}

static void memo_add(uint64_t key, bool no_art) {
    memo[memo_next].key = key;
    memo[memo_next].no_art = no_art;
    memo[memo_next].at = lv_tick_get();
    memo_next = (memo_next + 1) % COVER_MEMO_MAX;
}

static bool memo_has_failures(void) {
    for (int i = 0; i < COVER_MEMO_MAX; i++) if (memo[i].key && !memo[i].no_art) return true;
    return false;
}

static void memo_forget_failures(void) {
    for (int i = 0; i < COVER_MEMO_MAX; i++) if (!memo[i].no_art) memo[i].key = 0;
}

/* ---- Decoded covers ---- */

static cover_entry_t * lru_find(uint64_t key) {
    for (int i = 0; i < COVER_LRU_SIZE; i++) if (lru[i].pixels && lru[i].key == key) return &lru[i];
    return NULL;
}

static bool list_on_screen(lv_obj_t * list) {
    return list && compact_list_is_live(list) && lv_obj_get_screen(list) == lv_screen_active();
}

static void lists_refresh_visible(void) {
    for (int i = 0; i < COVER_LISTS_MAX; i++) if (list_on_screen(lists[i])) compact_list_refresh_decorations(lists[i]);
}

/* Frees one entry after every row stopped showing it. True if a row had it. */
static bool lru_evict(cover_entry_t * e) {
    uint16_t * pixels = e->pixels;
    bool lost = false;
    if (!pixels) return false;
    e->pixels = NULL;
    e->dsc.data = NULL; /* cleared first, so a redecorate cannot attach it again */
    for (int i = 0; i < COVER_LISTS_MAX; i++)
        if (lists[i] && compact_list_is_live(lists[i]) && compact_list_detach_image_src(lists[i], &e->dsc)) lost = true;
    free(pixels);
    memset(e, 0, sizeof(*e));
    return lost;
}

static cover_entry_t * lru_victim(void) {
    cover_entry_t * victim = &lru[0];
    for (int i = 0; i < COVER_LRU_SIZE; i++) {
        if (!lru[i].pixels) return &lru[i];
        if ((int32_t) (lru[i].used - victim->used) < 0) victim = &lru[i];
    }
    return victim;
}

static void lru_fill(cover_entry_t * e, uint64_t key, uint16_t * pixels) {
    e->key = key;
    e->pixels = pixels;
    e->used = ++use_clock;
    gui_image_dsc_init_rgb565(&e->dsc, pixels, COVER_PX);
}

static bool match_key(uint64_t artwork_key, void * ctx) { return artwork_key == *(const uint64_t *) ctx; }

static void lists_refresh_key(uint64_t key) {
    for (int i = 0; i < COVER_LISTS_MAX; i++)
        if (lists[i] && compact_list_is_live(lists[i])) compact_list_refresh_decorations_for(lists[i], match_key, &key);
}

static void apply_result(const cover_result_t * r) {
    if (r->gen != generation) {
        free(r->pixels);
        return;
    }
    if (r->status != COVER_OK) {
        memo_add(r->key, r->status == COVER_NO_ART);
        return;
    }
    if (lru_find(r->key)) {
        free(r->pixels);
        return;
    }
    cover_entry_t * slot = lru_victim();
    bool lost = lru_evict(slot);
    lru_fill(slot, r->key, r->pixels);
    if (lost) lists_refresh_visible();
    lists_refresh_key(r->key);
}

/* ---- Rows ---- */

static bool row_in_view(lv_obj_t * list, int index) {
    int32_t top, bottom, viewport, scroll_y = lv_obj_get_scroll_y(list);
    if (!compact_list_row_span(list, index, &top, &bottom, &viewport) || viewport <= 0) return true;
    return bottom >= scroll_y && top <= scroll_y + viewport;
}

static void cover_row_decorator(lv_obj_t * list, lv_obj_t * row, lv_obj_t * image, int index, int pool_slot,
                                int64_t identity, uint64_t artwork_key, const char * artwork_name, void * ctx) {
    (void) pool_slot; (void) identity; (void) ctx;
    cover_entry_t * e = artwork_key ? lru_find(artwork_key) : NULL;
    library_thumbnails_show_row_image(list, row, image, e ? &e->dsc : NULL);
    if (e) e->used = ++use_clock;
    else if (artwork_key && artwork_name && artwork_name[0] && !released && !memo_has(artwork_key) && row_in_view(list, index))
        cover_request(artwork_key, artwork_name);
}

/* ---- Public ---- */

/* Like the library lists: a scroll drops what is queued (those rows may be gone) and,
 * once it stops, the rows now in view are decorated again so they ask for their covers.
 * Rows are recycled while the list still moves, so they cannot be judged "in view" before. */
static void cover_scroll_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN) {
        pthread_mutex_lock(&lock);
        generation++;
        queue_n = 0;
        pthread_mutex_unlock(&lock);
    } else if (lv_event_get_code(e) == LV_EVENT_SCROLL_END && compact_list_is_live(list)) {
        compact_list_refresh_decorations(list);
    }
}

void gui_subsonic_covers_bind(lv_obj_t * list) {
    int free_slot = -1;
    for (int i = 0; i < COVER_LISTS_MAX; i++) {
        if (lists[i] == list) free_slot = i;
        else if (free_slot < 0 && (!lists[i] || !compact_list_is_live(lists[i]))) free_slot = i;
    }
    if (free_slot < 0) return;
    released = false;
    lists[free_slot] = list;
    compact_list_set_row_decorator(list, cover_row_decorator, NULL);
    lv_obj_remove_event_cb(list, cover_scroll_cb); /* bind can run again for a list */
    lv_obj_add_event_cb(list, cover_scroll_cb, LV_EVENT_ALL, NULL);
}

void gui_subsonic_covers_reset(void) {
    pthread_mutex_lock(&lock);
    generation++;
    queue_n = 0;
    pthread_mutex_unlock(&lock);
    memo_forget_failures();
    if (!released) lists_refresh_visible(); /* the generation bump dropped every list's requests, not just the changed one */
}

void gui_subsonic_covers_set_server(const subsonic_server_t * s) {
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_cache_server_key(s, key);
    pthread_mutex_lock(&lock);
    server = *s; /* credentials may change under the same key */
    pthread_mutex_unlock(&lock);
    if (strcmp(key, server_key) == 0) return;
    memcpy(server_key, key, sizeof(server_key));
    gui_subsonic_covers_reset();
}

uint64_t gui_subsonic_covers_key(const char * cover_id) {
    return cover_id && cover_id[0] && server_key[0] ? subsonic_cover_key(server_key, cover_id) : 0;
}

void gui_subsonic_covers_shutdown(void) {
    worker_stop();
    for (int i = 0; i < COVER_LRU_SIZE; i++) lru_evict(&lru[i]);
    memo_forget_failures();
    released = true;
    lists_refresh_visible(); /* placeholders only; covers are asked for again at wake */
}

void gui_subsonic_covers_wake(void) {
    if (!released) return;
    released = false;
    lists_refresh_visible();
}

void poll_subsonic_covers(void) {
    if (gui_navigation_transition_in_progress()) return; /* a slide shows a frozen snapshot */
    static bool was_connected;
    bool connected = gui_shell_wifi_connected();
    if (connected && !was_connected) { /* a cover that could not be fetched may be now */
        memo_forget_failures();
        lists_refresh_visible();
    }
    was_connected = connected;
    gui_subsonic_cover_flow_poll(connected);
    gui_subsonic_artist_poll();
    static uint32_t retried_at;
    if (lv_tick_elaps(retried_at) >= COVER_RETRY_MS) { /* failed covers on a still screen get their second try */
        retried_at = lv_tick_get();
        if (!released && memo_has_failures()) lists_refresh_visible();
    }
    cover_result_t done[COVER_RESULT_MAX];
    pthread_mutex_lock(&lock);
    if (!current && !retiring && queue_n > 0) (void) worker_start_locked(); /* an earlier start failed: try once per poll */
    int n = result_n;
    memcpy(done, results, sizeof(done[0]) * (size_t) n);
    result_n = 0;
    if (n) pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++) {
        apply_result(&done[i]);
    }
}

bool gui_subsonic_covers_copy(const char * id, uint16_t * pixels, bool * no_art) {
    uint64_t key = gui_subsonic_covers_key(id);
    *no_art = !key;
    cover_entry_t * e = key ? lru_find(key) : NULL;
    if (e) {
        memcpy(pixels, e->pixels, COVER_PX * COVER_PX * sizeof(*pixels));
        e->used = ++use_clock;
        return true;
    }
    if (key && !released && !memo_has(key)) cover_request(key, id);
    return false;
}
