#include "src/library/remote/remote_state.h"
#include "src/library/tagcache/core/tagcache_internal.h"
#include <assert.h>
#include "src/library/metadata/db/metadata_db_internal.h"

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation;
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;
static int album_ratings[4];
static unsigned test_rating_revision;
static int rating_order_reads;
void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
void metadata_db_copy_song(const tagcache_song_t *src, song_row_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->id = src->id;
    snprintf(dst->path, sizeof(dst->path), "%s", src->path);
    snprintf(dst->tags.genre, sizeof(dst->tags.genre), "%s", src->genre);
    snprintf(dst->tags.artist, sizeof(dst->tags.artist), "%s", src->artist);
    snprintf(dst->tags.album, sizeof(dst->tags.album), "%s", src->album);
    snprintf(dst->tags.album_artist, sizeof(dst->tags.album_artist), "%s", src->album_artist);
}
void metadata_db_copy_group(const tagcache_group_t *src, group_row_t *dst) {
    memset(dst, 0, sizeof(*dst));
    snprintf(dst->name, sizeof(dst->name), "%s", src->name);
    snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", src->album_artist);
}
remote_state_rating_t *remote_state_list_prefix(const char *prefix, int *count) {
    rating_order_reads++;
    *count = 4;
    remote_state_rating_t *rows = calloc(4, sizeof(*rows));
    for (int i = 0; i < 4; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%sArtist\t%c", prefix, 'A' + i);
        rows[i].path = strdup(path);
        rows[i].rating = album_ratings[i] ? 6 - album_ratings[i] : 0;
    }
    return rows;
}
int rating_writer_effective_stars(const char *path, int stored) { (void)path; return stored; }
unsigned remote_state_revision(void) { return test_rating_revision; }

static void check_rating_order(void) {
    album_ratings[0] = 2;
    album_ratings[1] = 5;
    album_ratings[2] = 5;
    album_ratings[3] = 0;
    int *ranks = NULL, sections[6], count = 0;
    int32_t generation;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[5] == 2 && sections[2] == 1 && sections[0] == 1);
    assert(ranks[0] == 1 && ranks[1] == 2 && ranks[2] == 0 && ranks[3] == 3);
    group_row_t page[2];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 1, 2, page) == 2);
    assert(strcmp(page[0].name, "C") == 0 && strcmp(page[1].name, "A") == 0);
    free(ranks);

    album_ratings[0] = album_ratings[1] = album_ratings[2] = album_ratings[3] = 0;
    test_rating_revision++;
    assert(metadata_db_load_album_rating_order(&ranks, sections, &count, &generation));
    assert(count == 4 && sections[0] == 4);
    assert(ranks[0] == 0 && ranks[1] == 1 && ranks[2] == 2 && ranks[3] == 3);
    free(ranks);
}

static void check_rating_page_cache(void) {
    group_row_t page[2];
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 0, 2, page) == 2);
    int reads = rating_order_reads;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 2, 2, page) == 2);
    assert(rating_order_reads == reads);
    album_ratings[3] = 5;
    test_rating_revision++;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 0, 2, page) == 2);
    assert(!strcmp(page[0].name, "D") && rating_order_reads == reads + 1);
    metadata_db_db_incarnation++;
    assert(metadata_db_get_albums_page_sorted(NULL, METADATA_DB_ALBUM_SORT_RATING, 2, 2, page) == 2);
    assert(rating_order_reads == reads + 2);
}


