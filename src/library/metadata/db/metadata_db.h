#ifndef METADATA_DB_H
#define METADATA_DB_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Must match tagcache's maximum metadata text field, including the NUL. */
#define METADATA_DB_TEXT_MAX 600
#define METADATA_DB_CATALOG_PAGE_MAX 100
#define METADATA_DB_CATALOG_LIBRARY_ID_SIZE 37
#define METADATA_DB_CATALOG_REVISION_SIZE 64

/* On-disk cache of every scanned song's title/artist/album/album_artist/
 * genre tags, keyed by path + the file's mtime/size at the time it was
 * cached --
 * lets library_scan_once() (gui.c) skip re-reading tags (metadata_read(),
 * one open+parse per file) for every file that hasn't changed since the
 * last scan. Backed by a POSIX port of Rockbox's tagcache (database_idx.tcd
 * plus per-tag database_N.tcd files), with versioned generation-local browse
 * indices. Queries seek into those files and copy only requested rows. */

typedef struct {
    char title[128];
    char artist[128];
    char album[128];
    char album_artist[128];
    char genre[128];
    int32_t track_number; /* -1 when absent; 0 marks a legacy cache row */
    int32_t disc_number;  /* -1 when absent; 0 marks a legacy cache row */
    int32_t release_year; /* -1 when absent; 0 marks a legacy cache row */
} cached_tags_t;

typedef enum {
    METADATA_DB_ALBUM_SORT_NAME = 0,
    METADATA_DB_ALBUM_SORT_RECENT = 1,
    METADATA_DB_ALBUM_SORT_YEAR = 2,
    METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR = 3
} metadata_db_album_sort_t;

typedef enum {
    METADATA_DB_LOAD_SUCCESS_NORMAL,
    METADATA_DB_LOAD_SUCCESS_FRESH,
    METADATA_DB_LOAD_SUCCESS_RECOVERED,
    METADATA_DB_LOAD_FAILED,
    METADATA_DB_LOAD_UNMOUNTED
} metadata_db_load_outcome_t;

/* Opens the on-disk tagcache at its fixed path (the SD-card root on target,
 * ./ on host). If it is already open, returns its existing load outcome. */
metadata_db_load_outcome_t metadata_db_open(void);
/* For inspection: no folder, identity marker or index migration is written, and
 * the database must not be updated. */
metadata_db_load_outcome_t metadata_db_open_read_only(void);
typedef struct tagcache_snapshot metadata_db_snapshot_t;
metadata_db_snapshot_t *metadata_db_snapshot_open(bool recency);
int metadata_db_snapshot_count(const metadata_db_snapshot_t *snapshot);
bool metadata_db_snapshot_path_at(const metadata_db_snapshot_t *snapshot, int rank, char *out, size_t out_size);
void metadata_db_snapshot_close(metadata_db_snapshot_t *snapshot);
metadata_db_snapshot_t *metadata_db_snapshot_retain(metadata_db_snapshot_t *snapshot);
int metadata_db_snapshot_dup_directory_fd(const metadata_db_snapshot_t *snapshot);
/* True while the currently open database still belongs to the mounted
 * directory captured at open time. */
bool metadata_db_storage_current(void);
int metadata_db_dup_directory_fd(void);
bool metadata_db_numeric_write_failed(void);
void metadata_db_close(void);

/* Reopens the database after an unmount or interrupted operation. A saved
 * database that unexpectedly reloads as FRESH is rejected as a failed load. */
metadata_db_load_outcome_t metadata_db_reload(void);

/* Makes an explicit user-requested rebuild possible after a failed load.
 * Returns true when a writable database is available for scanning. */
bool metadata_db_prepare_rebuild(void);

/* Returns the outcome of the most recent open, update, or abort.
 * FRESH means no saved database files exist; a saved empty database is NORMAL. */
metadata_db_load_outcome_t metadata_db_get_load_outcome(void);
/* Tagcache generation protected by the metadata database lock. The blocking
 * form belongs on workers; the try form never waits and leaves out unchanged
 * when a rebuild owns the lock. */
