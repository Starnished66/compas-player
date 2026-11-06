#include "player_stream_cover.h"
#include <pthread.h>
#include <stdio.h>
#include "../../../library/remote/subsonic/subsonic_covers.h"
#include "../../../network/http/client/http_client.h"

#define STREAM_COVER_MAX_BYTES (1024u * 1024u)
#define STREAM_COVER_CONNECT_TIMEOUT_MS 5000
#define STREAM_COVER_READ_TIMEOUT_MS 8000
#define STREAM_COVER_TOTAL_TIMEOUT_MS 20000

/* Only one cover fetch runs at a time, so one token is enough. */
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;
static http_cancel_token_t * active_token;

static bool fetch_url(const char * url, bool verify_tls, http_cancel_token_t * cancel, uint8_t ** body, size_t * n) {
    http_request_t request = {0};
    snprintf(request.url, sizeof(request.url), "%s", url);
    request.method = HTTP_METHOD_GET;
    request.verify_tls = verify_tls;
    request.connect_timeout_ms = STREAM_COVER_CONNECT_TIMEOUT_MS;
    request.read_timeout_ms = STREAM_COVER_READ_TIMEOUT_MS;
    request.total_timeout_ms = STREAM_COVER_TOTAL_TIMEOUT_MS;
    request.max_response_bytes = STREAM_COVER_MAX_BYTES;
    request.redirect_limit = 5;
    http_response_t resp;
    bool ok = http_request_ex(&request, cancel, &resp) && resp.status == 200 && resp.body && resp.body_len > 0;
    if (ok) {
        *body = resp.body;
        *n = resp.body_len;
        resp.body = NULL; /* the caller owns it now */
    }
    http_response_free(&resp);
    return ok;
}

bool player_stream_cover_fetch(const cover_decode_request_t * req, bool (*stale)(void *), void * stale_arg,
                               uint8_t ** body, size_t * n) {
    http_cancel_token_t cancel;
    http_cancel_token_init(&cancel);
    pthread_mutex_lock(&active_lock);
    active_token = &cancel;
    pthread_mutex_unlock(&active_lock);
    if (stale(stale_arg)) http_cancel_token_cancel(&cancel); /* superseded before the fetch began */
    bool ok = req->subsonic_cover_id[0]
        ? subsonic_cover_large_bytes(&req->subsonic_server, req->subsonic_cover_id, req->subsonic_allow_net,
                                     &cancel, body, n)
        : fetch_url(req->stream_url, req->stream_verify_tls, &cancel, body, n);
    pthread_mutex_lock(&active_lock);
    active_token = NULL;
    pthread_mutex_unlock(&active_lock);
    http_cancel_token_destroy(&cancel);
    return ok;
}

void player_stream_cover_cancel(void) {
    pthread_mutex_lock(&active_lock);
    http_cancel_token_cancel(active_token); /* NULL-safe */
    pthread_mutex_unlock(&active_lock);
}