static void check_orders(int known) {
    int count = tagcache_group_count(TAGCACHE_GROUP_ALBUM);
    assert(count == 4);
    for (int sort = 0; sort < 4; sort++) {
        for (int row = 0; row < count; row++) {
            tagcache_group_t forward, reverse;
            int expected = sort == 2 ? (row < known ? known - 1 - row : row) : count - 1 - row;
            assert(tagcache_album_group_at_sorted(sort, expected, &forward));
            assert(tagcache_album_group_at_sorted(sort | TAGCACHE_ALBUM_SORT_REVERSE, row, &reverse));
            assert(strcmp(forward.name, reverse.name) == 0);
            int canonical = tagcache_group_index(TAGCACHE_GROUP_ALBUM, reverse.name, reverse.album_artist);
            int inverse = -1;
            assert(tagcache_album_group_sorted_rank(sort | TAGCACHE_ALBUM_SORT_REVERSE, canonical, &inverse));
            assert(inverse == row);
            metadata_db_album_sort_t order = (metadata_db_album_sort_t)(sort | TAGCACHE_ALBUM_SORT_REVERSE);
            group_row_t page[2];
            int got = metadata_db_get_albums_for_group_sorted(METADATA_DB_GROUP_ARTIST,
                                                               "Artist", order, row, 2, page);
            assert(got == (row == count - 1 ? 1 : 2));
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_get_album_for_group_sorted_offset(METADATA_DB_GROUP_ARTIST,
                      "Artist", order, reverse.name, reverse.album_artist) == row);
            assert(metadata_db_get_albums_for_group_sorted(METADATA_DB_GROUP_ALBUM_ARTIST,
                      "Artist", order, row, 2, page) == (row == count - 1 ? 1 : 2));
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_get_album_for_group_sorted_offset(METADATA_DB_GROUP_ALBUM_ARTIST,
                      "Artist", order, reverse.name, reverse.album_artist) == row);
            assert(metadata_db_get_albums_page_sorted(NULL, order, row, 1, page) == 1);
            assert(strcmp(page[0].name, reverse.name) == 0);
            assert(metadata_db_album_canonical_to_sorted(order, canonical) == row);
            assert(tagcache_album_group_sorted_offset(sort | TAGCACHE_ALBUM_SORT_REVERSE,
                                                       reverse.name, reverse.album_artist) == row);
        }
    }
}

typedef struct {
    const char *path;
    char title[64];
} committed_read_t;

static void *read_committed_title(void *arg) {
    committed_read_t *read = arg;
    tagcache_song_t song;
    if (tagcache_song_by_path(read->path, &song))
        snprintf(read->title, sizeof(read->title), "%s", song.title);
    return NULL;
}

typedef struct { char parts[12][700]; int n; } genre_parts_t;

static bool collect_part(const char *start, size_t len, void *user) {
    genre_parts_t *p = user;
    memcpy(p->parts[p->n], start, len);
    p->parts[p->n++][len] = '\0';
    return true;
}

static int split_genres(const char *raw, genre_parts_t *p) {
    p->n = 0;
    int n = tagcache_genre_each(raw, collect_part, p);
    assert(n == p->n);
    return n;
}