int32_t metadata_db_get_generation(void);
bool metadata_db_try_get_generation(int32_t * out_generation);
/* Copies the already-loaded persistent catalog identity without waiting for a
 * database operation or performing storage I/O. Returns false with an empty
 * output when the database/identity is unavailable or busy. */
bool metadata_db_try_get_catalog_library_id(char * out_id, size_t out_size);

/* Resolves the Artists row a track belongs to in one lock scope. Returns false
 * without waiting when the database is busy (a split rebuild holds the lock for
 * its whole run), leaving the caller to skip a cosmetic highlight rather than
 * stall the UI. */
bool metadata_db_try_artist_row(const char * raw_artist, int64_t * out_offset);

/* Starts a scan pass against the SD-resident tagcache. Unchanged files
 * are marked seen; new/changed files are upserted. metadata_db_end_update()
 * removes rows whose file is confirmed missing. */
void metadata_db_begin_update(void);
/* Targeted refresh mode keeps all unselected rows and their song IDs. */
void metadata_db_begin_targeted_update(void);
/* Removes a selected path confirmed missing during a targeted refresh. */
bool metadata_db_delete_target_path(const char * path);

/* Looks up `path`, marking it as seen for this pass either way. Returns
 * true and fills *out only if a cached row exists whose stored mtime/size
 * match the values passed in -- callers should treat false as "the file is
 * new or has changed, re-read its real tags and call metadata_db_put()". */
bool metadata_db_get(const char * path, int64_t mtime, int64_t size, cached_tags_t * out);

/* Inserts or replaces the cached row for `path`. */
/* Returns false only when the row existed and its stored tags came out
 * identical (tag strings are stored in their interned spelling). */
bool metadata_db_put(const char * path, int64_t mtime, int64_t size, const cached_tags_t * tags);

/* Ends a successful scan pass: removes only rows whose file is confirmed
 * missing, then commits. Returns false if the on-disk write failed; the
 * last committed generation remains loadable. */
bool metadata_db_end_update(void);

/* Ends an interrupted scan without committing. Reloads the last committed
 * database so staged upserts from the aborted pass are discarded. */
void metadata_db_abort_update(void);

/* ---- Bounded, paged access to the tagcache -- Rockbox-style: the unique
 * tag files and master index are the lookup structure, and nothing beyond
 * the rows a caller actually asks for is copied out. Replaces the old
 * "load every song into one big array" boot-time model for the paged/
 * on-demand queries below. `id` in every row below is the song's stable
 * tagcache slot id (1-based). Deleted slots stay holes and are never reused,
 * so a surviving song keeps its id across rescans. ---- */

typedef struct {
    int64_t id;
    char path[600];
    cached_tags_t tags;
} song_row_t;

typedef enum {
    METADATA_DB_CATALOG_OK,
    METADATA_DB_CATALOG_STALE,
    METADATA_DB_CATALOG_UNAVAILABLE,
    METADATA_DB_CATALOG_BUSY
} metadata_db_catalog_result_t;
typedef bool (*metadata_db_cancel_fn)(void *user_data);
bool metadata_db_catalog_revision_matches(const char *expected_revision);
/* Copies the current revision without fetching any catalog rows. Returns
 * false with an empty output when the catalog is unavailable or the buffer is
 * too small. An available empty library still has a revision. */
bool metadata_db_catalog_get_revision(char *out_revision, size_t out_size);
/* Nonblocking revision validation for UI polling. Returns BUSY if another
 * metadata operation currently owns the lock. */
metadata_db_catalog_result_t metadata_db_try_catalog_revision_check(const char *expected_revision);
bool metadata_db_try_catalog_revision_matches(const char *expected_revision);

/* Opens a pinned catalog snapshot and resolves a full collection into title
 * ranks. Album results are ordered by disc, track (unknown last), then path,
 * matching native album playback. Metadata is read in cancellable 64-candidate
 * lock windows; ranks may be in any order but are unique. */
metadata_db_catalog_result_t metadata_db_filtered_snapshot_open(
    const char *expected_revision, const char *artist, const char *album_artist,
    bool exact_album_artist, const char *album, const char *genre,
    metadata_db_snapshot_t **out_snapshot,
    int **out_title_ranks, int *out_count, metadata_db_cancel_fn is_cancelled,
    void *cancel_userdata);

