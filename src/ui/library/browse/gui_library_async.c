#include "gui_library_async.h"
#include "../../../core/process/worker_thread.h"
#include <unistd.h>

static atomic_int orphans;

bool library_async_handoff(atomic_int * fin) {
    return atomic_fetch_add_explicit(fin, 1, memory_order_acq_rel) == 1;
}

bool library_async_abandon(atomic_int * fin, atomic_int * kind) {
    atomic_fetch_add_explicit(&orphans, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(kind, 1, memory_order_relaxed);
    bool worker_gone = library_async_handoff(fin);
    if (worker_gone) {
        atomic_fetch_sub_explicit(&orphans, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(kind, 1, memory_order_relaxed);
    }
    return worker_gone;
}

bool library_async_worker_done(atomic_int * fin, atomic_int * kind) {
    bool owner_gone = library_async_handoff(fin);
    if (owner_gone) {
        atomic_fetch_sub_explicit(&orphans, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(kind, 1, memory_order_relaxed);
    }
    return owner_gone;
}

bool library_async_kind_has_orphan(atomic_int * kind) {
    return atomic_load_explicit(kind, memory_order_relaxed) > 0;
}

bool library_async_kind_saturated(atomic_int * kind) {
    return atomic_load_explicit(kind, memory_order_relaxed) >= LIBRARY_ASYNC_MAX_KIND_ORPHANS;
}

void library_async_wait_orphans(int64_t deadline_ms) {
    while (atomic_load_explicit(&orphans, memory_order_relaxed) > 0 && worker_ms_left(deadline_ms) > 0)
        usleep(1000);
}
