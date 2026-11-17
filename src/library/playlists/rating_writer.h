#ifndef RATING_WRITER_H
#define RATING_WRITER_H

#include <stdbool.h>

/* Keys are opaque strings, including album:// identities. */

/* Single ordered writer for per-song ratings, shared by the player's
 * heart and Remote Control. Writes to one path never run concurrently, so
 * they land in the order they were taken; a later write supersedes an
 * earlier one still waiting. */

/* Starts the worker thread. Idempotent; the calls below start it on first
 * use too, but the player calls this at init so the first heart tap does
 * not pay for thread setup on the LVGL thread. */
void rating_writer_start(void);

/* Debounced asynchronous write for the player's heart: rapid taps on one
 * path collapse into the last state, persisted off the calling thread. */
void rating_writer_submit(const char * path, int stars);
/* Forces every debounced request due and waits until the ordered writer has
 * handed them to metadata persistence. Follow with tagcache_flush_numeric(). */
bool rating_writer_flush(void);

/* Synchronous durable write. Replaces a pending debounced write for path,
 * waits for one already being written, and returns whether the new state
 * reached the card (metadata_db_song_rating_set_durable()). If a later
 * request for the same path arrives while it waits, that request wins: this
 * one is skipped and returns true. */
bool rating_writer_write_now(const char * path, int stars);

/* Effective state: a queued or in-flight write for path wins over the
 * stored rating. */
int rating_writer_effective_stars(const char *path, int stored);
int rating_writer_stars(const char * path);

#endif /* RATING_WRITER_H */