typedef struct {
    char library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE];
    char revision[METADATA_DB_CATALOG_REVISION_SIZE];
    int64_t total;
    int count;
} metadata_db_catalog_page_t;

typedef struct {
    song_row_t song;
    int64_t album_representative_id; /* 0 means the song has no album group */
} metadata_db_catalog_song_t;

typedef struct {
    song_row_t representative;
    int64_t representative_id;
    int64_t scanned_mtime;
    int64_t scanned_size;
} metadata_db_catalog_cover_t;

typedef struct {
    char name[128];
    /* Genre rows preserve the full tagcache value so long names remain
     * distinct and can be passed back to the genre filter. Empty otherwise. */
    char genre_name[METADATA_DB_TEXT_MAX];
    int song_count;
    int64_t first_song_id; /* one representative song, e.g. for cover art -- not a full song_row_t, callers query that separately if needed */
    /* Only populated by metadata_db_get_albums_page_filtered() below (empty
     * string from metadata_db_get_groups_page(), whose ARTIST/ALBUM_ARTIST
     * kinds have no second identity component) -- see that function's own
     * comment on why an album's real identity is (album, album_artist), not
     * album name alone. */
    char album_artist[128];
} group_row_t;

int64_t metadata_db_get_song_count(void);

/* Distinct artist / album_artist / album counts -- for the group screens'
 * own paging math (total row count) without materializing the groups
 * themselves. */
void metadata_db_get_group_counts(int * out_artist_count, int * out_album_artist_count, int * out_album_count);

/* Offset-paginated page of songs ordered by first_seen DESC (path
 * case-insensitive tiebreak) -- backs the Recently Added screen's compact_list_set_paged_
 * provider(), the same bounded-page-at-a-time shape metadata_db_get_songs_
 * filtered_page() gives All Songs, rather than loading the whole library
 * into RAM. Pair with metadata_db_get_song_recency_offset() below for a
 * "now playing" position in this same order. */
int metadata_db_get_songs_page_by_recency(int offset, int max_rows, song_row_t * out_rows);

/* Offset-paginated page of distinct values, ordered alphabetically
 * (ASCII case-insensitive). ARTIST/ALBUM_ARTIST/ALBUM use tagcache's indexed
 * groups; GENRE streams song ranks and retains only the bounded distinct-name
 * aggregate. Every real album caller wants "all albums" grouped by the *pair*
 * (album, album_artist) instead, which is what metadata_db_get_albums_
 * page_filtered() below does -- ALBUM here would just group by album name
 * alone, merging different artists' same-titled albums, so nothing calls
 * it that way). Returns the number of rows written into out_rows (a
 * caller-owned buffer of at least max_rows entries) -- fewer than max_rows
 * (including 0) means this was the last page. */
typedef enum { METADATA_DB_GROUP_ARTIST, METADATA_DB_GROUP_ALBUM_ARTIST, METADATA_DB_GROUP_ALBUM, METADATA_DB_GROUP_GENRE } metadata_db_group_kind_t;
int metadata_db_get_groups_page(metadata_db_group_kind_t kind, int offset, int max_rows, group_row_t * out_rows);

/* Every song credited to one artist, across all their albums -- Artist ->
 * (their own songs) drill-down. Caller-owned out_rows buffer; returns the
 * number of rows written, capped at max_rows (if a real artist somehow has
 * more songs than max_rows, load_from_cache_only()'s own boot totals
 * already bound how large this can plausibly get -- see its own comment). */
int metadata_db_get_artist_songs(const char * artist, int offset, song_row_t * out_rows, int max_rows);

/* Mirrors metadata_db_get_artist_songs() above, keyed by album_artist
 * instead of artist -- Album Artist -> (their own songs) drill-down. */
int metadata_db_get_album_artist_songs(const char * album_artist, int offset, song_row_t * out_rows, int max_rows);

/* Every song in one album -- keyed by (album, album_artist) together, not
 * album name alone, so two different artists' same-titled albums ("Greatest
 * Hits", a self-titled album) don't collide. */
int metadata_db_get_album_songs(const char * album, const char * album_artist, int offset, song_row_t * out_rows,
                                 int max_rows);

