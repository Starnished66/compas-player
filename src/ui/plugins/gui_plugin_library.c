#include "gui_plugin_library.h"
#include "../../library/artwork/support/library_player_art.h"
#include "../../library/metadata/db/metadata_db.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char ** gui_plugin_get_artist_albums(const char * artist, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int album_count = (int) count64;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) album_count);
    char ** names = calloc((size_t) album_count, sizeof(*names));
    if (!rows || !names) { free(rows); free(names); return NULL; }
    int n = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, 0, album_count, rows);
    for (int i = 0; i < n; i++) {
        names[i] = strdup(rows[i].name);
        if (!names[i]) {
            for (int j = 0; j < i; j++) free(names[j]);
            free(names);
            free(rows);
            return NULL;
        }
    }
    free(rows);
    *out_count = n;
    return names;
}

static char ** load_plugin_album_group_paths(const char * album, const char * album_artist, int * out_count) {
    *out_count = 0;
    char ** paths = NULL;
    size_t capacity = 0;
    int loaded = 0;
    song_row_t rows[64];
    for (;;) {
        int got = metadata_db_get_album_songs(album, album_artist, loaded, rows, 64);
        if (got <= 0) break;
        if ((size_t) (loaded + got) > capacity) {
            size_t new_capacity = capacity == 0 ? 64 : capacity * 2;
            while (new_capacity < (size_t) (loaded + got)) new_capacity *= 2;
            char ** grown = realloc(paths, new_capacity * sizeof(*paths));
            if (!grown) {
                for (int j = 0; j < loaded; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
            paths = grown;
            capacity = new_capacity;
        }
        for (int i = 0; i < got; i++) {
            paths[loaded + i] = strdup(rows[i].path);
            if (!paths[loaded + i]) {
                for (int j = 0; j < loaded + i; j++) free(paths[j]);
                free(paths);
                return NULL;
            }
        }
        loaded += got;
        if (got < 64) break;
    }
    if (loaded == 0) {
        free(paths);
        return NULL;
    }
    *out_count = loaded;
    return paths;
}

char ** gui_plugin_get_album_tracks(const char * artist, const char * album, int * out_count) {
    *out_count = 0;
    if (!artist || !artist[0] || !album || !album[0]) return NULL;

    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 0 || count64 > INT_MAX) return NULL;
    int total = (int) count64;

    char matched_album_artist[128] = "";
    int match_count = 0;
    group_row_t rows[32];
    int offset = 0;
    while (offset < total) {
        int want = total - offset;
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, album) != 0) continue;
            match_count++;
            snprintf(matched_album_artist, sizeof(matched_album_artist), "%s", rows[i].album_artist);
        }
        offset += got;
        if (got < want) break;
    }
    if (match_count != 1) return NULL;

    return load_plugin_album_group_paths(album, matched_album_artist, out_count);
}

char ** gui_plugin_get_next_album_tracks(const char * artist, const char * current_album, int * out_count) {
    *out_count = 0;
    int64_t count64 = metadata_db_count_albums_for_group(METADATA_DB_GROUP_ARTIST, artist);
    if (count64 <= 1 || count64 > INT_MAX) return NULL;
    group_row_t rows[32];
    int offset = 0;
    while (offset < count64) {
        int want = (int) (count64 - offset);
        if (want > 32) want = 32;
        int got = metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset, want, rows);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            if (strcasecmp(rows[i].name, current_album) != 0) continue;
            if (i + 1 < got) return load_plugin_album_group_paths(rows[i + 1].name, rows[i + 1].album_artist, out_count);
            group_row_t next;
            if (metadata_db_get_albums_for_group(METADATA_DB_GROUP_ARTIST, artist, offset + got, 1, &next) == 1)
                return load_plugin_album_group_paths(next.name, next.album_artist, out_count);
            return NULL;
        }
        offset += got;
        if (got < want) break;
    }
    return NULL;
}

void gui_plugin_free_string_array(char ** array, int count) {
    for (int i = 0; i < count; i++) free(array[i]);
    free(array);
}

/* ---- plugin.library_* -- see gui.h's own comment for the design intent.
 * Every one of these goes straight to metadata_db.c (its own METADATA_DB_
 * GUARD), same as gui_plugin_get_artist_albums() and friends above -- no
 * plugin call in this file forces any whole-library load just because it
 * looked at the library first. ---- */

static int gui_plugin_library_clamp_limit(int limit) {
    if (limit <= 0 || limit > GUI_PLUGIN_LIBRARY_MAX_PAGE) return GUI_PLUGIN_LIBRARY_MAX_PAGE;
    return limit;
}

int64_t gui_plugin_library_song_count(void) {
    return metadata_db_get_song_count();
}

int gui_plugin_library_get_songs(const char * query, const char * artist, const char * album_artist,
                                  const char * album, int offset, int limit, song_row_t * out_rows,
                                  int64_t * out_total) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    if (out_total) *out_total = metadata_db_count_songs_filtered(query, artist, album_artist, album, NULL);
    return metadata_db_get_songs_filtered_page(query, artist, album_artist, album, NULL, offset, limit, out_rows);
}

int gui_plugin_library_search(const char * query, int limit, song_row_t * out_rows) {
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_search_songs(query, out_rows, limit);
}

bool gui_plugin_library_get_song(int64_t id, song_row_t * out_row) {
    return metadata_db_get_song_by_id(id, out_row);
}

bool gui_plugin_library_album_cover(int64_t song_id, int size_px, char * out, size_t out_size) {
    return gui_library_album_cover_cached(song_id, size_px, out, out_size);
}

/* Fetches a page of artists with offset and clamped limit directly from the database. */
int gui_plugin_library_get_artists(int offset, int limit, group_row_t * out_rows) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, offset, limit, out_rows);
}

int gui_plugin_library_get_albums(int offset, int limit, const char * artist_filter, group_row_t * out_rows) {
    if (offset < 0) offset = 0;
    limit = gui_plugin_library_clamp_limit(limit);
    return metadata_db_get_albums_page_filtered(artist_filter, offset, limit, out_rows);
}
