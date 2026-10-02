#include "player_cover_fade.h"
#include "frosted_glass.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

enum {
    PLAYER_COVER_FADE_MAX_DIMENSION = 1024,
    PLAYER_COVER_FADE_MAX_BYTES = 1024 * 1024,
    PLAYER_COVER_FADE_SOLID_TAIL_ROWS = 4,
};

typedef struct {
    lv_image_dsc_t descriptor;
    uint8_t * pixels;
} player_cover_fade_data_t;

static uint8_t player_cover_fade_alpha(int32_t x, int32_t y, double base, double modulation) {
    double noise = ((double)frosted_glass_spatial_threshold(x, y) / 8.0) - 4.0;
    long alpha = lround(base + noise * modulation);
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    return (uint8_t)alpha;
}

static void player_cover_fade_delete_cb(lv_event_t * event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    lv_obj_t * overlay = lv_event_get_target(event);
    player_cover_fade_data_t * data = lv_obj_get_user_data(overlay);
    if (!data) return;
    lv_obj_set_user_data(overlay, NULL);
    lv_image_set_src(overlay, NULL);
    lv_image_cache_drop(&data->descriptor);
    free(data->pixels);
    free(data);
}

bool player_cover_fade_attach(lv_obj_t * image, lv_color_t color) {
    if (!image || !lv_obj_check_type(image, &lv_image_class)) return false;

    lv_obj_update_layout(image);
    int32_t width = lv_obj_get_width(image);
    int32_t height = lv_obj_get_height(image);
    if (width <= 0 || height <= 0 || width > PLAYER_COVER_FADE_MAX_DIMENSION ||
        height > PLAYER_COVER_FADE_MAX_DIMENSION ||
        (uint64_t)width * (uint64_t)height > PLAYER_COVER_FADE_MAX_BYTES) return false;

    player_cover_fade_data_t * data = calloc(1, sizeof(*data));
    if (!data) return false;
    data->pixels = malloc((size_t)width * (size_t)height);
    if (!data->pixels) {
        free(data);
        return false;
    }

    data->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    data->descriptor.header.cf = LV_COLOR_FORMAT_A8;
    data->descriptor.header.w = (uint16_t)width;
    data->descriptor.header.h = (uint16_t)height;
    data->descriptor.header.stride = (uint16_t)width;
    data->descriptor.data_size = (uint32_t)((size_t)width * (size_t)height);
    data->descriptor.data = data->pixels;
    for (int32_t y = 0; y < height; ++y) {
        uint8_t * row = data->pixels + (size_t)y * (size_t)width;
        if (height <= PLAYER_COVER_FADE_SOLID_TAIL_ROWS ||
            y >= height - PLAYER_COVER_FADE_SOLID_TAIL_ROWS) {
            memset(row, LV_OPA_COVER, (size_t)width);
            continue;
        }
        if (y == 0) {
            memset(row, LV_OPA_TRANSP, (size_t)width);
            continue;
        }
        /* Curve math is constant across a row. Compute it once, rather than
         * doing a floating-point square root for every cover pixel. */
        double t = (double)y / (double)(height - 1);
        double base = 255.0 * t * sqrt(t);
        double edge = base < 255.0 - base ? base : 255.0 - base;
        double modulation = edge / 8.0;
        if (modulation > 1.0) modulation = 1.0;
        for (int32_t x = 0; x < width; ++x)
            row[x] = player_cover_fade_alpha(x, y, base, modulation);
    }

    lv_obj_t * overlay = lv_image_create(image);
    if (!overlay) {
        free(data->pixels);
        free(data);
        return false;
    }
    lv_obj_set_user_data(overlay, data);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_image_recolor(overlay, color, 0);
    lv_obj_set_style_image_recolor_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(overlay, LV_OPA_COVER, 0);
    lv_image_set_inner_align(overlay, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_src(overlay, &data->descriptor);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(overlay, player_cover_fade_delete_cb, LV_EVENT_DELETE, NULL);
    return true;
}
