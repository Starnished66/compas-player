#include "gui_image_dsc.h"

void gui_image_dsc_init_rgb565(lv_image_dsc_t * dsc, const void * pixels, int px) {
    *dsc = (lv_image_dsc_t) {
        .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = px, .h = px, .stride = px * 2 },
        .data_size = (uint32_t) px * (uint32_t) px * 2,
        .data = (const uint8_t *) pixels
    };
}
