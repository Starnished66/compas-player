#ifndef LIBRARY_AZ_INDEX_H
#define LIBRARY_AZ_INDEX_H

#include "lvgl/lvgl.h"
#include "metadata_db.h"
#include <stdbool.h>

typedef lv_indev_t * (*library_az_index_pointer_indev_getter_t)(void);
typedef bool (*library_az_index_strip_allowed_t)(lv_obj_t * list);

/* Singleton UI-thread component. Both callbacks are copied function pointers;
 * their code must remain available for the component's lifetime. init replaces
 * the hooks without taking ownership of any caller state and retains the one
 * drag timer across repeated init/reload cycles. */
void library_az_index_init(library_az_index_pointer_indev_getter_t pointer_indev_getter,
                           library_az_index_strip_allowed_t strip_allowed);
void library_az_index_register(lv_obj_t * screen, lv_obj_t * list,
                               metadata_db_az_kind_t db_kind);
/* Clears registry and visibility-timer state only. Callers preserve the
 * existing screen deletion/re-registration order; drag state/timer are not
 * reset here. */
void library_az_index_reset_bindings(void);
lv_obj_t * library_az_index_strip_for_screen(lv_obj_t * screen);
bool library_az_index_overlay_visible(void);
void library_az_index_resume_timer(void);
void library_az_index_reset_drag(void);
void library_az_index_poll(lv_timer_t * timer);

#endif
