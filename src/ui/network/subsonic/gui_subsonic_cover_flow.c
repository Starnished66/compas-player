#include "gui_subsonic_cover_flow.h"
#include "../../shell/gui_shell.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/cover_flow/cover_flow.h"
#include "../../../library/remote/subsonic/subsonic_covers.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/* The carousel's own copy of the server, written on the UI thread and read by
 * the face loader threads, so no Subsonic state is shared with them. */
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static subsonic_server_t face_server;
static atomic_bool net_ok; /* Wi-Fi was connected when the carousel was opened */
static gui_subsonic_cover_flow_open_album_fn open_album_fn;

/* Downloads in flight, so closing the carousel can cut them. A token is listed
 * only between its init and its destroy, both under server_lock. */
#define MAX_LIVE_DOWNLOADS 4
static http_cancel_token_t * live[MAX_LIVE_DOWNLOADS];

static void live_add(http_cancel_token_t * tok) {
    pthread_mutex_lock(&server_lock);
    for (int i = 0; i < MAX_LIVE_DOWNLOADS; i++) {
        if (live[i]) continue;
        live[i] = tok;
        break;
    }
    pthread_mutex_unlock(&server_lock);
}

static void live_remove(http_cancel_token_t * tok) {
    pthread_mutex_lock(&server_lock);
    for (int i = 0; i < MAX_LIVE_DOWNLOADS; i++)
        if (live[i] == tok) live[i] = NULL;
    pthread_mutex_unlock(&server_lock);
}

void gui_subsonic_cover_flow_poll(bool wifi_connected) {
    static bool was_connected;
    atomic_store(&net_ok, wifi_connected);
    if (wifi_connected && !was_connected) cover_flow_retry_faces(); /* misses that had no network */
    was_connected = wifi_connected;
    if (wifi_connected && cover_flow_item(0)) return;
    pthread_mutex_lock(&server_lock); /* the carousel is gone, or the network */
    for (int i = 0; i < MAX_LIVE_DOWNLOADS; i++)
        if (live[i]) http_cancel_token_cancel(live[i]);
    pthread_mutex_unlock(&server_lock);
}

static void on_select(int index) {
    const cover_flow_item_t * item = cover_flow_item(index);
    if (item && open_album_fn) open_album_fn(item->ref, item->title);
}

/* Loader thread. The file on the card first; the network only for the centre cover. */
static uint16_t * decode_face(const char * cover_ref, int px, bool allow_net, bool (*cancelled)(void *), void * user) {
    subsonic_server_t server;
    pthread_mutex_lock(&server_lock);
    server = face_server;
    pthread_mutex_unlock(&server_lock);
    http_cancel_token_t cancel;
    http_cancel_token_init(&cancel);
    live_add(&cancel);
    uint8_t * bytes = NULL;
    size_t n = 0;
    bool got = (!cancelled || !cancelled(user)) &&
               subsonic_cover_large_bytes(&server, cover_ref, allow_net && atomic_load(&net_ok), &cancel, &bytes, &n);
    live_remove(&cancel);
    http_cancel_token_destroy(&cancel);
    if (!got) return NULL;
    return subsonic_cover_decode_large(&server, cover_ref, bytes, n, px, cancelled, user);
}

void gui_subsonic_cover_flow_open(const subsonic_server_t * server, const subsonic_album_t * albums, int count,
                                  gui_subsonic_cover_flow_open_album_fn open_album) {
    if (count > COVER_FLOW_MAX_ITEMS) count = COVER_FLOW_MAX_ITEMS;
    cover_flow_item_t * items = count > 0 ? calloc((size_t) count, sizeof(*items)) : NULL;
    if (!items) return;
    for (int i = 0; i < count; i++)
        items[i] = (cover_flow_item_t) { .title = albums[i].name, .subtitle = albums[i].artist,
                                         .ref = albums[i].id, .cover_ref = albums[i].cover_art };
    pthread_mutex_lock(&server_lock);
    face_server = *server;
    pthread_mutex_unlock(&server_lock);
    atomic_store(&net_ok, gui_shell_wifi_connected());
    open_album_fn = open_album;
    cover_flow_params_t params = { .title = TR("Cover Flow"), .items = items, .count = count, .on_select = on_select,
                                   .az_index = true, .face = decode_face };
    (void) cover_flow_show(&params);
    free(items);
}
