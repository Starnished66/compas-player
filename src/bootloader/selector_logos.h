#ifndef BOOTLOADER_SELECTOR_LOGOS_H
#define BOOTLOADER_SELECTOR_LOGOS_H

#include <stdint.h>

/* Embedded selector marks. Wordmark pixels are A8 (stride = width), tinted
 * at draw time. Rockbox pixels are RGBA8888 (stride = width * 4). Generated
 * from assets/brand/compas-wordmark.svg and
 * assets/bootloader/selector/rockbox-logo.svg.
 *
 * The linked raster is chosen at compile time from BOARD_SCREEN_WIDTH.
 * selector_logo_rockbox() is NULL on boards that never show a Rockbox row. */

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

const selector_logo_a8_t * selector_logo_wordmark(void);
const selector_logo_rgba_t * selector_logo_rockbox(void);

#endif /* BOOTLOADER_SELECTOR_LOGOS_H */
