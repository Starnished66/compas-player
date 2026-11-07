#include "../library_artwork_internal.h"
#include "../cover_decode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char track[] = "./music/Artist/Album/song.flac";
static bool malformed;
static int decoded;
bool metadata_db_get_song_by_id(int64_t id, song_row_t * row) {
    (void) id; memset(row, 0, sizeof(*row));
    snprintf(row->path, sizeof(row->path), "%s", track);
    snprintf(row->tags.artist, sizeof(row->tags.artist), "Artist");
    return true;
}
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t bytes,
        int w, int h, artwork_priority_t priority, artwork_cancel_fn cancel, void * user, uint16_t ** out) {
    (void) data; (void) bytes; (void) priority; (void) cancel; (void) user;
    decoded++;
    assert(w == ALBUM_THUMBNAIL_PX && h == ALBUM_THUMBNAIL_PX);
    if (malformed) return COVER_DECODE_FAIL_UNSUPPORTED;
    *out = calloc((size_t) w * h, 2); assert(*out);
    return COVER_DECODE_OK;
}
/* Album fallback is forbidden for photo requests. */
int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * artist) {
    (void) kind; (void) artist; assert(!"photo requested album fallback"); return 0;
}
int metadata_db_get_albums_for_group(metadata_db_group_kind_t k, const char * n, int o, int c, group_row_t * r) {
    (void) k; (void) n; (void) o; (void) c; (void) r; abort();
}
int64_t metadata_db_get_album_for_group_offset(metadata_db_group_kind_t k, const char * n, const char * a, const char * aa) {
    (void) k; (void) n; (void) a; (void) aa; abort();
}
void albumart_info_from_song_row(const song_row_t * row, albumart_info_t * info) { (void) row; (void) info; abort(); }
uint64_t song_album_thumbnail_key(const song_row_t * row) { (void) row; abort(); }
time_t album_source_mtime_with_art_mtime(const song_row_t * row, uint32_t mtime) { (void) row; (void) mtime; abort(); }
bool album_thumbnail_load_or_decode_with_mtime(const song_row_t * row, artwork_priority_t p,
        artwork_cancel_fn cb, void * u, time_t t, bool checked, const char * path, bool sc, uint16_t ** pixels) {
    (void) row; (void) p; (void) cb; (void) u; (void) t; (void) checked; (void) path; (void) sc; (void) pixels; abort();
}
static void put(const char * path) { FILE * f = fopen(path, "wb"); assert(f); fputs("image", f); fclose(f); }
int main(void) {
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/Artist", 0700) == 0);
    assert(mkdir("music/Artist/Album", 0700) == 0);
    put("music/Artist/folder.jpg");
    artist_thumbnail_resolution_t result;
    assert(artist_thumbnail_load_photo(1, METADATA_DB_GROUP_ARTIST, "Artist", ARTWORK_PRIO_THUMBNAIL,
        1, NULL, NULL, &result) == ARTIST_THUMB_MISSING);
    assert(result.negative_confirmed && !result.has_alias && !result.pixels && decoded == 0);
    /* Legacy fallback pixels must not satisfy the real-photo namespace. */
    uint16_t pixels[ALBUM_THUMBNAIL_PX * ALBUM_THUMBNAIL_PX] = {0};
    assert(albumart_artist_store_rgb565("Artist", 123, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, pixels));
    char cache[4096];
    assert(!albumart_artist_photo_sized_thumb_fresh("Artist", 123, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, cache, sizeof(cache)));
    put("music/Artist/ARTIST.PNG");
    assert(artist_thumbnail_load_photo(1, METADATA_DB_GROUP_ARTIST, "Artist", ARTWORK_PRIO_THUMBNAIL,
        1, NULL, NULL, &result) == ARTIST_THUMB_FOUND);
    assert(result.pixels && !result.has_alias && result.pixel_key_kind == THUMBNAIL_KEY_ARTIST_PHOTO);
    free(result.pixels);
    assert(decoded == 1);
    assert(albumart_artist_photo_sized_thumb_fresh("Artist", 0, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, cache, sizeof(cache)));
    uint64_t key = albumart_artist_thumbnail_key("Artist");
    albumart_info_t info = {0}; snprintf(info.album, sizeof(info.album), "Album");
    assert(albumart_delete_generated_cache_for_album(&info, &key, 1));
    assert(access(cache, F_OK) != 0);
    malformed = true;
    assert(artist_thumbnail_load_photo(1, METADATA_DB_GROUP_ARTIST, "Artist", ARTWORK_PRIO_THUMBNAIL,
        1, NULL, NULL, &result) == ARTIST_THUMB_MISSING);
    assert(!result.pixels && !result.has_alias && result.negative_confirmed);
    assert(unlink("music/Artist/ARTIST.PNG") == 0);
    put("music/Artist/artist.jpg");
    malformed = false;
    assert(artist_thumbnail_load_photo(1, METADATA_DB_GROUP_ARTIST, "Artist", ARTWORK_PRIO_THUMBNAIL,
        1, NULL, NULL, &result) == ARTIST_THUMB_FOUND);
    free(result.pixels);
    puts("artist photo source/cache tests passed");
}