static void check_genre_names(void) {
    genre_parts_t p;
    assert(split_genres("Rock; Pop", &p) == 2 && !strcmp(p.parts[0], "Rock") && !strcmp(p.parts[1], "Pop"));
    assert(split_genres("Rock, Pop/Jazz", &p) == 3 && !strcmp(p.parts[2], "Jazz"));
    assert(split_genres("  Rock ;; rock ;  ", &p) == 1 && !strcmp(p.parts[0], "Rock")); /* trim, dedupe */
    assert(split_genres(";,/", &p) == 1 && !strcmp(p.parts[0], ";,/")); /* whole string kept */
    assert(split_genres("Hip.Hop", &p) == 1 && !strcmp(p.parts[0], "Hip.Hop")); /* dot is no separator */
    /* ASCII case folding, independent of the C library locale. */
    assert(tagcache_genre_matches("INDIE; Rock", "indie") && tagcache_genre_matches("indie; Pop", "INDIE"));
    assert(split_genres("INDIE; indie", &p) == 1);
    assert(split_genres("", &p) == 0);
    assert(tagcache_genre_matches("Rock; Pop", "pop") && !tagcache_genre_matches("Rock; Pop", "Rock; Pop"));
    assert(!tagcache_genre_matches("Rockabilly", "Rock"));
    /* Nine genres: the ninth is still seen and matchable (artists stop at eight). */
    const char *nine = "g1;g2;g3;g4;g5;g6;g7;g8;g9";
    assert(split_genres(nine, &p) == 9 && !strcmp(p.parts[8], "g9"));
    assert(tagcache_genre_matches(nine, "G9") && !tagcache_genre_matches(nine, "g10"));
    char names[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_TAG_MAX];
    assert(tagcache_artist_names("a;b;c;d;e;f;g;h;i", names, 12) == TAGCACHE_ARTIST_SPLIT_MAX);
    /* Artists keep their bounds: parts clip to 127 bytes before dedupe, 8 names at most. */
    char art[440], clipped[128];
    memset(clipped, 'x', 127); clipped[127] = '\0';
    char longx[200], longy[200];
    memset(longx, 'x', 150); longx[150] = '\0';
    memset(longy, 'x', 140); longy[140] = 'y'; longy[141] = '\0';
    snprintf(art, sizeof(art), "%s;%s;a2;a3;a4;a5;a6;a7;a8", longx, longy);
    assert(tagcache_artist_names(art, names, 12) == TAGCACHE_ARTIST_SPLIT_MAX); /* x-parts count once */
    assert(!strcmp(names[0], clipped) && !strcmp(names[7], "a8"));
    /* Two long genres sharing a 127-byte prefix stay distinct and fully matchable. */
    char longs[420], a[200], b[200];
    memset(a, 'x', 150); a[150] = '\0';
    memset(b, 'x', 140); b[140] = 'y'; b[141] = '\0';
    snprintf(longs, sizeof(longs), "%s; %s", a, b);
    assert(split_genres(longs, &p) == 2 && !strcmp(p.parts[0], a) && !strcmp(p.parts[1], b));
    assert(tagcache_genre_matches(longs, b) && tagcache_genre_matches(longs, a));
    char prefix[128];
    memset(prefix, 'x', 127); prefix[127] = '\0';
    assert(!tagcache_genre_matches(longs, prefix));
}

static void check_genre_split_lists(void) {
    char dir[] = "/tmp/compas-genre-XXXXXX";
    assert(mkdtemp(dir));
    assert(tagcache_open(dir));
    tagcache_begin_update_with_lock(NULL, NULL);
    /* Distinct titles make the first-seen order explicit: song 0 sorts first. */
    const char *titles[] = {"T0", "T1", "T2", "T3"};
    char longa[200], longb[200], nine_tail[420];
    memset(longa, 'x', 150); longa[150] = '\0';
    memset(longb, 'x', 140); longb[140] = 'y'; longb[141] = '\0';
    snprintf(nine_tail, sizeof(nine_tail), "%s; %s", longa, longb);
    const char *genres[] = {"Rock; Pop", "pop", "Rock/Jazz", nine_tail};
    char artist3[420]; /* two artists sharing a 127-byte prefix are one artist */
    snprintf(artist3, sizeof(artist3), "%s;%s", longa, longb);
    for (int i = 0; i < 4; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/music/g%d.mp3", i);
        tagcache_upsert_year_changed(path, 100 + i, 1000, titles[i], i == 3 ? artist3 : "Artist", "Album",
                                     "Artist", genres[i], 1, 1, 2000, NULL);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    assert(metadata_db_get_genre_count() == 5); /* Jazz, Pop, Rock, xxx..., xxx...y */
    group_row_t rows[6];
    assert(metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, 0, 6, rows) == 5);
    tagcache_song_t first;
    assert(tagcache_song_by_path("/music/g0.mp3", &first));
    int64_t id0 = first.id;
    assert(tagcache_song_by_path("/music/g2.mp3", &first));
    int64_t id2 = first.id;
    int seen = 0;
    tagcache_song_t last;
    assert(tagcache_song_by_path("/music/g3.mp3", &last));
    for (int i = 0; i < 5; i++) {
        const char *name = rows[i].genre_name;
        if (!strcmp(name, "Jazz")) { assert(rows[i].song_count == 1 && rows[i].first_song_id == id2); seen |= 1; }
        else if (!strcmp(name, "Pop")) { assert(rows[i].song_count == 2 && rows[i].first_song_id == id0); seen |= 2; }
        else if (!strcmp(name, "Rock")) { assert(rows[i].song_count == 2 && rows[i].first_song_id == id0); seen |= 4; }
        else if (!strcmp(name, longa)) { assert(rows[i].song_count == 1 && rows[i].first_song_id == last.id); seen |= 8; }
        else if (!strcmp(name, longb)) { assert(rows[i].song_count == 1 && rows[i].first_song_id == last.id); seen |= 16; }
        else assert(0);
    }
    assert(seen == 31); /* all five names, once each */
    char clipped[128];
    memset(clipped, 'x', 127); clipped[127] = '\0';
    assert(metadata_db_count_songs_filtered(NULL, clipped, NULL, NULL, NULL) == 1); /* clipped artist name */
    assert(metadata_db_count_songs_filtered(NULL, clipped, NULL, "Album", NULL) == 1); /* with the album too */
    assert(metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, "Pop") == 2);
    assert(metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, "rock") == 2);
    assert(metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, longb) == 1);
    assert(metadata_db_count_songs_filtered(NULL, NULL, NULL, NULL, "Rock; Pop") == 0);
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
}

