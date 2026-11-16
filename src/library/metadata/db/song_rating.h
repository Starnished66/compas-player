#ifndef SONG_RATING_H
#define SONG_RATING_H

#include <stdint.h>

/* Stored 0 = unrated, 1 = 5 stars (legacy favourite), 2..5 = 4..1 stars.
 * This encoding lets old cards work without migration. */
static inline int song_rating_stars(int32_t stored) {
    return stored <= 0 ? 0 : stored >= 5 ? 1 : 6 - stored;
}

static inline int32_t song_rating_stored(int stars) {
    return stars <= 0 ? 0 : stars > 5 ? 1 : 6 - stars;
}

#endif /* SONG_RATING_H */
