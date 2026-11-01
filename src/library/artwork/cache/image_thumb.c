#include "image_thumb.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../decode/cover_decode.h"
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

static unsigned tiff_u(const uint8_t * p, bool le, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned) p[le ? i : n - 1 - i] << (8 * i);
    return v;
}

int image_exif_orientation(const uint8_t * data, size_t size) {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return 1;
    for (size_t pos = 2; pos + 4 <= size && data[pos] == 0xFF;) {
        int marker = data[pos + 1];
        if (marker == 0xFF) { pos++; continue; }
        size_t len = ((size_t) data[pos + 2] << 8) | data[pos + 3];
        if (marker == 0xDA || marker == 0xD9 || marker < 0xC0 || len < 2 || pos + 2 + len > size) break;
        if (marker == 0xE1 && len >= 16 && memcmp(data + pos + 4, "Exif\0\0", 6) == 0) {
            const uint8_t * t = data + pos + 10;
            size_t tl = len - 8;
            bool le = t[0] == 'I';
            if (t[1] != t[0] || (!le && t[0] != 'M') || tiff_u(t + 2, le, 2) != 0x2A) break;
            size_t off = tiff_u(t + 4, le, 4);
            if (off > tl - 2) break;
            unsigned n = tiff_u(t + off, le, 2);
            for (size_t e = off + 2; n-- && e + 12 <= tl; e += 12)
                if (tiff_u(t + e, le, 2) == 0x112) {
                    unsigned v = tiff_u(t + e + 8, le, 2);
                    return tiff_u(t + e + 2, le, 2) == 3 && tiff_u(t + e + 4, le, 4) == 1 && v >= 1 && v <= 8
                        ? (int) v : 1;
                }
            break;
        }
        pos += 2 + len;
    }
    return 1;
}

uint16_t * image_rgb565_orient(const uint16_t * src, int w, int h, int orientation, int * out_w, int * out_h) {
    bool swap = orientation >= 5;
    int ow = swap ? h : w, oh = swap ? w : h;
    uint16_t * dst = malloc((size_t) ow * oh * sizeof(*dst));
    if (!dst) return NULL;
    bool flip_u = (0x18C >> orientation) & 1, flip_v = (0xD8 >> orientation) & 1; /* 2,3,7,8 / 3,4,6,7 */
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            int u = swap ? y : x, v = swap ? x : y;
            if (flip_u) u = w - 1 - u;
            if (flip_v) v = h - 1 - v;
            dst[(size_t) y * ow + x] = src[(size_t) v * w + u];
        }
    *out_w = ow;
    *out_h = oh;
    return dst;
}

static bool write_all(FILE * f, const void * data, size_t len) {
    return fwrite(data, 1, len, f) == len;
}

static const char * decode_failure_reason(cover_decode_result_t res, artwork_cancel_fn cancel_cb,
                                          void * cancel_user) {
    if (cancel_cb && cancel_cb(cancel_user)) return "cancelled";
    if (res == COVER_DECODE_FAIL_OVERSIZED) return "image dimensions too large";
    if (cover_decode_result_is_temporary(res)) return "busy"; /* the decoder or memory was taken; worth trying again later */
    return "could not decode image";
}

bool image_thumb_decode(const uint8_t * data, size_t size, int max_w, int max_h,
                        artwork_priority_t prio, artwork_cancel_fn cancel_cb, void * cancel_user,
                        uint16_t ** out_pixels, int * out_w, int * out_h, const char ** reason) {
    const char * dummy;
    if (!reason) reason = &dummy;
    *reason = NULL;
    int native_w = 0, native_h = 0;
    if (max_w < 1 || max_h < 1 || size > UINT32_MAX ||
        !image_thumb_native_size(data, size, &native_w, &native_h)) {
        *reason = "unsupported image";
        return false;
    }
    image_thumb_fit(native_w, native_h, max_w, max_h, out_w, out_h);
    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(data, (uint32_t) size, *out_w, *out_h, prio,
                                                          cancel_cb, cancel_user, &pixels);
    if (res != COVER_DECODE_OK || !pixels) {
        free(pixels);
        *reason = decode_failure_reason(res, cancel_cb, cancel_user);
        return false;
    }
    *out_pixels = pixels;
    return true;
}

