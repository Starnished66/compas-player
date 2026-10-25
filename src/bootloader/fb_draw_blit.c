#include "fb_draw.h"

#include <stddef.h>
#include <stdint.h>

/* Dest-buffer A8/RGBA blits used by selector_ui to compose logos onto its
 * RGB565 canvas. These write the caller buffer, not the framebuffer
 * back_buffer -- the selector painter stays independent of /dev/fb0. */

static uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void blend_pixel(uint16_t * dest, int dest_w, int dest_h, size_t stride,
                        int x, int y, uint8_t r, uint8_t g, uint8_t b, unsigned alpha) {
    if (!dest || x < 0 || y < 0 || x >= dest_w || y >= dest_h || alpha == 0) return;
    if (stride < (size_t) dest_w) return;
    size_t i = (size_t) y * stride + (size_t) x;
    if (alpha >= 255) {
        dest[i] = pack565(r, g, b);
        return;
    }
    uint16_t old = dest[i];
    unsigned br = (unsigned) (((old >> 11) & 31) * 255 / 31);
    unsigned bg = (unsigned) (((old >> 5) & 63) * 255 / 63);
    unsigned bb = (unsigned) ((old & 31) * 255 / 31);
    unsigned inv = 255u - alpha;
    dest[i] = pack565((uint8_t) ((r * alpha + br * inv + 127u) / 255u),
                      (uint8_t) ((g * alpha + bg * inv + 127u) / 255u),
                      (uint8_t) ((b * alpha + bb * inv + 127u) / 255u));
}

void fb_draw_blit_a8(uint16_t * dest, int dest_w, int dest_h, size_t dest_stride,
                     int x, int y, const uint8_t * pixels, int width, int height,
                     uint32_t rgb) {
    if (!dest || !pixels || dest_w <= 0 || dest_h <= 0 || width <= 0 || height <= 0) return;
    uint8_t r = (uint8_t) ((rgb >> 16) & 255);
    uint8_t g = (uint8_t) ((rgb >> 8) & 255);
    uint8_t b = (uint8_t) (rgb & 255);
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t) row * (size_t) width;
        for (int col = 0; col < width; col++)
            blend_pixel(dest, dest_w, dest_h, dest_stride, x + col, y + row, r, g, b, src[col]);
    }
}

void fb_draw_blit_rgba(uint16_t * dest, int dest_w, int dest_h, size_t dest_stride,
                       int x, int y, const uint8_t * pixels, int width, int height) {
    if (!dest || !pixels || dest_w <= 0 || dest_h <= 0 || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t) row * (size_t) width * 4u;
        for (int col = 0; col < width; col++) {
            const uint8_t * p = src + (size_t) col * 4u;
            blend_pixel(dest, dest_w, dest_h, dest_stride, x + col, y + row, p[0], p[1], p[2], p[3]);
        }
    }
}
