#ifndef GUI_LIBRARY_ASYNC_H
#define GUI_LIBRARY_ASYNC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Shared helpers of the library's background reads (album tracks, genre songs,
 * artist songs). The UI thread never joins a read that is still running: it
 * hands the request over to the worker, which frees it when it finishes. */

/* Called once by the worker as its last touch of the request, and once by the
 * owner that gives the request up. Whichever comes second gets true and frees. */
bool library_async_handoff(atomic_int * fin);
/* Owner side: counts the request as an orphan (overall, and in the kind's own counter) until
 * its worker has freed it. Returns true if the worker was already done and the caller must
 * free now. */
bool library_async_abandon(atomic_int * fin, atomic_int * kind);
/* Worker side, after its last touch: true if the owner left, so the worker frees. */
bool library_async_worker_done(atomic_int * fin, atomic_int * kind);
/* True while a read of this kind that was given up is still running (a stalled card or
 * metadata lock). Callers then keep the running read and retarget it instead of giving it up
 * too, so a kind never has more than one abandoned and one active read. */
bool library_async_kind_has_orphan(atomic_int * kind);
/* How many reads of this kind were given up and are still running. Teardown gives up the active
 * read whatever the count, so a new read is refused while two are stalled: a stalled card
 * cannot make repeated teardown and rebuild cycles pile up threads. */
#define LIBRARY_ASYNC_MAX_KIND_ORPHANS 2
bool library_async_kind_saturated(atomic_int * kind);
/* Waits (bounded) until every abandoned read has ended: library teardown, reload, shutdown. */
void library_async_wait_orphans(int64_t deadline_ms);

#endif
