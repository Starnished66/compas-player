#include "gui_plugin_manage.h"
#include "gui_plugin_store.h"
#include "screen_builders.h"
#include "gui_theme.h"
#include "gui_navigation.h"
#include "gui_reload.h"
#include "gui_notifications.h"
#include "plugin_manager.h"
#include "plugin_disabled_list.h"
#include "assets.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <strings.h>

/* A bound, not a promise -- plugin_manager_scan_available() truncates its
 * on-disk scan at this count. It guarantees every currently-LOADED plugin
 * (at most PLUGIN_MAX_FILES=32 of those) is always included even when
 * truncating, so only *disabled, never-loaded* files can ever be the ones
 * left off; still, no realistic .plugins folder approaches 64 files. */
#define PLUGIN_MANAGE_MAX_ROWS 64

static plugin_available_entry_t manage_entries[PLUGIN_MANAGE_MAX_ROWS];
/* Row text: the plugin name, plus why an enabled plugin is not running. */
static char manage_labels[PLUGIN_MANAGE_MAX_ROWS][sizeof(manage_entries[0].display_name) + 64];
static int manage_entry_count = 0;
static bool manage_changes_dirty = false;

/* AsyncHttp.lua is a bundled request helper installed beside user-facing
 * plugins. It has no settings or actions of its own, so keep it out of the
 * user-facing manager while leaving every other .lua file visible. Match the
 * exact filename only; plugins with similar names remain manageable. */
static bool plugin_manage_is_support_module(const char * filename) {
    return filename && strcasecmp(filename, "AsyncHttp.lua") == 0;
}

static void plugin_manage_format_label(int index, const plugin_available_entry_t * entry) {
    char display_name[sizeof(entry->display_name)];
    snprintf(display_name, sizeof(display_name), "%s", entry->display_name);

    /* Unloaded plugins have no manifest name yet, so use their filename but
     * omit the implementation suffix from the label. The callback still
     * uses entry->filename unchanged as the stable toggle identity. */
    size_t length = strlen(display_name);
    if (!entry->loaded && length > 4 && strcasecmp(display_name + length - 4, ".lua") == 0)
        display_name[length - 4] = '\0';

    const char * status = "";
    if (!entry->disabled && !entry->loaded) {
        status = entry->over_limit ? " · Not loaded: limit reached" : " · Not loaded";
    }
    snprintf(manage_labels[index], sizeof(manage_labels[index]), "%s%s", display_name, status);
}

static void plugin_manage_apply_changes(void) {
    if (!manage_changes_dirty) return;
    /* Clear first: deleting the active management screen during the reload
     * emits SCREEN_UNLOADED, which must not schedule a second reload. */
    manage_changes_dirty = false;
    gui_reload_request();
}

static void plugin_manage_reload_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Triggers a soft reload to rescan plugins from disk, displaying a toast
     * notification that survives the screen rebuild. */
    manage_changes_dirty = false;
    show_info_toast("Refreshing plugins...");
    gui_reload_request();
}

static lv_obj_t * plugin_manage_screen;

static void plugin_manage_screen_unloaded_cb(lv_event_t * e) {
    (void) e;
    /* Back button, swipe-back, and Home all converge here. Persist each
     * toggle immediately, but rebuild the UI only once after the user has
     * finished changing the set. A screen opened on top (the Plugin Store
     * and its busy screen) only covers this one: wait until it is left. */
    if (plugin_manage_screen && gui_navigation_contains(plugin_manage_screen)) return;
    plugin_manage_apply_changes();
}

/* Reuses the exact reload path plugin.reload_ui() already uses -- see
 * plugin_manager.c's own comments on why a full reload (rather than a
 * surgical single-plugin unload) is the correct default here: it's the
 * only mechanism that already knows how to reset the global "last plugin
 * wins" style/layout/EQ state cleanly. It does NOT undo a plugin's own
 * durable side effects made outside that state -- most notably
 * set_icon()'s files under /usr/data/theme_overrides/, which have no
 * per-plugin attribution and so no revert path (plugin_manager.c's own
 * comment on PLUGIN_THEME_OVERRIDE_ROOT). Disabling a theme plugin stops
 * its script from running again on the next reload; it does not, by
 * itself, remove icons that plugin already copied to disk. */
static void plugin_manage_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= manage_entry_count) return;
    lv_obj_t * toggle_img = lv_event_get_target(e);
    bool enabled_now = lv_obj_has_state(toggle_img, LV_STATE_CHECKED);

    if (plugin_disabled_list_set(manage_entries[index].filename, !enabled_now)) {
        manage_entries[index].disabled = !enabled_now;
        manage_changes_dirty = true;
        return;
    }

    /* Persisting the change failed (full/removed SD card) -- don't reload
     * on a toggle that was never actually saved to disk, and put this
     * row's sprite/state back to the last known-good value rather than
     * leaving the UI showing a position that doesn't match what's on disk
     * (plugin_disabled_list_set() itself already rolled its own in-memory
     * state back on failure -- manage_entries[index].disabled still holds
     * that last known-good value). */
    /* Real lv_switch now (PILL_ACCESSORY_TOGGLE, screen_builders.c) --
     * CHECKED state alone drives its visual, no sprite swap needed. */
    bool restore_checked = !manage_entries[index].disabled;
    if (restore_checked) lv_obj_add_state(toggle_img, LV_STATE_CHECKED);
    else lv_obj_clear_state(toggle_img, LV_STATE_CHECKED);
    show_info_toast("Couldn't save -- plugin change was not applied");
}

lv_obj_t * gui_plugin_manage_build_screen(void) {
    manage_entry_count = plugin_manager_scan_available(manage_entries, PLUGIN_MANAGE_MAX_ROWS);

    static pill_list_item_t items[2 + PLUGIN_MANAGE_MAX_ROWS];
    items[0] = (pill_list_item_t){ "Plugin Store", PILL_ACCESSORY_CHEVRON, false,
                                    gui_plugin_store_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ "Refresh Plugins", PILL_ACCESSORY_NONE, false,
                                    plugin_manage_reload_row_cb, NULL, NULL };
    int count = 2;
    for (int i = 0; i < manage_entry_count; i++) {
        const plugin_available_entry_t * en = &manage_entries[i];
        if (plugin_manage_is_support_module(en->filename)) continue;
        plugin_manage_format_label(i, en);
        items[count++] = (pill_list_item_t){
            manage_labels[i], PILL_ACCESSORY_TOGGLE,
            !manage_entries[i].disabled, NULL, plugin_manage_toggle_cb,
            (void *) (intptr_t) i
        };
    }

    lv_obj_t * scr = build_pill_list_screen("Plugin Manager", generic_back_cb, items, count,
                                             gui_theme_accent_style(), 6, 100);
    lv_obj_add_event_cb(scr, plugin_manage_screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_plugin_manage_init(void) {
    plugin_manage_screen = gui_plugin_manage_build_screen();
}

void gui_plugin_manage_teardown(void) {
    if (plugin_manage_screen) {
        lv_obj_delete(plugin_manage_screen);
        plugin_manage_screen = NULL;
    }
}

void gui_plugin_manage_poll(void) {
    /* Home drops a covered Plugin Manager (under the store) from the stack
     * without another unload event. */
    if (manage_changes_dirty && plugin_manage_screen && !gui_navigation_contains(plugin_manage_screen))
        plugin_manage_apply_changes();
}

void gui_plugin_manage_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(plugin_manage_screen);
}
