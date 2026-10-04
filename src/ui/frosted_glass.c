#include "frosted_glass.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FROSTED_RGB565_WORK_WIDTH 60
#define FROSTED_RGB565_WORK_HEIGHT 100
#define FROSTED_MAX_SOURCE_PIXELS (16u * 1024u * 1024u)
#define FROSTED_MAX_OUTPUT_PIXELS (4u * 1024u * 1024u)
#define FROSTED_MAX_IMAGE_DIMENSION 8192

/* 1D box blur via a sliding running sum -- O(length) regardless of radius,
 * rather than O(length * radius) from re-summing the whole window at every
 * pixel. `stride` is measured in elements, not bytes, so the same function
 * serves both passes of separable 2D blurs: 1 for a horizontal pass along
 * a contiguous row, `width` for a vertical pass down a column. Edges are
 * clamped (repeats the edge pixel) rather than wrapping or zero-padding,
 * so the blur doesn't darken/lighten near the image boundary. */
void box_blur_1d(const uint8_t * src, uint8_t * dst, int length, int stride, int radius) {
    if (length <= 0) return;
    int window = radius * 2 + 1;
    int sum = 0;
    for (int i = -radius; i <= radius; i++) {
        int idx = i < 0 ? 0 : (i >= length ? length - 1 : i);
        sum += src[idx * stride];
    }
    for (int i = 0; i < length; i++) {
        dst[i * stride] = (uint8_t) (sum / window);
        int add_idx = i + radius + 1;
        if (add_idx >= length) add_idx = length - 1;
        int rem_idx = i - radius;
        if (rem_idx < 0) rem_idx = 0;
        sum += src[add_idx * stride] - src[rem_idx * stride];
    }
}

