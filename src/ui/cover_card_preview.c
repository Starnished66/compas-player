#include "cover_card_preview.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/library/cover_decode.h"
#include "src/library/image_thumb.h"
#include "src/misc/cache/instance/lv_image_cache.h"

enum {
    COVER_CARD_PREVIEW_MAX_FILE_BYTES = 512 * 1024,
    COVER_CARD_PREVIEW_MAX_WIDTH = 240,
    COVER_CARD_PREVIEW_MAX_HEIGHT = 400,
    COVER_CARD_PREVIEW_MAX_PIXELS_BYTES = 2 * 1024 * 1024,
};

/* Accessed only by the LVGL/UI thread, including image deletion. */
static size_t cover_card_preview_pixel_bytes;

typedef struct {
    lv_image_dsc_t descriptor;
    uint16_t * pixels;
} cover_card_preview_data_t;

typedef struct {
    struct timespec deadline;
} cover_card_preview_cancel_t;

/* The RGB565 preview path cannot retain PNG transparency. Only use it for
 * PNGs whose chunks describe opaque pixels; the decoder remains responsible
 * for validating and decoding the image data itself. */
static bool cover_card_preview_png_is_opaque(const uint8_t * bytes, size_t size) {
    static const uint8_t signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    if (!bytes || size < sizeof(signature) + 12 + 13 + 4 ||
        memcmp(bytes, signature, sizeof(signature)) != 0)
        return false;

    size_t offset = sizeof(signature);
    uint32_t ihdr_length = ((uint32_t)bytes[offset] << 24) |
                           ((uint32_t)bytes[offset + 1] << 16) |
                           ((uint32_t)bytes[offset + 2] << 8) |
                           (uint32_t)bytes[offset + 3];
    if (ihdr_length != 13 || memcmp(bytes + offset + 4, "IHDR", 4) != 0)
        return false;
    uint8_t color_type = bytes[offset + 8 + 9];
    if (color_type != 0 && color_type != 2 && color_type != 3) return false;

    offset += 12 + 13; /* length, type, data and CRC */
    while (offset <= size && size - offset >= 12) {
        uint32_t length = ((uint32_t)bytes[offset] << 24) |
                          ((uint32_t)bytes[offset + 1] << 16) |
                          ((uint32_t)bytes[offset + 2] << 8) |
                          (uint32_t)bytes[offset + 3];
        /* Check against remaining bytes before adding length to offset. */
        if ((size_t)length > size - offset - 12) return false;
        const uint8_t * type = bytes + offset + 4;
        if (memcmp(type, "tRNS", 4) == 0) return false;
        if (memcmp(type, "IEND", 4) == 0) return length == 0;
        offset += (size_t)length + 12;
    }
    return false;
}