/* mtime and size of a song's database row (the identity the tagcache and the
 * duration cache key a file by). False when the path is not in the library. */
bool metadata_db_get_song_identity(const char * path, int32_t * out_mtime, int32_t * out_size);
/* Visit every song once, taking the database lock in short windows, so
 * workers can aggregate a whole library or one artist without holding it.
 * Stops early (false) when `visit` returns false, the database closes, or the
 * generation changes underneath a library walk. */
bool metadata_db_walk_song_identities(bool (*visit)(const char * path, int32_t mtime, int32_t size, void * user),
                                      void * user);
/* The same for every song of one ARTIST or ALBUM_ARTIST group, in index order.
 * Copies a bounded batch under the lock and calls `visit` after releasing it;
 * only path, album, album_artist, genre and release_year of `song` are filled
 * (the rest is empty). Aborts (false) when the database closes or its
 * generation changes between batches, like the library walk above. */
bool metadata_db_walk_group_songs(metadata_db_group_kind_t kind, const char * name,
                                  bool (*visit)(const song_row_t * song, int32_t mtime, int32_t size, void * user),
                                  void * user);

/* Calls visit for each song whose genre matches (case-insensitively), in title
 * order, at most max_songs of them, from one pass over the song list. Safe from
 * a worker thread: the database lock is not held while visit runs, nor across the
 * whole scan. Returns false when visit stopped it, the database was replaced
 * mid-walk, cancel (optional) was set, or memory ran out; the matches seen so
 * far were still visited. */
bool metadata_db_walk_genre_songs(const char * genre, int max_songs,
                                  bool (*visit)(const song_row_t * song, void * user), void * user,
                                  const atomic_bool * cancel);

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out_row);
bool metadata_db_get_song_by_path(const char * path, song_row_t * out_row);
/* Nonblocking display hint backed only by rows already copied from the
 * database. Returns false on a miss or when the small cache is busy. This is
 * not authoritative for fresh ReplayGain or other playback metadata. */
bool metadata_db_try_get_cached_song_by_path(const char * path, song_row_t * out_row);

/* Batched equivalents of the two functions above. out_rows must have room
 * for `count` entries; out_rows[i].id is set to -1 for any ids[i]/paths[i]
 * that doesn't match a real song, so a caller that needs to skip missing
 * ones just checks that field -- every other field of a -1 entry is left
 * untouched (whatever out_rows[i] already held), not zeroed. */
void metadata_db_get_songs_by_ids(const int64_t * ids, int count, song_row_t * out_rows);
void metadata_db_get_songs_by_paths(const char * const * paths, int count, song_row_t * out_rows);
int64_t metadata_db_get_song_title_offset(const char * path);

/* Same role as metadata_db_get_song_title_offset() above, but for the
 * Recently Added screen's own order -- see metadata_db_get_songs_page_by_
 * recency()'s own comment for the exact ORDER BY this matches. */
int64_t metadata_db_get_song_recency_offset(const char * path);
int64_t metadata_db_get_group_offset(metadata_db_group_kind_t kind, const char * name, const char * album_artist);

/* 27-entry (A-Z, then '#') table of "display offset of the first entry at
 * or after this letter", matching gui.c's "jump forward to nearest match"
 * semantics. Built by walking the already-sorted in-memory group or title
 * index once -- out_table[i] for i in 0..25 is a letter's offset (-1 if
 * nothing sorts at/after it); out_table[26] is 0 if any row sorts before
 * 'A' (digits/symbols), else -1. kind selects which of the four top-level
 * library screens' own order to match: ARTIST/ALBUM_ARTIST/ALBUM as in
 * metadata_db_get_groups_page()/metadata_db_get_albums_page_filtered()
 * (offsets are into that grouped list), or METADATA_DB_AZ_ALL_SONGS for
 * the ungrouped All Songs screen's title-ordered list. */
typedef enum {
    METADATA_DB_AZ_ARTIST,
    METADATA_DB_AZ_ALBUM_ARTIST,
    METADATA_DB_AZ_ALBUM,
    METADATA_DB_AZ_ALL_SONGS
} metadata_db_az_kind_t;
void metadata_db_get_az_table(metadata_db_az_kind_t kind, int out_table[27]);

