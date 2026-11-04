#ifndef METADATA_DB_INTERNAL_H
#define METADATA_DB_INTERNAL_H

/* Private to the metadata_db translation units: the shared lock, database
 * state and row-copy helpers. metadata_db.c owns every definition here except
 * metadata_db_genre_cache_free (metadata_db_browse.c). Lock order is the update mutex,
 * then the database mutex, then the display song cache mutex. */

#include "metadata_db.h"

#include "../../tagcache/core/tagcache.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

extern bool metadata_db_db_ready;
extern uint32_t metadata_db_db_incarnation;
extern char metadata_db_catalog_library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE];
extern bool metadata_db_catalog_rebuild_pending;
extern pthread_once_t metadata_db_mutex_once;
extern pthread_mutex_t metadata_db_mutex;
extern _Thread_local unsigned metadata_query_depth;
void metadata_db_mutex_init(void);

typedef struct { bool locked; } metadata_db_guard_t;
static inline void metadata_db_guard_release(metadata_db_guard_t * guard) {
    if (guard->locked) { metadata_query_depth--; pthread_mutex_unlock(&metadata_db_mutex); }
}
#define METADATA_DB_GUARD \
    pthread_once(&metadata_db_mutex_once, metadata_db_mutex_init); \
    pthread_mutex_lock(&metadata_db_mutex); \
    metadata_query_depth++; \
    metadata_db_guard_t metadata_db_guard __attribute__((cleanup(metadata_db_guard_release))) = { true }

bool metadata_db_query_try_lock(void);
void metadata_db_query_unlock(void);
bool metadata_db_catalog_identity_prepare(void);
void metadata_db_genre_cache_free(void);
void metadata_db_copy_song(const tagcache_song_t * src, song_row_t * dst);
void metadata_db_copy_group(const tagcache_group_t * src, group_row_t * dst);

#endif
