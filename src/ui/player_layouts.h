#ifndef PLAYER_LAYOUTS_H
#define PLAYER_LAYOUTS_H

#include "lvgl.h"
#include <stdbool.h>

/* Registry of Player screen layouts. A layout is anything that can fill an
 * empty screen with a widget tree that follows the named-widget contract in
 * docs/PLAYER_LAYOUTS.md; gui_player.c then wires that tree up by widget
 * name. Three kinds exist:
 *
 *  - BUILTIN_C: the hand-written layout in gui_player.c. Always registered
 *    under PLAYER_LAYOUT_ID_DEFAULT and always the fallback.
 *  - EXPORTED_C: a component exported to C by LVGL's UI Editor ("XML to C"),
 *    compiled in and registered with player_layouts_register_c().
 *  - XML_FILE: an LVGL XML file read at runtime, found by scanning the
 *    layout directories or registered by a plugin for the current session. */

#define PLAYER_LAYOUT_ID_DEFAULT "default"
#define PLAYER_LAYOUT_ID_MAX 64
#define PLAYER_LAYOUT_NAME_MAX 64

typedef enum {
    PLAYER_LAYOUT_BUILTIN_C,
    PLAYER_LAYOUT_EXPORTED_C,
    PLAYER_LAYOUT_XML_FILE,
} player_layout_kind_t;

/* Creates the widget tree under `parent` (an empty screen) and returns its
 * root, or NULL on failure. The built-in layout creates its widgets directly
 * on `parent` and returns `parent`. An LVGL Editor export has exactly this
 * shape: `lv_obj_t * <component>_create(lv_obj_t * parent)`. */
typedef lv_obj_t * (*player_layout_create_fn)(lv_obj_t * parent);

/* Optional companion of an exported layout: returns the named timeline of the
 * layout rooted at `root`, or NULL. XML layouts do not need one. */
typedef lv_anim_timeline_t * (*player_layout_timeline_fn)(lv_obj_t * root, const char * name);

typedef struct {
    char id[PLAYER_LAYOUT_ID_MAX];
    char name[PLAYER_LAYOUT_NAME_MAX];
    player_layout_kind_t kind;
} player_layout_info_t;

/* Registers the built-in layout with its creator and rescans the layout
 * directories. Call after lv_init(). Safe to call on every UI (re)build: C
 * registrations persist, XML directory entries are refreshed. The XML engine
 * itself (LVGL's lv_xml_init(), which lv_init() no longer calls in 9.5) is
 * started by the first XML layout that is created, once per process. */
void player_layouts_init(player_layout_create_fn builtin_create);

/* Re-reads the layout directories, so a file copied to the device while the
 * app runs shows up in the Settings list. The active layout is unaffected. */
void player_layouts_rescan(void);

/* Registers an exported C layout. `id` is 1..63 characters of
 * [A-Za-z0-9_.-] and not "default". Registering an existing C id replaces it.
 * Returns false when the id or table is unusable. May be called before
 * player_layouts_init(). */
bool player_layouts_register_c(const char * id, const char * display_name, player_layout_create_fn create);
bool player_layouts_register_c_ex(const char * id, const char * display_name, player_layout_create_fn create,
                                  player_layout_timeline_fn timeline);

/* True for an id player_layouts_register_c() / a plugin may use. */
bool player_layouts_id_is_valid(const char * id);

/* Registers an XML layout at `path` for this session only and makes it the
 * effective layout (plugin.set_player_layout). `path` must already be a
 * resolved, trusted path. Returns true when the effective layout changed.
 *
 * A layout registered while a plugin loads (`from_callback` false) belongs to
 * that plugin: it goes away on plugin reset and is registered again when the
 * plugin reloads. One registered from a plugin callback has no code that would
 * register it again, so it survives plugin resets until the app restarts. */
bool player_layouts_session_select_xml(const char * id, const char * display_name, const char * path,
                                       bool from_callback);

/* Plugin reset: drops the session layouts that belong to plugins' top-level
 * code, and the selection if it pointed at one. */
void player_layouts_session_reset(void);

/* Forgets a session selection so the persisted Settings choice applies
 * again. Returns true when the effective layout changed. */
bool player_layouts_session_clear_selection(void);

int player_layouts_count(void);
const player_layout_info_t * player_layouts_get(int index);
int player_layouts_find(const char * id);

/* Session selection if any and still registered, else
 * current_settings.player_layout if registered, else the default id. */
const char * player_layouts_effective_id(void);

/* Creates layout `id` under `parent`. Returns the root, or NULL on failure
 * (logged, `parent` left as it was). *kind_out receives the layout kind. */
lv_obj_t * player_layouts_create(const char * id, lv_obj_t * parent, player_layout_kind_t * kind_out);

/* Named timeline of the layout rooted at `root` (XML layouts, or exported
 * ones registered with a timeline function); NULL if absent. Looked up once
 * at bind time, never per frame. The timeline belongs to `root`. */
lv_anim_timeline_t * player_layouts_find_timeline(lv_obj_t * root, const char * name);

/* Releases the XML component registered for the last XML layout. Call after
 * the tree built from it has been deleted: its styles live in that component. */
void player_layouts_release(void);

#endif /* PLAYER_LAYOUTS_H */
