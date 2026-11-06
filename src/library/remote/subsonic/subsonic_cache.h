#ifndef SUBSONIC_CACHE_H
#define SUBSONIC_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../../network/services/subsonic_client.h"

/* Artist, album and playlist lists kept on the SD card so they open without a
 * network round trip. One folder per server (.compas/subsonic/<key>/) holds a
 * tab-separated file per list plus meta.tsv, and one file of songs per album
 * (albums/) and per playlist (playlists/). The password is never written.
 * With no card, every call fails and callers behave as if nothing were cached.
 * Lists are the packed arrays of subsonic_client.h: free() releases them. */

typedef enum {
    SUBSONIC_LIST_ARTISTS,
    SUBSONIC_LIST_ALBUMS,
    SUBSONIC_LIST_PLAYLISTS,
    SUBSONIC_LIST_KINDS,                              /* the lists above have an entry in meta.tsv */
    SUBSONIC_LIST_ALBUM_SONGS = SUBSONIC_LIST_KINDS,  /* the songs of one album, keyed by its id */
    SUBSONIC_LIST_PLAYLIST_SONGS,                     /* the songs of one playlist, keyed by its id */
    SUBSONIC_LIST_ALL_KINDS
} subsonic_list_kind_t;

static inline bool subsonic_list_is_songs(subsonic_list_kind_t kind) { return kind >= SUBSONIC_LIST_KINDS; }

/* What is known about one cached list. Each list carries its own stamp so a
 * list that was not refreshed is never judged by another list's marker. A
 * songs file keeps its entry in its own first line. Its stamp is what the file
 * was saved under: the library stamp for an album, for a playlist the song
 * count (last_modified) and the changed time (last_scan). */
typedef struct {
    long long synced_at; /* seconds since the epoch of the last server check; 0 = never */
    uint64_t hash;       /* FNV-64 of the list as written; 0 = unknown */
    int rows;            /* rows in the list file as written; set by save, kept by save_entry */
    subsonic_library_stamp_t stamp;
} subsonic_cache_entry_t;

#define SUBSONIC_FNV64_INIT UINT64_C(14695981039346656037)
uint64_t subsonic_fnv64(uint64_t hash, const void * data, size_t len);

/* 16 hex digits of the normalised URL and user name, plus the NUL. */
#define SUBSONIC_CACHE_KEY_SIZE 17
void subsonic_cache_server_key(const subsonic_server_t * server, char out[SUBSONIC_CACHE_KEY_SIZE]);

/* True when this server has any cached data / this list is cached. Cheap. */
bool subsonic_cache_server_known(const subsonic_server_t * server);
bool subsonic_cache_list_exists(const subsonic_server_t * server, subsonic_list_kind_t kind);

/* Reads a list and its entry (zeroed when meta.tsv lacks it). Malformed lines
 * are skipped; when any were, or the row count differs from the one written,
 * the entry's hash and stamp are cleared so the next fetch rewrites the file.
 * A file whose lines are all unusable (one saved by another version) fails to
 * load. On false nothing is returned. Safe on any thread. */
bool subsonic_cache_load(const subsonic_server_t * server, subsonic_list_kind_t kind, void ** items, int * count,
                         subsonic_cache_entry_t * entry);

/* Hash of what subsonic_cache_save() would write, to detect an unchanged list. */
uint64_t subsonic_cache_hash(subsonic_list_kind_t kind, const void * items, int count);

/* Writes the list and its entry atomically. Safe on any thread. */
bool subsonic_cache_save(const subsonic_server_t * server, subsonic_list_kind_t kind, const void * items, int count,
                         const subsonic_cache_entry_t * entry);

/* Writes only the entry, for when the list itself is unchanged. */
bool subsonic_cache_save_entry(const subsonic_server_t * server, subsonic_list_kind_t kind,
                               const subsonic_cache_entry_t * entry);

/* Songs of one album or playlist (kind is SUBSONIC_LIST_ALBUM_SONGS or
 * SUBSONIC_LIST_PLAYLIST_SONGS). The songs are one malloc'd array: free() it.
 * Loading fails for a file that is missing, unreadable or saved for another id;
 * lost rows clear the entry's hash and stamp, as for lists. subsonic_cache_hash()
 * covers songs too. Safe on any thread. */
bool subsonic_cache_songs_exists(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id);
bool subsonic_cache_songs_load(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id,
                               void ** items, int * count, subsonic_cache_entry_t * entry);

/* Writes the songs and their entry atomically (a list too big to load is left absent). */
bool subsonic_cache_songs_save(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id,
                               const void * items, int count, const subsonic_cache_entry_t * entry);
void subsonic_cache_songs_remove(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id);

/* Most songs files kept per server, albums and playlists together. */
#define SUBSONIC_SONG_FILES_MAX 2000

/* Deletes the files with the oldest fetched_at beyond keep. Reads file headers
 * only when there are more than keep files. Never call from the UI thread. */
void subsonic_cache_songs_prune(const subsonic_server_t * server, int keep);

#endif /* SUBSONIC_CACHE_H */
