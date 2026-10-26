#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../db/metadata_db.h"

typedef enum {
    LIBRARY_SCAN_MODE_NORMAL,
    LIBRARY_SCAN_MODE_FORCE_METADATA,
    LIBRARY_SCAN_MODE_TARGETED
} library_scan_mode_t;

typedef enum {
    LIBRARY_SCAN_SCOPE_NONE,
    LIBRARY_SCAN_SCOPE_PATHS,
    LIBRARY_SCAN_SCOPE_ALBUM,
    LIBRARY_SCAN_SCOPE_ARTIST,
    LIBRARY_SCAN_SCOPE_ALBUM_ARTIST
} library_scan_scope_t;

enum {
    LIBRARY_SCAN_PHASE_WALK,
    LIBRARY_SCAN_PHASE_TAGS,
    LIBRARY_SCAN_PHASE_SAVE
};

typedef struct {
    library_scan_mode_t mode;
    bool allow_rebuild;
    char **target_paths; /* Ownership transfers to the scanner; no path copies are made. */
    int target_count;
    library_scan_scope_t scope;
    const char *scope_name;
    const char *scope_album_artist;
    metadata_db_load_outcome_t outcome_seed;
    void (*books_rescan)(void);
    void (*playlists_rescan)(void);
} library_scan_request_t;

typedef struct {
    bool succeeded;
    bool incomplete;
    bool load_blocked;
    bool saved;
    bool published;
    bool identity_changed;
    int refreshed_count;
    int changed_count;
    int failed_count;
    int matched_existing;
    metadata_db_load_outcome_t operation_outcome;
} library_scan_result_t;

typedef struct {
    int total;
    int done;
    int walk_items;
    int phase;
} library_scan_progress_t;

/* Prepare the single in-flight job before creating its worker thread. */
void library_scan_prepare(const library_scan_request_t *request);
void library_scan_run(void);
/* Preserve the coordinator's failed-thread-start result before it releases
 * the prepared request and target paths. */
void library_scan_record_start_failure(void);
/* Legacy no-argument GUI entry point reuses current policy/result and target
 * ownership while supplying its original rescan hooks. */
void library_scan_run_full_current(void (*books_rescan)(void),
                                   void (*playlists_rescan)(void));
/* Releases the transferred path array at the coordinator's historical cleanup points. */
void library_scan_release(void);
void library_scan_get_result(library_scan_result_t *out);
void library_scan_get_progress(library_scan_progress_t *out);
void library_scan_reset_progress(void);
void library_scan_request_cancel(void);
void library_scan_reset_cancel(void);
bool library_scan_cancel_pending(void);
