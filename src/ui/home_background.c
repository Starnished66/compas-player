#include "home_background.h"
#include "assets.h"
#include "board_config.h"
#include "src/library/cover_decode.h"
#include "src/library/artwork_coordinator.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { HOME_JPEG_CACHE_MAX_BYTES = 4 * 1024 * 1024 };

typedef struct {
    lv_image_dsc_t descriptor;
    uint16_t * pixels;
} home_background_rgb565_t;

typedef struct {
    struct timespec deadline;
} home_background_decode_cancel_t;

static bool home_background_decode_should_cancel(void * user_data) {
    const home_background_decode_cancel_t * cancel = user_data;
    struct timespec now;
    if (!cancel || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
    return now.tv_sec > cancel->deadline.tv_sec ||
           (now.tv_sec == cancel->deadline.tv_sec && now.tv_nsec >= cancel->deadline.tv_nsec);
}

/* Read only small, native-display-sized JPEGs into a single RGB565
 * framebuffer. PNGs retain LVGL's decoder path so alpha remains intact. */
static home_background_rgb565_t * home_background_decode_jpeg(const char * relative_path) {
    const char * ext = strrchr(relative_path, '.');
    if (!ext || (strcasecmp(ext, ".jpg") != 0 && strcasecmp(ext, ".jpeg") != 0)) return NULL;
    const char * path = asset_path_plain(relative_path);
    if (!path) return NULL;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size > 0 && st.st_size <= HOME_JPEG_CACHE_MAX_BYTES;
    uint8_t * bytes = valid ? malloc((size_t)st.st_size) : NULL;
    if (!bytes) valid = false;
    size_t offset = 0;
    while (valid && offset < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + offset, (size_t)st.st_size - offset);
        if (n <= 0) valid = false;
        else offset += (size_t)n;
    }
    close(fd);
    if (!valid) { free(bytes); return NULL; }

    jpeg_probe_t probe;
    lv_display_t * display = lv_display_get_default();
    int32_t display_width = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
    int32_t display_height = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
    if (!jpeg_probe(bytes, (uint32_t)offset, &probe) || !probe.supported ||
        probe.native_w <= 0 || probe.native_h <= 0 ||
        probe.native_w > display_width || probe.native_h > display_height) {
        free(bytes);
        return NULL;
    }

    /* Avoid starting a UI-thread decode while another artwork request owns
     * the coordinator. The decoder then performs its normal memory admission. */
    if (artwork_coordinator_acquire(ARTWORK_PRIO_THUMBNAIL, 0, 0, NULL, NULL) !=
        ARTWORK_ACQUIRE_OK) {
        free(bytes);
        return NULL;
    }
    artwork_coordinator_release(ARTWORK_PRIO_THUMBNAIL);

    home_background_decode_cancel_t cancel;
    if (clock_gettime(CLOCK_MONOTONIC, &cancel.deadline) != 0) {
        free(bytes);
        return NULL;
    }
    cancel.deadline.tv_nsec += 100000000L;
    if (cancel.deadline.tv_nsec >= 1000000000L) {
        cancel.deadline.tv_sec++;
        cancel.deadline.tv_nsec -= 1000000000L;
    }

    home_background_rgb565_t * cache = calloc(1, sizeof(*cache));
    if (!cache) {
        free(bytes);
        return NULL;
    }
    cover_decode_result_t result = cover_decode_to_rgb565_ex(
        bytes, (uint32_t)offset, probe.native_w, probe.native_h,
        ARTWORK_PRIO_THUMBNAIL, home_background_decode_should_cancel, &cancel,
        &cache->pixels);
    if (result != COVER_DECODE_OK || !cache->pixels) {
        free(cache->pixels);
        free(cache);
        free(bytes);
        return NULL;
    }
    free(bytes);
    cache->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    cache->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    cache->descriptor.header.w = probe.native_w;
    cache->descriptor.header.h = probe.native_h;
    cache->descriptor.header.stride = probe.native_w * 2;
    cache->descriptor.data = (const uint8_t *)cache->pixels;
    cache->descriptor.data_size = (uint32_t)probe.native_w * (uint32_t)probe.native_h * 2;
    return cache;
}

/* LVGL's file decoder streams .bin rows from storage on this target. Read a
 * validated, single-frame wallpaper into the same screen-owned RGB565 buffer
 * used by the JPEG fast path so drawing never performs SD reads per row. */
static home_background_rgb565_t * home_background_decode_bin(const char * relative_path) {
    const char * path = asset_path_plain(relative_path);
    if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0;
    lv_image_header_t header;
    memset(&header, 0, sizeof(header));
    if (valid) {
        size_t got = 0;
        while (got < sizeof(header)) {
            ssize_t n = read(fd, (uint8_t *)&header + got, sizeof(header) - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t)n;
        }
        valid = got == sizeof(header);
    }
    lv_display_t * display = lv_display_get_default();
    uint32_t width = (uint32_t)(display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH);
    uint32_t height = (uint32_t)(display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT);
    uint64_t frame_bytes = (uint64_t)width * height * 2u;
    uint64_t expected = (uint64_t)sizeof(header) + frame_bytes;
    valid = valid && width > 0 && height > 0 && frame_bytes <= UINT32_MAX &&
            header.magic == LV_IMAGE_HEADER_MAGIC && header.cf == LV_COLOR_FORMAT_RGB565 &&
            header.flags == 0 && header.reserved_2 == 0 &&
            header.w == width && header.h == height && header.stride == width * 2u &&
            st.st_size > 0 && (uint64_t)st.st_size == expected;
    home_background_rgb565_t * cache = valid ? calloc(1, sizeof(*cache)) : NULL;
    if (valid && !cache) valid = false;
    if (valid) {
        cache->pixels = malloc((size_t)frame_bytes);
        if (!cache->pixels) valid = false;
    }
    size_t offset = 0;
    while (valid && offset < (size_t)frame_bytes) {
        ssize_t n = read(fd, (uint8_t *)cache->pixels + offset, (size_t)frame_bytes - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) valid = false;
        else offset += (size_t)n;
    }
    close(fd);
    if (!valid) {
        if (cache) { free(cache->pixels); free(cache); }
        return NULL;
    }
    cache->descriptor.header = header;
    cache->descriptor.data = (const uint8_t *)cache->pixels;
    cache->descriptor.data_size = (uint32_t)frame_bytes;
    return cache;
}

static void home_background_rgb565_delete_cb(lv_event_t * event) {
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    lv_obj_t * scr = lv_event_get_target(event);
    home_background_rgb565_t * cache = lv_event_get_user_data(event);
    if (!scr || !cache) return;
    lv_obj_set_style_bg_image_src(scr, NULL, 0);
    lv_image_cache_drop(&cache->descriptor);
    free(cache->pixels);
    free(cache);
}

void home_background_apply(lv_obj_t * scr, const char * relative_path) {
    const char * ext = strrchr(relative_path, '.');
    home_background_rgb565_t * cache = NULL;
    if (ext && strcasecmp(ext, ".bin") == 0)
        cache = home_background_decode_bin(relative_path);
    else
        cache = home_background_decode_jpeg(relative_path);
    if (cache) {
        lv_obj_add_event_cb(scr, home_background_rgb565_delete_cb, LV_EVENT_DELETE, cache);
        lv_obj_set_style_bg_image_src(scr, &cache->descriptor, 0);
    } else {
        lv_obj_set_style_bg_image_src(scr, asset_path(relative_path), 0);
    }
}
