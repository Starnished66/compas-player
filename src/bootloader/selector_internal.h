#ifndef BOOTLOADER_SELECTOR_INTERNAL_H
#define BOOTLOADER_SELECTOR_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Shared canvas and primitives between selector_ui.c and selector_paint.c. */

extern uint16_t * canvas;
extern int canvas_width;
extern int canvas_height;
extern size_t canvas_stride;
extern int clip_top;
extern int clip_bottom;

void put_pixel(int x, int y, uint32_t color, unsigned alpha);
void blit_a8(int x, int y, const uint8_t * pixels, int width, int height, uint32_t color);
void blit_rgba(int x, int y, const uint8_t * pixels, int width, int height);
void fill_rect(int x, int y, int w, int h, uint32_t color);
void rounded_fill(int x, int y, int w, int h, int radius, uint32_t color);
void line(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
int text_width(const char * text, int px);
int text_width_medium(const char * text, int px);
void draw_text(int x, int top, const char * text, int px, uint32_t color);
void draw_text_medium(int x, int top, const char * text, int px, uint32_t color);
void draw_text_centered(int center_x, int top, const char * text, int px, uint32_t color);
void draw_music_note(int x, int y, int size, uint32_t color);
void draw_mountain(int x, int y, int size, uint32_t color);
void draw_tools_icon(int x, int y, int size, uint32_t color);

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
