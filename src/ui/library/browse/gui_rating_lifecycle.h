#ifndef GUI_RATING_LIFECYCLE_H
#define GUI_RATING_LIFECYCLE_H
#include "gui_library_internal.h"

typedef struct {
    int *ranks, count, sections[6];
    int32_t generation;
    uint32_t incarnation;
    unsigned revision;
    bool loaded;
} rating_order_t;

typedef struct rating_job rating_job_t;
typedef struct {
    pthread_mutex_t mutex;
    bool enabled;
    rating_order_t order;
    rating_job_t *job;
    unsigned lifetime;
    lv_timer_t *refresh_timer;
    bool (*available)(void);
    bool (*visible)(void);
    bool (*mode)(void);
    bool (*load)(int **, int *, int *, int32_t *);
    void (*landed)(bool first);
} rating_list_t;
int rating_order_copy_page(const rating_order_t *order, int offset, int count,
                           compact_list_page_row_t rows[], int *ranks, int *item_count);
void rating_list_start(rating_list_t *list);
void rating_list_teardown(rating_list_t *list);
#endif
