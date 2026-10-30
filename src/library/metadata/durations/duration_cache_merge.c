#include "duration_cache.h"
#include "duration_cache_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Folding queued puts into the sorted table, and pruning it. Both replace the
 * table under dc_merge_lock (one at a time) from an owned copy, never holding
 * the lookup lock while sorting or allocating. */
/* Folds the recent puts into a new sorted table outside the lookup lock. */
bool dc_do_merge(void) {
    pthread_mutex_lock(&dc_merge_lock);
    pthread_mutex_lock(&dc_lock);
    if (dc_state != STATE_LOADED || dc_pending_count == 0) { pthread_mutex_unlock(&dc_lock); pthread_mutex_unlock(&dc_merge_lock); return true; }
    uint32_t owner = dc_epoch;
    bool merged_ok = true;
    dc_table_t * base = dc_cur;
    dc_table_ref(base);
    dc_entry_t batch[DURATION_PENDING_MAX];
    size_t n = dc_pending_count;
    memcpy(batch, dc_pending, n * sizeof(dc_entry_t));
    memcpy(dc_inflight, dc_pending, n * sizeof(dc_entry_t));
    dc_inflight_count = n;
    dc_pending_count = 0;
    pthread_mutex_unlock(&dc_lock);

    qsort(batch, n, sizeof(dc_entry_t), dc_cmp_entry);
    size_t base_count = base ? base->count : 0;
    dc_table_t * merged = dc_table_alloc(base_count + n);
    if (merged) {
        size_t i = 0, j = 0, w = 0;
        while (i < base_count || j < n) {
            if (j >= n || (i < base_count && base->e[i].key < batch[j].key)) merged->e[w++] = base->e[i++];
            else if (i >= base_count || batch[j].key < base->e[i].key) merged->e[w++] = batch[j++];
            else { merged->e[w++] = batch[j++]; i++; }
        }
        merged->count = w;
    }

    pthread_mutex_lock(&dc_lock);
    if (owner == dc_epoch) {
        if (merged) {
            dc_table_unref(dc_cur);
            dc_cur = merged;
            merged = NULL;
        } else { /* out of memory: hand the batch back (newer puts win) and back off */
            merged_ok = false;
            dc_merge_backoff_ms = dc_now_ms() + 1000;
            for (size_t k = 0; k < dc_inflight_count && dc_pending_count < DURATION_PENDING_MAX; k++)
                if (!dc_find_run(dc_pending, dc_pending_count, dc_inflight[k].key)) dc_pending[dc_pending_count++] = dc_inflight[k];
        }
        dc_inflight_count = 0;
    }
    dc_table_unref(base);
    pthread_cond_broadcast(&dc_flushed_cond);
    pthread_mutex_unlock(&dc_lock);
    free(merged);
    pthread_mutex_unlock(&dc_merge_lock);
    return merged_ok;
}

size_t duration_cache_prune(uint32_t at_epoch, const uint64_t * valid, size_t count) {
    if (!valid && count > 0) return 0;
    if (!duration_cache_wait_loaded() || at_epoch != duration_cache_epoch()) return 0;
    (void) dc_do_merge(); /* everything queued so far is in the table */
    pthread_mutex_lock(&dc_merge_lock);
    pthread_mutex_lock(&dc_lock);
    uint32_t owner = dc_epoch;
    if (owner != at_epoch) { pthread_mutex_unlock(&dc_lock); pthread_mutex_unlock(&dc_merge_lock); return 0; }
    dc_table_t * base = dc_cur;
    dc_table_ref(base);
    pthread_mutex_unlock(&dc_lock);

    size_t base_count = base ? base->count : 0;
    dc_table_t * kept = dc_table_alloc(base_count);
    size_t dropped = 0;
    if (kept) {
        size_t j = 0;
        for (size_t i = 0; i < base_count; i++) {
            while (j < count && valid[j] < base->e[i].key) j++;
            if (j < count && valid[j] == base->e[i].key) kept->e[kept->count++] = base->e[i];
        }
        dropped = base_count - kept->count;
    }
    pthread_mutex_lock(&dc_lock);
    if (kept && owner == dc_epoch && dc_cur == base && dropped > 0) {
        dc_table_unref(dc_cur);
        dc_cur = kept;
        kept = NULL;
        dc_note_dirty_locked();
    } else dropped = 0;
    dc_table_unref(base);
    pthread_mutex_unlock(&dc_lock);
    free(kept);
    pthread_mutex_unlock(&dc_merge_lock);
    return dropped;
}

