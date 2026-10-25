#ifndef LIBRARY_SEARCH_H
#define LIBRARY_SEARCH_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>
#include "compact_list.h"
#include "metadata_db.h"

typedef enum {
    SEARCH_BINDING_ARTISTS,
    SEARCH_BINDING_ALBUMS,
    SEARCH_BINDING_ALBUM_ARTIST,
    SEARCH_BINDING_ALL_SONGS,
    SEARCH_BINDING_FILES,
    SEARCH_BINDING_SUBSONIC_ARTISTS,
    SEARCH_BINDING_SUBSONIC_ALBUMS,
    SEARCH_BINDING_RECENTLY_ADDED,
    SEARCH_BINDING_GROUP_SONGS,
    SEARCH_BINDING_COUNT
} search_binding_id_t;

typedef struct {
    const char *path; /* borrowed for this synchronous item lookup only */
    int64_t song_id;
    uint64_t artwork_key;
} library_search_group_item_t;

typedef library_search_group_item_t (*library_search_group_item_cb_t)(int index);
typedef lv_obj_t *(*library_search_az_strip_cb_t)(lv_obj_t *screen);
typedef void (*library_search_refresh_now_playing_cb_t)(void);
typedef const char *(*library_search_name_of_t)(int index);

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    library_search_name_of_t name_of;
    const int *count_ptr;
    bool is_overlay_list;
    bool db_backed;
    metadata_db_az_kind_t db_kind;
    compact_list_fetch_page_cb_t restore_fetch_page;
    lv_obj_t *files_title;
    lv_obj_t *files_options;
    bool inset_search_beside_options;
    lv_obj_t *restore_thumbnail_list;
    /* Optional except for SEARCH_BINDING_GROUP_SONGS. */
    library_search_group_item_cb_t group_item;
    /* Required non-NULL; resolves the live A–Z strip for this screen. */
    library_search_az_strip_cb_t az_strip_for_screen;
    /* Required non-NULL; called synchronously at the old refresh points. */
    library_search_refresh_now_playing_cb_t refresh_now_playing;
} library_search_config_t;

/* All operations and callbacks run on the LVGL thread. Registration copies
 * this configuration rather than retaining its address. screen/list/widgets,
 * count_ptr, name_of, provider, and callback pointers remain borrowed: widgets
 * live through close/invalidation and are replaced by re-register after screen
 * rebuild; count/name/path borrows must remain valid for each synchronous
 * filtering call. The DB worker sees only copied id/kind/query data and never
 * calls or retains any UI callback. */
void library_search_register(search_binding_id_t id, const library_search_config_t *config);
int library_search_find_filtered_row(search_binding_id_t id, int source_index);
bool library_search_showing_results(search_binding_id_t id);
bool library_search_try_map_filtered_row(search_binding_id_t id, int display_index, int *source_index);
bool library_search_is_active(search_binding_id_t id);
bool library_search_is_active_on_list(lv_obj_t *list);
void library_search_invalidate_catalog(void);
void library_search_invalidate_bindings(void);
void library_search_quiesce_for_ui_reload(void);
bool library_search_has_active_job(void);
void library_search_join_for_shutdown(void);
void library_search_update_header(search_binding_id_t id, lv_obj_t *title,
                                  lv_obj_t *preferred_action, bool editing);

/* Existing public entry points keep their original names and signatures. */
int search_remap_index(search_binding_id_t id, int display_index);
void poll_search_job(void);
bool search_close_if_active_for_screen(lv_obj_t *screen);
void search_textarea_value_changed_cb(lv_event_t *e);
void search_textarea_clicked_cb(lv_event_t *e);
void search_keypad_dismissed(void);

#endif
