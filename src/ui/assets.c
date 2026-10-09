#include "assets.h"
#include "frosted_glass.h"
#include "gui_themes.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define THEME_ROOT "assets/theme2/"
#else
  /* Present on every real R1 as part of the stock firmware itself. */
  #define THEME_ROOT "/usr/resource/litegui/theme2/"
  /* Writable override, checked first by asset_path(), for custom assets
   * not present in the read-only THEME_ROOT squashfs pack. /usr/data is the
   * persistent writable partition. */
  #define THEME_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#endif

void assets_init(void) {
    lv_lodepng_init();
    lv_fs_posix_init();
    lv_tjpgd_init();
    lv_fs_memfs_init();
}

typedef struct asset_path_entry {
    struct asset_path_entry * next;
    char value[];
} asset_path_entry_t;

static asset_path_entry_t * asset_paths;

/* LVGL keeps raw path pointers in some styles. Intern the fully resolved
 * value so the pointer remains stable without leaking another strdup on
 * every soft UI rebuild. The set is bounded by native asset names times
 * the two possible roots (stock/override), not by reload count. */
static const char * asset_path_intern(const char * value) {
    for (asset_path_entry_t * e = asset_paths; e; e = e->next) {
        if (strcmp(e->value, value) == 0) return e->value;
    }
    size_t len = strlen(value);
    asset_path_entry_t * e = malloc(sizeof(*e) + len + 1);
    if (!e) return NULL;
    memcpy(e->value, value, len + 1);
    e->next = asset_paths;
    asset_paths = e;
    return e->value;
}

/* Resolves relative_path into host absolute path (or plain path) buffer,
 * checking non-excluded plugin override first, then active native theme layer,
 * then stock fallback. On HOST_BUILD, any relative result is absolutized against
 * the process CWD so callers expecting absolute paths (like pill_row_apply_icon)
 * receive genuine absolute paths for all layers. */
static bool asset_resolve_plain(const char * relative_path, char * out, size_t out_size) {
    if (!relative_path || !out || out_size == 0) return false;

    char resolved[GUI_THEMES_MAX_PATH + 64];
    bool found = false;

#ifndef HOST_BUILD
    /* 1. Plugin override: checked if not legacy-excluded and accessible on disk */
    if (!gui_themes_is_legacy_excluded(relative_path)) {
        int n = snprintf(resolved, sizeof(resolved), THEME_OVERRIDE_ROOT "%s", relative_path);
        if (n > 0 && (size_t) n < sizeof(resolved) && access(resolved, R_OK) == 0) {
            found = true;
        }
    }
#endif

    /* 2. Native theme icon layer resolution */
    if (!found && gui_themes_resolve_icon_layer(relative_path, resolved, sizeof(resolved))) {
        found = true;
    }

    /* 3. Stock fallback */
    if (!found) {
        int n = snprintf(resolved, sizeof(resolved), THEME_ROOT "%s", relative_path);
        if (n <= 0 || (size_t) n >= sizeof(resolved)) return false;
        found = true;
    }

#ifdef HOST_BUILD
    /* Absolutize resolved path on host if not already an absolute path */
    if (resolved[0] != '/') {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd))) {
            int n = snprintf(out, out_size, "%s/%s", cwd, resolved);
            return (n > 0 && (size_t) n < out_size);
        }
    }
#endif

    int n = snprintf(out, out_size, "%s", resolved);
    return (n > 0 && (size_t) n < out_size);
}

const char * asset_path(const char * relative_path) {
    char plain[GUI_THEMES_MAX_PATH + 320];
    if (!asset_resolve_plain(relative_path, plain, sizeof(plain))) return NULL;
    char s_buf[GUI_THEMES_MAX_PATH + 350];
    int n = snprintf(s_buf, sizeof(s_buf), "S:%s", plain);
    if (n <= 0 || (size_t) n >= sizeof(s_buf)) return NULL;
    return asset_path_intern(s_buf);
}

const char * asset_path_plain(const char * relative_path) {
    char plain[GUI_THEMES_MAX_PATH + 320];
    if (!asset_resolve_plain(relative_path, plain, sizeof(plain))) return NULL;
    return asset_path_intern(plain);
}

const char * asset_stock_root(void) {
    return THEME_ROOT;
}

const char * asset_override_root(void) {
#ifdef HOST_BUILD
    return NULL;
#else
    return THEME_OVERRIDE_ROOT;
#endif
}

const lv_image_dsc_t * asset_png_memory(const char * relative_path) {
    const char * resolved = asset_path(relative_path);
    const char * path = resolved;
    if (path[0] && path[1] == ':') path += 2; /* strip LVGL's POSIX drive prefix */

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return NULL;
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t * data = malloc((size_t) st.st_size);
    bool ok = data && fread(data, 1, (size_t) st.st_size, f) == (size_t) st.st_size;
    fclose(f);
    if (!ok) { free(data); return NULL; }

    lv_image_dsc_t * dsc = calloc(1, sizeof(*dsc));
    if (!dsc) { free(data); return NULL; }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->data = data;
    dsc->data_size = (uint32_t) st.st_size;
    return dsc;
}

