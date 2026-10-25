#ifndef PLAYER_INLINE_LYRICS_H
#define PLAYER_INLINE_LYRICS_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl/lvgl.h>

typedef struct {
    const lv_image_dsc_t * descriptor; /* borrowed; owned by Player cover state */
    bool has_pixels;
    uint64_t generation;
} player_inline_cover_info_t;

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * cover_card_obj;
    lv_obj_t * cover_image;
    lv_obj_t * cover_thumbnail;
    lv_obj_t * cover_fade;
    lv_obj_t * overlay;
    lv_obj_t * title;
    lv_obj_t * artist;
    lv_obj_t * album;
    lv_obj_t * favorite_circle_obj;
    lv_obj_t * lyrics_area;
    lv_anim_timeline_t * lyrics_open;
    lv_anim_timeline_t * lyrics_close;
    bool builtin_layout;
    bool artist_is_standin;
    bool album_is_standin;
    bool backdrop_contains_cover;
    lv_event_cb_t cover_size_changed_cb;

    /* All callbacks are borrowed and remain valid for the player screen
     * lifetime. Queries run on the UI thread at each point the cover state
     * is used; no cover descriptor or pixels are owned by this module. */
    lv_obj_t * (*current_screen)(void);
    bool (*query_cover)(player_inline_cover_info_t * info);
    bool (*lyrics_enabled)(void);
    bool (*display_refresh_allowed)(void);
    void (*fit_cover_img_to_card)(void);
    void (*pause_artwork_cache_timer)(void);
    void (*mark_transition_dirty)(void);
    uint32_t (*timeline_play)(lv_anim_timeline_t * timeline,
                              lv_anim_completed_cb_t completed_cb,
                              bool * completion_attached,
                              bool * runner_available);
} player_inline_lyrics_view_t;

/* UI-thread owned inline lyrics controller. Widget pointers and authored
 * timelines in the view are borrowed from the Player screen and invalid after
 * reset; generated fallback timelines remain owned by this module. */
void player_inline_lyrics_bind_cover(const player_inline_lyrics_view_t * view);
void player_inline_lyrics_bind_pane(const player_inline_lyrics_view_t * view);
void player_inline_lyrics_reset(void);
void player_inline_lyrics_reset_hit_targets(void);
void player_inline_lyrics_reset_controls(void);
void player_inline_lyrics_add_hit_target(lv_obj_t * obj);
void player_inline_lyrics_add_control(lv_obj_t * obj);
void player_inline_lyrics_update_thumbnail_clickable(lv_obj_t * thumbnail);

void player_inline_lyrics_set_open(bool open, bool animate);
bool player_inline_lyrics_is_open(void);
bool player_inline_lyrics_area_is_open(void);
bool player_inline_lyrics_is_animating(void);
void player_inline_lyrics_cancel_timer(void);
void player_inline_lyrics_generated_release(bool keep_backdrop);
void player_inline_lyrics_invalidate_cover_source(void);
void player_inline_lyrics_release_cache_memory(void);
const void * player_inline_lyrics_source_for_scaling(const void * image_source);
void player_inline_lyrics_register_perf_display_events(void);

void player_inline_lyrics_cover_tap_cb(lv_event_t * e);
void player_inline_lyrics_screen_unloaded_cb(lv_event_t * e);
void player_inline_lyrics_area_empty_tap_cb(void);

#endif