static uint16_t rgb888_to_565_with_threshold(int r, int g, int b, int threshold) {
    r += threshold / 8 - 4;
    g += threshold / 16 - 2;
    b += threshold / 8 - 4;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t) (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* RGB565 has only 32 red/blue and 64 green levels, so a strong blur exposes
 * broad contour bands even though all filtering above is done in 8-bit
 * planes. Ordered 8x8 dithering trades those coherent bands for a tiny,
 * stable sub-pixel texture. Stable (not random) matters because a changing
 * noise field would shimmer on every redraw. */
uint16_t rgb888_to_565_dithered(int r, int g, int b, int x, int y) {
    static const uint8_t bayer8[8][8] = {
        { 0,48,12,60, 3,51,15,63 }, { 32,16,44,28,35,19,47,31 },
        { 8,56, 4,52,11,59, 7,55 }, { 40,24,36,20,43,27,39,23 },
        { 2,50,14,62, 1,49,13,61 }, { 34,18,46,30,33,17,45,29 },
        { 10,58, 6,54, 9,57, 5,53 }, { 42,26,38,22,41,25,37,21 }
    };
    int threshold = bayer8[y & 7][x & 7];
    return rgb888_to_565_with_threshold(r, g, b, threshold);
}

uint8_t frosted_glass_spatial_threshold(int x, int y) {
    /* Coordinate-only noise: immutable between redraws and decodes. Unlike
     * the Bayer matrix it does not concentrate high/low pixels on alternate
     * scanlines or columns across the full-screen Player background. Keep
     * the same quantization amplitude; removing dither would restore bands. */
    uint32_t h = (uint32_t)x * 0x1f123bb5u ^ (uint32_t)y * 0x5f356495u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (uint8_t)(h >> 26);
}

uint16_t rgb888_to_565_spatial_dithered(int r, int g, int b, int x, int y) {
    return rgb888_to_565_with_threshold(r, g, b, frosted_glass_spatial_threshold(x, y));
}

static uint8_t frosted_glass_bilerp_plane(const uint8_t * plane, int width, int height, uint32_t x_fp, uint32_t y_fp) {
    int x0 = (int) (x_fp >> 16), y0 = (int) (y_fp >> 16);
    int x1 = x0 + 1 < width ? x0 + 1 : x0;
    int y1 = y0 + 1 < height ? y0 + 1 : y0;
    uint32_t fx = x_fp & 0xffffu, fy = y_fp & 0xffffu;
    uint32_t top = ((uint32_t) plane[y0 * width + x0] * (65536u - fx) +
                    (uint32_t) plane[y0 * width + x1] * fx) >> 16;
    uint32_t bottom = ((uint32_t) plane[y1 * width + x0] * (65536u - fx) +
                       (uint32_t) plane[y1 * width + x1] * fx) >> 16;
    return (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
}

static void frosted_glass_blur_passes(uint8_t * plane, uint8_t * tmp, int width, int height, int radius, int passes) {
    for (int pass = 0; pass < passes; pass++) {
        for (int y = 0; y < height; y++) box_blur_1d(plane + y * width, tmp + y * width, width, 1, radius);
        memcpy(plane, tmp, (size_t) width * height);
        for (int x = 0; x < width; x++) box_blur_1d(plane + x, tmp + x, height, width, radius);
        memcpy(plane, tmp, (size_t) width * height);
    }
}

uint8_t * frosted_glass_blur_rgb565(const uint16_t * source,
                                    int source_width, int source_height,
                                    int out_width, int out_height,
                                    int blur_radius, int blur_passes,
                                    int darken_num, int darken_den,
                                    bool mirror_y) {
    if (!source || source_width <= 0 || source_height <= 0 ||
        out_width <= 0 || out_height <= 0 ||
        source_width > FROSTED_MAX_IMAGE_DIMENSION || source_height > FROSTED_MAX_IMAGE_DIMENSION ||
        out_width > FROSTED_MAX_IMAGE_DIMENSION || out_height > FROSTED_MAX_IMAGE_DIMENSION ||
        (size_t) source_width > FROSTED_MAX_SOURCE_PIXELS / (size_t) source_height ||
        (size_t) out_width > FROSTED_MAX_OUTPUT_PIXELS / (size_t) out_height) {
        return NULL;
    }

    if (blur_radius < 0) blur_radius = 0;
    if (blur_radius > 64) blur_radius = 64;
    if (blur_passes < 0) blur_passes = 0;
    if (blur_passes > 16) blur_passes = 16;
    if (darken_num < 0) darken_num = 0;
    if (darken_den <= 0) darken_den = 1;

    const int w = FROSTED_RGB565_WORK_WIDTH;
    const int h = FROSTED_RGB565_WORK_HEIGHT;
    const size_t work_bytes = (size_t) w * (size_t) h;
    uint8_t * r = malloc(work_bytes);
    uint8_t * g = malloc(work_bytes);
    uint8_t * b = malloc(work_bytes);
    uint8_t * tmp = malloc(work_bytes);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }

    /* Center-crop the input to the output aspect ratio before downsampling. */
    int crop_x = 0, crop_y = 0;
    int crop_w = source_width, crop_h = source_height;
    if ((int64_t) source_width * out_height > (int64_t) source_height * out_width) {
        crop_w = (int) ((int64_t) source_height * out_width / out_height);
        if (crop_w < 1) crop_w = 1;
        crop_x = (source_width - crop_w) / 2;
    } else {
        crop_h = (int) ((int64_t) source_width * out_height / out_width);
        if (crop_h < 1) crop_h = 1;
        crop_y = (source_height - crop_h) / 2;
    }

    if (crop_w >= w && crop_h >= h) {
        int x_start[FROSTED_RGB565_WORK_WIDTH];
        int x_end[FROSTED_RGB565_WORK_WIDTH];
        for (int x = 0; x < w; x++) {
            x_start[x] = crop_x + (int) (((int64_t) x * crop_w) / w);
            x_end[x] = crop_x + (int) (((int64_t) (x + 1) * crop_w) / w);
        }

        uint32_t acc_r[FROSTED_RGB565_WORK_WIDTH];
        uint32_t acc_g[FROSTED_RGB565_WORK_WIDTH];
        uint32_t acc_b[FROSTED_RGB565_WORK_WIDTH];

        for (int y = 0; y < h; y++) {
            int dst_y = mirror_y ? (h - 1 - y) : y;
            int y0, y1;
            if (!mirror_y) {
                y0 = crop_y + (int) (((int64_t) dst_y * crop_h) / h);
                y1 = crop_y + (int) (((int64_t) (dst_y + 1) * crop_h) / h);
            } else {
                y0 = crop_y + crop_h - (int) (((int64_t) (dst_y + 1) * crop_h) / h);
                y1 = crop_y + crop_h - (int) (((int64_t) dst_y * crop_h) / h);
            }
            uint32_t cell_h = (uint32_t) (y1 - y0);

            memset(acc_r, 0, sizeof(acc_r));
            memset(acc_g, 0, sizeof(acc_g));
            memset(acc_b, 0, sizeof(acc_b));

            for (int sy = y0; sy < y1; sy++) {
                const uint16_t * src_row = source + (size_t) sy * source_width;
                for (int x = 0; x < w; x++) {
                    uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
                    int sx_end = x_end[x];
                    for (int sx = x_start[x]; sx < sx_end; sx++) {
                        uint16_t px = src_row[sx];
                        r_sum += (px >> 11) & 0x1fu;
                        g_sum += (px >> 5) & 0x3fu;
                        b_sum += px & 0x1fu;
                    }
                    acc_r[x] += r_sum;
                    acc_g[x] += g_sum;
                    acc_b[x] += b_sum;
                }
            }

            size_t dst_row_offset = (size_t) dst_y * w;
            for (int x = 0; x < w; x++) {
                uint32_t cell_w = (uint32_t) (x_end[x] - x_start[x]);
                uint64_t count = (uint64_t) cell_w * cell_h;
                if (count == 0) {
                    r[dst_row_offset + x] = 0;
                    g[dst_row_offset + x] = 0;
                    b[dst_row_offset + x] = 0;
                } else {
                    r[dst_row_offset + x] = (uint8_t) (((uint64_t) acc_r[x] * 255u) / (count * 31u));
                    g[dst_row_offset + x] = (uint8_t) (((uint64_t) acc_g[x] * 255u) / (count * 63u));
                    b[dst_row_offset + x] = (uint8_t) (((uint64_t) acc_b[x] * 255u) / (count * 31u));
                }
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            uint32_t crop_y_fp = h > 1
                ? (uint32_t) (((uint64_t) y * (uint32_t) (crop_h - 1) << 16) / (h - 1)) : 0;
            if (mirror_y) crop_y_fp = ((uint32_t) (crop_h - 1) << 16) - crop_y_fp;
            uint32_t sy_fp = ((uint32_t) crop_y << 16) + crop_y_fp;
            int sy = (int) (sy_fp >> 16);
            int sy1 = sy + 1 < source_height ? sy + 1 : sy;
            uint32_t fy = sy_fp & 0xffffu;
            for (int x = 0; x < w; x++) {
                uint32_t sx_fp = (uint32_t) crop_x << 16;
                if (w > 1) sx_fp += (uint32_t) (((uint64_t) x * (uint32_t) (crop_w - 1) << 16) / (w - 1));
                int sx = (int) (sx_fp >> 16);
                int sx1 = sx + 1 < source_width ? sx + 1 : sx;
                uint32_t fx = sx_fp & 0xffffu;
                uint16_t px[4] = {
                    source[(size_t) sy * source_width + sx], source[(size_t) sy * source_width + sx1],
                    source[(size_t) sy1 * source_width + sx], source[(size_t) sy1 * source_width + sx1]
                };
                uint8_t * planes[3] = { r, g, b };
                for (int c = 0; c < 3; c++) {
                    uint32_t v[4];
                    for (int i = 0; i < 4; i++) {
                        if (c == 0) v[i] = ((px[i] >> 11) & 0x1f) * 255u / 31u;
                        else if (c == 1) v[i] = ((px[i] >> 5) & 0x3f) * 255u / 63u;
                        else v[i] = (px[i] & 0x1f) * 255u / 31u;
                    }
                    uint32_t top = (v[0] * (65536u - fx) + v[1] * fx) >> 16;
                    uint32_t bottom = (v[2] * (65536u - fx) + v[3] * fx) >> 16;
                    planes[c][y * w + x] = (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
                }
            }
        }
    }

    frosted_glass_blur_passes(r, tmp, w, h, blur_radius, blur_passes);
    frosted_glass_blur_passes(g, tmp, w, h, blur_radius, blur_passes);
    frosted_glass_blur_passes(b, tmp, w, h, blur_radius, blur_passes);

    size_t output_pixels = (size_t) out_width * (size_t) out_height;
    uint8_t * out_bytes = malloc(output_pixels * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }
    uint16_t * out = (uint16_t *) out_bytes;
    for (int y = 0; y < out_height; y++) {
        if (y > 0 && y % 40 == 0) usleep(1000);
        uint32_t y_fp = out_height > 1
            ? (uint32_t) (((uint64_t) y * (h - 1) << 16) / (out_height - 1)) : 0;
        uint16_t * row = out + (size_t) y * out_width;
        for (int x = 0; x < out_width; x++) {
            uint32_t x_fp = out_width > 1
                ? (uint32_t) (((uint64_t) x * (w - 1) << 16) / (out_width - 1)) : 0;
            int rv = (int) ((int64_t) frosted_glass_bilerp_plane(r, w, h, x_fp, y_fp) * darken_num / darken_den);
            int gv = (int) ((int64_t) frosted_glass_bilerp_plane(g, w, h, x_fp, y_fp) * darken_num / darken_den);
            int bv = (int) ((int64_t) frosted_glass_bilerp_plane(b, w, h, x_fp, y_fp) * darken_num / darken_den);
            if (rv > 255) rv = 255;
            if (gv > 255) gv = 255;
            if (bv > 255) bv = 255;
            row[x] = rgb888_to_565_spatial_dithered(rv, gv, bv, x, y);
        }
    }
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}

uint8_t * frosted_glass_blur_screen_rgb565(lv_obj_t * source_screen,
                                           int work_width, int work_height,
                                           int blur_radius, int blur_passes,
                                           int darken_num, int darken_den,
                                           int out_width, int out_height) {
    lv_draw_buf_t * snapshot = lv_snapshot_take(source_screen, LV_COLOR_FORMAT_RGB565);
    if (!snapshot) return NULL;

    int w = work_width, h = work_height;
    uint8_t * r = malloc((size_t) w * h);
    uint8_t * g = malloc((size_t) w * h);
    uint8_t * b = malloc((size_t) w * h);
    uint8_t * tmp = malloc((size_t) w * h);
    if (!r || !g || !b || !tmp) {
        free(r); free(g); free(b); free(tmp);
        lv_draw_buf_destroy(snapshot);
        return NULL;
    }

    int src_w = (int) snapshot->header.w;
    int src_h = (int) snapshot->header.h;
    for (int y = 0; y < h; y++) {
        uint32_t sy_fp = h > 1 ? (uint32_t) (((uint64_t) y * (src_h - 1) << 16) / (h - 1)) : 0;
        int sy = (int) (sy_fp >> 16);
        int sy1 = sy + 1 < src_h ? sy + 1 : sy;
        uint32_t fy = sy_fp & 0xffffu;
        const uint16_t * row0 = (const uint16_t *) lv_draw_buf_goto_xy(snapshot, 0, (uint32_t) sy);
        const uint16_t * row1 = (const uint16_t *) lv_draw_buf_goto_xy(snapshot, 0, (uint32_t) sy1);
        for (int x = 0; x < w; x++) {
            uint32_t sx_fp = w > 1 ? (uint32_t) (((uint64_t) x * (src_w - 1) << 16) / (w - 1)) : 0;
            int sx = (int) (sx_fp >> 16);
            int sx1 = sx + 1 < src_w ? sx + 1 : sx;
            uint32_t fx = sx_fp & 0xffffu;
            uint16_t pixels[4] = { row0[sx], row0[sx1], row1[sx], row1[sx1] };
            uint8_t * channels[3] = { r, g, b };
            for (int c = 0; c < 3; c++) {
                uint32_t values[4];
                for (int p = 0; p < 4; p++) {
                    if (c == 0) values[p] = ((pixels[p] >> 11) & 0x1F) * 255 / 31;
                    else if (c == 1) values[p] = ((pixels[p] >> 5) & 0x3F) * 255 / 63;
                    else values[p] = (pixels[p] & 0x1F) * 255 / 31;
                }
                uint32_t top = (values[0] * (65536u - fx) + values[1] * fx) >> 16;
                uint32_t bottom = (values[2] * (65536u - fx) + values[3] * fx) >> 16;
                channels[c][y * w + x] = (uint8_t) ((top * (65536u - fy) + bottom * fy) >> 16);
            }
        }
    }
    lv_draw_buf_destroy(snapshot);

    frosted_glass_blur_passes(r, tmp, w, h, blur_radius, blur_passes);
    frosted_glass_blur_passes(g, tmp, w, h, blur_radius, blur_passes);
    frosted_glass_blur_passes(b, tmp, w, h, blur_radius, blur_passes);

    uint8_t * out_bytes = malloc((size_t) out_width * out_height * 2);
    if (!out_bytes) {
        free(r); free(g); free(b); free(tmp);
        return NULL;
    }
    uint16_t * out = (uint16_t *) out_bytes;
    for (int y = 0; y < out_height; y++) {
        uint32_t y_fp = h > 1 ? (uint32_t) (((uint64_t) y * (h - 1) << 16) / (out_height - 1)) : 0;
        uint16_t * out_row = out + (size_t) y * out_width;
        for (int x = 0; x < out_width; x++) {
            uint32_t x_fp = w > 1 ? (uint32_t) (((uint64_t) x * (w - 1) << 16) / (out_width - 1)) : 0;
            int rv = frosted_glass_bilerp_plane(r, w, h, x_fp, y_fp) * darken_num / darken_den;
            int gv = frosted_glass_bilerp_plane(g, w, h, x_fp, y_fp) * darken_num / darken_den;
            int bv = frosted_glass_bilerp_plane(b, w, h, x_fp, y_fp) * darken_num / darken_den;
            out_row[x] = rgb888_to_565_spatial_dithered(rv, gv, bv, x, y);
        }
    }
    free(r); free(g); free(b); free(tmp);
    return out_bytes;
}
