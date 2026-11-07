#include "src/library/tagcache/core/tagcache_internal.h"
#include <assert.h>

static void refresh_add(const char *path) {
    tagcache_upsert_year_changed(path, 107, 1007, "t", "Artist", "Album", "Artist", "g", 8, 1, -1, NULL);
}

/* Old WAV rows must refresh once, even when their recovered tags are identical. */
static void wav_refresh_once(const char *dir) {
    const char *wav = "/m/refresh.WaV";
    const char *mp3 = "/m/refresh.mp3";
    const char *rf64 = "/m/refresh.Rf64";
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    refresh_add(wav);
    refresh_add(mp3);
    refresh_add(rf64);
    /* Simulate a row published by the old parser, using the unchanged layout. */
    {
        SCAN_SCOPE;
        int32_t slot = find_path(wav);
        struct index_entry idx;
        assert(slot >= 0 && reader_index(slot, &idx));
        idx.flag &= ~FLAG_WAV_ID3_TAGS;
        assert(scan_write_index(slot, &idx));
        slot = find_path(rf64);
        assert(slot >= 0 && reader_index(slot, &idx));
        idx.flag &= ~FLAG_WAV_ID3_TAGS;
        assert(scan_write_index(slot, &idx));
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_set_rating(wav, 4);
    assert(tagcache_flush_numeric());
    tagcache_close();
    assert(tagcache_open(dir));
    assert(!tagcache_lookup(wav, 107, 1007, NULL));
    assert(!tagcache_lookup(rf64, 107, 1007, NULL));
    assert(tagcache_lookup(mp3, 107, 1007, NULL));

    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    bool changed = false;
    tagcache_upsert_year_changed(wav, 107, 1007, "t", "Artist", "Album", "Artist", "g", 8, 1, -1, &changed);
    assert(changed);
    tagcache_abort_update();
    assert(!tagcache_lookup(wav, 107, 1007, NULL));
    assert(!tagcache_lookup(rf64, 107, 1007, NULL));
    tagcache_song_t song;
    assert(tagcache_song_by_path(wav, &song) && song.rating == 4);

    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    changed = false;
    tagcache_upsert_year_changed(wav, 107, 1007, "t", "Artist", "Album", "Artist", "g", 8, 1, -1, &changed);
    assert(changed);
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_close();
    assert(tagcache_open(dir));
    assert(tagcache_lookup(wav, 107, 1007, NULL));
    assert(tagcache_lookup(mp3, 107, 1007, NULL));
    assert(tagcache_song_by_path(wav, &song) && song.rating == 4);
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    changed = true;
    tagcache_upsert_year_changed(wav, 107, 1007, "t", "Artist", "Album", "Artist", "g", 8, 1, -1, &changed);
    assert(!changed);
    assert(tagcache_end_update_with_lock(NULL, NULL));
}

int main(void) {
    char dir[] = "/tmp/compas-wav-refresh-XXXXXX";
    assert(mkdtemp(dir));
    assert(tagcache_open(dir));
    wav_refresh_once(dir);
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
    puts("tagcache WAV refresh: ok");
    return 0;
}