int main(void) {
    check_genre_names();
    check_genre_split_lists();
    char dir[] = "/tmp/compas-sort-XXXXXX";
    assert(mkdtemp(dir));
    assert(tagcache_open(dir));
    tagcache_begin_update_with_lock(NULL, NULL);
    const int years[] = {2000, -1, 1990, -1};
    for (int i = 0; i < 4; i++) {
        char path[64], album[16];
        snprintf(path, sizeof(path), "/music/%d.mp3", i);
        snprintf(album, sizeof(album), "%c", 'A' + i);
        tagcache_upsert_year_changed(path, 100 + i, 1000, "Song", "Artist", album,
                                     "Artist", "Genre", 1, 1, years[i], NULL);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    check_rating_order();
    check_rating_page_cache();
    check_orders(2);
    tagcache_close();
    assert(tagcache_open(dir));
    check_orders(2);
    /* Publishing an all-unknown year index must invalidate the cached boundary. */
    tagcache_begin_update_with_lock(NULL, NULL);
    for (int i = 0; i < 4; i++) {
        char path[64], album[16];
        snprintf(path, sizeof(path), "/music/%d.mp3", i);
        snprintf(album, sizeof(album), "%c", 'A' + i);
        tagcache_upsert_year_changed(path, 200 + i, 1000, "Song", "Artist", album,
                                     "Artist", "Genre", 1, 1, -1, NULL);
    }
    assert(tagcache_end_update_with_lock(NULL, NULL));
    check_orders(0);
    /* The worker's staging reader is thread-local. A concurrent UI query
     * must keep seeing the old committed generation until publication. */
    tagcache_begin_update_with_lock(NULL, NULL);
    tagcache_upsert_year_changed("/music/0.mp3", 300, 1000, "Updated Song", "Artist",
                                 "A", "Artist", "Genre", 1, 1, 2026, NULL);
    committed_read_t concurrent_read = { .path = "/music/0.mp3" };
    pthread_t reader;
    assert(pthread_create(&reader, NULL, read_committed_title, &concurrent_read) == 0);
    assert(pthread_join(reader, NULL) == 0);
    assert(strcmp(concurrent_read.title, "Song") == 0);
    assert(tagcache_end_update_with_lock(NULL, NULL));
    tagcache_song_t published_song;
    assert(tagcache_song_by_path("/music/0.mp3", &published_song));
    assert(strcmp(published_song.title, "Updated Song") == 0);
    tagcache_close();
    char command[128];
    snprintf(command, sizeof(command), "rm -rf '%s'", dir);
    assert(system(command) == 0);
    puts("tagcache reverse: ok");
    return 0;
}
