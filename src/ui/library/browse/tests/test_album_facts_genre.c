#include "../gui_album_facts.c"
#include <assert.h>

int metadata_db_album_rating_get(const char *artist, const char *album) { (void)artist; (void)album; return 0; }

static uint64_t record(const char *const *genres, int count) {
    song_row_t *songs = calloc((size_t) count, sizeof(*songs));
    uint64_t key = gui_album_facts_seed(count);
    for (int i = 0; i < count; i++) {
        snprintf(songs[i].path, sizeof(songs[i].path), "/m/%s/%d.mp3", genres[0], i);
        snprintf(songs[i].tags.genre, sizeof(songs[i].tags.genre), "%s", genres[i]);
        key = gui_album_facts_add(key, songs[i].path);
    }
    gui_album_header_record(songs, count, "A", "B");
    free(songs);
    return key;
}

static void expect(const char *const *genres, int count, const char *want) {
    uint64_t key = record(genres, count);
    int year; char genre[128]; int *track = NULL;
    assert(gui_album_facts_lookup(key, count, &year, genre, sizeof(genre), &track));
    assert(strcmp(genre, want) == 0);
    free(track);
}

int main(void) {
    expect((const char *const[]){"Rock; Pop", "Rock/Pop", "Rock"}, 3, "Rock");
    expect((const char *const[]){"Rock; Pop", "pop", "Pop, Jazz"}, 3, "Pop");
    expect((const char *const[]){"Unknown Genre", "Unknown Genre; Jazz", ""}, 3, "Jazz");
    expect((const char *const[]){"Unknown Genre", ""}, 2, "");
    puts("album facts genre: ok");
    return 0;
}
