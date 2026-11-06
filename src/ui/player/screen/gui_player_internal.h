#ifndef GUI_PLAYER_INTERNAL_H
#define GUI_PLAYER_INTERNAL_H

/* Declarations shared between the gui_player*.c files and the artwork code in
 * ../artwork/. Nothing else includes this header; public API stays in
 * gui_player.h. Grows as gui_player.c is split. */

#include <stdbool.h>
#include <lvgl/lvgl.h>
#include "../../network/subsonic/gui_subsonic.h"
#include "../../../audio/engine/audio.h"
#include "../artwork/player_artwork_jobs.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/remote/remote_track.h"

/* Used by the queue code in ../queue/. */
void request_track_metadata(int index, bool notify, bool probe, bool audio_tags);
void player_show_empty_queue(void);

/* Used by the queue controller and remote queue in ../queue/. */
void player_set_song_count(int index, int total);
void playback_intent_bump(void);
void player_playback_generation_reset(void);

/* Used by the playback controller in ../transport/player_playback.c. */
bool seed_track_metadata(int index, track_metadata_t * out_meta);
void prepare_track_metadata(int index, bool notify, bool probe, bool audio_tags,
                            bool submit_request);
void clear_selection_metadata_seed(void);
void request_track_audio_metadata(int index);

/* Display-label and progress owner in gui_player.c, used by the metadata
 * pipeline in gui_player_metadata.c. */
extern lv_obj_t * song_count_label;
extern lv_obj_t * favorite_icon;
extern lv_obj_t * progress_slider;
extern bool favorite_is_set;
void player_set_track_display_text(const char * title, const char * artist, const char * album);
void player_set_format_badge_text(const char * text);
void player_progress_display_reset(void);
void player_progress_display_set_deferred(double position, double duration_seconds);
void player_track_change_timeline_play(void);

/* Cover decode launchers in gui_player.c, used for artwork dispatch. */
void cover_decode_request_snapshot_frost(cover_decode_request_t * req);
void launch_cover_decode_req(cover_decode_request_t r);
void launch_cover_decode_from_track(int for_index, const char * track_path, const char * artist,
                                    const char * album, const char * album_artist);
void launch_cover_decode_from_url(int for_index, const char * url, bool verify_tls);
void launch_cover_decode_from_subsonic(int for_index, const subsonic_server_t * server, const char * cover_id,
                                       bool allow_net);

/* Metadata pipeline in gui_player_metadata.c, used by gui_player.c. */
void player_metadata_request_current_for_init(void);
void player_metadata_invalidate_queue(void);
void player_metadata_invalidate_ui(void);
void player_reset_now_playing_extras(void);
void show_favorite_state(bool is_favorite);
void maybe_prefetch_next_cover(void);
void player_next_cover_prefetch_reset(void);

/* Track-source helpers in gui_player_track_source.c. */
typedef struct {
    const subsonic_stream_song_meta_t * subsonic; /* borrowed, NULL if none */
    bool is_subsonic;
    remote_track_meta_t remote;                   /* valid only when is_remote */
    bool is_remote;                               /* a registered entry matched */
} track_source_t;

bool is_http_url(const char * path);
const char * info_path_hint(const char * path);
audio_codec_t info_codec_from_hint(const char * hint);
void info_container_from_hint(const char * hint, char out[16]);
const subsonic_stream_song_meta_t * subsonic_meta_for_path(int index, const char * path);
void track_source_resolve(int index, const char * path, track_source_t * out);
void replaygain_for_mode(int mode, const track_metadata_t * meta, bool * out_has_gain,
                         double * out_gain_db, bool * out_has_peak, double * out_peak);
void player_track_source_set_cached_format(const audio_current_format_info_t * format);

/* Count/badge row geometry in ../layout/player_count_row.c. The widgets stay
 * owned by gui_player.c. */
extern lv_obj_t * quality_pill;
extern lv_obj_t * format_badge_label;
void player_count_row_capture(lv_obj_t * count, lv_obj_t * pill, lv_obj_t * badge);
void player_count_row_refresh(void);
void player_count_row_reset(void);

/* More menu and delete-song popup in gui_player_more_menu.c. */
void build_delete_song_popup(void);
void build_more_menu_popup(void);
void more_icon_event_cb(lv_event_t * e);
void player_more_menu_teardown(void);
bool player_delete_song_popup_is_visible(void);
bool player_more_menu_is_visible(void);

/* Backdrop and cover widgets owned by gui_player.c, read live by
 * ../artwork/player_backdrop.c and player_cover.c. They are cleared with the
 * rest of the widget globals on teardown and layout rebuild, so the artwork
 * code never holds a copy. */
extern lv_obj_t * player_overlay_panel;
extern lv_obj_t * player_background_img;
extern lv_obj_t * cover_card;
extern lv_obj_t * cover_img;

/* Layout state owned by gui_player.c, read by the cover fit in
 * ../artwork/player_cover.c. */
extern bool player_layout_builtin;
extern lv_obj_t * player_cover_thumbnail;

#endif /* GUI_PLAYER_INTERNAL_H */
