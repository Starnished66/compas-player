#include "src/network/http/stream/http_stream.h"
#include "src/network/http/client/http_conn.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static unsigned char wire[16384];
static size_t wire_len, cursor;
static bool requested;
bool __wrap_http_conn_open_deadline_ex(http_conn_t * conn, const char * host, const char * port,
                                      bool https, bool verify, uint32_t connect_timeout,
                                      uint32_t read_timeout, struct http_cancel_token * cancel,
                                      http_conn_error_t * error, uint64_t deadline) {
    (void)host; (void)port; (void)https; (void)verify; (void)connect_timeout;
    (void)read_timeout; (void)cancel; (void)deadline;
    if (error) *error = HTTP_CONN_OK;
    memset(conn, 0, sizeof(*conn)); conn->net.fd = -1; cursor = 0;
    return true;
}
int __wrap_mbedtls_net_send(void * ctx, const unsigned char * bytes, size_t n) {
    (void)ctx;
    char request[4096]; assert(n < sizeof(request)); memcpy(request, bytes, n); request[n] = 0;
    requested = strstr(request, "Icy-MetaData: 1") != NULL;
    return (int)n;
}
int __wrap_mbedtls_net_recv(void * ctx, unsigned char * bytes, size_t n) {
    (void)ctx;
    if (cursor == wire_len) return 0;
    if (n > 3) n = 3;
    if (n > wire_len - cursor) n = wire_len - cursor;
    memcpy(bytes, wire + cursor, n); cursor += n; return (int)n;
}
static size_t block(unsigned char * out, const char * text) {
    size_t n = (strlen(text) + 15) / 16;
    out[0] = (unsigned char)n;
    memset(out + 1, 0, n * 16); memcpy(out + 1, text, strlen(text));
    return 1 + n * 16;
}
static void response(bool chunked, bool metadata) {
    unsigned char body[1024]; size_t n = 0;
    memcpy(body + n, "ABCD", 4); n += 4;
    if (metadata) n += block(body + n, "StreamTitle='Artist - Title';");
    memcpy(body + n, "EFGH", 4); n += 4;
    if (metadata) body[n++] = 0;
    memcpy(body + n, "IJKL", 4); n += 4;
    if (metadata) n += block(body + n, "StreamTitle='Only title';StreamUrl='http://station.test/a.png';");
    memcpy(body + n, "MNOP", 4); n += 4;
    wire_len = (size_t)sprintf((char *)wire, "ICY 200 OK\r\nContent-Type: audio/mpeg\r\n%s%s\r\n",
                              metadata ? "icy-metaint: 4\r\n" : "",
                              chunked ? "Transfer-Encoding: chunked\r\n" : "");
    if (chunked) {
        for (size_t i = 0; i < n; ++i) {
            memcpy(wire + wire_len, "1\r\n", 3); wire_len += 3;
            wire[wire_len++] = body[i];
            memcpy(wire + wire_len, "\r\n", 2); wire_len += 2;
        }
        memcpy(wire + wire_len, "0\r\n\r\n", 5); wire_len += 5;
    } else { memcpy(wire + wire_len, body, n); wire_len += n; }
}
int main(void) {
    icy_metadata_t state = {0};
    assert(icy_metadata_parse("StreamTitle='Artist - A - B';", &state));
    assert(!strcmp(state.artist, "Artist") && !strcmp(state.title, "A - B"));
    assert(!icy_metadata_parse("StreamTitle='Artist - A - B';", &state));
    assert(icy_metadata_parse("StreamTitle='Don't split this';StreamUrl='https://x/logo.JPG?v=1';", &state));
    assert(!state.artist[0] && !strcmp(state.title, "Don't split this"));
    assert(!strcmp(state.artwork_url, "https://x/logo.JPG?v=1"));
    assert(icy_metadata_parse("StreamUrl='javascript:bad';StreamTitle='broken", &state));
    assert(!state.artwork_url[0]);
    assert(icy_metadata_parse("StreamTitle='';StreamUrl='';", &state));
    assert(!state.title[0] && !state.artist[0] && !state.artwork_url[0]);
    uint64_t previous_id = 0;
    for (int mode = 0; mode < 3; ++mode) {
        response(mode == 1, mode < 2);
        http_stream_t * stream = http_stream_open("http://station.test/live", true, true, 0);
        assert(stream && requested && !http_stream_can_seek(stream));
        unsigned char audio[64]; size_t n = http_stream_read(stream, audio, sizeof(audio));
        assert(n == 16 && !memcmp(audio, "ABCDEFGHIJKLMNOP", 16));
        bool found = false;
        for (int i = 0; i < 100 && !found; ++i) {
            found = http_stream_metadata_for_url("http://station.test/live", &state);
            if (!found) usleep(1000);
        }
        if (mode < 2) {
            assert(found && !strcmp(state.title, "Only title") && !state.artist[0]);
            assert(!strcmp(state.artwork_url, "http://station.test/a.png"));
            assert(state.stream_id != previous_id); previous_id = state.stream_id;
        } else assert(!found);
        http_stream_close(stream);
        assert(!http_stream_metadata_for_url("http://station.test/live", &state));
    }
    wire_len = (size_t)sprintf((char *)wire, "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-3/4\r\nContent-Length: 4\r\nicy-metaint: 0\r\n\r\nABCD");
    http_stream_t * song = http_stream_open("https://server.test/rest/stream.view?id=1", true, false, 0);
    assert(song && !requested && http_stream_can_seek(song) && http_stream_length(song) == 4);
    http_stream_close(song);
    const char * invalid[] = { "no", "4294967296", "-1" };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        wire_len = (size_t)sprintf((char *)wire, "HTTP/1.1 200 OK\r\nicy-metaint: %s\r\n\r\n", invalid[i]);
        assert(!http_stream_open("http://station.test/live", true, true, 0));
    }
    puts("ICY parsing, chunk and read boundaries, audio stripping and stream lifetime passed");
}
