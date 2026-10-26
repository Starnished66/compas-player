#include "fb_draw_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host in-memory framebuffer: same fb_draw primitives and flush path as the
 * device, with a compact RGB565 front buffer instead of mmap(/dev/fb0). */

bool fb_open(void) {
    fb_fd = -1;
    fb_stride_pixels = FB_WIDTH;
    fb_base_offset_pixels = 0;
    fb_mem_len = (size_t) FB_WIDTH * (size_t) FB_HEIGHT * sizeof(uint16_t);
    fb_mem = calloc(1, fb_mem_len);
    back_buffer = malloc(fb_mem_len);
    if (!fb_mem || !back_buffer) {
        perror("fb_draw: host buffer malloc");
        free(fb_mem);
        free(back_buffer);
        fb_mem = NULL;
        back_buffer = NULL;
        fb_mem_len = 0;
        return false;
    }
    return true;
}

void fb_close(void) {
    free(fb_mem);
    fb_mem = NULL;
    fb_mem_len = 0;
    fb_stride_pixels = 0;
    fb_base_offset_pixels = 0;
    free(bg_cache);
    bg_cache = NULL;
    free(back_buffer);
    back_buffer = NULL;
}
