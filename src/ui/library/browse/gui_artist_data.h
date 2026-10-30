#pragma once
/* Everything the artist page shows that needs more than the paged album rows:
 * year and track count per album, the year span, the most common genre, the
 * first albums' covers and the total playing time.
 *
 * One worker walks the artist's songs once per open (the database is only
 * touched there, never per row on the UI thread) and aggregates; the total
 * time then comes from the duration cache, and the tracks it misses are probed
 * lazily on the existing track-probe worker while the page is visible. The UI
 * thread only reads the published summary. */
#include <stdbool.h>
#include <stdint.h>
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/metadata/durations/duration_cache.h"

#define ARTIST_PAGE_MOSAIC_ALBUMS 4

typedef struct {
    bool ready;            /* the aggregate below is valid */
    int songs, albums;     /* counted from the songs walked */
    int year_min, year_max; /* 0 when no song carries a year */
    char genre[128];       /* most common, never "Unknown Genre"; may be empty */
    bool duration_known;   /* every track's duration is known */
    double total_seconds;
    int mosaic_count;
    int64_t mosaic_song_id[ARTIST_PAGE_MOSAIC_ALBUMS];
    uint64_t mosaic_key[ARTIST_PAGE_MOSAIC_ALBUMS];
} gui_artist_summary_t;

/* UI thread. Starts (or keeps, for the same artist, sort and database
 * generation) the aggregation worker. While too many cancelled workers are still
 * finishing, the start is deferred to a later poll instead of waiting for them. */
void gui_artist_data_open(metadata_db_group_kind_t kind, const char * name, metadata_db_album_sort_t sort);
/* UI thread. Cancels what is running; the page is gone or being replaced. */
void gui_artist_data_close(void);
/* Independent change counters, so the page redraws only what changed: `albums`
 * (the per-album year and track counts the row subtitles show), `summary` (the
 * counts, genre, year span and mosaic albums) and `duration` (the total time
 * chip, which only changes when it appears or its value does). */
typedef struct { uint32_t albums, summary, duration; } gui_artist_revisions_t;
/* Copies the summary and the revisions it belongs to under one lock, so a
 * publication can never be paired with the previous revision. Returns whether
 * the aggregate has arrived. */
bool gui_artist_data_snapshot(gui_artist_summary_t * out, gui_artist_revisions_t * rev);
/* Any thread. Year (0 unknown) and track count of one album of this artist;
 * false until the aggregate has arrived or when the album is not found.
 * album_artist may be empty (match by album name alone). */
bool gui_artist_data_album(const char * album, const char * album_artist, int * year, int * tracks);
/* The same lookup with the album artist given as gui_artist_data_album_key() of it (0 = any). */
uint64_t gui_artist_data_album_key(const char * album_artist);
bool gui_artist_data_album_by_key(const char * album, uint64_t album_artist_key, int * year, int * tracks);
/* UI thread, every probe tick. Reaps the worker and runs the lazy probe of the
 * cache misses while `visible`. */
void gui_artist_data_poll(bool visible);
/* The aggregate worker or a probe batch is still running. */
bool gui_artist_data_busy(void);
/* Final-shutdown cleanup: waits for the workers and flushes the duration cache,
 * all within `deadline_ms` (a worker_deadline_in_ms() value shared with the
 * other shutdown steps). */
void gui_artist_data_shutdown(int64_t deadline_ms);
