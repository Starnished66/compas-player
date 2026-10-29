#include "gui_album_header_internal.h"
#include "gui_album_cover.h"
#include "../../../audio/probe/track_probe.h"
#include "../../../library/metadata/durations/duration_lookup.h"
#include "../../../core/process/worker_thread.h"
#include "../../../core/process/worker_notify.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The album header's duration and probe side: which tracks are known, what the
 * duration cache and the whole-album probe learned, and when it is all settled.
 * UI thread only. */
album_t A; /* the one album the header is showing; see gui_album_header_internal.h */

static track_probe_job_t * job;
static uint64_t job_key;
static int * batch;
static int batch_n, start_fails;
static bool job_cancelled;
static uint32_t retry_tick;
/* Cache lookup of the current album's tracks, plus finished-with ones whose
 * worker had not returned yet when the album changed. A retired lookup is never
 * joined here (its worker may be waiting on the card): a new one is only started
 * while fewer than LOOKUP_RETIRE_ADMIT are still winding down, else on a later poll. */
#define LOOKUP_RETIRE_SLOTS 8
#define LOOKUP_RETIRE_ADMIT 4
static duration_lookup_t * lookup;
static uint64_t lookup_key;
static bool lookup_wanted;
static duration_lookup_t * retired[LOOKUP_RETIRE_SLOTS];

void album_state_free(void) {
    for (int i = 0; A.paths && i < A.count; i++) free(A.paths[i]);
    free(A.paths); free(A.state); free(A.dur); free(A.ident_mtime); free(A.ident_size);
    free(A.identified); free(A.unplayable); free(A.track);
    memset(&A, 0, sizeof(A));
}

static void retire_lookup(void) {
    lookup_wanted = false;
    if (!lookup) return;
    duration_lookup_cancel(lookup);
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (!retired[i]) { retired[i] = lookup; lookup = NULL; return; }
    lookup = NULL; /* unreachable with the admission limit; the cancelled worker is left to finish */
}

static int retiring_lookups(void) {
    int n = 0;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) if (retired[i]) n++;
    return n;
}

static void reap_retired(void) {
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++)
        if (retired[i] && duration_lookup_done(retired[i])) { duration_lookup_destroy(retired[i]); retired[i] = NULL; }
}

static void start_lookup(void) {
    if (lookup || A.count <= 0 || !A.paths) { lookup_wanted = false; return; }
    if (retiring_lookups() >= LOOKUP_RETIRE_ADMIT) { lookup_wanted = true; return; }
    lookup_wanted = false;
    lookup = duration_lookup_start((const char * const *) A.paths, (size_t) A.count);
    lookup_key = A.key;
}

void album_state_reset(uint64_t key, const group_song_entry_t * entries, int count) {
    retire_lookup();
    album_state_free();
    A.key = key;
    A.epoch = duration_cache_epoch(); /* puts for this album are dropped if the card changes */
    A.paths = calloc((size_t) count + 1, sizeof(char *));
    A.state = calloc((size_t) count + 1, 1);
    A.dur = calloc((size_t) count + 1, sizeof(*A.dur));
    A.ident_mtime = calloc((size_t) count + 1, sizeof(int32_t));
    A.ident_size = calloc((size_t) count + 1, sizeof(int32_t));
    A.identified = calloc((size_t) count + 1, 1);
    A.unplayable = calloc((size_t) count + 1, 1);
    if (!A.unplayable || !A.paths || !A.state || !A.dur ||
        !A.ident_mtime || !A.ident_size || !A.identified) {
        album_state_free();
        return;
    }
    for (int i = 0; i < count; i++) {
        A.paths[i] = strdup(entries[i].path ? entries[i].path : "");
        if (!A.paths[i]) { A.count = i; album_state_free(); return; } /* out of memory: no cached durations this time */
    }
    A.count = count;
    start_fails = 0;
    /* Known tracks come from the duration cache; only the misses get probed. */
    start_lookup();
}

bool album_settled(void) {
    if (!A.state || A.count <= 0) return false;
    for (int i = 0; i < A.count; i++)
        if (A.state[i] == 0 || A.state[i] == 3) return false;
    return true;
}

/* ---- Probing every track -------------------------------------------------- */
static bool duration_ok(const audio_current_format_info_t * info) {
    duration_info_t cached;
    return info->duration_seconds <= UINT_MAX - 1.0 && duration_info_from_probe(info, &cached);
}

static void store(int index, const audio_current_format_info_t * info) {
    if (!duration_info_from_probe(info, &A.dur[index])) return;
    A.state[index] = 1;
    A.dirty = true;
    if (A.identified[index])
        duration_cache_put(A.epoch, A.paths[index], A.ident_mtime[index],
                           A.ident_size[index], &A.dur[index]);
}

/* Applies a finished cache lookup: hits become known without a probe, and
 * tracks the page's own probe learned meanwhile are written to the cache. */
