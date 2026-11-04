/* Private to the Songs page files (gui_library_group_songs.c, gui_group_songs_rows.c, gui_group_songs_edit.c). */
#ifndef GUI_GROUP_SONGS_INTERNAL_H
#define GUI_GROUP_SONGS_INTERNAL_H

#include "gui_library_internal.h"
#include "gui_list_reorder.h"
#include <sys/stat.h>

/* State owned by gui_library_group_songs.c. */
extern char group_songs_refresh_album_artist[128];
extern int group_songs_page_start;
extern lv_obj_t * group_songs_selection_checks[GROUP_SONGS_PAGE_SIZE];
extern lv_obj_t * group_songs_delete_btn;
extern lv_obj_t * group_songs_select_all_row;
extern lv_obj_t * group_songs_select_all_checkbox;
extern lv_obj_t * group_songs_select_all_label;
extern int group_songs_populated_page_start;
extern bool group_songs_anchor_pending;
extern int group_songs_anchor_index;
extern int32_t group_songs_anchor_offset;
extern bool group_songs_music_submenu;
extern const char * group_songs_edit_m3u_path;
extern struct stat group_songs_file_stat;
extern bool group_songs_file_stat_valid;
extern bool group_songs_edit_mode;

/* gui_library_group_songs.c */
void group_play_cb(lv_event_t * e);
void group_shuffle_cb(lv_event_t * e);
group_song_entry_t * group_song_entries_take_paths(char ** paths, int count);
void group_song_row_click_cb(lv_event_t * e);
void group_song_row_long_press_cb(lv_event_t * e);
extern bool group_song_row_long_press_fired; /* cleared by ui_clear_flag_on_press_cb */
bool group_songs_album_layout(void);
int group_songs_page_end(void);
void group_songs_place_now_playing_bar(bool laid_out);
int group_songs_visible_count(void);

/* gui_group_songs_rows.c */
bool group_song_has_cover(int index);
void group_song_row_show_detail(lv_obj_t * row, const char * detail);
void group_song_row_text_offset(lv_obj_t * row, bool with_thumbnail);
void group_songs_add_disc_header(int i);
void group_songs_build_pause(void);
void group_songs_build_reset(void);
void group_songs_build_resume(void);
void group_songs_forget_row_objects(void);
lv_obj_t * group_songs_new_row(int i, const char * title, int32_t reserve);
void group_songs_scroll_cb(lv_event_t * e);
void group_songs_wake_cb(lv_event_t * e);

/* gui_group_songs_edit.c */
lv_obj_t * add_group_songs_select_all_row(void);
void group_songs_add_edit_entry(int i);
void group_songs_capture_scroll_anchor(const int * deleted_indexes, int deleted_count);
void group_songs_clear_selection(void);
bool group_songs_reset_selection(void);
void group_songs_delete_selected_cb(lv_event_t * e);
void group_songs_edit_btn_cb(lv_event_t * e);
bool group_songs_playlist_stale(void);
void group_songs_reorder_detach(void);
int32_t group_songs_reorder_handle_width(void);
void group_songs_reorder_rearm(void);
void group_songs_update_edit_header(void);

#endif
