#include "../metadata_db.c"
#include <assert.h>

#define ROWS 150
static int queued, flushes, removed, rejected;
static bool fail_flush, inject_full;
static bool durable[ROWS], enqueued[ROWS];

remote_state_rating_t *remote_state_list_prefix(const char *prefix, int *count) {
    (void)prefix;
    *count = ROWS;
    remote_state_rating_t *rows = calloc(ROWS, sizeof(*rows));
    assert(rows);
    for (int i = 0; i < ROWS; i++) {
        char path[20];
        snprintf(path, sizeof(path), "%d", i);
        rows[i].path = strdup(path);
    }
    return rows;
}
bool remote_state_get(const char *path, int32_t *rating, int32_t *plays, int32_t *last) {
    (void)path;
    *rating = 8; *plays = 12; *last = 100;
    return true;
}
bool tagcache_set_stats(const char *path, int32_t rating, int32_t plays, int32_t last) {
    assert(rating == 8 && plays == 12 && last == 100);
    if (inject_full) { inject_full = false; queued = 64; }
    if (queued >= 64) { rejected++; return false; }
    enqueued[atoi(path)] = true;
    queued++;
    return true;
}
bool tagcache_flush_numeric(void) {
    flushes++;
    if (fail_flush && flushes > 1) return false;
    for (int i = 0; i < ROWS; i++) durable[i] |= enqueued[i];
    memset(enqueued, 0, sizeof(enqueued));
    queued = 0;
    return true;
}
void remote_state_remove_matches(const remote_state_rating_t *rows, const int32_t (*tuples)[3],
                                 const bool *migrated, int count) {
    assert(count == ROWS);
    for (int i = 0; i < count; i++) {
        assert(migrated[i] && durable[atoi(rows[i].path)] && tuples[i][0] == 8);
        removed++;
    }
}
int main(void) {
    migrate_sidecar_stats();
    assert(removed == ROWS && flushes >= 5 && !rejected);
    removed = flushes = 0;
    inject_full = true;
    migrate_sidecar_stats();
    assert(removed == ROWS && rejected == 1);
    removed = flushes = 0;
    fail_flush = true;
    migrate_sidecar_stats();
    assert(!removed);
    puts("Sidecar migration batches, queue rejection retry and failed persistence passed");
    return 0;
}
