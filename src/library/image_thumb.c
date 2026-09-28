#include "image_thumb.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cover_decode.h"
#include "lvgl/lvgl.h"

static uint32_t read_be32(const uint8_t * p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

static int32_t read_le32(const uint8_t * p) {
    return (int32_t) ((uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24));
}

bool image_thumb_native_size(const uint8_t * data, size_t size, int * out_w, int * out_h) {
    if (!data || !out_w || !out_h) return false;
    *out_w = *out_h = 0;
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (size >= 24 && memcmp(data, png_sig, 8) == 0 && memcmp(data + 12, "IHDR", 4) == 0) {
        uint32_t w = read_be32(data + 16), h = read_be32(data + 20);
        if (w == 0 || h == 0 || w > 65535 || h > 65535) return false;
        *out_w = (int) w;
        *out_h = (int) h;
        return true;
    }
    if (size >= 26 && data[0] == 'B' && data[1] == 'M') {
        int32_t w = read_le32(data + 18), h = read_le32(data + 22);
        if (h < 0) h = -h;
        if (w <= 0 || h <= 0 || w > 65535 || h > 65535) return false;
        *out_w = (int) w;
        *out_h = (int) h;
        return true;
    }
    if (size >= 4 && data[0] == 0xFF && data[1] == 0xD8) {
        jpeg_probe_t probe;
        if (size > UINT32_MAX || !jpeg_probe(data, (uint32_t) size, &probe) || !probe.supported) return false;
        if (probe.native_w <= 0 || probe.native_h <= 0) return false;
        *out_w = probe.native_w;
        *out_h = probe.native_h;
        return true;
    }
    return false;
}

void image_thumb_fit(int native_w, int native_h, int max_w, int max_h, int * out_w, int * out_h) {
    int w = native_w, h = native_h;
    if (w > max_w || h > max_h) {
        /* Compare native_w/max_w against native_h/max_h without floats. */
        if ((int64_t) native_w * max_h >= (int64_t) native_h * max_w) {
            w = max_w;
            h = (int) (((int64_t) native_h * max_w + native_w / 2) / native_w);
        } else {
            h = max_h;
            w = (int) (((int64_t) native_w * max_h + native_h / 2) / native_h);
        }
    }
    *out_w = w < 1 ? 1 : w;
    *out_h = h < 1 ? 1 : h;
}

static bool write_all(FILE * f, const void * data, size_t len) {
    return fwrite(data, 1, len, f) == len;
}

bool image_thumb_write_bin(const uint8_t * data, size_t size, int max_w, int max_h,
                           const char * dest_path, artwork_priority_t prio,
                           artwork_cancel_fn cancel_cb, void * cancel_user,
                           const char ** reason) {
    const char * dummy;
    if (!reason) reason = &dummy;
    *reason = NULL;
    int native_w = 0, native_h = 0;
    if (!dest_path || max_w < 1 || max_h < 1 || size > UINT32_MAX ||
        !image_thumb_native_size(data, size, &native_w, &native_h)) {
        *reason = "unsupported image";
        return false;
    }
    int w, h;
    image_thumb_fit(native_w, native_h, max_w, max_h, &w, &h);

    /* The target keeps the image's own aspect ratio, so the decoder's
     * cover-fit only scales; nothing is cropped. */
    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(data, (uint32_t) size, w, h, prio,
                                                          cancel_cb, cancel_user, &pixels);
    if (res != COVER_DECODE_OK || !pixels) {
        free(pixels);
        if (cancel_cb && cancel_cb(cancel_user))
            *reason = "cancelled";
        else if (cover_decode_result_is_temporary(res))
            *reason = "busy"; /* the decoder or memory was taken; worth trying again later */
        else
            *reason = "could not decode image";
        return false;
    }

    lv_image_header_t header;
    memset(&header, 0, sizeof(header));
    header.magic = LV_IMAGE_HEADER_MAGIC;
    header.cf = LV_COLOR_FORMAT_RGB565;
    header.w = (uint32_t) w;
    header.h = (uint32_t) h;
    header.stride = (uint32_t) w * 2;

    char tmp[PATH_MAX + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", dest_path);
    bool ok = n > 0 && (size_t) n < sizeof(tmp);
    FILE * f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        ok = write_all(f, &header, sizeof(header)) &&
             write_all(f, pixels, (size_t) w * (size_t) h * 2);
        ok = fflush(f) == 0 && ok;
        ok = fsync(fileno(f)) == 0 && ok;
        ok = fclose(f) == 0 && ok;
        if (!ok || rename(tmp, dest_path) != 0) {
            ok = false;
            unlink(tmp);
        }
    } else {
        ok = false;
    }
    free(pixels);
    if (!ok) *reason = "could not write image";
    return ok;
}
