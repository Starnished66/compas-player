#include "gui_album_header.h"
#include "gui_album_facts.h"
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
typedef struct { uint64_t key; bool used; int year; char genre[128]; int count; int * track; } record_t;
static pthread_mutex_t record_mutex = PTHREAD_MUTEX_INITIALIZER;
static record_t records[RECORD_SLOTS];
static int record_next;

uint64_t gui_album_facts_seed(int count) { return 1469598103934665603ULL ^ (uint64_t) count; }
uint64_t gui_album_facts_add(uint64_t h, const char * path) {
    for (const unsigned char * c = (const unsigned char *) path; *c; c++) { h ^= *c; h *= 1099511628211ULL; }
    return (h ^ 0xff) * 1099511628211ULL;
}
static int cmp_int(const void * a, const void * b) { return *(const int *) a - *(const int *) b; }
static int cmp_text(const void * a, const void * b) {
    return strcasecmp(*(const char * const *) a, *(const char * const *) b);
}

void gui_album_header_record(const song_row_t * songs, int count) {
    if (!songs || count <= 0) return;
    int * years = malloc(sizeof(int) * (size_t) count);
    int * track = malloc(sizeof(int) * (size_t) count);
    const char ** genres = malloc(sizeof(char *) * (size_t) count);
    if (!years || !track || !genres) { free(years); free(track); free(genres); return; }
    uint64_t key = gui_album_facts_seed(count);
    int year_n = 0, genre_n = 0;
    for (int i = 0; i < count; i++) {
        key = gui_album_facts_add(key, songs[i].path);
        track[i] = songs[i].tags.track_number > 0 ? songs[i].tags.track_number : 0;
        if (songs[i].tags.release_year > 0) years[year_n++] = songs[i].tags.release_year;
        /* The scanner stores "Unknown Genre" for untagged files; leave it out. */
        if (songs[i].tags.genre[0] && strcmp(songs[i].tags.genre, "Unknown Genre") != 0)
            genres[genre_n++] = songs[i].tags.genre;
    }
    /* The most common value wins; sorted order makes a tie go to the first. */
    int year = 0, best = 0;
    qsort(years, (size_t) year_n, sizeof(int), cmp_int);
    for (int i = 0; i < year_n;) {
        int j = i;
        while (j < year_n && years[j] == years[i]) j++;
        if (j - i > best) { best = j - i; year = years[i]; }
        i = j;
    }
    const char * genre = "";
    best = 0;
    qsort(genres, (size_t) genre_n, sizeof(char *), cmp_text);
    for (int i = 0; i < genre_n;) {
        int j = i;
        while (j < genre_n && strcasecmp(genres[j], genres[i]) == 0) j++;
        if (j - i > best) { best = j - i; genre = genres[i]; }
        i = j;
    }
    pthread_mutex_lock(&record_mutex);
    record_t * slot = NULL;
    for (int i = 0; i < RECORD_SLOTS; i++) if (records[i].used && records[i].key == key) slot = &records[i];
    if (!slot) slot = &records[record_next++ % RECORD_SLOTS];
    free(slot->track);
    slot->used = true; slot->key = key; slot->year = year; slot->count = count; slot->track = track;
    snprintf(slot->genre, sizeof(slot->genre), "%s", genre);
    pthread_mutex_unlock(&record_mutex);
    free(years); free(genres);
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
