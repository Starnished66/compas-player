#include "ui_blit_verify.h"
#include "ui_frame_trace.h"

#ifdef UI_PERF_TRACE
#include "lvgl/src/display/lv_display_private.h"
#if LV_USE_LINUX_FBDEV
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "lvgl/src/core/lv_obj_private.h"
#include "lvgl/src/core/lv_obj_draw_private.h"

#define VERIFY_FILE "/usr/data/mnt/sd_0/compas_scroll_blit.verify"
#define VERIFY_RECHECK_MS 500
#define EXCL_MAX 24
#define LV_HOR_RES_MAX_GUESS 1024

static ui_blit_verify_stats_t stats = { .bad_x1 = 1, .bad_x2 = 0 };
static bool selftest_mode; /* the pending check follows a frame that was not a scroll blit */
static bool dumped;
static lv_draw_buf_t * reference;
static uint32_t frame_counter;
static bool pending; /* a check is due right after the render of this pass */

static lv_obj_t * list;
static lv_area_t rect;
static lv_area_t excl[EXCL_MAX + LV_INV_BUF_SIZE]; /* the layers' objects, then the areas dirtied since the frame */
static int excl_n;
static int32_t scroll_y;

static bool compare_column; /* second word of the verify file is "col": compare the scrollbar column too */

static uint32_t every_nth(void) {
    static uint32_t n;
    static uint32_t checked_at;
    static bool checked;
    if (!checked || lv_tick_elaps(checked_at) >= VERIFY_RECHECK_MS) {
        n = 0;
        compare_column = false;
        FILE * f = fopen(VERIFY_FILE, "r");
        if (f) {
            int v = 1;
            char word[16] = "";
            int got = fscanf(f, "%d %15s", &v, word);
            if (got < 1 || v < 1) v = 1;
            n = (uint32_t) v;
            compare_column = got == 2 && strcmp(word, "col") == 0;
            fclose(f);
        }
        checked_at = lv_tick_get();
        checked = true;
    }
    return n;
}


#define DUMP_DIR "/usr/data/mnt/sd_0/"

