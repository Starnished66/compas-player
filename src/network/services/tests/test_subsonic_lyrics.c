#include "src/network/services/subsonic_client.h"
#include "src/library/text/lyrics/lyrics.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static bool extension, unsupported;
static const char * payload;
static int lyrics_calls, probes;
bool __wrap_http_request_ex(const http_request_t * req, http_cancel_token_t * cancel, http_response_t * out) {
    (void)cancel;
    memset(out, 0, sizeof(*out));
    const char * json;
    if (strstr(req->url, "getOpenSubsonicExtensions.view")) {
        ++probes;
        if (unsupported) { out->status = 404; return true; }
        json = extension ? "{\"subsonic-response\":{\"status\":\"ok\",\"openSubsonicExtensions\":[{\"name\":\"songLyrics\",\"versions\":[1]}]}}"
                         : "{\"subsonic-response\":{\"status\":\"ok\",\"openSubsonicExtensions\":[]}}";
    } else {
        ++lyrics_calls;
        assert(req->max_response_bytes <= 2 * LYRICS_MAX_FILE_BYTES);
        if (extension) assert(strstr(req->url, "getLyricsBySongId.view") && strstr(req->url, "id=id%26x"));
        else assert(strstr(req->url, "getLyrics.view") && strstr(req->url, "artist=A%26B") && strstr(req->url, "title=T%20X"));
        json = payload;
    }
    out->status = 200;
    out->body = (uint8_t *)strdup(json);
    out->body_len = strlen(json);
    return true;
}
static char * fetch(subsonic_server_t * server, bool * synced, http_cancel_token_t * token) {
    char * text = NULL;
    assert(subsonic_get_lyrics(server, "id&x", "A&B", "T X", &text, synced, token));
    return text;
}
int main(void) {
    subsonic_server_t server = { .base_url = "https://example.invalid", .username = "user", .password = "pass" };
    bool synced;
    extension = true;
    payload = "{\"subsonic-response\":{\"status\":\"ok\",\"lyricsList\":{\"structuredLyrics\":[{\"synced\":false,\"line\":[{\"value\":\"plain\"}]},{\"synced\":true,\"offset\":-100,\"line\":[{\"start\":50,\"value\":\"First\"},{\"start\":1234,\"value\":\"[offset:99] literal\"}]}]}}}";
    char * text = fetch(&server, &synced, NULL);
    assert(text && synced);
    lyrics_doc_t doc;
    assert(lyrics_parse_buffer(text, strlen(text), &doc));
    assert(doc.count == 2 && doc.lines[0].time_ms == 150 && doc.lines[1].time_ms == 1334);
    assert(strstr(doc.lines[1].text, "[offset:99] literal"));
    lyrics_doc_free(&doc); free(text);
    payload = "{\"subsonic-response\":{\"status\":\"ok\",\"lyricsList\":{\"structuredLyrics\":[{\"synced\":true,\"offset\":100,\"line\":[{\"start\":50,\"value\":\"First\"},{\"start\":1234,\"value\":\"Second\"}]}]}}}";
    text = fetch(&server, &synced, NULL);
    assert(lyrics_parse_buffer(text, strlen(text), &doc));
    assert(doc.lines[0].time_ms == 0 && doc.lines[1].time_ms == 1134);
    assert(probes == 1);
    lyrics_doc_free(&doc); free(text);
    strcpy(server.base_url, "https://classic.invalid");
    extension = false;
    payload = "{\"subsonic-response\":{\"status\":\"ok\",\"lyrics\":{\"value\":\"A plain line\\nAnother line\"}}}";
    text = fetch(&server, &synced, NULL);
    assert(!synced && !strcmp(text, "A plain line\nAnother line")); free(text);
    payload = "{\"subsonic-response\":{\"status\":\"ok\",\"lyrics\":{}}}";
    text = fetch(&server, &synced, NULL); assert(!text);
    assert(probes == 2);
    strcpy(server.base_url, "https://old-server.invalid"); unsupported = true;
    text = fetch(&server, &synced, NULL); assert(!text);
    text = fetch(&server, &synced, NULL); assert(!text);
    assert(probes == 3);
    http_cancel_token_t token; http_cancel_token_init(&token); http_cancel_token_cancel(&token);
    int before = lyrics_calls;
    assert(!subsonic_get_lyrics(&server, "id&x", "A&B", "T X", &text, &synced, &token));
    assert(!text && before == lyrics_calls);
    http_cancel_token_destroy(&token);
    puts("Subsonic lyrics capability selection, parsing, empty result and cancellation passed");
}
