/* gui_plugin_get_next_album_tracks() walks albums, then artists, over a fake library. */
#include "src/ui/plugins/gui_plugin_library.c"
#include <assert.h>

#define TEST_SORT ((metadata_db_album_sort_t)5)

typedef struct { const char * name; const char * paths[2]; } fake_album_t;
typedef struct { const char * name; fake_album_t albums[3]; } fake_artist_t;

/* Artists in screen order. C has no albums; E's only album has no songs. */
static const fake_artist_t library[] = {
    { "A", { { "A1", { "/a1/1.flac" } } } },
    { "B", { { "B1", { "/b1/1.flac", "/b1/2.flac" } }, { "B2", { "/b2/1.flac" } } } },
    { "C", { { 0 } } },
    { "D", { { "D1", { "/d1/1.flac" } } } },
    { "E", { { "E1", { 0 } } } },
};
#define ARTISTS ((int)(sizeof(library) / sizeof(library[0])))

static const fake_artist_t * find_artist(const char * name) {
    for (int i = 0; i < ARTISTS; ++i) if (!strcmp(library[i].name, name)) return &library[i];
    return NULL;
}
static int album_total(const fake_artist_t * artist) {
    int n = 0;
    while (n < 3 && artist->albums[n].name) ++n;
    return n;
}

metadata_db_album_sort_t gui_library_group_album_sort(metadata_db_group_kind_t kind) { (void)kind; return TEST_SORT; }
int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name) {
    const fake_artist_t * artist = find_artist(name);
    return artist ? album_total(artist) : 0;
}
int metadata_db_get_albums_for_group_sorted(metadata_db_group_kind_t kind, const char * name,
                                            metadata_db_album_sort_t sort, int offset, int max_rows, group_row_t * out) {
    assert(sort == TEST_SORT); /* The artist page's album order is what is walked. */
    const fake_artist_t * artist = find_artist(name);
    int n = 0;
    for (int i = offset; artist && i < album_total(artist) && n < max_rows; ++i, ++n) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].name, sizeof(out[n].name), "%s", artist->albums[i].name);
        snprintf(out[n].album_artist, sizeof(out[n].album_artist), "%s", name);
    }
    return n;
}
int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist) {
    for (int i = 0; i < ARTISTS; ++i) if (!strcmp(library[i].name, name)) return i;
    return -1;
}
int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out) {
    int n = 0;
    for (int i = offset; i < ARTISTS && n < max_rows; ++i, ++n) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].name, sizeof(out[n].name), "%s", library[i].name);
    }
    return n;
}
int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * rows, int max_rows) {
    const fake_artist_t * artist = find_artist(album_artist);
    for (int i = 0; artist && i < album_total(artist); ++i) {
        if (strcmp(artist->albums[i].name, album)) continue;
        int n = 0;
        for (int j = offset; j < 2 && artist->albums[i].paths[j] && n < max_rows; ++j, ++n) {
            memset(&rows[n], 0, sizeof(rows[n]));
            snprintf(rows[n].path, sizeof(rows[n].path), "%s", artist->albums[i].paths[j]);
        }
        return n;
    }
    return 0;
}
bool cue_reference_is(const char * reference) { (void)reference; return false; }
bool cue_parse_embedded(const char * path, cue_sheet_t * sheet) { (void)path; (void)sheet; return false; }
void cue_sheet_free(cue_sheet_t * sheet) { (void)sheet; }
char ** cue_sheet_references(const char * path, int count) { (void)path; (void)count; return NULL; }

static void expect(const char * artist, const char * album, int want, const char * first) {
    int count;
    char ** tracks = gui_plugin_get_next_album_tracks(artist, album, &count);
    assert(count == want);
    assert(want ? tracks && !strcmp(tracks[0], first) : !tracks);
    gui_plugin_free_string_array(tracks, count);
}

bool metadata_db_album_sort_available(metadata_db_album_sort_t sort) { (void)sort; return true; }

int main(void) {
    expect("A", "A1", 2, "/b1/1.flac");   /* Single-album artist continues to the next artist. */
    expect("B", "B1", 1, "/b2/1.flac");   /* Inside an artist the next album comes first. */
    expect("B", "B2", 1, "/d1/1.flac");   /* Artists with no albums are skipped. */
    expect("D", "D1", 0, NULL);           /* Last playable album of the library stops, no wrap. */
    expect("E", "E1", 0, NULL);           /* Final artist: nothing after it. */
    expect("B", "Missing", 0, NULL);      /* Unknown album. */
    expect("Nobody", "A1", 0, NULL);      /* Unknown artist. */
    puts("Next album: same artist first, then the next artist, no wrap, passed");
    return 0;
}