static void apply_lookup(void) {
    bool current = lookup_key == A.key && A.state;
    for (int i = 0; current && i < A.count; i++) {
        const duration_lookup_result_t * r = duration_lookup_result(lookup, (size_t) i);
        if (!r || !r->identified) continue;
        A.identified[i] = 1;
        A.ident_mtime[i] = r->mtime;
        A.ident_size[i] = r->size;
        if (A.state[i] == 0 && r->hit && duration_info_is_failed(&r->info)) {
            A.state[i] = 2; A.unplayable[i] = 1; A.dirty = true; /* positively rejected, remembered by the cache */
        }
        else if (A.state[i] == 0 && r->hit) { A.dur[i] = r->info; A.state[i] = 1; A.dirty = true; }
        else if (A.state[i] == 1 && !r->hit)
            duration_cache_put(A.epoch, A.paths[i], r->mtime, r->size, &A.dur[i]);
        else if (A.state[i] == 2 && A.unplayable[i] && !r->hit)
            duration_cache_put_failed(A.epoch, A.paths[i], r->mtime, r->size);
    }
    duration_lookup_destroy(lookup);
    lookup = NULL;
}

void gui_album_header_note_cached(int index, const duration_info_t * info) {
    if (!A.state || index < 0 || index >= A.count || !info || A.state[index] != 0) return;
    if (duration_info_is_failed(info)) { A.state[index] = 2; A.unplayable[index] = 1; }
    else if (info->duration_ms > 0) { A.dur[index] = *info; A.state[index] = 1; }
    else return;
    A.dirty = true;
}

void gui_album_header_note_probe(int index, const audio_current_format_info_t * info) {
    if (!A.state || index < 0 || index >= A.count || !info || !duration_ok(info)) return;
    store(index, info);
}

static void free_job(void) {
    track_probe_job_destroy(job);
    job = NULL;
    free(batch);
    batch = NULL;
    batch_n = 0;
}

bool album_probe_busy(void) { return job || lookup || lookup_wanted; }

void album_probe_poll(bool allowed) {
    reap_retired();
    if (lookup_wanted && !lookup) start_lookup();
    if (lookup && duration_lookup_done(lookup)) apply_lookup();
    if (job) {
        bool current = job_key == A.key && A.state;
        if ((!allowed || !current) && !job_cancelled) { track_probe_job_cancel(job); job_cancelled = true; }
        bool done = track_probe_job_done(job), drained = false;
        for (int budget = 0; budget < 8; budget++) {
            size_t index;
            audio_current_format_info_t info;
            if (!track_probe_job_next(job, &index, &info)) { drained = true; break; }
            if (budget == 7) worker_notify_ui(); /* budget spent with results left: come back next pass */
            if (!current || (int) index >= batch_n || batch[index] >= A.count) continue;
            if (duration_ok(&info)) store(batch[index], &info);
            /* A cancelled job may publish empty results for skipped files. */
            else if (!job_cancelled && A.state[batch[index]] != 1) {
                int t = batch[index];
                /* Only a positive rejection counts as known-unplayable; an I/O or
                 * resource failure leaves the track unknown, so no partial total shows. */
                bool rejected = track_probe_job_confirmed_failure(job, index);
                A.state[t] = rejected ? 2 : 3; A.dirty = true;
                if (rejected) {
                    A.unplayable[t] = 1;
                    if (A.identified[t])
                        duration_cache_put_failed(A.epoch, A.paths[t], A.ident_mtime[t],
                                                  A.ident_size[t]);
                }
            }
        }
        if (done && drained) free_job();
    }
    if (!job && !lookup && !lookup_wanted && allowed && A.state && A.count > 0 &&
        (start_fails == 0 || lv_tick_elaps(retry_tick) >= 1000)) {
        const char * paths[TRACK_PROBE_MAX_TRACKS];
        int * picked = malloc(sizeof(int) * TRACK_PROBE_MAX_TRACKS);
        int n = 0;
        for (int i = 0; picked && i < A.count && n < TRACK_PROBE_MAX_TRACKS; i++) {
            if (A.state[i] != 0) continue;
            if (strlen(A.paths[i]) >= TRACK_PROBE_MAX_PATH_BYTES) { A.state[i] = 3; A.dirty = true; continue; }
            paths[n] = A.paths[i];
            picked[n++] = i;
        }
        if (n > 0) {
            job = track_probe_job_start(paths, (size_t) n);
            if (job) { batch = picked; batch_n = n; job_key = A.key; job_cancelled = false; start_fails = 0; }
            else {
                free(picked);
                retry_tick = lv_tick_get();
                /* Give up after three refusals so the page stops retrying; the
                 * tracks stay unknown, so the chips stay hidden. */
                if (++start_fails >= 3)
                    for (int i = 0; i < A.count; i++) if (A.state[i] == 0) { A.state[i] = 3; A.dirty = true; }
            }
        } else free(picked);
    }
}

/* Final shutdown: gives a lookup worker a moment (until the shared deadline) to
 * notice it was cancelled rather than waiting for a card read that may never return. */
static void lookup_destroy_bounded(duration_lookup_t * l, int64_t deadline_ms) {
    if (!l) return;
    duration_lookup_cancel(l);
    while (!duration_lookup_done(l) && worker_ms_left(deadline_ms) > 0) usleep(5000);
    duration_lookup_destroy(l); /* a no-op for a worker that is still stuck: the process is exiting */
}

void album_probe_shutdown(int64_t deadline_ms) {
    lookup_destroy_bounded(lookup, deadline_ms);
    lookup = NULL;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) {
        lookup_destroy_bounded(retired[i], deadline_ms);
        retired[i] = NULL;
    }
    (void) duration_cache_flush_bounded((uint32_t) worker_ms_left(deadline_ms));
    if (job) track_probe_job_cancel_and_destroy(job);
    job = NULL;
    free(batch);
    batch = NULL;
    batch_n = 0;
    album_state_free();
}
