/* Standalone regression for gui_library.c's bounded row-selection snapshots. */
#include "../browse/gui_library.c"
#include "../library_selection_snapshot.c"

#include <assert.h>

static int32_t test_generation = 7;
static bool generation_busy;
static unsigned generation_reads, seed_calls;
static char seeded_path[sizeof(((song_row_t *) 0)->path)];
static track_metadata_t seeded_metadata;

bool metadata_db_try_get_generation(int32_t *out_generation) {
    ++generation_reads;
    if (generation_busy || !out_generation) return false;
    *out_generation = test_generation;
    return true;
}

void gui_player_seed_selection_song(const char *path, const track_metadata_t *metadata) {
    ++seed_calls;
    seeded_path[0] = '\0';
    memset(&seeded_metadata, 0, sizeof(seeded_metadata));
    if (!path || !metadata) return;
    snprintf(seeded_path, sizeof(seeded_path), "%s", path);
    seeded_metadata = *metadata;
}

static song_row_t row(int64_t id, const char *path, const char *title) {
    song_row_t out = { .id = id };
    snprintf(out.path, sizeof(out.path), "%s", path);
    snprintf(out.tags.title, sizeof(out.tags.title), "%s", title);
    snprintf(out.tags.artist, sizeof(out.tags.artist), "Artist %lld", (long long)id);
    out.tags.track_number = (int32_t)id;
    return out;
}

static void reset_cache(void) {
    pthread_mutex_lock(&selection_snapshot_mutex);
    memset(selection_snapshots, 0, sizeof(selection_snapshots));
    selection_snapshot_age = 0;
    pthread_mutex_unlock(&selection_snapshot_mutex);
    test_generation = 7;
    generation_busy = false;
    generation_reads = seed_calls = 0;
    seeded_path[0] = '\0';
    memset(&seeded_metadata, 0, sizeof(seeded_metadata));
}

static void test_rank_source_and_owned_copy(void) {
    reset_cache();
    song_row_t visible = row(42, "/music/original.mp3", "Original tag title");
    library_selection_snapshot_store(&visible, SELECTION_SNAPSHOT_ALL_SONGS, 11, test_generation);
    library_selection_snapshot_store(&visible, SELECTION_SNAPSHOT_RECENTLY_ADDED, 3, test_generation);
    snprintf(visible.tags.title, sizeof(visible.tags.title), "mutated after row build");

    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, 11, 0, NULL);
    assert(strcmp(seeded_path, "/music/original.mp3") == 0);
    assert(strcmp(seeded_metadata.title, "Original tag title") == 0);
    assert(seeded_metadata.has_title && seeded_metadata.has_artist);
    assert(seeded_metadata.has_track_number && seeded_metadata.track_number == 42);

    library_selection_snapshot_seed(SELECTION_SNAPSHOT_RECENTLY_ADDED, 3, 0, NULL);
    assert(strcmp(seeded_path, "/music/original.mp3") == 0);
    assert(strcmp(seeded_metadata.title, "Original tag title") == 0);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, 3, 0, NULL);
    assert(seeded_path[0] == '\0'); /* Same row cannot leak across rank/source. */
}

static void test_group_identity_generation_and_misses(void) {
    reset_cache();
    song_row_t first = row(51, "/music/first.flac", "First");
    song_row_t second = row(52, "/music/second.flac", "Second");
    library_selection_snapshot_store(&first, SELECTION_SNAPSHOT_GROUP, -1, test_generation);
    library_selection_snapshot_store(&second, SELECTION_SNAPSHOT_GROUP, -1, test_generation);

    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, 52, second.path);
    assert(strcmp(seeded_path, second.path) == 0 && strcmp(seeded_metadata.title, "Second") == 0);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, 51, second.path);
    assert(seeded_path[0] == '\0'); /* Both ID and path must identify the row. */

    pthread_mutex_lock(&selection_snapshot_mutex);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, 52, second.path);
    pthread_mutex_unlock(&selection_snapshot_mutex);
    assert(seeded_path[0] == '\0'); /* Cache contention never blocks the UI. */

    test_generation++;
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, 52, second.path);
    assert(seeded_path[0] == '\0'); /* A rescan invalidates every prior snapshot. */
    assert(generation_reads == 4);

    generation_busy = true;
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, 51, first.path);
    assert(seeded_path[0] == '\0'); /* Busy DB clears immediately. */
    assert(seed_calls == 5);
}

