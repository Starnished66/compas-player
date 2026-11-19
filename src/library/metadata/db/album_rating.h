#ifndef ALBUM_RATING_H
#define ALBUM_RATING_H
#include <stdbool.h>
#include <stddef.h>
#include "metadata_db.h"

#define ALBUM_RATING_PREFIX "album://"
#define ALBUM_RATING_KEY_SIZE 600

typedef struct {
    char *album_artist;
    char *album;
    int stars;
} rated_album_t;

bool album_rating_key(const char *album_artist, const char *album, char *out, size_t size);
int metadata_db_album_rating_get(const char *album_artist, const char *album);
/* Queues persistence through the shared rating writer. */
void metadata_db_album_rating_set(const char *album_artist, const char *album, int stars);
void metadata_db_load_rated_albums(rated_album_t **out, int *count);
void metadata_db_free_rated_albums(rated_album_t *albums, int count);

/* Rated albums at or above this many stars make up Top rated albums. */
#define TOP_RATED_ALBUM_MIN_STARS 3

typedef struct {
    group_row_t group;
    int stars;
} top_rated_album_t;

/* Top rated albums in the rating loader's order, each resolved as the Albums
 * list has it; albums no longer listed are skipped. Free with free(). */
void metadata_db_load_top_rated_albums(top_rated_album_t **out, int *count);
#endif
