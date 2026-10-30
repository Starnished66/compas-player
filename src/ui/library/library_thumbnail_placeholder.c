#include "library_thumbnail_placeholder.h"
#include "../assets/assets.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "src/draw/snapshot/lv_snapshot.h"
#include "lvgl/src/display/lv_display_private.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PLACEHOLDER_ASSET "touch_list/list_default_album.png"

/* One buffer per distinct source path, kept for the life of the process. A
 * bound list row may still show a buffer long after the theme changed, so a
 * buffer of a path still in use is never freed before shutdown (a path's buffer is retired when another
 * path comes into use, and freed once no row shows it). The set is bounded by the theme and
 * override asset paths; if it ever fills, callers draw the stock picture.
 * When the assets' path cache is invalidated (theme apply, overrides, soft
 * reload) the next request renders the picture again, and the old buffer is
 * retired instead of freed (rows may still show it). Each retirement first sweeps
 * the retired list: a buffer no image widget of any screen shows any more is freed,
 * so the list holds only what a live row (a screen further down the stack) still
 * points at. The list grows as needed, so an invalidation is never skipped; if the new picture or the list
 * entry cannot be allocated the entry stays out of date and the next request
 * tries again (a failed render waits PLACEHOLDER_RETRY_MS first, so a missing
 * asset is not decoded on every row bind). */
#define PLACEHOLDER_MAX 16
#define PLACEHOLDER_RETRY_MS 1000
static struct {
    char * src;
    lv_draw_buf_t * buf;
    int32_t overhang;
    uint32_t epoch;
    bool render_failed;
    uint32_t failed_tick;
} built[PLACEHOLDER_MAX];
static int built_count;
static lv_draw_buf_t ** retired;
static int retired_count, retired_capacity;

static void retired_sweep(void);

static bool retired_reserve(void) {
    if (retired_count > 0) retired_sweep();
    if (retired_count < retired_capacity) return true;
    int capacity = retired_capacity ? retired_capacity * 2 : 8;
    lv_draw_buf_t ** grown = realloc(retired, sizeof(*grown) * (size_t) capacity);
    if (!grown) return false;
    retired = grown;
    retired_capacity = capacity;
    return true;
}

/* Marks, in one walk of a subtree, every retired buffer an image below `obj` still shows. */
static void mark_shown_below(lv_obj_t * obj, uint8_t * shown) {
    if (lv_obj_check_type(obj, &lv_image_class)) {
        const void * src = lv_image_get_src(obj);
        if (src)
            for (int i = 0; i < retired_count; i++)
                if (src == retired[i]) shown[i] = 1;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) mark_shown_below(lv_obj_get_child(obj, i), shown);
}

/* Frees the retired buffers no widget shows. UI thread (the walk reads the tree). The tree is
 * walked once for all of them. */
static void retired_sweep(void) {
    lv_display_t * disp = lv_display_get_default();
    uint8_t * shown = disp ? calloc((size_t) retired_count, 1) : NULL;
    if (!shown) return; /* cannot look: keep them */
    for (uint32_t i = 0; i < disp->screen_cnt; i++)
        if (disp->screens[i]) mark_shown_below(disp->screens[i], shown);
    for (int layer = 0; layer < 3; layer++) {
        lv_obj_t * top = layer == 0 ? lv_display_get_layer_top(disp)
                       : layer == 1 ? lv_display_get_layer_sys(disp) : lv_display_get_layer_bottom(disp);
        if (top) mark_shown_below(top, shown);
    }
    int kept = 0;
    for (int i = 0; i < retired_count; i++) {
        if (shown[i]) { retired[kept++] = retired[i]; continue; }
        lv_image_cache_drop(retired[i]); /* an address reused later must not hit the old pixels */
        lv_image_header_cache_drop(retired[i]);
        lv_draw_buf_destroy(retired[i]);
    }
    retired_count = kept;
    free(shown);
}

/* Renders the asset exactly as the list row drew it: an lv_image of the PNG
 * with scale ALBUM_THUMBNAIL_PX/width, snapshotted with its transform overhang.
 * When the picture is shrunk (boards under 72 px) the snapshot is the 72 px box
 * with the picture centred in it; it is cropped to exactly ALBUM_THUMBNAIL_PX so
 * the placeholder fills the same square the real cover draws in. */
