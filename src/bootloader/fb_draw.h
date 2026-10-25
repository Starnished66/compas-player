#ifndef BOOTLOADER_FB_DRAW_H
#define BOOTLOADER_FB_DRAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

/* Display resolution from board_config.h.
 * HiBy R1 is 480x800 RGB565; R3 Pro II is 480x720 RGB565.
 * fb_open() verifies the runtime framebuffer geometry against these dimensions. */
#define FB_WIDTH BOARD_SCREEN_WIDTH
#define FB_HEIGHT BOARD_SCREEN_HEIGHT

#define BOOTLOADER_BG_PATH "/usr/resource/litegui/theme2/boot_animation/en/0.jpg"

typedef uint16_t fb_color_t;

/* Opens the display. On device this mmaps /dev/fb0 and verifies its
 * geometry/format actually match FB_WIDTH/FB_HEIGHT/RGB565 via
 * FBIOGET_VSCREENINFO -- a mismatch is treated as fatal (returns false)
 * rather than drawing garbage into a differently-shaped or differently-packed
 * buffer. Mirrors src/main.c's own wait_for_fbdev_ready() retry shape (the
 * device node can exist before the underlying driver has actually settled).
 * The host selector simulator (make selector-sim) links fb_host.c instead,
 * which allocates an in-memory RGB565 buffer of the same size. */
bool fb_open(void);

/* Unmaps and closes /dev/fb0. Must be called before execve()'ing into a
 * player -- see main.c's own doc comment on why this matters even though
 * execve() itself would drop the mapping regardless. */
void fb_close(void);

fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_fill(fb_color_t color);
void fb_fill_rect(int x, int y, int w, int h, fb_color_t color);

/* Copy a complete RGB565 frame into the off-screen buffer. Dimensions must
 * exactly match this board; stride is measured in uint16_t pixels. This does
 * not flush to the panel -- callers still assemble their frame before calling
 * fb_flush(). */
bool fb_draw_blit_rgb565(const uint16_t * pixels, int width, int height, size_t stride_pixels);

/* Blend an A8 image tinted with 0xRRGGBB, or an RGBA8888 image, into an
 * RGB565 destination. dest_stride is in pixels. These write dest, not the
 * framebuffer back_buffer. Transparent source pixels are skipped. */
void fb_draw_blit_a8(uint16_t * dest, int dest_w, int dest_h, size_t dest_stride,
                     int x, int y, const uint8_t * pixels, int width, int height,
                     uint32_t rgb);
void fb_draw_blit_rgba(uint16_t * dest, int dest_w, int dest_h, size_t dest_stride,
                       int x, int y, const uint8_t * pixels, int width, int height);

/* Uppercase letters, digits, space, ':', '-', and '_' only -- see glyph5x7.c's own
 * font table doc comment for exactly why the character set is this
 * narrow. Any other byte is drawn as a blank cell rather than skipped, so
 * a typo in a caller's string is visible (a gap) instead of silently
 * shifting all following characters left. */
void fb_draw_text(int x, int y, const char * text, fb_color_t color);
void fb_draw_text_centered(int y, const char * text, fb_color_t color);
int fb_text_width(const char * text);
int fb_text_height(void);

/* Copies the off-screen back buffer to the visible framebuffer.
 * Call after a complete frame has been drawn. */
void fb_flush(void);

/* Compact RGB565 back buffer, stride FB_WIDTH. Valid after fb_open(). */
const uint16_t * fb_back_pixels(void);

/* Decodes a JPEG file straight into the off-screen drawing buffer at
 * (0,0), pixel for pixel, via tjpgd (already vendored for the main
 * player's own cover-art decode, lvgl/src/libs/tjpgd/ -- fully
 * standalone, no other LVGL dependency). Deliberately does NOT scale or
 * crop: this only exists to
 * draw the boot splash asset, expected to be exactly FB_WIDTH x FB_HEIGHT
 * already -- a
 * general-purpose image-fit pipeline is exactly the kind of scope this
 * bootloader's own design deliberately avoids pulling in. Returns false
 * (caller should fall back to a plain fb_fill()) if the file can't be
 * opened, isn't a JPEG tjpgd can parse, or its dimensions don't match the
 * screen exactly -- never scales/crops to fit as a fallback.
 *
 * On success, also caches the decoded frame (a plain heap copy, no
 * padding) so fb_restore_background() below can redraw it without
 * decoding the JPEG again. */
bool fb_draw_background_jpeg(const char * path);

/* Draws a valid configured Compás RGB565 startup image, falling back to the
 * bundled JPEG (optionally tinted from saved settings) on any load failure. */
bool fb_draw_startup_background(void);

/* Restores the cached boot splash, or fills with fallback_color if decoding
 * the splash failed. The installer uses this before drawing update status. */
void fb_restore_background(fb_color_t fallback_color);

/* Draws installer status over the cached splash with a dark backing panel. */
void fb_draw_installer_warning(void);

#endif /* BOOTLOADER_FB_DRAW_H */
