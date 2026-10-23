#ifndef UI_SNAPSHOT_PREFETCH_H
#define UI_SNAPSHOT_PREFETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const void *identity;
    const uint8_t *data;
    size_t size;
} ui_snapshot_prefetch_span_t;

/* Best-effort, nonblocking UI-thread request. Replaces pending work with up
 * to two spans. Returns false when inputs are invalid or the worker owns the
 * state lock. The data must remain alive until forget(identity) returns. */
bool ui_snapshot_prefetch_request(const ui_snapshot_prefetch_span_t *spans,
                                  size_t count);

/* Cancel queued/in-progress page walks. Safe before freeing any submitted
 * buffers; returns only after no worker can still read them. */
void ui_snapshot_prefetch_cancel_all(void);

/* Remove one identity and wait out an in-progress page touch. Call before
 * freeing or replacing the memory associated with identity. */
void ui_snapshot_prefetch_forget(const void *identity);

#endif