void asset_png_memory_free(const lv_image_dsc_t * image) {
    if (!image) return;
    free((void *) image->data);
    free((void *) image);
}

bool asset_decoded_image_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!image || !relative_path) return false;
    asset_decoded_image_close(image);

    const char * resolved = asset_path(relative_path);
    image->path = resolved ? strdup(resolved) : NULL;
    if (!image->path) return false;

    lv_image_decoder_args_t args = { .no_cache = true };
    if (lv_image_decoder_open(&image->decoder, image->path, &args) != LV_RESULT_OK ||
        !image->decoder.decoded) {
        free(image->path);
        memset(image, 0, sizeof(*image));
        return false;
    }
    image->open = true;
    return true;
}

bool asset_decoded_image_prepare_retint(asset_decoded_image_t * image, const char * relative_path) {
    if (!image || !relative_path) return false;
    const char * resolved = asset_path(relative_path);
    if (!resolved) return false;

    bool same_source = image->open && image->path && strcmp(image->path, resolved) == 0;
    if (!same_source && !asset_decoded_image_open(image, relative_path)) return false;

    lv_draw_buf_t * buffer = (lv_draw_buf_t *) image->decoder.decoded;
    if (!buffer || !buffer->data) return false;
    uint64_t pixel_size_wide = (uint64_t) buffer->header.stride * buffer->header.h;
    if (pixel_size_wide == 0 || pixel_size_wide > SIZE_MAX) return false;
    size_t pixel_size = (size_t) pixel_size_wide;

    if (same_source && image->pristine_pixels && image->pristine_size == pixel_size) {
        /* The cache must stop referring to the published pixels before they
         * are restored and recolored in place. */
        lv_image_cache_drop(buffer);
        memcpy(buffer->data, image->pristine_pixels, pixel_size);
        return true;
    }

    if (same_source) {
        /* The first snapshot allocation may have failed. Re-decode from the
         * retained resolved path so a previous tint can never compound. */
        lv_image_cache_drop(buffer);
        lv_image_decoder_close(&image->decoder);
        memset(&image->decoder, 0, sizeof(image->decoder));
        lv_image_decoder_args_t args = { .no_cache = true };
        if (lv_image_decoder_open(&image->decoder, image->path, &args) != LV_RESULT_OK ||
            !image->decoder.decoded) {
            image->open = false;
            return false;
        }
        buffer = (lv_draw_buf_t *) image->decoder.decoded;
        if (!buffer->data) {
            lv_image_decoder_close(&image->decoder);
            memset(&image->decoder, 0, sizeof(image->decoder));
            image->open = false;
            return false;
        }
        pixel_size_wide = (uint64_t) buffer->header.stride * buffer->header.h;
        if (pixel_size_wide == 0 || pixel_size_wide > SIZE_MAX) return false;
        pixel_size = (size_t) pixel_size_wide;
    }

    free(image->pristine_pixels);
    image->pristine_pixels = malloc(pixel_size);
    image->pristine_size = image->pristine_pixels ? pixel_size : 0;
    if (image->pristine_pixels) memcpy(image->pristine_pixels, buffer->data, pixel_size);
    image->open = true;
    return true;
}

void asset_decoded_image_close(asset_decoded_image_t * image) {
    if (!image) return;
    if (image->open) {
        if (image->decoder.decoded) lv_image_cache_drop(image->decoder.decoded);
        lv_image_decoder_close(&image->decoder);
    }
    free(image->path);
    free(image->pristine_pixels);
    memset(image, 0, sizeof(*image));
}

bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * relative_path) {
    if (!asset_decoded_image_open(image, relative_path)) return false;
    const lv_draw_buf_t * buffer = image->decoder.decoded;
    if (buffer->header.cf != LV_COLOR_FORMAT_ARGB8888) return true;
    /* Keep the decoder's owned ARGB buffer and its alpha plane. Expanding
     * the dithered 565 result back to 888 makes the eventual framebuffer
     * conversion exact without changing LVGL's image/decoder ownership. */
    for (uint32_t y = 0; y < buffer->header.h; ++y) {
        lv_color32_t * row = (lv_color32_t *) (buffer->data + y * buffer->header.stride);
        for (uint32_t x = 0; x < buffer->header.w; ++x) {
            uint16_t pixel = rgb888_to_565_dithered(row[x].red, row[x].green, row[x].blue, x, y);
            uint8_t r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
            row[x].red = (r << 3) | (r >> 2);
            row[x].green = (g << 2) | (g >> 4);
            row[x].blue = (b << 3) | (b >> 2);
        }
    }
    return true;
}

const void * asset_decoded_image_source(const asset_decoded_image_t * image) {
    if (!image || !image->open || !image->decoder.decoded) return NULL;
    /* Publication promises that this complete, owned buffer stays immutable
     * until close, which drops renderer and decoder cache entries first. */
    lv_draw_buf_set_flag((lv_draw_buf_t *)image->decoder.decoded, LV_IMAGE_FLAGS_USER2);
    return image->decoder.decoded;
}