static bool cover_card_preview_should_cancel(void * user_data) {
    const cover_card_preview_cancel_t * cancel = user_data;
    struct timespec now;
    if (!cancel || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
    if (now.tv_sec > cancel->deadline.tv_sec ||
        (now.tv_sec == cancel->deadline.tv_sec && now.tv_nsec >= cancel->deadline.tv_nsec))
        return true;
    return false;
}

static void cover_card_preview_delete_cb(lv_event_t * event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    lv_obj_t * image = lv_event_get_target(event);
    cover_card_preview_data_t * data = lv_event_get_user_data(event);
    if (!image || !data) return;

    /* The descriptor is variable memory, so clear LVGL's reference/cache
     * entry before releasing the backing pixels. */
    lv_image_set_src(image, NULL);
    lv_image_cache_drop(&data->descriptor);
    cover_card_preview_pixel_bytes -= data->descriptor.data_size;
    free(data->pixels);
    free(data);
}

static bool cover_card_preview_read_png(const char * resolved_src,
                                        uint8_t ** out_data, size_t * out_size) {
    static const uint8_t png_signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    *out_data = NULL;
    *out_size = 0;
    if (!resolved_src || strncmp(resolved_src, "S:/", 3) != 0) return false;

    /* LVGL's S: POSIX drive maps directly to the host/device filesystem. */
    int fd = open(resolved_src + 2, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size >= 24 && st.st_size <= COVER_CARD_PREVIEW_MAX_FILE_BYTES;
    uint8_t * bytes = valid ? malloc((size_t)st.st_size) : NULL;
    if (valid && !bytes) valid = false;

    size_t offset = 0;
    while (valid && offset < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + offset, (size_t)st.st_size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            valid = false;
            break;
        }
        offset += (size_t)n;
    }
    close(fd);

    if (valid) {
        int native_w = 0, native_h = 0;
        valid = memcmp(bytes, png_signature, sizeof(png_signature)) == 0 &&
                cover_card_preview_png_is_opaque(bytes, (size_t)st.st_size) &&
                image_thumb_native_size(bytes, (size_t)st.st_size, &native_w, &native_h) &&
                native_w > 0 && native_w <= COVER_CARD_PREVIEW_MAX_WIDTH &&
                native_h > 0 && native_h <= COVER_CARD_PREVIEW_MAX_HEIGHT;
    }
    if (!valid) {
        free(bytes);
        return false;
    }

    *out_data = bytes;
    *out_size = (size_t)st.st_size;
    return true;
}

bool cover_card_preview_set(lv_obj_t * image, const char * resolved_src,
                            int max_w, int max_h) {
    if (!image || !lv_obj_check_type(image, &lv_image_class) || max_w <= 0 || max_h <= 0)
        return false;

    uint8_t * compressed = NULL;
    size_t compressed_size = 0;
    if (!cover_card_preview_read_png(resolved_src, &compressed, &compressed_size)) return false;

    int native_w = 0, native_h = 0;
    int target_w = 0, target_h = 0;
    if (!image_thumb_native_size(compressed, compressed_size, &native_w, &native_h)) {
        free(compressed);
        return false;
    }
    /* Preserve the existing card renderer's enlargement of tiny images. */
    if (native_w < max_w && native_h < max_h) {
        free(compressed);
        return false;
    }
    image_thumb_fit(native_w, native_h, max_w, max_h, &target_w, &target_h);
    size_t pixel_bytes = (size_t)target_w * (size_t)target_h * sizeof(uint16_t);
    if (pixel_bytes > COVER_CARD_PREVIEW_MAX_PIXELS_BYTES - cover_card_preview_pixel_bytes) {
        free(compressed);
        return false;
    }

    /* A zero-time probe avoids starting a preview decode when the shared
     * artwork coordinator is already occupied. The decoder performs its
     * authoritative target-sized memory admission immediately afterward. */
    if (artwork_coordinator_acquire(ARTWORK_PRIO_THUMBNAIL, 0, 0, NULL, NULL) !=
        ARTWORK_ACQUIRE_OK) {
        free(compressed);
        return false;
    }
    artwork_coordinator_release(ARTWORK_PRIO_THUMBNAIL);

    uint16_t * pixels = NULL;
    cover_card_preview_cancel_t cancel;
    if (clock_gettime(CLOCK_MONOTONIC, &cancel.deadline) != 0) {
        free(compressed);
        return false;
    }
    cancel.deadline.tv_nsec += 100000000L;
    if (cancel.deadline.tv_nsec >= 1000000000L) {
        cancel.deadline.tv_sec++;
        cancel.deadline.tv_nsec -= 1000000000L;
    }
    cover_decode_result_t result = cover_decode_to_rgb565_ex(
        compressed, (uint32_t)compressed_size, target_w, target_h,
        ARTWORK_PRIO_THUMBNAIL, cover_card_preview_should_cancel, &cancel, &pixels);
    free(compressed);
    if (result != COVER_DECODE_OK || !pixels) {
        free(pixels);
        return false;
    }

    cover_card_preview_data_t * data = calloc(1, sizeof(*data));
    if (!data) {
        free(pixels);
        return false;
    }
    if (pixel_bytes > UINT32_MAX) {
        free(pixels);
        free(data);
        return false;
    }

    data->pixels = pixels;
    data->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    data->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    data->descriptor.header.w = (uint16_t)target_w;
    data->descriptor.header.h = (uint16_t)target_h;
    data->descriptor.header.stride = (uint16_t)(target_w * 2);
    data->descriptor.data_size = (uint32_t)pixel_bytes;
    data->descriptor.data = (const uint8_t *)pixels;

    cover_card_preview_pixel_bytes += pixel_bytes;
    lv_obj_add_event_cb(image, cover_card_preview_delete_cb, LV_EVENT_DELETE, data);
    lv_image_set_src(image, &data->descriptor);
    lv_image_set_scale(image, LV_SCALE_NONE);
    return true;
}
