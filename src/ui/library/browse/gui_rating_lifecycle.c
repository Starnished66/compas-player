#include "gui_rating_lifecycle.h"
#include "../../../library/remote/remote_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "gui_rating_rows.h"
#include "../../text/i18n/i18n.h"

struct rating_job {
    pthread_t thread;
    atomic_bool done;
    unsigned lifetime;
    rating_order_t order;
    rating_list_t *list;
};

static void *rating_load(void *data) {
    rating_job_t *job = data;
    rating_order_t *result = &job->order;
    uint32_t before = metadata_db_get_incarnation();
    result->loaded = job->list->load(&result->ranks, result->sections,
                                     &result->count, &result->generation);
    result->incarnation = metadata_db_get_incarnation();
    result->loaded &= before == result->incarnation;
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static void rating_land(lv_timer_t *timer) {
    rating_list_t *list = lv_timer_get_user_data(timer);
    rating_job_t *job = list->job;
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;
    int32_t generation;
    uint32_t incarnation;
    if (!metadata_db_try_get_generation(&generation) ||
        !metadata_db_try_get_incarnation(&incarnation)) return;
    pthread_join(job->thread, NULL);
    bool valid = job->lifetime == list->lifetime && list->enabled && list->mode() &&
        job->order.loaded && job->order.revision == remote_state_revision() &&
        generation == job->order.generation && incarnation == job->order.incarnation;
    if (valid) {
        bool first = !list->order.loaded;
        pthread_mutex_lock(&list->mutex);
        free(list->order.ranks);
        list->order = job->order;
        pthread_mutex_unlock(&list->mutex);
        job->order.ranks = NULL;
        list->landed(first);
    }
    free(job->order.ranks);
    free(job);
    list->job = NULL;
    lv_timer_delete(timer);
}

static void rating_schedule(rating_list_t *list) {
    if (list->job || library_rescan_active || !list->available()) return;
    rating_job_t *job = calloc(1, sizeof(*job));
    if (!job) return;
    job->lifetime = list->lifetime;
    job->list = list;
    job->order.revision = remote_state_revision();
    atomic_init(&job->done, false);
    lv_timer_t *timer = lv_timer_create(rating_land, 25, list);
    if (!timer) { free(job); return; }
    if (pthread_create(&job->thread, NULL, rating_load, job)) {
        lv_timer_delete(timer);
        free(job);
        return;
    }
    list->job = job;
}

static void rating_poll(lv_timer_t *timer) {
    rating_list_t *list = lv_timer_get_user_data(timer);
    if (!list->mode() || !list->visible()) return;
    if (!list->order.loaded || list->order.revision != remote_state_revision()) {
        rating_schedule(list);
        return;
    }
    int32_t generation;
    uint32_t incarnation;
    if (metadata_db_try_get_generation(&generation) && metadata_db_try_get_incarnation(&incarnation) &&
        (list->order.generation != generation || list->order.incarnation != incarnation))
        rating_schedule(list);
}

void rating_list_teardown(rating_list_t *list) {
    list->lifetime++;
    if (list->refresh_timer) lv_timer_delete(list->refresh_timer);
    list->refresh_timer = NULL;
    pthread_mutex_lock(&list->mutex);
    free(list->order.ranks);
    list->order = (rating_order_t){0};
    list->enabled = false;
    pthread_mutex_unlock(&list->mutex);
}

void rating_list_start(rating_list_t *list) {
    pthread_mutex_lock(&list->mutex);
    list->enabled = true;
    pthread_mutex_unlock(&list->mutex);
    if (!list->refresh_timer) list->refresh_timer = lv_timer_create(rating_poll, 250, list);
    rating_schedule(list);
}

int rating_order_copy_page(const rating_order_t *order, int offset, int count,
                           compact_list_page_row_t rows[], int *ranks, int *item_count) {
    *item_count = 0;
    if (!order->loaded) return -1;
    int total = order->count;
    for (int s = 0; s < 6; s++) total += order->sections[s] > 0;
    if (offset < 0 || count <= 0 || offset >= total) return 0;
    if (count > total - offset) count = total - offset;
    for (int i = 0; i < count; i++) {
        memset(&rows[i], 0, sizeof(rows[i]));
        int stars = -1;
        int pos = rating_row_position(order->sections, offset + i, &stars);
        if (stars >= 0) {
            rows[i].is_section_header = true;
            rows[i].section_stars = stars;
            if (!stars) snprintf(rows[i].label, sizeof(rows[i].label), "%s", TR("Unrated"));
        } else if (pos >= 0) ranks[(*item_count)++] = order->ranks[pos];
    }
    return count;
}