typedef struct {
    char label[128];
    int offset; /* position in the same unfiltered sequence metadata_db_get_groups_page()/
                 * get_albums_page_filtered()/get_songs_filtered_page() themselves page
                 * through for this kind -- a clicked hit means the same thing as an
                 * ordinary unfiltered row tap at that offset. */
} metadata_db_search_hit_t;

/* Case-insensitive substring search (matching gui.c's own former in-memory
 * search_matches() semantics -- plain substring, not a raw LIKE pattern, so
 * the needle's own '%'/'_'/'\' are escaped first) against the column each
 * screen's own paged display leads its ORDER BY with, capped at max_rows
 * results (same bounded-search-cache convention as metadata_db_search_
 * songs()'s own cap) and returned in that same alphabetical order. An empty
 * needle always returns 0 (gui.c's search shows nothing until you've
 * actually typed something, not the unfiltered list). METADATA_DB_AZ_ALL_
 * SONGS matches against the raw title column only, not the basename
 * fallback metadata_db_song_display_title() shows for an untagged file (no
 * embedded title) -- same accepted divergence class as metadata_db_get_az_
 * table()'s own Unicode-ordering caveat, not worth a second query pass for
 * what's normally a small minority of a library. */
int metadata_db_search_names(metadata_db_az_kind_t kind, const char * needle, int max_rows,
                              metadata_db_search_hit_t * out_hits);

/* row->tags.title verbatim, or the file's own basename (with extension) if
 * title is empty (no embedded title tag) -- matches gui.c's own
 * song_display_title_of() fallback for the local on-device list screens,
 * so every song_row_t consumer (remote_control.c, plugin.library_*) shows
 * the same thing for an untagged file instead of a blank title. */
void metadata_db_song_display_title(const song_row_t * row, char * out, size_t out_size);

/* Revision-consistent, bounded snapshots for remote library sync. Each page
 * captures library ID, tagcache generation, total, and rows under one database
 * guard. `expected_revision == NULL` is accepted only for the initial songs
 * page; covers and source resolution require a revision. */
metadata_db_catalog_result_t metadata_db_catalog_songs_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_song_t * out_rows);
metadata_db_catalog_result_t metadata_db_catalog_covers_page(
    const char * expected_revision, int offset, int limit,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_rows);
metadata_db_catalog_result_t metadata_db_catalog_cover_source(
    const char * expected_revision, int64_t representative_id,
    metadata_db_catalog_page_t * out_page, metadata_db_catalog_cover_t * out_source);
/* Validates a catalog-origin numeric song id against a supplied revision
 * inside one metadata DB guard. */
metadata_db_catalog_result_t metadata_db_catalog_validate_song_revision(
    const char * expected_revision, int64_t song_id);
/* Revalidates a catalog-origin ID and copies its concrete row in the same DB
 * guard, closing the gap between HTTP acceptance and UI-thread consumption. */
metadata_db_catalog_result_t metadata_db_catalog_get_song_revision(
    const char * expected_revision, int64_t song_id, song_row_t * out_song);

/* Title/artist substring search (case-insensitive), ordered by title then
 * stable id -- capped at max_rows, always replace-not-append the caller's
 * previous result set (see this app's own bounded-search-cache convention). */
int metadata_db_search_songs(const char * query, song_row_t * out_rows, int max_rows);

/* remote_control.c's GET /api/library: query (title/artist substring) and
 * exact-match filters (artist/album_artist/album/genre) are each
 * independently optional -- NULL or "" skips that condition. Offset-based
 * (not keyset) since a remote HTTP client passes an arbitrary offset it
 * doesn't control the shape of, same paging contract that endpoint already
 * had against remote_control.c's own now-removed in-memory copy. Query the
 * count first (metadata_db_count_songs_filtered()) for the response's own
 * "total" field, then the page. */
int64_t metadata_db_count_songs_filtered(const char * query, const char * artist_filter,
                                          const char * album_artist_filter, const char * album_filter,
                                          const char * genre_filter);
