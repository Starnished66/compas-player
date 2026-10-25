#ifndef TESTS_PLAYER_LV_SNAPSHOT_PPM_H
#define TESTS_PLAYER_LV_SNAPSHOT_PPM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <lvgl.h>

/* LV_COLOR_FORMAT_RGB888 snapshots use LVGL's in-memory BGR byte order here;
 * P6 PPM stores RGB. Keep all screenshot writers on this conversion path. */
static bool test_write_lv_snapshot_ppm(FILE *file, const lv_draw_buf_t *snapshot) {
    if (!file || !snapshot || !snapshot->data ||
        snapshot->header.cf != LV_COLOR_FORMAT_RGB888) return false;
    uint32_t width = snapshot->header.w, height = snapshot->header.h;
    if (!width || !height || width > SIZE_MAX / 3U) return false;
    size_t row_bytes = (size_t)width * 3U;
    if (snapshot->header.stride < row_bytes ||
        (uint64_t)snapshot->header.stride * height > snapshot->data_size) return false;
    uint8_t *row = malloc(row_bytes);
    if (!row) return false;
    bool ok = fprintf(file, "P6\n%u %u\n255\n", width, height) > 0;
    for (uint32_t y = 0; ok && y < height; ++y) {
        const uint8_t *source = snapshot->data + (size_t)y * snapshot->header.stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[(size_t)x * 3U] = source[(size_t)x * 3U + 2U];
            row[(size_t)x * 3U + 1U] = source[(size_t)x * 3U + 1U];
            row[(size_t)x * 3U + 2U] = source[(size_t)x * 3U];
        }
        ok = fwrite(row, 1, row_bytes, file) == row_bytes;
    }
    free(row);
    return ok;
}

#endif
