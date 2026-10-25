#ifndef LIBRARY_SCAN_DECISION_H
#define LIBRARY_SCAN_DECISION_H

#include <stdbool.h>
#include <stdint.h>
#include "metadata_db.h"
#include "metadata_refresh_targets.h"

/* The scanner's decisions about whether a database needs writing, shared by
 * the player's scan (gui_library.c) and compas-sdtool so both agree. */

/* True when the files found could still match the database exactly, so the
 * cheap proof (every file has an up-to-date row) is worth trying. A database
 * that was recovered or is being rebuilt is never left as it is, and a walk
 * that skipped anything is incomplete. */
static inline bool library_scan_may_skip_update(metadata_db_load_outcome_t open_outcome,
                                                bool preserve_rebuild, int skipped,
                                                int discovered, int64_t song_count) {
    return open_outcome == METADATA_DB_LOAD_SUCCESS_NORMAL && !preserve_rebuild && skipped == 0 &&
           discovered >= 0 && song_count == (int64_t) discovered;
}

/* A rebuild must not replace a library with an empty one. */
static inline bool library_scan_rebuild_rejected(bool preserve_rebuild, int discovered) {
    return preserve_rebuild && discovered == 0;
}

/* After a forced refresh: nothing changed, was added or is missing, so
 * committing would only rewrite the same library. */
static inline bool library_scan_refresh_skips_commit(bool force_all, bool preserve_rebuild, int skipped,
                                                     int changed, bool row_inserted, bool missing_paths) {
    return force_all && !preserve_rebuild && skipped == 0 && !missing_paths &&
           metadata_refresh_should_skip_commit(changed, false, row_inserted);
}

#endif /* LIBRARY_SCAN_DECISION_H */
