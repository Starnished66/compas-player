#pragma once

#include <stdint.h>
#include "metadata_db.h"

typedef enum {
    SELECTION_SNAPSHOT_ALL_SONGS,
    SELECTION_SNAPSHOT_RECENTLY_ADDED,
    SELECTION_SNAPSHOT_GROUP
} selection_snapshot_source_t;

/* Store owns a copy of the row identity and tags. Seed uses the shared cache
 * without waiting; a miss or lock contention clears the pending selection. */
void library_selection_snapshot_store(const song_row_t * row,
                                      selection_snapshot_source_t source, int rank,
                                      int32_t generation);
void library_selection_snapshot_seed(selection_snapshot_source_t source, int rank,
                                     int64_t song_id, const char * path);
