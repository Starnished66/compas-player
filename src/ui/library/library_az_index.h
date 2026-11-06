#ifndef LIBRARY_AZ_INDEX_H
#define LIBRARY_AZ_INDEX_H

#include "lvgl/lvgl.h"
#include "../../library/metadata/db/metadata_db.h"
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
/* An in-memory list's strip. `table` fills the 27-entry jump table (first row
 * of each letter, as metadata_db_get_az_table does) from the list's current
 * contents; it runs at each touch-down, so no refresh is needed when items
 * change. `allowed` (optional) says whether the strip may show now. The
 * binding lives until library_az_index_unregister(screen), which the owner
 * calls before deleting the screen. Not dropped by reset_bindings. */
typedef void (*library_az_index_table_t)(int jump[27]);
typedef bool (*library_az_index_allowed_t)(void);
/* False (nothing registered) when the registry is full. */
bool library_az_index_register_table(lv_obj_t * screen, lv_obj_t * list, library_az_index_table_t table,
                                     library_az_index_allowed_t allowed);
/* Whether the strip of this list may show now (true for a list with no strip). */
bool library_az_index_list_allowed(lv_obj_t * list);
void library_az_index_unregister(lv_obj_t * screen);
/* Clears the library's own (db-backed) registry and visibility-timer state only. Callers preserve the
 * existing screen deletion/re-registration order; drag state/timer are not
 * reset here. */
void library_az_index_reset_bindings(void);
/* The letter bubble of an A-Z strip: hidden, centred in `parent`; `*label` is its text. */
lv_obj_t * library_az_index_bubble_create(lv_obj_t * parent, lv_obj_t ** label);
lv_obj_t * library_az_index_strip_for_screen(lv_obj_t * screen);
bool library_az_index_overlay_visible(void);
void library_az_index_resume_timer(void);
void library_az_index_reset_drag(void);
void library_az_index_poll(lv_timer_t * timer);

#endif