static void test_bounded_lru_eviction(void) {
    reset_cache();
    song_row_t rows[SELECTION_SNAPSHOT_CAPACITY + 1];
    for (int i = 0; i < SELECTION_SNAPSHOT_CAPACITY; ++i) {
        char path[64], title[64];
        snprintf(path, sizeof(path), "/music/evict-%02d.mp3", i);
        snprintf(title, sizeof(title), "Title %02d", i);
        rows[i] = row(100 + i, path, title);
        library_selection_snapshot_store(&rows[i], SELECTION_SNAPSHOT_ALL_SONGS, i, test_generation);
    }
    /* Refresh rank zero; the next insertion must evict rank one. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, 0, 0, NULL);
    rows[SELECTION_SNAPSHOT_CAPACITY] = row(999, "/music/new.mp3", "Newest");
    library_selection_snapshot_store(&rows[SELECTION_SNAPSHOT_CAPACITY],
                             SELECTION_SNAPSHOT_ALL_SONGS,
                             SELECTION_SNAPSHOT_CAPACITY, test_generation);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, 0, 0, NULL);
    assert(strcmp(seeded_metadata.title, "Title 00") == 0);
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, 1, 0, NULL);
    assert(seeded_path[0] == '\0');
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, SELECTION_SNAPSHOT_CAPACITY, 0, NULL);
    assert(strcmp(seeded_metadata.title, "Newest") == 0);
}

static void test_paged_rows_and_sorted_album_window(void) {
    reset_cache();
    song_row_t page[128];
    for (int i = 0; i < 128; ++i) {
        char path[64], title[64];
        snprintf(path, sizeof(path), "/music/page-%03d.mp3", i);
        snprintf(title, sizeof(title), "Page title %03d", i);
        page[i] = row(1000 + i, path, title);
        library_selection_snapshot_store(&page[i], SELECTION_SNAPSHOT_ALL_SONGS, i, test_generation);
    }
    const int page_probe[] = { 0, 64, 127 };
    for (size_t i = 0; i < sizeof(page_probe) / sizeof(page_probe[0]); ++i) {
        int rank = page_probe[i];
        library_selection_snapshot_seed(SELECTION_SNAPSHOT_ALL_SONGS, rank, 0, NULL);
        assert(strcmp(seeded_metadata.title, page[rank].tags.title) == 0);
        assert(strcmp(seeded_path, page[rank].path) == 0);
    }

    reset_cache();
    enum { ALBUM_TRACKS = GROUP_SONGS_PAGE_SIZE + 60 };
    song_row_t album[ALBUM_TRACKS];
    for (int i = 0; i < ALBUM_TRACKS; ++i) {
        int track = ALBUM_TRACKS - i;
        char path[64], title[64];
        snprintf(path, sizeof(path), "/music/album/track-%03d.flac", track);
        snprintf(title, sizeof(title), "Album title %03d", track);
        album[i] = row(2000 + track, path, title);
        album[i].tags.track_number = track;
    }
    qsort(album, ALBUM_TRACKS, sizeof(album[0]), cmp_song_disc_track);
    for (int i = 0; i < GROUP_SONGS_PAGE_SIZE; ++i)
        library_selection_snapshot_store(&album[i], SELECTION_SNAPSHOT_GROUP, -1, test_generation);

    const int album_probe[] = { 0, GROUP_SONGS_PAGE_SIZE / 2, GROUP_SONGS_PAGE_SIZE - 1 };
    for (size_t i = 0; i < sizeof(album_probe) / sizeof(album_probe[0]); ++i) {
        const song_row_t *expected = &album[album_probe[i]];
        library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, expected->id, expected->path);
        assert(strcmp(seeded_path, expected->path) == 0);
        assert(strcmp(seeded_metadata.title, expected->tags.title) == 0);
    }
    /* The first page follows sorted playback order; tail rows aren't admitted. */
    library_selection_snapshot_seed(SELECTION_SNAPSHOT_GROUP, -1, album[GROUP_SONGS_PAGE_SIZE].id,
                            album[GROUP_SONGS_PAGE_SIZE].path);
    assert(seeded_path[0] == '\0');
}

int main(void) {
    test_rank_source_and_owned_copy();
    test_group_identity_generation_and_misses();
    test_bounded_lru_eviction();
    test_paged_rows_and_sorted_album_window();
    puts("Library selection snapshot spec passed");
    return 0;
}
