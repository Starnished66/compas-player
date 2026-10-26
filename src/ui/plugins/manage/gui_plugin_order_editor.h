#ifndef GUI_PLUGIN_ORDER_EDITOR_H
#define GUI_PLUGIN_ORDER_EDITOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the single plugin order editor screen. Call on the UI thread. The
 * editor copies all input strings and state. The change callback is owned by
 * plugin_manager.c and is invoked with the slot returned here after a toggle
 * or committed drag. */
int gui_plugin_show_order_editor(const char * title, const char * const * keys,
                                 const char * const * labels, const bool * visible, int count);

/* UI-thread lifecycle hook called by gui_settings_teardown() while the
 * settings screens are being torn down. */
void gui_plugin_order_editor_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* GUI_PLUGIN_ORDER_EDITOR_H */