int metadata_db_get_songs_filtered_page(const char * query, const char * artist_filter,
                                         const char * album_artist_filter, const char * album_filter,
                                         const char * genre_filter, int offset, int max_rows, song_row_t * out_rows);

/* Albums grouped and filtered by artist OR album_artist matching one name.
 * filter may be NULL/"" for every album, unfiltered. Groups by distinct
 * (album, album_artist) pairs so identically named albums remain separate;
 * out_rows[i].album_artist carries the album artist identity. Offset-paginated. */
int metadata_db_get_albums_page_filtered(const char * artist_or_album_artist_filter, int offset, int max_rows,
                                         group_row_t * out_rows);
int metadata_db_get_albums_page_sorted(const char * artist_filter, metadata_db_album_sort_t sort,
                                      int offset, int max_rows, group_row_t * out_rows);
int metadata_db_get_album_sorted_offset(metadata_db_album_sort_t sort, const char * album,
                                        const char * album_artist);
int metadata_db_album_canonical_to_sorted(metadata_db_album_sort_t sort, int canonical_rank);
int metadata_db_album_sorted_to_canonical(metadata_db_album_sort_t sort, int sorted_rank);
bool metadata_db_album_sort_available(metadata_db_album_sort_t sort);

/* Exact Artist/Album-Artist drill-down variants. Unlike the legacy remote
 * helper above, these filter only the requested tag column. */
int64_t metadata_db_get_album_for_group_offset(metadata_db_group_kind_t kind, const char *name,
                                               const char *album, const char *album_artist);
int64_t metadata_db_get_album_for_group_sorted_offset(metadata_db_group_kind_t kind, const char *name,
                                                      metadata_db_album_sort_t sort,
                                                      const char *album, const char *album_artist);
int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name);

/* One genre's distinct artists (ARTIST), album artists (ALBUM_ARTIST, the
 * artist when a song has none) or albums (ALBUM, keyed by album and album
 * artist), alphabetical, for browsing a genre by album (issue #191). For
 * ALBUM, artist_filter keeps albums credited to that artist (artist_kind
 * ARTIST) or album artist (artist_kind ALBUM_ARTIST). Like the GENRE groups it
 * streams every song and keeps at most 2000 groups; song_count counts the
 * genre's songs only. *out_total, when given, receives the group count. */
int metadata_db_get_genre_groups_page(metadata_db_group_kind_t kind, const char * genre,
                                      metadata_db_group_kind_t artist_kind, const char * artist_filter,
                                      int offset, int max_rows, group_row_t * out_rows, int * out_total);
int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char * name, int offset, int max_rows,
                                      group_row_t * out_rows);
int metadata_db_get_albums_for_group_sorted(metadata_db_group_kind_t kind, const char * name,
                                            metadata_db_album_sort_t sort, int offset, int max_rows,
                                            group_row_t * out_rows);

/* Per-song favorite flag, keyed by path. Local scan rows store this as the
 * Rockbox rating tag. Paths that are not tagcache rows (remote:// plugin
 * tracks, Subsonic stream URLs, unscanned files) persist in a sidecar so
 * the heart and play count accumulate across plays. Favorites / Most Played
 * screens still list only rows present in the local library. */
bool metadata_db_song_favorite_is_set(const char * path);
void metadata_db_song_favorite_set(const char * path, bool is_favorite);
/* Same write, but returns only after it is on disk: the tagcache numeric
 * flush for library rows (waited for outside the database lock), the
 * sidecar save otherwise. False when it did not persist. Slower; for
 * callers that must confirm the write. */
bool metadata_db_song_favorite_set_durable(const char * path, bool is_favorite);

/* Enumerates every favorited path that's also still in the current media
 * cache, alphabetically. */
void metadata_db_load_favorite_songs(char *** out_paths, int * out_count);

/* Bumps path's play count by one and stamps last_played. Local library
 * rows update tagcache; any other path creates/updates a sidecar row with
 * count=1 on first play. Called once per real "this track started playing"
 * event (explicit pick and gapless auto-advance, see gui.c's
 * apply_track_metadata_to_ui()), never on every UI refresh of the same
 * still-playing track. */
void metadata_db_song_play_count_increment(const char * path);

