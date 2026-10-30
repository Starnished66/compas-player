#ifndef DURATION_CACHE_INTERNAL_H
#define DURATION_CACHE_INTERNAL_H

/* Shared by duration_cache.c (state and worker) and duration_cache_file.c
 * (the on-card format). Not for other callers. */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t key;
    uint32_t duration_ms, sample_rate;
    uint8_t bit_depth, codec, flags;
} dc_entry_t;

/* An immutable sorted run of records. `refs` is guarded by the cache lock: the
 * worker takes a reference to read or merge a table outside that lock, and the
 * last holder frees it, so a storage reset never frees memory in use. */
typedef struct {
    size_t count;
    int refs;
    dc_entry_t e[];
} dc_table_t;

int dc_cmp_entry(const void * a, const void * b);
dc_table_t * dc_table_alloc(size_t count);

/* The directory of the file, opened when the file is loaded and kept for the
 * epoch. Writes go through it (openat/renameat), so a write can only ever land
 * on the filesystem the table was loaded from, even if the path is remounted
 * to another card meanwhile. */
typedef struct {
    int dirfd;               /* -1 when the directory could not be opened */
    char name[256];          /* the file's name inside it */
} dc_store_t;

typedef enum {
    DC_LOAD_OK,              /* a valid table */
    DC_LOAD_EMPTY,           /* missing, or damaged and discarded: start empty, a write replaces it */
    DC_LOAD_FAILED           /* could not be read (I/O error, no memory): never overwrite what is there */
} dc_load_status_t;

/* Opens the directory (creating it when missing), then reads and validates the
 * file. `*table` is NULL unless DC_LOAD_OK. Damaged records are dropped; a bad
 * header or size discards the whole file. */
dc_load_status_t dc_file_load(const char * path, dc_store_t * store, dc_table_t ** table);
void dc_store_close(dc_store_t * store);

/* Atomically replaces the file with `base` merged with `extra` (sorted, newer
 * values win), inside `dirfd` (which the caller owns). `still_current` is asked
 * just before the file is created and again before it is renamed into place,
 * so a write that belongs to a card that has since been replaced never lands. */
bool dc_file_write(int dirfd, const char * name, const dc_table_t * base, const dc_entry_t * extra,
                   size_t extra_count, bool (*still_current)(void * ctx), void * ctx);

/* ---- Cache state shared by duration_cache.c and duration_cache_merge.c ----
 * Everything below is guarded by dc_lock unless noted. */
#define DURATION_PENDING_MAX 256u
typedef enum { STATE_UNLOADED, STATE_LOADING, STATE_LOADED } load_state_t;

extern pthread_mutex_t dc_lock;
extern pthread_mutex_t dc_merge_lock; /* merge and prune swap the table */
extern pthread_cond_t dc_flushed_cond;
extern load_state_t dc_state;
extern dc_table_t * dc_cur;
extern dc_entry_t dc_pending[], dc_inflight[];
extern size_t dc_pending_count, dc_inflight_count;
extern uint32_t dc_epoch;
extern int64_t dc_merge_backoff_ms;

void dc_table_ref(dc_table_t * t);
void dc_table_unref(dc_table_t * t);
const dc_entry_t * dc_find_run(const dc_entry_t * run, size_t n, uint64_t key);
int64_t dc_now_ms(void);
void dc_note_dirty_locked(void);

/* Folds the queued puts into the table. False when memory ran out (the batch
 * is handed back and a back-off is set). */
bool dc_do_merge(void);

#endif