static lv_obj_t * deepest_at(lv_obj_t * o, int32_t x, int32_t y) {
    for (int32_t i = (int32_t) lv_obj_get_child_count(o) - 1; i >= 0; i--) {
        lv_obj_t * c = lv_obj_get_child(o, (uint32_t) i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
        if (x < c->coords.x1 || x > c->coords.x2 || y < c->coords.y1 || y > c->coords.y2) continue;
        lv_obj_t * d = deepest_at(c, x, y);
        return d ? d : c;
    }
    return NULL;
}

/* Writes what the panel shows and what a stock render gives around the first bad box, and who owns it. */
static void dump_evidence(const uint8_t * front, uint32_t fb_stride, int32_t bx1, int32_t by1, int32_t bx2, int32_t by2) {
    int32_t x1 = LV_MAX(bx1 - 8, 0), y1 = LV_MAX(by1 - 8, 0);
    int32_t x2 = LV_MIN(bx2 + 8, (int32_t) reference->header.w - 1), y2 = LV_MIN(by2 + 8, (int32_t) reference->header.h - 1);
    int32_t w = x2 - x1 + 1, h = y2 - y1 + 1;
    char tag[64], path[160];
    snprintf(tag, sizeof(tag), "%s", ui_frame_trace_tag());
    for (char * c = tag; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) *c = '_';
    snprintf(path, sizeof(path), DUMP_DIR "blit_bad_%s_exp.raw", tag);
    FILE * fe = fopen(path, "wb");
    snprintf(path, sizeof(path), DUMP_DIR "blit_bad_%s_act.raw", tag);
    FILE * fa = fopen(path, "wb");
    if (fe && fa) {
        for (int32_t y = y1; y <= y2; y++) {
            fwrite((const uint8_t *) reference->data + (size_t) y * reference->header.stride + (size_t) x1 * 2, 2, (size_t) w, fe);
            fwrite(front + (size_t) y * fb_stride + (size_t) x1 * 2, 2, (size_t) w, fa);
        }
    }
    if (fe) fclose(fe);
    if (fa) fclose(fa);
    snprintf(path, sizeof(path), DUMP_DIR "blit_bad_%s_info.txt", tag);
    FILE * fi = fopen(path, "w");
    if (!fi) return;
    fprintf(fi, "raw RGB565 little-endian, x=%d y=%d w=%d h=%d (bad box %d,%d-%d,%d), selftest=%d\n",
            x1, y1, w, h, bx1, by1, bx2, by2, (int) selftest_mode);
    lv_obj_t * o = deepest_at(list, (bx1 + bx2) / 2, (by1 + by2) / 2);
    for (int depth = 0; o && depth < 4; depth++, o = lv_obj_get_parent(o)) {
        const lv_obj_class_t * c = o->class_p;
        fprintf(fi, "obj %p class=%s coords=(%d,%d)-(%d,%d) ext=%d\n", (void *) o,
                c == &lv_obj_class ? "obj" : c == &lv_label_class ? "label" : c == &lv_image_class ? "image" : "other",
                (int) o->coords.x1, (int) o->coords.y1, (int) o->coords.x2, (int) o->coords.y2,
                (int) lv_obj_get_ext_draw_size(o));
        if (c == &lv_image_class) {
            const void * src = lv_image_get_src(o);
            lv_image_src_t type = lv_image_src_get_type(src);
            fprintf(fi, "  image src=%p type=%d scale=%d offset=(%d,%d)\n", src, (int) type,
                    (int) lv_image_get_scale(o), (int) lv_image_get_offset_x(o), (int) lv_image_get_offset_y(o));
            if (type == LV_IMAGE_SRC_VARIABLE) {
                const lv_image_dsc_t * d = src;
                fprintf(fi, "  dsc %dx%d cf=%d stride=%d data=%p first_px=%04x\n", (int) d->header.w, (int) d->header.h,
                        (int) d->header.cf, (int) d->header.stride, (const void *) d->data,
                        d->data ? ((const uint16_t *) d->data)[0] : 0);
            }
            else if (type == LV_IMAGE_SRC_FILE) fprintf(fi, "  file=%s\n", (const char *) src);
        }
    }
    fclose(fi);
}

enum { SKIP_GONE, SKIP_NOFRONT, SKIP_SCROLLED, SKIP_SNAPSHOT, SKIP_EXCLUDED };
static void skip(int why) {
    stats.skipped++;
    stats.skip_reason[why]++;
}

/* Runs right after the render of the pass that produced the frame (ui_blit_verify_after_render()):
 * nothing can have scrolled or been rebound between the flush and this comparison. */
static void verify_now(void) {
    /* The list may have been deleted (screen change) since the check was scheduled */
    lv_obj_t * current = NULL;
    lv_area_t current_rect, unused[EXCL_MAX];
    int unused_n = 0;
    if (!lv_refr_scroll_blit_last(&current, &current_rect, unused, EXCL_MAX, &unused_n) || current != list ||
        !lv_obj_is_valid(list)) {
        skip(SKIP_GONE);
        return;
    }
    lv_display_t * disp = lv_obj_get_display(list);
    lv_obj_t * screen = lv_obj_get_screen(list);
#if LV_USE_LINUX_FBDEV
    const uint8_t * front = disp ? lv_linux_fbdev_get_active_page(disp) : NULL;
    uint32_t fb_stride = disp ? lv_linux_fbdev_get_stride(disp) : 0;
#else
    const uint8_t * front = NULL; /* host build: no front page to read, every check is skipped */
    uint32_t fb_stride = 0;
#endif
    if (!front || !fb_stride || screen != lv_screen_active() ||
        lv_obj_get_scroll_y(list) != scroll_y || lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        skip(!front || !fb_stride ? SKIP_NOFRONT : SKIP_SCROLLED); /* nothing to compare against */
        return;
    }
    if (!reference) reference = lv_snapshot_create_draw_buf(screen, LV_COLOR_FORMAT_RGB565);
    if (!reference || lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565, reference) != LV_RESULT_OK) {
        skip(SKIP_SNAPSHOT);
        return;
    }
    if (lv_obj_get_scroll_y(list) != scroll_y) { /* changed while rendering */
        skip(SKIP_SCROLLED);
        return;
    }

    /* Anything visible on the top and system layers is not part of a screen render */
    lv_obj_t * layers[2] = { lv_display_get_layer_top(disp), lv_display_get_layer_sys(disp) };
    for (int l = 0; l < 2; l++) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(layers[l]) && excl_n < EXCL_MAX; i++) {
            lv_obj_t * c = lv_obj_get_child(layers[l], i);
            if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN)) continue;
            int32_t ext = lv_obj_get_ext_draw_size(c);
            lv_area_t a = c->coords;
            lv_area_increase(&a, ext, ext);
            excl[excl_n++] = a;
        }
    }
    /* A marquee step, a landing label or any other change made since the frame was drawn is
     * already queued as an invalid area and not on the panel yet: the model has moved on there
     * (the list itself has not, see scroll_y), so those areas are left out of the comparison.
     * Everything else must still equal a stock render. */
    for (uint32_t i = 0; i < disp->inv_p && i < LV_INV_BUF_SIZE; i++) {
        if (disp->inv_area_joined[i]) continue; /* covered by the area it was merged into */
        excl[excl_n++] = disp->inv_areas[i];
    }
    uint64_t bad = 0, compared = 0, excluded = 0;
    int32_t bx1 = INT32_MAX, by1 = INT32_MAX, bx2 = -1, by2 = -1;
    int32_t width = rect.x2 - rect.x1 + 1;
    bool skip_x[LV_HOR_RES_MAX_GUESS];
    for (int32_t y = rect.y1; y <= rect.y2; y++) {
        memset(skip_x, 0, (size_t) width * sizeof(bool));
        for (int i = 0; i < excl_n; i++) {
            if (y < excl[i].y1 || y > excl[i].y2) continue;
            for (int32_t x = LV_MAX(excl[i].x1, rect.x1); x <= LV_MIN(excl[i].x2, rect.x2); x++) skip_x[x - rect.x1] = true;
        }
        const uint16_t * a = (const uint16_t *) (front + (size_t) y * fb_stride) + rect.x1;
        const uint16_t * b = (const uint16_t *) ((const uint8_t *) reference->data + (size_t) y * reference->header.stride) + rect.x1;
        for (int32_t x = 0; x < width; x++) {
            if (skip_x[x]) { excluded++; continue; }
            compared++;
            if (a[x] == b[x]) continue;
            bad++;
            if (rect.x1 + x < bx1) bx1 = rect.x1 + x;
            if (rect.x1 + x > bx2) bx2 = rect.x1 + x;
            if (y < by1) by1 = y;
            if (y > by2) by2 = y;
        }
    }
    /* A check whose whole rectangle was left out compared nothing: it is a skip, not a pass. */
    if (!compared) {
        skip(SKIP_EXCLUDED);
        return;
    }
    stats.compared_px += compared;
    stats.excluded_px += excluded;
    if (selftest_mode) stats.selftest_checked++;
    else stats.checked++;
    if (bad && selftest_mode) {
        stats.selftest_bad++;
    }
    else if (bad) {
        if (!stats.bad_frames) { stats.bad_x1 = bx1; stats.bad_y1 = by1; stats.bad_x2 = bx2; stats.bad_y2 = by2; }
        stats.bad_frames++;
        stats.bad_pixels += bad;
    }
    if (bad && !dumped) {
        dumped = true;
        dump_evidence(front, fb_stride, bx1, by1, bx2, by2);
    }
}

