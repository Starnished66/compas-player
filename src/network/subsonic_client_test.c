#include "subsonic_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char album_response[] =
    "{\"subsonic-response\":{\"status\":\"ok\",\"album\":{\"artist\":\"Compilation\","
    "\"albumArtist\":\"Various Artists\",\"song\":["
    "{\"id\":\"one\",\"title\":\"First\",\"artist\":\"Singer One\",\"albumArtist\":\"\",\"album\":\"Mix\"},"
    "{\"id\":\"two\",\"title\":\"Second\",\"artist\":\"Singer Two\",\"album\":\"Mix\"}]}}}";
static const char playlist_response[] =
    "{\"subsonic-response\":{\"status\":\"ok\",\"playlist\":{\"entry\":["
    "{\"id\":\"three\",\"title\":\"Third\",\"artist\":\"Singer Three\",\"albumArtist\":\"Band\"},"
    "{\"id\":\"four\",\"title\":\"Fourth\",\"artist\":\"Singer Four\",\"albumArtist\":\"\"}]}}}";
static const char * response_override;

bool http_request_ex(const http_request_t * req, http_cancel_token_t * cancel, http_response_t * resp) {
    (void) cancel;
    const char * body = response_override ? response_override :
                       strstr(req->url, "getAlbum.view") ? album_response : playlist_response;
    resp->status = 200;
    resp->body_len = strlen(body);
    resp->body = malloc(resp->body_len);
    assert(resp->body);
    memcpy(resp->body, body, resp->body_len);
    resp->error = NULL;
    return true;
}

void http_response_free(http_response_t * resp) {
    free(resp->body);
    resp->body = NULL;
}

static void test_album_artists(void) {
    subsonic_server_t server = { .base_url = "https://example.test", .username = "u", .password = "p" };
    subsonic_song_t * songs = NULL;
    int count = 0;
    assert(subsonic_get_album_songs(&server, "album", &songs, &count, NULL));
    assert(count == 2);
    assert(strcmp(songs[0].artist, "Singer One") == 0);
    assert(strcmp(songs[0].album_artist, "Various Artists") == 0);
    assert(strcmp(songs[1].artist, "Singer Two") == 0);
    assert(strcmp(songs[1].album_artist, "Various Artists") == 0);
    free(songs);

    response_override = "{\"subsonic-response\":{\"status\":\"ok\",\"album\":{\"artist\":\"Album Artist\","
                        "\"albumArtist\":\"\",\"song\":[{\"artist\":\"Guest Singer\"}]}}}";
    assert(subsonic_get_album_songs(&server, "album", &songs, &count, NULL));
    assert(count == 1 && strcmp(songs[0].album_artist, "Album Artist") == 0);
    assert(strcmp(songs[0].artist, "Guest Singer") == 0);
    free(songs);
    response_override = "{\"subsonic-response\":{\"status\":\"ok\",\"album\":{"
                        "\"song\":[{\"artist\":\"Only Artist\"}]}}}";
    assert(subsonic_get_album_songs(&server, "album", &songs, &count, NULL));
    assert(count == 1 && strcmp(songs[0].album_artist, "Only Artist") == 0);
    free(songs);
    response_override = NULL;

    songs = NULL;
    assert(subsonic_get_playlist_songs(&server, "playlist", &songs, &count, NULL));
    assert(count == 2);
    assert(strcmp(songs[0].artist, "Singer Three") == 0);
    assert(strcmp(songs[0].album_artist, "Band") == 0);
    assert(strcmp(songs[1].artist, "Singer Four") == 0);
    assert(strcmp(songs[1].album_artist, "Singer Four") == 0);
    free(songs);
}

static void test_stream_urls(void) {
    subsonic_server_t server = { .base_url = "https://example.test", .username = "u", .password = "p" };
    char url[1536];
    subsonic_build_stream_url(&server, "id with/slash", url, sizeof(url));
    assert(strstr(url, "id=id%20with%2Fslash&format=raw"));
    assert(!strstr(url, "maxBitRate"));
    const int presets[] = {96, 192, 320};
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        char expected[80];
        snprintf(expected, sizeof(expected), "id=id%%20with%%2Fslash&format=mp3&maxBitRate=%d", presets[i]);
        subsonic_build_stream_url_quality(&server, "id with/slash", presets[i], url, sizeof(url));
        assert(strstr(url, expected));
        assert(!strstr(url, "format=raw"));
    }
    subsonic_build_stream_url_quality(&server, "id", 0, url, sizeof(url));
    assert(strstr(url, "&id=id&format=raw"));
}

int main(void) {
    test_album_artists();
    test_stream_urls();
    puts("Subsonic album artists and stream quality: PASS");
    return 0;
}
