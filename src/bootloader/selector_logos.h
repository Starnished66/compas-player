#ifndef BOOTLOADER_SELECTOR_LOGOS_H
#define BOOTLOADER_SELECTOR_LOGOS_H

#include <stdint.h>

/* Embedded selector marks. Wordmark pixels are A8 (stride = width), tinted
 * at draw time. Rockbox pixels are RGBA8888 (stride = width * 4). Generated
 * from assets/brand/compas-wordmark.svg and
 * assets/bootloader/selector/rockbox-logo.svg. */

typedef struct {
    int width;
    int height;
    const uint8_t * pixels;
} selector_logo_a8_t;

typedef struct {
    int width;
    int height;
    const uint8_t * pixels;
} selector_logo_rgba_t;

extern const selector_logo_a8_t selector_wordmark_480;
extern const selector_logo_a8_t selector_wordmark_320;
extern const selector_logo_rgba_t selector_rockbox_480;
extern const selector_logo_rgba_t selector_rockbox_320;

const selector_logo_a8_t * selector_logo_wordmark(int canvas_width);
const selector_logo_rgba_t * selector_logo_rockbox(int canvas_width);

#endif /* BOOTLOADER_SELECTOR_LOGOS_H */
