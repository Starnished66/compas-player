#ifndef BOOTLOADER_SELECTOR_UI_H
#define BOOTLOADER_SELECTOR_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure selector painter. It owns an RGB565 buffer but never opens the
 * framebuffer, reads input, starts a process, changes settings, or boots a
 * player. The caller owns selection/timing/actions and may copy pixels with
 * fb_draw_blit_rgb565() followed by fb_flush(). NULL paths select the target
 * defaults; explicit paths are useful for the standalone host renderer. */
bool selector_ui_init(int width, int height, const char * font_path,
                       const char * settings_path);
void selector_ui_shutdown(void);

const uint16_t * selector_ui_pixels(size_t * stride_pixels);
int selector_ui_width(void);
int selector_ui_height(void);

void selector_ui_draw_selector(int selected, bool rockbox_available,
                               unsigned countdown_seconds,
                               bool countdown_cancelled, bool adb_enabled);
void selector_ui_draw_starting(bool rockbox);
void selector_ui_draw_tools(int selected, int scroll_pixels, bool adb_enabled);

/* Return row indices, SELECTOR_UI_HIT_BACK, or SELECTOR_UI_HIT_NONE. Tools
 * hit testing includes the supplied scroll offset. */
#define SELECTOR_UI_HIT_NONE (-1)
#define SELECTOR_UI_HIT_BACK (-2)
int selector_ui_hit_test_selector(int x, int y);
int selector_ui_hit_test_selector_available(int x, int y, bool rockbox_available);
int selector_ui_hit_test_tools(int x, int y, int scroll_pixels);
int selector_ui_tools_max_scroll(void);
int selector_ui_ensure_visible_tools(int selected, int scroll_pixels);

/* Caller-owned generic picker/progress surfaces for the Tools controller.
 * The painter does not enumerate files or execute the selected item. Lists
 * are capped at 64 rows and displayed item text is bounded before rendering. */
#define SELECTOR_UI_MAX_LIST_ITEMS 64
void selector_ui_draw_list(const char * title, const char * const * items,
                           size_t count, int selected, int scroll_pixels,
                           const char * notice);
int selector_ui_list_max_scroll(size_t count);
int selector_ui_hit_test_list(int x, int y, size_t count, int scroll_pixels);
int selector_ui_ensure_visible_list(size_t count, int selected, int scroll_pixels);
void selector_ui_draw_notice(const char * title, const char * message);
void selector_ui_draw_progress(const char * title, const char * message, int percent);

#endif /* BOOTLOADER_SELECTOR_UI_H */
