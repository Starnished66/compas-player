#ifndef FAVORITE_WRITER_H
#define FAVORITE_WRITER_H

#include <stdbool.h>

/* Single ordered writer for per-song favorite flags, shared by the player's
 * heart and Remote Control. Writes to one path never run concurrently, so
 * they land in the order they were taken; a later write supersedes an
 * earlier one still waiting. */

/* Starts the worker thread. Idempotent; the calls below start it on first
 * use too, but the player calls this at init so the first heart tap does
 * not pay for thread setup on the LVGL thread. */
void favorite_writer_start(void);

/* Debounced asynchronous write for the player's heart: rapid taps on one
 * path collapse into the last state, persisted off the calling thread. */
void favorite_writer_submit(const char * path, bool is_favorite);

/* Synchronous durable write. Replaces a pending debounced write for path,
 * waits for one already being written, and returns whether the new state
 * reached the card (metadata_db_song_favorite_set_durable()). If a later
 * request for the same path arrives while it waits, that request wins: this
 * one is skipped and returns true. */
bool favorite_writer_write_now(const char * path, bool is_favorite);

/* Effective state: a queued or in-flight write for path wins over the
 * stored flag. */
bool favorite_writer_is_set(const char * path);

#endif /* FAVORITE_WRITER_H */
