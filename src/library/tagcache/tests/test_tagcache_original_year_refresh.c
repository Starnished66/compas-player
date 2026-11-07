#include "src/library/tagcache/core/tagcache_internal.h"
#include <assert.h>

static const char *const paths[] = {
    "/m/a.MP3", "/m/a.aac", "/m/a.wav", "/m/a.rf64", "/m/a.w64",
    "/m/a.flac", "/m/a.ogg", "/m/a.oga", "/m/a.opus", "/m/a.wv",
    "/m/a.caf", "/m/a.alac", "/m/a.aiff", "/m/a.aif", "/m/a.aifc",
    "/m/a.dsf", "/m/a.dff", "/m/a.flac#compas-cue=1", "/m/a.cue#compas-cue=1",
    "/m/a.m4a", "/m/a.mp4", "/m/a.m4b", "/m/a.ape", "/m/a.wma", "/m/a.iso"
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static void add(const char *path, bool *changed) {
    tagcache_upsert_year_changed(path, 107, 1007, "t", "Artist", "Album", "Artist", "g", 8, 1, 1990, changed);
}
static void check(bool migrated) {
    for (size_t i = 0; i < COUNT(paths); i++)
        assert(tagcache_lookup(paths[i], 107, 1007, NULL) == migrated);
}
static void refresh(const char *dir, bool targeted) {
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    for (size_t i = 0; i < COUNT(paths); i++) add(paths[i], NULL);
    {
        SCAN_SCOPE;
        for (size_t i = 0; i < COUNT(paths); i++) {
            int32_t slot = find_path(paths[i]);
            struct index_entry idx;
            assert(slot >= 0 && reader_index(slot, &idx));
            assert(idx.flag & FLAG_ORIGINAL_YEAR_TAGS);
            idx.flag &= ~FLAG_ORIGINAL_YEAR_TAGS;
            assert(scan_write_index(slot, &idx));
        }
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_set_rating(paths[0], 4);
    assert(tagcache_flush_numeric());
    tagcache_close();
    assert(tagcache_open(dir));
    check(false);
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    bool changed = false;
    add(paths[0], &changed);
    assert(changed);
    tagcache_abort_update();
    check(false);
    if (targeted) tagcache_begin_targeted_update_with_lock(NULL, NULL);
    else tagcache_begin_update_with_lock(NULL, NULL);
    for (size_t i = 0; i < COUNT(paths); i++) {
        changed = false;
        add(paths[i], &changed);
        assert(changed); /* Migration publishes even with an identical year. */
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_close();
    assert(tagcache_open(dir));
    check(true);
    tagcache_song_t song;
    assert(tagcache_song_by_path(paths[0], &song) && song.rating == 4);
    tagcache_begin_targeted_update_with_lock(NULL, NULL);
    for (size_t i = 0; i < COUNT(paths); i++) {
        changed = true;
        add(paths[i], &changed);
        assert(!changed);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
}
int main(void) {
    char dir[] = "/tmp/compas-original-year-refresh-XXXXXX";
    assert(mkdtemp(dir));
    assert(tagcache_open(dir));
    refresh(dir, false);
    refresh(dir, true);
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
    puts("tagcache original-year refresh: ok");
    return 0;
}
