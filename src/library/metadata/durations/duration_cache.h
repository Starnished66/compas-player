#ifndef DURATION_CACHE_H
#define DURATION_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-track duration and format, kept on the card in one compact file
 * (<sd>/.compas/durations.bin) so album and artist pages show their totals
 * without probing every file again.
 *
 * A track is identified by a hash of its path plus the file's mtime and size,
 * the same identity the tagcache uses to decide that a file changed: a retagged
 * or replaced file simply misses and is probed again. Entries are RAM-resident
 * (sorted array, binary search), loaded lazily on a worker thread, and written
 * back atomically (temp file, fsync, rename), debounced and off the caller's
 * thread. A card that is read-only or full only loses persistence.
 *
 * Threading: lookups and puts take a short mutex and never do I/O or merge
 * work. A put is a bounded enqueue (it is dropped if the queue is full, and
 * the track is simply probed again); one background worker loads the file,
 * folds the queue into the sorted table and writes it back from an owned
 * snapshot, five seconds after the last put (and at the latest sixty seconds
 * after the first unwritten one). A card change calls
 * duration_cache_new_storage_epoch(): RAM records are dropped and any load or
 * write job of the old card is ignored.
 *
 * File: 16-byte header (magic "CDUR", version, entry count, header check) then
 * `count` 24-byte records sorted by key, each with its own check word. Any
 * mismatch discards the file; the next write replaces it. */

#define DURATION_CACHE_MAX_ENTRIES 100000u
#define DURATION_CACHE_FLAG_DSD 1u
/* The decoder rejected this file (for example FLAC in fragmented MP4): the
 * record has duration 0 and its `bit_depth` byte holds the probe revision. */
#define DURATION_CACHE_FLAG_FAILED 2u

/* Bump when the decoders gain formats, so remembered failures are probed again. */
#define DURATION_PROBE_REVISION 1

typedef struct {
    uint32_t duration_ms;
    uint32_t sample_rate;
    uint8_t bit_depth;
    uint8_t codec;   /* audio_codec_t */
    uint8_t flags;   /* DURATION_CACHE_FLAG_* */
} duration_info_t;

/* mtime and size are folded to 32 bits, as the tagcache stores them. */
uint64_t duration_cache_key(const char * path, int64_t mtime, int64_t size);

/* Never blocks: false while the file is still loading (and starts the load). */
bool duration_cache_get_key(uint64_t key, duration_info_t * out);
bool duration_cache_get(const char * path, int64_t mtime, int64_t size, duration_info_t * out);
/* Worker threads only: waits for the load to finish first. False when the cache
 * cannot load (no card mounted): the table then stays unloaded, never an empty one. */
bool duration_cache_get_blocking(uint64_t key, duration_info_t * out);
bool duration_cache_wait_loaded(void);

/* The current storage epoch. Take it before reading the cache (or measuring a
 * file) and pass it back with the result. */
uint32_t duration_cache_epoch(void);
/* Never blocks on I/O or merges. Ignored for zero durations (except FAILED
 * records), once the cache is full, and when `epoch` is no longer the current
 * one (the card changed since the measurement began). */
void duration_cache_put(uint32_t epoch, const char * path, int64_t mtime, int64_t size, const duration_info_t * info);
/* Remembers that the decoder rejected the file, under the current probe revision. */
void duration_cache_put_failed(uint32_t epoch, const char * path, int64_t mtime, int64_t size);
/* A FAILED record of another probe revision is a miss, so the file is probed again. */
bool duration_info_is_failed(const duration_info_t * info);

size_t duration_cache_count(void);
/* Writes now if anything changed (blocking up to a minute; tests). */
bool duration_cache_flush(void);
/* The same, but gives up after `timeout_ms` and returns false: the final
 * shutdown path, which must not wait on a stuck card. */
bool duration_cache_flush_bounded(uint32_t timeout_ms);
/* Worker threads and tests: waits until every queued put is in the sorted table. */
void duration_cache_wait_merged(void);
/* Worker threads only: keeps just the entries whose key is in `valid` (sorted
 * ascending, `count` long). `epoch` is the one taken before `valid` was built;
 * a card change since then makes this do nothing. Returns how many were dropped. */
size_t duration_cache_prune(uint32_t epoch, const uint64_t * valid, size_t count);

/* The SD card was removed, inserted, replaced or remounted, or the library was
 * reopened: forgets everything held in
 * RAM (unwritten records of the old card are discarded, never written to the
 * new one), ignores load and write jobs still running for the old card, and
 * reloads lazily from the new card on the next lookup. Never blocks. */
void duration_cache_new_storage_epoch(void);

/* Whether the card is mounted; asked on the load worker only. Without a check
 * the card is taken to be there. While it is not, nothing is loaded and nothing
 * is written (the cache stays unloaded rather than empty). */
void duration_cache_set_mount_check(bool (*is_mounted)(void));
/* True when this epoch's file could not be read: records live in RAM only and
 * the file is never overwritten. */
bool duration_cache_read_only(void);

/* Tests: use another file, forget everything held in RAM (the file stays), and
 * shorten the write debounce / maximum dirty age (milliseconds). */
void duration_cache_set_path(const char * path);
void duration_cache_reset(void);
void duration_cache_set_timing(uint32_t debounce_ms, uint32_t max_age_ms);

#endif