/* Enumerates up to `limit` paths with the highest play count (ties broken
 * by most-recently-played), filtered against the current media cache same
 * as metadata_db_load_favorite_songs() -- backs the "Most Played"
 * auto-generated playlist. Caller-owned array; *out_paths is NULL and
 * *out_count is 0 if there's no play history yet. */
void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count);

/* Recently Played: library songs with a last-played stamp, newest first
 * (ties by ascending song ID). tagcache keeps only each song's latest play
 * time, not a play log, so a song appears once however often it was played;
 * plays of paths outside the library (remote_state sidecar) are not listed.
 * Both calls see at most the newest METADATA_DB_RECENTLY_PLAYED_MAX songs.
 * The loader follows metadata_db_load_top_played_songs()'s array convention.
 * The page call writes up to max_rows rows from offset into out_rows and,
 * when non-NULL, each row's Unix play time into out_last_played; *out_total
 * receives the capped number of available songs. Returns rows written, or
 * -1 when the database is unavailable or out of memory. */
#define METADATA_DB_RECENTLY_PLAYED_MAX 500
void metadata_db_load_recently_played_songs(int limit, char *** out_paths, int * out_count);
int metadata_db_get_recently_played_page(int offset, int max_rows, song_row_t * out_rows,
                                         int64_t * out_last_played, int * out_total);

/* Enumerates up to `limit` paths, most-recently-added first (first_seen,
 * set only when a path is first inserted). A later retag bumps mtime but
 * must not resurface the song at the top of this list. Caller-owned array;
 * *out_paths is NULL and *out_count is 0 if the cache is empty or unopened. */
void metadata_db_load_recently_added_songs(int limit, char *** out_paths, int * out_count);

/* Books live in a path-list sidecar (not tagcache). Same caller-owned
 * array convention as the song enumerators above. */
bool metadata_db_book_favorite_is_set(const char * path);
void metadata_db_book_favorite_set(const char * path, bool is_favorite);
void metadata_db_book_replace_all(char * const * paths, int count);
void metadata_db_load_favorite_books(char *** out_paths, int * out_count);
void metadata_db_load_all_books(char *** out_paths, int * out_count);

/* Persistent playlist (.m3u) cache -- same "load whatever's cached, don't
 * touch the filesystem" reasoning as the book cache above (the Playlists
 * screen in gui.c was slow to open, ~5s against a real SD card, because it
 * ran playlist_files_scan() on every visit). Unlike books there's no fast
 * stock-db path to warm-start from (m3u playlists are this app's own
 * concept, not something the stock player ever indexes) -- populated by
 * the single-row insert/delete functions below for ordinary in-app
 * playlist create/delete, so those stay instant without a full rescan. */

/* Enumerates every cached playlist path, alphabetically -- caller-owned
 * array, same convention as metadata_db_load_all_books(). */
void metadata_db_load_all_playlists(char *** out_paths, int * out_count);

/* Adds/removes a single path from the playlist cache -- for gui.c's own
 * playlist_files_create()/playlist_files_delete() call sites, so creating
 * or deleting one playlist doesn't pay for a full rescan just to reflect
 * that one file. INSERT is a no-op if the path's already cached; DELETE is
 * a no-op if it wasn't. */
void metadata_db_playlist_insert_one(const char * path);
void metadata_db_playlist_delete_one(const char * path);

/* Saved Subsonic server profiles for the multi-server list in GUI setup.
 * url is the unique key (saving replaces existing row). Password is stored
 * in plain text. Persisted on /usr/data (see subsonic_saved_servers.c). */
void metadata_db_subsonic_server_save(const char * url, const char * username, const char * password, bool verify_tls);

/* Enumerates every saved server, alphabetically by URL -- caller-owned
 * array of subsonic_server_row_t, same convention as
 * metadata_db_load_all_books(). *out_rows is NULL and *out_count is 0 if
 * there are none. */
typedef struct {
    char url[256];
    char username[128];
    char password[128];
    bool verify_tls;
} subsonic_server_row_t;
void metadata_db_load_subsonic_servers(subsonic_server_row_t ** out_rows, int * out_count);

#endif /* METADATA_DB_H */
