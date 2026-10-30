#include "gui_page_durations.h"
#include "../../../library/metadata/durations/duration_lookup.h"
#include <stdlib.h>

#define PAGE_RETIRE_SLOTS 6
#define PAGE_RETIRE_ADMIT 4

static duration_lookup_t * page;
static int page_count;
static duration_lookup_t * retired[PAGE_RETIRE_SLOTS];

static int retiring(void) {
    int n = 0;
    for (int i = 0; i < PAGE_RETIRE_SLOTS; i++) {
        if (retired[i] && duration_lookup_done(retired[i])) { duration_lookup_destroy(retired[i]); retired[i] = NULL; }
        if (retired[i]) n++;
    }
    return n;
}

void gui_page_durations_reset(void) {
    if (!page) return;
    duration_lookup_cancel(page);
    for (int i = 0; i < PAGE_RETIRE_SLOTS; i++)
        if (!retired[i]) { retired[i] = page; page = NULL; break; }
    page = NULL; /* no slot is unreachable under the admission limit; a cancelled worker just finishes */
    page_count = 0;
}

bool gui_page_durations_begin(const char * const * paths, int count) {
    gui_page_durations_reset();
    if (count <= 0 || retiring() >= PAGE_RETIRE_ADMIT) return false;
    page = duration_lookup_start(paths, (size_t) count);
    page_count = page ? count : 0;
    return page != NULL;
}

bool gui_page_durations_busy(void) { return page && !duration_lookup_done(page); }

bool gui_page_durations_ready(void) { return !page || duration_lookup_done(page); }

int gui_page_durations_plan(int * miss, int max, void (*hit)(int, const duration_info_t *, void *), void * user) {
    int n = 0;
    for (int i = 0; i < page_count; i++) {
        const duration_lookup_result_t * r = duration_lookup_result(page, (size_t) i);
        if (r && r->identified && r->hit) {
            if (!duration_info_is_failed(&r->info) && hit) hit(i, &r->info, user);
            continue;
        }
        if (n < max) miss[n++] = i;
    }
    return n;
}

void gui_page_durations_note_probe(int index, const audio_current_format_info_t * info) {
    const duration_lookup_result_t * r = index >= 0 && index < page_count ? duration_lookup_result(page, (size_t) index) : NULL;
    duration_info_t value;
    if (r && r->identified && !r->hit && duration_info_from_probe(info, &value))
        duration_cache_put(duration_lookup_epoch(page), duration_lookup_path(page, (size_t) index), r->mtime, r->size, &value);
}

void gui_page_durations_note_failed(int index) {
    const duration_lookup_result_t * r = index >= 0 && index < page_count ? duration_lookup_result(page, (size_t) index) : NULL;
    if (r && r->identified && !r->hit)
        duration_cache_put_failed(duration_lookup_epoch(page), duration_lookup_path(page, (size_t) index), r->mtime, r->size);
}

void gui_page_durations_shutdown(void) {
    if (page) duration_lookup_cancel(page);
    for (int i = 0; i < PAGE_RETIRE_SLOTS; i++) if (retired[i]) duration_lookup_cancel(retired[i]);
    /* Final shutdown: the process exits next; nothing is joined here. */
}
