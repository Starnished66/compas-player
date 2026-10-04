#ifndef GUI_LOCK_SCREEN_H
#define GUI_LOCK_SCREEN_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

typedef enum {
    LOCK_SCREEN_MODE_OFF = 0,
    LOCK_SCREEN_MODE_ALBUM_ART,
    LOCK_SCREEN_MODE_IMAGE,
    LOCK_SCREEN_MODE_CLOCK,
} gui_lock_screen_mode_t;

typedef enum {
    /* Preserve the historical native behavior when older plugins omit it. */
    LOCK_SCREEN_IMAGE_FIT_NATURAL = 0,
    LOCK_SCREEN_IMAGE_FIT_CONTAIN,
    LOCK_SCREEN_IMAGE_FIT_COVER,
} gui_lock_screen_image_fit_t;

typedef struct {
    gui_lock_screen_mode_t mode;
    gui_lock_screen_image_fit_t image_fit;
    char image_path[256];
    bool clock_24h;
} gui_lock_screen_options_t;

/* Returns the lock screen LVGL object, or NULL if not currently created. */
lv_obj_t * gui_lock_screen_get_screen(void);

/* Returns true if the lock screen is currently visible/active on screen. */
bool gui_lock_screen_is_showing(void);

/* Shows the lock screen with the specified options. Returns true on success. */
bool gui_lock_screen_show(const gui_lock_screen_options_t * options);
/* Uses the same renderer without navigating; the parent owns the preview. */
lv_obj_t * gui_lock_screen_create_preview(lv_obj_t * parent, const gui_lock_screen_options_t * options);
/* Refresh metadata, and optionally artwork, on the UI thread. */
void gui_lock_screen_refresh(bool artwork_changed);
/* File-backed photos may be excluded for latency-sensitive wake readiness. */
bool gui_lock_screen_has_background_work(bool include_photos);

/* Drag-state recovery hooks for gui_navigation.c / gui.c */
void gui_lock_screen_swipe_recover(void * ctx);
void gui_lock_screen_reset_drag_state(void);

/* Lifecycle functions for gui_reload.c */
void gui_lock_screen_init(void);
void gui_lock_screen_teardown(void);

#endif /* GUI_LOCK_SCREEN_H */
