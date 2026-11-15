#include "src/ui/library/library_thumbnails_internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    lv_init();
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache[i].known = true;
        album_thumbnail_cache[i].key_kind = THUMBNAIL_KEY_ALBUM;
        album_thumbnail_cache[i].key = i + 1;
        album_thumbnail_cache[i].last_use = i + 1;
        album_thumbnail_cache[i].pixels = calloc(1, 2);
        assert(album_thumbnail_cache[i].pixels);
    }
    album_thumbnail_cache_entry_t before[ALBUM_THUMBNAIL_CACHE_SIZE];
    memcpy(before, album_thumbnail_cache, sizeof(before));
    uint32_t commits = library_thumbnails_commit_count();
    album_thumbnail_cache_commit(THUMBNAIL_KEY_ALBUM, 999, 999, NULL, false, 0);
    album_thumbnail_cache_commit(THUMBNAIL_KEY_ARTIST_PHOTO, 999, 999, NULL, false, 0);
    album_thumbnail_cache_commit(THUMBNAIL_KEY_ALBUM, 1, 1, NULL, false, 0);
    assert(!memcmp(before, album_thumbnail_cache, sizeof(before)));
    assert(library_thumbnails_commit_count() == commits + 3);
    /* Confirmed absence still takes a slot. */
    album_thumbnail_cache_commit(THUMBNAIL_KEY_ARTIST_PHOTO, 999, 999, NULL, true, 0);
    assert(album_thumbnail_cache_find(THUMBNAIL_KEY_ARTIST_PHOTO, 999));
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) free(album_thumbnail_cache[i].pixels);
    puts("Temporary thumbnail failures preserve the full cache");
}
