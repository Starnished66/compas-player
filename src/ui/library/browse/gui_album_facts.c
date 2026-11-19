#include "gui_album_header.h"
#include "gui_album_facts.h"
#include "../../../library/metadata/db/album_rating.h"
#include "../../../library/tagcache/core/tagcache.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Album facts recorded by the album loader (worker thread).
 * Entries carry no year, genre or track number. The loader that already read
 * every song row records them here, keyed by a hash of the ordered paths, and
 * the UI thread looks them up when it builds the header. */
#define RECORD_SLOTS 4
typedef struct { uint64_t key; bool used; int year; int stars; char genre[128]; int count; int * track; } record_t;
static pthread_mutex_t record_mutex = PTHREAD_MUTEX_INITIALIZER;
static record_t records[RECORD_SLOTS];
static int record_next;
static unsigned rating_revision;

uint64_t gui_album_facts_seed(int count) { return 1469598103934665603ULL ^ (uint64_t) count; }
uint64_t gui_album_facts_add(uint64_t h, const char * path) {
    for (const unsigned char * c = (const unsigned char *) path; *c; c++) { h ^= *c; h *= 1099511628211ULL; }
    return (h ^ 0xff) * 1099511628211ULL;
}
static int cmp_int(const void * a, const void * b) { return *(const int *) a - *(const int *) b; }

static void store_record(uint64_t key, int year, const char *genre, int *track,
                         int count, const char *artist, const char *album) {
    unsigned revision;
    int stars;
    do {
        pthread_mutex_lock(&record_mutex);
        revision = rating_revision;
        pthread_mutex_unlock(&record_mutex);
        stars = metadata_db_album_rating_get(artist, album);
        pthread_mutex_lock(&record_mutex);
        if (revision == rating_revision) break;
        pthread_mutex_unlock(&record_mutex);
    } while (true);

    record_t * slot = NULL;
    for (int i = 0; i < RECORD_SLOTS; i++) if (records[i].used && records[i].key == key) slot = &records[i];
    if (!slot) slot = &records[record_next++ % RECORD_SLOTS];
    free(slot->track);
    slot->used = true; slot->key = key; slot->year = year; slot->count = count; slot->track = track; slot->stars = stars;
    snprintf(slot->genre, sizeof(slot->genre), "%s", genre);
    pthread_mutex_unlock(&record_mutex);
}

int gui_album_facts_rating(uint64_t key) {
    int stars = 0;
    pthread_mutex_lock(&record_mutex);
    for (int i = 0; i < RECORD_SLOTS; i++)
        if (records[i].used && records[i].key == key) stars = records[i].stars;
    pthread_mutex_unlock(&record_mutex);
    return stars;
}

void gui_album_facts_note_rating(uint64_t key, int stars) {
    pthread_mutex_lock(&record_mutex);
    rating_revision++;
    for (int i = 0; i < RECORD_SLOTS; i++)
        if (records[i].used && records[i].key == key) records[i].stars = stars;
    pthread_mutex_unlock(&record_mutex);
}

static int common_year(int *years, int count) {
    int year = 0, best = 0;
    qsort(years, (size_t)count, sizeof(*years), cmp_int);
    for (int i = 0; i < count;) {
        int j = i;
        while (j < count && years[j] == years[i]) j++;
        if (j - i > best) { best = j - i; year = years[i]; }
        i = j;
    }
    return year;
}

/* Songs per distinct genre name; each name is counted once per song. */
typedef struct { char *name; int count; } genre_tally_t;
typedef struct { genre_tally_t *rows; int used, cap; bool failed; } genre_tallies_t;

static bool tally_part(const char *start, size_t len, void *user) {
    genre_tallies_t *t = user;
    char part[TAGCACHE_PATH_MAX];
    if (len >= sizeof(part)) len = sizeof(part) - 1;
    memcpy(part, start, len);
    part[len] = '\0';
    if (tagcache_cmp_ascii(part, "Unknown Genre") == 0) return true;
    for (int i = 0; i < t->used; i++) {
        if (tagcache_cmp_ascii(t->rows[i].name, part) == 0) {
            t->rows[i].count++;
            return true;
        }
    }
    if (t->used == t->cap) {
        int cap = t->cap ? t->cap * 2 : 8;
        genre_tally_t *grown = realloc(t->rows, (size_t) cap * sizeof(*grown));
        if (!grown) { t->failed = true; return false; }
        t->rows = grown; t->cap = cap;
    }
    char *name = strdup(part);
    if (!name) { t->failed = true; return false; }
    t->rows[t->used++] = (genre_tally_t){ name, 1 };
    return true;
}

/* The genre most songs carry (ties go to the first by name), "" when none. */
static void common_genre(const song_row_t *songs, int count, char *out, size_t out_size) {
    genre_tallies_t t = {0};
    for (int i = 0; i < count && !t.failed; i++) tagcache_genre_each(songs[i].tags.genre, tally_part, &t);
    const genre_tally_t *best = NULL;
    for (int i = 0; i < t.used && !t.failed; i++) /* a failed tally has no winner */
        if (!best || t.rows[i].count > best->count ||
            (t.rows[i].count == best->count && tagcache_cmp_ascii(t.rows[i].name, best->name) < 0)) best = &t.rows[i];
    snprintf(out, out_size, "%s", best ? best->name : "");
    for (int i = 0; i < t.used; i++) free(t.rows[i].name);
    free(t.rows);
}

void gui_album_header_record(const song_row_t * songs, int count, const char *artist, const char *album) {
    if (!songs || count <= 0) return;
    int * years = malloc(sizeof(int) * (size_t) count);
    int * track = malloc(sizeof(int) * (size_t) count);
    if (!years || !track) { free(years); free(track); return; }
    uint64_t key = gui_album_facts_seed(count);
    int year_n = 0;
    for (int i = 0; i < count; i++) {
        key = gui_album_facts_add(key, songs[i].path);
        track[i] = songs[i].tags.track_number > 0 ? songs[i].tags.track_number : 0;
        if (songs[i].tags.release_year > 0) years[year_n++] = songs[i].tags.release_year;
    }
    char genre[128];
    common_genre(songs, count, genre, sizeof(genre));
    store_record(key, common_year(years, year_n), genre, track, count, artist, album);
    free(years);
}

bool gui_album_facts_lookup(uint64_t key, int count, int * year, char * genre, size_t genre_size, int ** track) {
    bool found = false;
    pthread_mutex_lock(&record_mutex);
    for (int i = 0; i < RECORD_SLOTS && !found; i++) {
        if (!records[i].used || records[i].key != key || records[i].count != count) continue;
        int * copy = malloc(sizeof(int) * (size_t) count);
        if (!copy) break;
        memcpy(copy, records[i].track, sizeof(int) * (size_t) count);
        *track = copy;
        *year = records[i].year;
        snprintf(genre, genre_size, "%s", records[i].genre);
        found = true;
    }
    pthread_mutex_unlock(&record_mutex);
    return found;
}
