#ifndef BOOTLOADER_SELECTOR_INTERNAL_H
#define BOOTLOADER_SELECTOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared canvas and primitives between selector_ui.c and selector_paint.c.
 * sel_ prefix avoids clashing with Rockbox symbols of the same names. */

extern uint16_t * sel_canvas;
extern int sel_canvas_width;
extern int sel_canvas_height;
extern size_t sel_canvas_stride;
extern int sel_clip_top;
extern int sel_clip_bottom;

void sel_put_pixel(int x, int y, uint32_t color, unsigned alpha);
void sel_blit_a8(int x, int y, const uint8_t * pixels, int width, int height, uint32_t color);
void sel_blit_rgba(int x, int y, const uint8_t * pixels, int width, int height);
void sel_fill_rect(int x, int y, int w, int h, uint32_t color);
void sel_rounded_fill(int x, int y, int w, int h, int radius, uint32_t color);
void sel_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
int sel_text_width(const char * text, int px);
int sel_text_width_medium(const char * text, int px);
void sel_draw_text(int x, int top, const char * text, int px, uint32_t color);
void sel_draw_text_medium(int x, int top, const char * text, int px, uint32_t color);
void sel_draw_text_centered(int center_x, int top, const char * text, int px, uint32_t color);
void sel_fit_text(char out[128], const char * input, int max_width, int px, bool medium);
void sel_draw_music_note(int x, int y, int size, uint32_t color);
void sel_draw_mountain(int x, int y, int size, uint32_t color);
void sel_draw_tools_icon(int x, int y, int size, uint32_t color);

void selector_paint_selector(int selected, bool rockbox_available,
                             unsigned countdown_seconds, bool countdown_cancelled);
void selector_paint_starting(bool rockbox);
void selector_paint_tools(int selected, int scroll_pixels, bool adb_enabled);
void selector_paint_list(const char * title, const char * const * items,
                         size_t count, int selected, int scroll_pixels,
                         const char * notice);
void selector_paint_notice(const char * title, const char * message);
void selector_paint_progress(const char * title, const char * message, int percent);

#endif /* BOOTLOADER_SELECTOR_INTERNAL_H */
