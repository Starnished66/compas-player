#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

typedef enum {
    GUI_FOLDER_ACTION_PLAY_SEQUENTIAL,
    GUI_FOLDER_ACTION_PLAY_SHUFFLE,
    GUI_FOLDER_ACTION_QUEUE,
    GUI_FOLDER_ACTION_PLAYLIST_APPEND,
    GUI_FOLDER_ACTION_PLAYLIST_CREATE
} gui_folder_action_t;

/* Registered on the UI thread; callbacks must remain valid for the module's
 * lifetime. Storage is queried at the original poll/completion points. */
typedef bool (*gui_folder_storage_invalidated_cb_t)(void);
typedef void (*gui_folder_open_picker_cb_t)(const char *folder_path);
typedef void (*gui_folder_page_jump_cb_t)(const char *folder_path);

void gui_folder_actions_init(gui_folder_storage_invalidated_cb_t storage_invalidated,
                             gui_folder_open_picker_cb_t open_picker,
                             gui_folder_page_jump_cb_t page_jump);
void gui_folder_actions_build_menu(void);
void gui_folder_actions_open_menu(const char *path, bool show_page_jump);
void gui_folder_actions_menu_screen_unloaded_cb(lv_event_t *event);
void gui_folder_actions_invalidate_storage_targets(void);
void gui_folder_actions_cancel(void);
/* Stop/join before other library teardown, then delete the menu at its UI slot. */
void gui_folder_actions_stop_work(void);
void gui_folder_actions_teardown(void);
bool gui_folder_actions_busy(void);
bool gui_folder_actions_menu_visible(void);

/* Copies request strings/token into the job. return_screen is borrowed and
 * remains valid under the navigation stack's existing lifetime rules. */
bool gui_folder_actions_start_playlist_job(bool create, const char *source_dir,
                                           const char *destination, const char *playlist_name,
                                           uint64_t target_generation, lv_obj_t *return_screen);
