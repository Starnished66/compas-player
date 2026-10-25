#ifndef BOOTLOADER_FB_DRAW_INTERNAL_H
#define BOOTLOADER_FB_DRAW_INTERNAL_H

#include "fb_draw.h"

/* Shared draw-buffer state for the Linux /dev/fb0 backend and the host
 * in-memory backend. Callers outside those two files must use fb_draw.h. */
extern int fb_fd;
extern uint16_t * fb_mem;
extern size_t fb_mem_len;
extern int fb_stride_pixels;
extern int fb_base_offset_pixels;
extern uint16_t * bg_cache;
extern uint16_t * back_buffer;

#endif /* BOOTLOADER_FB_DRAW_INTERNAL_H */
