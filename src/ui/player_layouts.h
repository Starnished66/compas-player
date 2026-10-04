#ifndef PLAYER_LAYOUTS_H
#define PLAYER_LAYOUTS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 *    layout directories, discovered under `.plugins` (root files and one
 *    bundle directory level), or registered by a plugin for this session. */

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

/* Re-reads the layout directories, including `.plugins` root and bundle XML,
 * so files added while the app runs show up in Settings. The active layout is
 * unaffected. */
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

/* Registers an XML layout at `path` for this session only so it appears in
 * the Settings list, without selecting it (plugin.set_player_layout called
 * from top-level code). `path` must already be a resolved, trusted path.
 * Registering an id again replaces its entry, so a plugin reload does not
 * duplicate it. Returns false when the id or path is unusable.
 * If another discovered id points at the same XML file, a currently selected
 * id remains available across directory rescans.
 *
 * A layout registered while a plugin loads (`from_callback` false) belongs to
 * that plugin: it goes away on plugin reset and is registered again when the
 * plugin reloads. One registered from a plugin callback has no code that would
 * register it again, so it survives plugin resets until the app restarts. */
bool player_layouts_session_register_xml(const char * id, const char * display_name, const char * path,
                                         bool from_callback);

/* Registers like player_layouts_session_register_xml() and also makes the
 * layout the session's effective one until the user picks a layout in
 * Settings (plugin.set_player_layout called from a callback). Returns true
 * when the effective layout changed. */
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
/* Returns a validated filesystem PNG preview path for this layout. */
bool player_layouts_get_preview(int index, char * out, size_t size);
int player_layouts_find(const char * id);
/* Resolves an existing layout ID to a session-registered XML alias for the
 * same contained file when one exists. C layouts and the built-in default
 * resolve to themselves. Returns false for invalid, unknown, or undersized
 * output arguments. */
bool player_layouts_canonical_id(const char * id, char * out, size_t size);

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

typedef struct {
    lv_obj_t * target;
    lv_style_prop_t prop;
    lv_style_selector_t selector;
    int32_t start_value;
    int32_t end_value;
    uint32_t start_time;
    uint32_t duration;
    int32_t current_value;
    bool has_current_value;
    lv_anim_path_cb_t path_cb;
    lv_anim_t animation;
} player_layout_style_transition_t;

/* A style transition descriptor uses target/prop/selector/start/end/time/
 * duration/path to describe one numeric local-style animation. Initialize its
 * animation field with lv_anim_init() to use LVGL's default execution flags
 * (including early_apply); the path defaults to linear when omitted. */
lv_anim_timeline_t * player_layouts_create_style_timeline(
    const player_layout_style_transition_t * transitions, uint32_t count);
/* Destroys only timelines created by player_layouts_create_style_timeline().
 * Call before deleting their target objects; XML-owned timelines stay owned by
 * their layout tree and must not be passed here. */
void player_layouts_destroy_style_timeline(lv_anim_timeline_t * timeline);

/* Finds a matching property animation in a prepared XML or generated style
 * timeline. Values and timing reflect the animation scale applied at binding. */
bool player_layouts_timeline_style_transition(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                               lv_style_prop_t prop, lv_style_selector_t selector,
                                               player_layout_style_transition_t * transition);
uint32_t player_layouts_timeline_style_transition_count(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                                         lv_style_prop_t prop, lv_style_selector_t selector);

/* Enumerates the style-property animations in a prepared timeline. */
uint32_t player_layouts_timeline_style_animation_count(lv_anim_timeline_t * timeline);
bool player_layouts_timeline_get_style_animation(lv_anim_timeline_t * timeline, uint32_t index,
                                                  player_layout_style_transition_t * transition);

/* True when a timeline contains any animation targeting obj. */
bool player_layouts_timeline_has_animation_for_obj(lv_anim_timeline_t * timeline, lv_obj_t * obj);
uint32_t player_layouts_timeline_animation_count_for_obj(lv_anim_timeline_t * timeline, lv_obj_t * obj);

/* Reapply animation_scale from preserved source timing, avoiding cumulative
 * scaling when the setting changes during a UI build. */
bool player_layouts_timeline_refresh_timing(lv_anim_timeline_t * timeline);

/* Observe act_time updates from a prepared style timeline. Up to four
 * independent observers may attach to a timeline. Returns false for timelines
 * not prepared by the shared style-timeline registry, or when all observer
 * slots are occupied.
 * Callbacks may detach observers, but must not release layouts or recursively
 * change the same timeline's progress. */
typedef void (*player_layouts_timeline_observer_cb_t)(lv_anim_timeline_t * timeline,
                                                       uint32_t act_time,
                                                       void * user_data);
bool player_layouts_timeline_observer_attach(lv_anim_timeline_t * timeline,
                                              player_layouts_timeline_observer_cb_t callback,
                                              void * user_data);
bool player_layouts_timeline_observer_detach(lv_anim_timeline_t * timeline,
                                              player_layouts_timeline_observer_cb_t callback,
                                              void * user_data);
/* Force synchronization after direct set_progress calls, including when the
 * requested progress maps to the already observed act_time. */
bool player_layouts_timeline_observer_sync(lv_anim_timeline_t * timeline);

/* Temporarily suppress one registered style write while a raster proxy
 * represents it. The registry is bounded and requires no per-frame allocation. */
bool player_layouts_timeline_suppress_style(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                             lv_style_prop_t prop, lv_style_selector_t selector,
                                             bool suppress);

/* Releases the XML component registered for the last XML layout. Call after
 * the tree built from it has been deleted: its styles live in that component. */
void player_layouts_release(void);

#endif /* PLAYER_LAYOUTS_H */
