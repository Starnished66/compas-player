#include "src/network/services/subsonic_client.h"
#include "src/library/remote/subsonic/subsonic_cache_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
static const char * response;
bool __wrap_http_request_ex(const http_request_t * req, http_cancel_token_t * cancel, http_response_t * out) {
    (void)cancel; assert(strstr(req->url, "getSong.view") && strstr(req->url, "id=song-id"));
    memset(out, 0, sizeof(*out)); out->status = 200;
    out->body = (uint8_t *)strdup(response); out->body_len = strlen(response); return true;
}
bool __wrap_sd_card_root_is_mounted(void) { return true; }
int main(void) {
    subsonic_server_t server = { .base_url = "https://server.test", .username = "user" };
    response = "{\"subsonic-response\":{\"status\":\"ok\",\"song\":{\"id\":\"song-id\",\"artistId\":\"artist-id\",\"albumId\":\"album-id\",\"artist\":\"Artist\",\"album\":\"Album\"}}}";
    subsonic_song_t song;
    assert(subsonic_get_song(&server, "song-id", &song, NULL));
    assert(!strcmp(song.artist_id, "artist-id") && !strcmp(song.album_id, "album-id"));
    subsonic_cache_entry_t entry = { .synced_at = 1, .rows = 1 };
    entry.hash = subsonic_cache_hash(SUBSONIC_LIST_ALBUM_SONGS, &song, 1);
    assert(subsonic_cache_songs_save(&server, SUBSONIC_LIST_ALBUM_SONGS, "album-id", &song, 1, &entry));
    void * items; int count;
    assert(subsonic_cache_songs_load(&server, SUBSONIC_LIST_ALBUM_SONGS, "album-id", &items, &count, &entry));
    assert(count == 1 && !strcmp(((subsonic_song_t *)items)[0].artist_id, "artist-id")); free(items);
    char root[512], path[640]; assert(subsonic_cache_path(&server, "albums", root, sizeof(root)));
    DIR * dir = opendir(root); assert(dir); struct dirent * de; path[0] = 0;
    while ((de = readdir(dir))) if (strstr(de->d_name, ".tsv")) snprintf(path, sizeof(path), "%s/%s", root, de->d_name);
    closedir(dir); assert(path[0]);
    FILE * f = fopen(path, "r"); assert(f); char header[2048], row[2048];
    assert(fgets(header, sizeof(header), f) && fgets(row, sizeof(row), f)); fclose(f);
    char * tab = strrchr(row, '\t'); assert(tab); *tab = 0;
    tab = strrchr(row, '\t'); assert(tab); strcpy(tab, "\n");
    uint64_t legacy_hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, row, strlen(row));
    snprintf(header, sizeof(header), "#v2\talbum-id\t1\t42:scan\t%016llx\t1\n", (unsigned long long)legacy_hash);
    f = fopen(path, "w"); assert(f); fputs(header, f); fputs(row, f); fclose(f);
    assert(subsonic_cache_songs_load(&server, SUBSONIC_LIST_ALBUM_SONGS, "album-id", &items, &count, &entry));
    assert(count == 1 && !((subsonic_song_t *)items)[0].artist_id[0] && !((subsonic_song_t *)items)[0].album_id[0]);
    assert(entry.hash == legacy_hash && entry.stamp.last_modified == 42);
    assert(subsonic_cache_hash(SUBSONIC_LIST_PLAYLIST_SONGS, items, count) == legacy_hash);
    assert(subsonic_cache_songs_save(&server, SUBSONIC_LIST_PLAYLIST_SONGS, "playlist", items, count, &entry));
    free(items);
    assert(subsonic_cache_songs_load(&server, SUBSONIC_LIST_PLAYLIST_SONGS, "playlist", &items, &count, &entry));
    assert(entry.hash == legacy_hash && entry.stamp.last_modified == 42);
    free(items);
    row[0] = 'X';
    f = fopen(path, "w"); assert(f); fputs(header, f); fputs(row, f); fclose(f);
    assert(subsonic_cache_songs_load(&server, SUBSONIC_LIST_ALBUM_SONGS, "album-id", &items, &count, &entry));
    assert(!entry.hash && !entry.stamp.last_modified); free(items);
    puts("Server song IDs and old/new cache row compatibility passed");
}
