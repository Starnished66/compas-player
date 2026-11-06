#ifndef SUBSONIC_REFRESH_H
#define SUBSONIC_REFRESH_H

#include <stdbool.h>

#include "subsonic_cache.h"

/* One background worker for the server checks of the cached lists, apart from
 * the blocking browse worker, plus one-shot cache reads. No UI code: the UI
 * thread starts a job, then collects it from its poll. At most one worker job
 * runs at a time. */

typedef enum {
    SUBSONIC_REFRESH_FETCH, /* ask the server whether the list changed, fetch it if so */
    SUBSONIC_REFRESH_PING,  /* only check the server answers */
    SUBSONIC_REFRESH_PRUNE  /* drop the oldest songs and cover files beyond their caps */
} subsonic_refresh_mode_t;

typedef struct {
    subsonic_refresh_mode_t mode;
    subsonic_list_kind_t kind;
    subsonic_server_t server;
    char id[64];                  /* FETCH of a songs kind: the album or playlist */
    subsonic_library_stamp_t save_stamp; /* FETCH of a songs kind: what the file is saved under */
    bool force;                   /* FETCH: skip the change check (manual refresh) */
    subsonic_cache_entry_t entry; /* in: what the shown list is based on; out: the updated entry */
    bool ok;                      /* FETCH: server answered; PING: reachable */
    bool not_found;               /* FETCH of a songs kind: the server no longer has it (its file is gone) */
    bool changed;                 /* FETCH: items is a new list (the caller owns and frees it) */
    void * items;                 /* FETCH result */
    int count;
} subsonic_refresh_job_t;

/* Copies the request and starts it. False while another job (even a cancelled
 * one that is still winding down) has not been collected. UI thread only. */
bool subsonic_refresh_start(const subsonic_refresh_job_t * request);

bool subsonic_refresh_active(void);

/* Stops the running job's network use; its result is dropped by _take(). */
void subsonic_refresh_cancel(void);

/* Hands over the finished job and returns true, or returns false while it is
 * still running, when none runs, or when it was cancelled (the result is freed). */
bool subsonic_refresh_take(subsonic_refresh_job_t * out);

/* Cancels, waits for the worker and frees whatever it produced. */
void subsonic_refresh_shutdown(void);

/* A cache read on its own detached thread, so opening a list never stops the
 * worker. The UI keeps the handle of the read it still wants: _drop() on it
 * (or a newer read replacing it) makes the thread free the result itself. */
typedef struct subsonic_load subsonic_load_t;

typedef struct {
    bool ok; /* false: unreadable, items is NULL */
    void * items;
    int count;
    subsonic_cache_entry_t entry;
} subsonic_load_result_t;

/* Reads in flight at once, abandoned ones included, so switching servers
 * quickly cannot pile up threads. */
#define SUBSONIC_LOAD_MAX 3

int subsonic_load_inflight(void);

subsonic_load_t * subsonic_load_start(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id);

/* Hands over a finished read (the caller frees result->items) and clears *load;
 * false while it still runs. */
bool subsonic_load_take(subsonic_load_t ** load, subsonic_load_result_t * result);

/* Gives up the read and clears *load. */
void subsonic_load_drop(subsonic_load_t ** load);

/* What one list kind (artists, albums, playlists, or the songs of one album or
 * playlist; id names the album or playlist, NULL for the others) shows, owned by the UI
 * thread. A cache read is installed only if nothing newer was installed since
 * it started; a result that cannot be shown yet is parked; a refresh that
 * could not start is remembered. */
typedef struct {
    unsigned installed_gen; /* moves on whenever a newer list is installed or parked */
    subsonic_load_t * load; /* the cache read in flight, if any */
    unsigned load_gen;      /* installed_gen when that read started */
    bool has_parked;
    void * parked;          /* newest result waiting for a search to end */
    int parked_count;
    subsonic_cache_entry_t parked_entry;
    bool wants_fetch;       /* a refresh is due but the worker was busy */
    bool wants_load;        /* opened while too many reads ran: open again from the poll */
} subsonic_list_state_t;

/* Starts the cache read, keeps the one already running, or at the read cap
 * sets wants_load for a later call. False if it cannot start. */
bool subsonic_list_load_open(subsonic_list_state_t * st, const subsonic_server_t * server, subsonic_list_kind_t kind,
                             const char * id);

/* Hands over a finished read that is still the newest news. A read that was
 * overtaken is freed and false is returned. */
bool subsonic_list_load_take(subsonic_list_state_t * st, subsonic_load_result_t * result);

/* A newer list is being shown: any read in flight and any parked result is out of date. */
void subsonic_list_supersede(subsonic_list_state_t * st);

/* Keeps the newest result for later, freeing an older parked one. */
void subsonic_list_park(subsonic_list_state_t * st, void * items, int count, const subsonic_cache_entry_t * entry);

/* Hands over the parked result, if any. */
bool subsonic_list_unpark(subsonic_list_state_t * st, void ** items, int * count, subsonic_cache_entry_t * entry);

/* True once when a refresh is due and the worker is idle; the caller starts it. */
bool subsonic_list_fetch_due(subsonic_list_state_t * st, bool worker_idle);

/* Server switch, Wi-Fi off or teardown: forget everything of this list. */
void subsonic_list_reset(subsonic_list_state_t * st);

/* A PING waiting for the busy worker (a server switch reaps the old worker
 * first). Owned by the UI thread. A job for another server is dropped. */
typedef struct {
    bool queued;
    char server_key[SUBSONIC_CACHE_KEY_SIZE];
} subsonic_next_job_t;

void subsonic_next_clear(subsonic_next_job_t * slot);
void subsonic_next_store(subsonic_next_job_t * slot, const char * server_key);

/* True once, when a PING waits for this server. */
bool subsonic_next_take(subsonic_next_job_t * slot, const char * server_key);

#endif /* SUBSONIC_REFRESH_H */