static void schedule(bool selftest) {
    lv_display_t * disp = lv_display_get_default();
    if (!disp || disp->hor_res > LV_HOR_RES_MAX_GUESS) return;
    if (!lv_refr_scroll_blit_last(&list, &rect, excl, EXCL_MAX, &excl_n)) return;
    /* The last scroll blit on record may belong to a list that is not shown now (the check of a
     * plain frame reaches back to it): such a check could only be skipped, as "scrolled" and
     * "gone" were for Artists and All Songs, whose own scrolls are never blitted (the A-Z strip
     * over the list refuses them), so it is not scheduled at all. */
    if (selftest && (!lv_obj_is_valid(list) || lv_obj_get_screen(list) != lv_screen_active())) return;
    selftest_mode = selftest;
    if (compare_column) excl_n = 0; /* nothing but the layers' objects is skipped then */
    scroll_y = lv_obj_get_scroll_y(list);
    pending = true;
}

void ui_blit_verify_after_render(void) {
    if (!pending) return;
    pending = false;
    verify_now();
}

void ui_blit_verify_frame(void) {
    uint32_t n = every_nth();
    if (!n || pending || (++frame_counter % n) != 0) return;
    schedule(false);
}

/* A frame that was not a scroll blit: the same comparison must find nothing, which validates the check itself. */
void ui_blit_verify_plain_frame(void) {
    static uint32_t plain_counter;
    uint32_t n = every_nth();
    if (!n || pending || (++plain_counter % (n * 8)) != 0) return;
    schedule(true);
}

void ui_blit_verify_reset(void) {
    stats = (ui_blit_verify_stats_t) { .bad_x1 = 1, .bad_x2 = 0 };
    frame_counter = 0;
    pending = false;
    dumped = false;
}

void ui_blit_verify_stats(ui_blit_verify_stats_t * out) {
    *out = stats;
}
#endif