static bool image_write_bin(const uint8_t * data, size_t size, int max_w, int max_h,
                            bool cover, const char * dest_path, artwork_priority_t prio,
                            artwork_cancel_fn cancel_cb, void * cancel_user,
                            const char ** reason) {
    const char * dummy;
    if (!reason) reason = &dummy;
    *reason = NULL;
    if (!dest_path) {
        *reason = "unsupported image";
        return false;
    }
    int w, h;
    uint16_t * pixels = NULL;
    if (!cover) {
        if (!image_thumb_decode(data, size, max_w, max_h, prio, cancel_cb, cancel_user, &pixels, &w, &h,
                                reason))
            return false;
    } else {
        int native_w = 0, native_h = 0;
        if (max_w < 1 || max_h < 1 || size > UINT32_MAX ||
            !image_thumb_native_size(data, size, &native_w, &native_h)) {
            *reason = "unsupported image";
            return false;
        }
        w = max_w;
        h = max_h;
        /* Wallpaper dimensions use cover-fit, which center-crops to the
         * requested screen rectangle. */
        cover_decode_result_t res = cover_decode_to_rgb565_ex(data, (uint32_t) size, w, h, prio,
                                                              cancel_cb, cancel_user, &pixels);
        if (res == COVER_DECODE_FAIL_OVERSIZED && !(cancel_cb && cancel_cb(cancel_user))) {
            /* The shared JPEG decoder avoids scaling when either source axis
             * would fall below the final target. For wide wallpapers that can
             * reject a source whose short axis is smaller than the tall screen.
             * Decode at a bounded aspect-fit intermediate, then center-cover it;
             * this keeps thumbnail policy intact and avoids a second JPEG path. */
            int intermediate_w = native_w, intermediate_h = native_h;
            /* Match JPEG decoder scale steps so the requested intermediate never
             * forces a larger, over-limit decode between powers of two. */
            for (int scale = 0; scale < 3 &&
                 (intermediate_w > MAX_DECODED_COVER_SIDE ||
                  intermediate_h > MAX_DECODED_COVER_SIDE); scale++) {
                intermediate_w >>= 1;
                intermediate_h >>= 1;
            }
            uint16_t * intermediate = NULL;
            res = cover_decode_to_rgb565_ex(data, (uint32_t) size, intermediate_w, intermediate_h,
                                            prio, cancel_cb, cancel_user, &intermediate);
            if (res == COVER_DECODE_OK && intermediate &&
                !(cancel_cb && cancel_cb(cancel_user))) {
                pixels = cover_resize_rgb565(intermediate, intermediate_w, intermediate_h, w, h);
                if (!pixels) res = COVER_DECODE_FAIL_ALLOC;
                else if (cancel_cb && cancel_cb(cancel_user)) {
                    free(pixels);
                    pixels = NULL;
                    res = COVER_DECODE_FAIL_CANCELLED;
                }
            }
            free(intermediate);
        }
        if (res != COVER_DECODE_OK || !pixels) {
            free(pixels);
            *reason = decode_failure_reason(res, cancel_cb, cancel_user);
            return false;
        }
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
    bool cancelled = false;
    FILE * f = ok ? fopen(tmp, "wb") : NULL;
    if (f) {
        ok = write_all(f, &header, sizeof(header)) &&
             write_all(f, pixels, (size_t) w * (size_t) h * 2);
        ok = fflush(f) == 0 && ok;
        ok = fsync(fileno(f)) == 0 && ok;
        ok = fclose(f) == 0 && ok;
        if (ok && cancel_cb && cancel_cb(cancel_user)) {
            ok = false;
            cancelled = true;
        }
        if (!ok || rename(tmp, dest_path) != 0) {
            ok = false;
            unlink(tmp);
        }
    } else {
        ok = false;
    }
    free(pixels);
    if (!ok) *reason = cancelled ? "cancelled" : "could not write image";
    return ok;
}

bool image_thumb_write_bin(const uint8_t * data, size_t size, int max_w, int max_h,
                           const char * dest_path, artwork_priority_t prio,
                           artwork_cancel_fn cancel_cb, void * cancel_user,
                           const char ** reason) {
    return image_write_bin(data, size, max_w, max_h, false, dest_path, prio,
                           cancel_cb, cancel_user, reason);
}

bool image_wallpaper_write_bin(const uint8_t * data, size_t size, int width, int height,
                               const char * dest_path, artwork_priority_t prio,
                               artwork_cancel_fn cancel_cb, void * cancel_user,
                               const char ** reason) {
    return image_write_bin(data, size, width, height, true, dest_path, prio,
                           cancel_cb, cancel_user, reason);
}
