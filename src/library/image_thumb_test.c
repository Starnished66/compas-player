/* Host selftest for image_thumb: header size detection, fit math, and a
 * BMP decoded, fitted and written as an LVGL RGB565 .bin file. */
#include "image_thumb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/libs/lodepng/lodepng.h"

/* Link stubs: this test decodes BMP only, so PNG and playback state are
 * never reached. */
bool audio_is_playing(void) { return false; }
void lv_draw_buf_destroy(lv_draw_buf_t * buf) { (void) buf; }
unsigned lodepng_get_bpp(const LodePNGColorMode * info) { (void) info; return 0; }
void lodepng_state_init(LodePNGState * state) { memset(state, 0, sizeof(*state)); }
void lodepng_state_cleanup(LodePNGState * state) { (void) state; }
unsigned lodepng_decode(unsigned char ** out, unsigned * w, unsigned * h, LodePNGState * state,
                        const unsigned char * in, size_t insize) {
    (void) out; (void) w; (void) h; (void) state; (void) in; (void) insize;
    return 1;
}
unsigned lodepng_inspect(unsigned * w, unsigned * h, LodePNGState * state,
                         const unsigned char * in, size_t insize) {
    (void) w; (void) h; (void) state; (void) in; (void) insize;
    return 1;
}

static void put_le32(uint8_t * p, uint32_t v) {
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

/* 24-bit bottom-up BMP: left half red, right half blue. */
static uint8_t * make_bmp(int w, int h, size_t * out_size) {
    int stride = (w * 3 + 3) & ~3;
    size_t size = 54 + (size_t) stride * h;
    uint8_t * b = calloc(1, size);
    b[0] = 'B'; b[1] = 'M';
    put_le32(b + 2, (uint32_t) size);
    put_le32(b + 10, 54);
    put_le32(b + 14, 40);
    put_le32(b + 18, (uint32_t) w);
    put_le32(b + 22, (uint32_t) h);
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t * px = b + 54 + (size_t) y * stride + x * 3; /* B, G, R */
            if (x < w / 2) px[2] = 255; else px[0] = 255;
        }
    }
    *out_size = size;
    return b;
}

int main(void) {
    int w, h;
    image_thumb_fit(300, 200, 100, 100, &w, &h);
    assert(w == 100 && h == 67);
    image_thumb_fit(200, 300, 100, 100, &w, &h);
    assert(w == 67 && h == 100);
    image_thumb_fit(50, 40, 100, 100, &w, &h);   /* never enlarged */
    assert(w == 50 && h == 40);
    image_thumb_fit(10000, 1, 100, 100, &w, &h); /* never zero */
    assert(w == 100 && h == 1);

    static const uint8_t png[24] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13,
                                     'I', 'H', 'D', 'R', 0, 0, 0x02, 0x58, 0, 0, 0x03, 0x84 };
    assert(image_thumb_native_size(png, sizeof(png), &w, &h) && w == 600 && h == 900);
    static const uint8_t junk[16] = "not an image....";
    assert(!image_thumb_native_size(junk, sizeof(junk), &w, &h));

    size_t size;
    uint8_t * bmp = make_bmp(300, 200, &size);
    assert(image_thumb_native_size(bmp, size, &w, &h) && w == 300 && h == 200);

    char dest[] = "/tmp/image-thumb-test-XXXXXX";
    int fd = mkstemp(dest);
    assert(fd >= 0);
    close(fd);
    const char * reason = NULL;
    assert(image_thumb_write_bin(bmp, size, 100, 100, dest, ARTWORK_PRIO_THUMBNAIL, NULL, NULL, &reason));

    FILE * f = fopen(dest, "rb");
    assert(f);
    lv_image_header_t header;
    assert(fread(&header, 1, sizeof(header), f) == sizeof(header));
    assert(header.magic == LV_IMAGE_HEADER_MAGIC && header.cf == LV_COLOR_FORMAT_RGB565);
    assert(header.w == 100 && header.h == 67 && header.stride == 200);
    uint16_t row[100];
    assert(fread(row, 2, 100, f) == 100);
    fclose(f);
    assert((row[5] >> 11) > 24 && (row[5] & 0x1F) < 8);   /* left: red */
    assert((row[94] & 0x1F) > 24 && (row[94] >> 11) < 8); /* right: blue */
    unlink(dest);

    assert(!image_thumb_write_bin(junk, sizeof(junk), 100, 100, dest, ARTWORK_PRIO_THUMBNAIL, NULL, NULL, &reason));
    assert(reason && access(dest, F_OK) != 0);
    free(bmp);
    printf("image thumb: PASS\n");
    return 0;
}
