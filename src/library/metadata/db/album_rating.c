#include "album_rating.h"
#include "song_rating.h"
#include "../../playlists/rating_writer.h"
#include "../../remote/remote_state.h"
#include <stdlib.h>
#include <string.h>

static char *copy_name(char *out, const char *name) {
    for (; *name; name++) *out++ = strchr("\t\r\n", *name) ? ' ' : *name;
    return out;
}

bool album_rating_key(const char *artist, const char *album, char *out, size_t size) {
    if (out && size) out[0] = 0;
    if (!artist || !*artist || !album || !*album || !out) return false;
    size_t prefix = strlen(ALBUM_RATING_PREFIX), a = strlen(artist), b = strlen(album);
    if (size <= prefix || a >= size - prefix || b >= size - prefix - a - 1) return false;
    memcpy(out, ALBUM_RATING_PREFIX, prefix);
    char *end = copy_name(out + prefix, artist);
    *end++ = '\t';
    char *album_end = copy_name(end, album);
    while (album_end > end && album_end[-1] == ' ') album_end--;
    *album_end = 0;
    return true;
}

int metadata_db_album_rating_get(const char *artist, const char *album) {
    char key[ALBUM_RATING_KEY_SIZE];
    return album_rating_key(artist, album, key, sizeof(key)) ? rating_writer_stars(key) : 0;
}

void metadata_db_album_rating_set(const char *artist, const char *album, int stars) {
    char key[ALBUM_RATING_KEY_SIZE];
    if (album_rating_key(artist, album, key, sizeof(key))) rating_writer_submit(key, stars);
}

static int compare_albums(const void *a, const void *b) {
    const rated_album_t *aa = a, *bb = b;
    if (aa->stars != bb->stars) return bb->stars - aa->stars;
    int order = strcmp(aa->album_artist, bb->album_artist);
    return order ? order : strcmp(aa->album, bb->album);
}

void metadata_db_free_rated_albums(rated_album_t *albums, int count) {
    for (int i = 0; i < count; i++) free(albums[i].album_artist);
    free(albums);
}

void metadata_db_load_rated_albums(rated_album_t **out, int *count) {
    *out = NULL; *count = 0;
    int n = 0;
    remote_state_rating_t *rows = remote_state_list_prefix(ALBUM_RATING_PREFIX, &n);
    rated_album_t *albums = n ? calloc((size_t)n, sizeof(*albums)) : NULL;
    for (int i = 0; i < n; i++) {
        char *artist = rows[i].path + strlen(ALBUM_RATING_PREFIX);
        char *album = strchr(artist, '\t');
        if (albums && rows[i].rating != 0 && album && album > artist && album[1] && !strchr(album + 1, '\t')) {
            char *copy = strdup(artist);
            if (copy) {
                copy[album - artist] = 0;
                albums[(*count)++] = (rated_album_t){copy, copy + (album - artist) + 1,
                                                       song_rating_stars(rows[i].rating)};
            }
        }
        free(rows[i].path);
    }
    free(rows);
    if (*count) qsort(albums, (size_t)*count, sizeof(*albums), compare_albums);
    *out = albums;
}
