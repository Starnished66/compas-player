#include "assets.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void install_thread_crash_altstack(void) {}

static size_t image_pixel_bytes(const asset_decoded_image_t * image) {
    assert(image && image->open && image->decoder.decoded);
    const lv_draw_buf_t * buffer = image->decoder.decoded;
    assert(buffer->data && buffer->header.cf == LV_COLOR_FORMAT_ARGB8888);
    return (size_t) buffer->header.stride * buffer->header.h;
}

static uint8_t * copy_pristine(const asset_decoded_image_t * image, size_t * out_size) {
    size_t size = image_pixel_bytes(image);
    uint8_t * copy = malloc(size);
    assert(copy);
    memcpy(copy, image->decoder.decoded->data, size);
    *out_size = size;
    return copy;
}

static void tint_pixels(asset_decoded_image_t * image, uint8_t red, uint8_t green, uint8_t blue) {
    lv_draw_buf_t * buffer = (lv_draw_buf_t *) image->decoder.decoded;
    for (uint32_t y = 0; y < buffer->header.h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buffer->data + (size_t)y * buffer->header.stride);
        for (uint32_t x = 0; x < buffer->header.w; x++) {
            if (row[x].alpha == 0) continue;
            row[x].red = (uint8_t) ((row[x].red + red) / 2);
            row[x].green = (uint8_t) ((row[x].green + green) / 2);
            row[x].blue = (uint8_t) ((row[x].blue + blue) / 2);
        }
    }
}

static void assert_pristine(const asset_decoded_image_t * image,
                            const uint8_t * pristine, size_t pristine_size) {
    assert(image_pixel_bytes(image) == pristine_size);
    assert(memcmp(image->decoder.decoded->data, pristine, pristine_size) == 0);
}

int main(void) {
    lv_init();

    asset_decoded_image_t image = {0};
    assert(asset_decoded_image_prepare_retint(&image, "pull_down/wifi_s.png"));
    const char * first_path = image.path;
    assert(first_path && strstr(first_path, "pull_down/wifi_s.png"));
    lv_draw_buf_t * first_buffer = (lv_draw_buf_t *) image.decoder.decoded;
    size_t first_size = 0;
    uint8_t * first_pristine = copy_pristine(&image, &first_size);

    tint_pixels(&image, 255, 32, 8);
    assert(asset_decoded_image_prepare_retint(&image, "pull_down/wifi_s.png"));
    assert(image.decoder.decoded == first_buffer);
    assert_pristine(&image, first_pristine, first_size);
    tint_pixels(&image, 8, 240, 48);
    assert(asset_decoded_image_prepare_retint(&image, "pull_down/wifi_s.png"));
    assert(image.decoder.decoded == first_buffer);
    assert_pristine(&image, first_pristine, first_size);

    asset_decoded_image_close(&image);
    assert(!image.open && !image.path && !image.pristine_pixels);
    assert(asset_decoded_image_prepare_retint(&image, "pull_down/wifi_s.png"));
    assert_pristine(&image, first_pristine, first_size);
    free(first_pristine);

    assert(asset_decoded_image_prepare_retint(&image, "playing_plane/btn_play.png"));
    assert(image.path && strstr(image.path, "playing_plane/btn_play.png"));
    assert(image.pristine_pixels && image.pristine_size == image_pixel_bytes(&image));
    assert(image.pristine_size != 0);
    size_t second_size = 0;
    uint8_t * second_pristine = copy_pristine(&image, &second_size);
    tint_pixels(&image, 240, 40, 200);
    assert(asset_decoded_image_prepare_retint(&image, "playing_plane/btn_play.png"));
    assert_pristine(&image, second_pristine, second_size);
    free(second_pristine);

    assert(!asset_decoded_image_prepare_retint(&image, "missing/retint_spec_missing.png"));
    assert(!image.open && !image.path && !image.pristine_pixels);
    asset_decoded_image_close(&image);
    puts("assets retint runtime spec passed");
    return 0;
}