static lv_draw_buf_t * render(const char * src, int32_t * overhang) {
    lv_image_header_t header;
    if (lv_image_decoder_get_info(src, &header) != LV_RESULT_OK || header.w <= 0 || header.h <= 0) return NULL;
    lv_obj_t * screen = lv_obj_create(NULL);
    if (!screen) return NULL;
    lv_obj_t * image = lv_image_create(screen);
    lv_image_set_src(image, src);
    lv_image_set_scale(image, (uint32_t) ((ALBUM_THUMBNAIL_PX * LV_SCALE_NONE) / header.w));
    lv_draw_buf_t * buf = lv_snapshot_take(image, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_delete(screen);
    if (!buf) return NULL;
    int32_t w = (int32_t) buf->header.w, h = (int32_t) buf->header.h;
    if (ALBUM_THUMBNAIL_PX < header.w && w >= ALBUM_THUMBNAIL_PX && h >= ALBUM_THUMBNAIL_PX) {
        lv_draw_buf_t * exact = lv_draw_buf_create(ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, LV_COLOR_FORMAT_ARGB8888, 0);
        if (!exact) { lv_draw_buf_destroy(buf); return NULL; }
        int32_t x = (w - ALBUM_THUMBNAIL_PX) / 2, y = (h - ALBUM_THUMBNAIL_PX) / 2;
        lv_area_t area = { x, y, x + ALBUM_THUMBNAIL_PX - 1, y + ALBUM_THUMBNAIL_PX - 1 };
        lv_draw_buf_copy(exact, NULL, buf, &area);
        lv_draw_buf_destroy(buf);
        *overhang = 0;
        return exact;
    }
    *overhang = (w - header.w) / 2;
    return buf;
}

/* A new source path arrived (another theme or override): the entries of the other paths are not
 * in use for new binds any more. Their pictures go to the retired list, where the sweep frees each
 * one as soon as no row shows it; the current path's entry never comes here (it is not stale). */
static void drop_stale_entries(const char * current) {
    int kept = 0;
    bool retired_any = false;
    for (int i = 0; i < built_count; i++) {
        if (!strcmp(built[i].src, current) || !retired_reserve()) { built[kept++] = built[i]; continue; }
        retired[retired_count++] = built[i].buf;
        free(built[i].src);
        retired_any = true;
    }
    for (int i = kept; i < built_count; i++) memset(&built[i], 0, sizeof(built[i]));
    built_count = kept;
    if (retired_any) retired_sweep();
}

bool thumbnail_placeholder_get(thumbnail_placeholder_t * out) {
    const char * src = asset_path(PLACEHOLDER_ASSET);
    if (!src || !out) return false;
    uint32_t epoch = assets_path_cache_epoch();
    for (int i = 0; i < built_count; i++)
        if (!strcmp(built[i].src, src)) {
            if (built[i].epoch != epoch &&
                (!built[i].render_failed || lv_tick_elaps(built[i].failed_tick) >= PLACEHOLDER_RETRY_MS) &&
                retired_reserve()) {
                int32_t fresh_overhang = 0;
                lv_draw_buf_t * fresh = render(src, &fresh_overhang);
                if (fresh) {
                    retired[retired_count++] = built[i].buf;
                    built[i].buf = fresh;
                    built[i].overhang = fresh_overhang;
                    built[i].epoch = epoch;
                    built[i].render_failed = false;
                } else {
                    built[i].render_failed = true; /* still out of date: tried again after the pause */
                    built[i].failed_tick = lv_tick_get();
                }
            }
            out->dsc = (const lv_image_dsc_t *) built[i].buf;
            out->overhang = built[i].overhang;
            return true;
        }
    drop_stale_entries(src);
    if (built_count == PLACEHOLDER_MAX) return false;
    int32_t overhang = 0;
    lv_draw_buf_t * buf = render(src, &overhang);
    if (!buf) return false;
    char * copy = strdup(src);
    if (!copy) { lv_draw_buf_destroy(buf); return false; }
    built[built_count].src = copy;
    built[built_count].buf = buf;
    built[built_count].overhang = overhang;
    built[built_count].epoch = epoch;
    built_count++;
    out->dsc = (const lv_image_dsc_t *) buf;
    out->overhang = overhang;
    return true;
}

void thumbnail_placeholder_release(void) {
    for (int i = 0; i < built_count; i++) {
        lv_draw_buf_destroy(built[i].buf);
        free(built[i].src);
    }
    built_count = 0;
    for (int i = 0; i < retired_count; i++) lv_draw_buf_destroy(retired[i]);
    free(retired);
    retired = NULL;
    retired_count = retired_capacity = 0;
}
