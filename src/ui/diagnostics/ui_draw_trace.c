#include "ui_draw_trace.h"
#include "ui_blit_verify.h"

#ifdef UI_PERF_TRACE
#include "lvgl/src/core/lv_global.h"
#include "lvgl/src/core/lv_obj_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "lvgl/src/draw/lv_draw_private.h"
#include "lvgl/src/draw/lv_image_decoder_private.h"
#if LV_USE_LINUX_FBDEV
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DRAW_UNIT_ID_SW 1 /* as in lv_draw_sw.c */
#define DRAW_CAPACITY 1024u

/* Task classes. A class groups tasks whose software cost differs sharply, so
 * the breakdown shows which style feature to remove. */
enum {
    CLS_FILL,         /* square, flat fill */
    CLS_FILL_RADIUS,  /* rounded (or circle) flat fill */
    CLS_FILL_GRAD,    /* gradient fill */
    CLS_BORDER,
    CLS_SHADOW,
    CLS_IMAGE,        /* plain blit, opaque or alpha */
    CLS_IMAGE_XF,     /* scale/rotate/skew or recolor */
    CLS_IMAGE_CLIP,   /* rounded clip / bitmap mask / partial opacity */
    CLS_LABEL,        /* label and letter glyphs */
    CLS_LAYER,        /* layer blended back at full opacity */
    CLS_LAYER_OPA,    /* layer blended with opacity < 255 */
    CLS_MASK,
    CLS_OTHER,        /* line, arc, triangle, blur, vector */
    CLS_COUNT
};

typedef struct { uint32_t ns, tasks, px; } class_sum_t;
typedef struct {
    uint32_t sequence;
    uint64_t timestamp_us;
    class_sum_t cls[CLS_COUNT];
    uint32_t blit;   /* 0 = scroll blit not involved, else outcome code + 1 */
    int32_t blit_dy;
    uint32_t blit_us;
    /* Slowest single task of the render and what it belonged to */
    uint32_t top_cls, top_ns, top_px, top_owner;
    uint32_t top_text[4];     /* first 16 bytes of a label's text */
    /* Callbacks timed during the render: tiny_ttf glyph metrics and bitmaps, image decoder opens */
    uint32_t ttf_dsc_n, ttf_dsc_ns, ttf_bmp_n, ttf_bmp_ns, dec_n, dec_ns;
} draw_record_t;

static draw_record_t ring[DRAW_CAPACITY];
static draw_record_t pending;
static uint32_t blit_code1, blit_us;
static int32_t blit_dy;
static uint32_t ring_next, ring_count;
static uint32_t export_pos, export_left;
static bool armed;
static int32_t (*sw_dispatch)(lv_draw_unit_t *, lv_layer_t *);

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static unsigned classify(const lv_draw_task_t * t) {
    const void * d = t->draw_dsc;
    switch (t->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
            const lv_draw_fill_dsc_t * f = d;
            if (f->grad.dir != LV_GRAD_DIR_NONE) return CLS_FILL_GRAD;
            return f->radius > 0 ? CLS_FILL_RADIUS : CLS_FILL;
        }
        case LV_DRAW_TASK_TYPE_BORDER: return CLS_BORDER;
        case LV_DRAW_TASK_TYPE_BOX_SHADOW: return CLS_SHADOW;
        case LV_DRAW_TASK_TYPE_IMAGE: {
            const lv_draw_image_dsc_t * i = d;
            if (i->rotation || i->skew_x || i->skew_y ||
                (i->scale_x && i->scale_x != LV_SCALE_NONE) ||
                (i->scale_y && i->scale_y != LV_SCALE_NONE) || i->recolor_opa > LV_OPA_TRANSP)
                return CLS_IMAGE_XF;
            if (i->clip_radius || i->bitmap_mask_src || i->opa < LV_OPA_COVER) return CLS_IMAGE_CLIP;
            return CLS_IMAGE;
        }
        case LV_DRAW_TASK_TYPE_LABEL:
        case LV_DRAW_TASK_TYPE_LETTER: return CLS_LABEL;
        case LV_DRAW_TASK_TYPE_LAYER:
            return ((const lv_draw_image_dsc_t *) d)->opa < LV_OPA_COVER ? CLS_LAYER_OPA : CLS_LAYER;
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE: return CLS_MASK;
        default: return CLS_OTHER;
    }
}

/* Owner kind: 0 none, 1 plain obj, 2 label, 3 image, 4 button, 5 other */
static void note_top(const lv_draw_task_t * t, unsigned cls, uint64_t ns, uint32_t px) {
    const lv_draw_dsc_base_t * base = t->draw_dsc;
    const lv_obj_t * o = base ? base->obj : NULL;
    const lv_obj_class_t * oc = o ? o->class_p : NULL;
    pending.top_ns = ns > UINT32_MAX ? UINT32_MAX : (uint32_t) ns;
    pending.top_cls = cls;
    pending.top_px = px;
    pending.top_owner = !oc ? 0 : oc == &lv_obj_class ? 1 : oc == &lv_label_class ? 2 :
                        oc == &lv_image_class ? 3 : oc == &lv_button_class ? 4 : 5;
    memset(pending.top_text, 0, sizeof(pending.top_text));
    if (t->type == LV_DRAW_TASK_TYPE_LABEL) {
        const char * text = ((const lv_draw_label_dsc_t *) t->draw_dsc)->text;
        if (text) memcpy(pending.top_text, text, strnlen(text, sizeof(pending.top_text)));
    }
}

/* Single-threaded SW unit: each dispatch call runs at most one task to
 * completion, and it is the first available one, which is peeked beforehand. */
static int32_t traced_dispatch(lv_draw_unit_t * unit, lv_layer_t * layer) {
    if (!armed) return sw_dispatch(unit, layer);
    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_SW);
    uint64_t start = t ? now_ns() : 0;
    int32_t ret = sw_dispatch(unit, layer);
    if (t && t->state == LV_DRAW_TASK_STATE_FINISHED) {
        uint64_t ns = now_ns() - start;
        lv_area_t clipped;
        uint32_t px = lv_area_intersect(&clipped, &t->area, &t->clip_area)
                          ? (uint32_t) lv_area_get_size(&clipped) : 0;
        unsigned cls = classify(t);
        class_sum_t * c = &pending.cls[cls];
        c->ns += ns > UINT32_MAX ? UINT32_MAX : (uint32_t) ns;
        c->tasks++;
        c->px += px;
        if (ns > pending.top_ns) note_top(t, cls, ns, px);
    }
    return ret;
}

/* Font and decoder callbacks are wrapped in place (no LVGL change) to time what a draw task
 * waits on: tiny_ttf rasterising a glyph or reading its outline, image decoders opening a file. */
#define WRAP_MAX 64
static struct {
    const void * key; /* the face's descriptor: shared by struct copies of the font */
    bool (*dsc)(const lv_font_t *, lv_font_glyph_dsc_t *, uint32_t, uint32_t);
    const void * (*bmp)(lv_font_glyph_dsc_t *, lv_draw_buf_t *);
} fonts[WRAP_MAX];
static struct { lv_image_decoder_t * dec; lv_result_t (*open)(lv_image_decoder_t *, lv_image_decoder_dsc_t *); } decs[8];
static unsigned font_n, dec_n_wrapped;

static void add_time(uint32_t * n, uint32_t * ns, uint64_t t0) {
    uint64_t d = now_ns() - t0;
    (*n)++;
    *ns += d > UINT32_MAX ? UINT32_MAX : (uint32_t) d;
}

static bool wrap_glyph_dsc(const lv_font_t * font, lv_font_glyph_dsc_t * out, uint32_t cp, uint32_t next) {
    for (unsigned i = 0; i < font_n; i++) {
        if (fonts[i].key != font->dsc) continue;
        uint64_t t0 = armed ? now_ns() : 0;
        bool r = fonts[i].dsc(font, out, cp, next);
        if (armed) add_time(&pending.ttf_dsc_n, &pending.ttf_dsc_ns, t0);
        return r;
    }
    return false;
}

static const void * wrap_glyph_bitmap(lv_font_glyph_dsc_t * g, lv_draw_buf_t * buf) {
    for (unsigned i = 0; i < font_n; i++) {
        if (fonts[i].key != g->resolved_font->dsc) continue;
        uint64_t t0 = armed ? now_ns() : 0;
        const void * r = fonts[i].bmp(g, buf);
        if (armed) add_time(&pending.ttf_bmp_n, &pending.ttf_bmp_ns, t0);
        return r;
    }
    return NULL;
}

void ui_draw_trace_wrap_font(lv_font_t * font) {
    if (!font || !font->get_glyph_dsc || font->get_glyph_dsc == wrap_glyph_dsc) return;
    unsigned i = 0;
    while (i < font_n && fonts[i].key != font->dsc) i++;
    if (i == font_n) {
        if (font_n >= WRAP_MAX) return;
        font_n++;
    }
    fonts[i].key = font->dsc;
    fonts[i].dsc = font->get_glyph_dsc;
    fonts[i].bmp = font->get_glyph_bitmap;
    font->get_glyph_dsc = wrap_glyph_dsc;
    if (font->get_glyph_bitmap) font->get_glyph_bitmap = wrap_glyph_bitmap;
}

void ui_draw_trace_unwrap_font(const lv_font_t * font) {
    for (unsigned i = 0; font && i < font_n; i++) {
        if (fonts[i].key != font->dsc) continue;
        fonts[i] = fonts[--font_n];
        return;
    }
}

static lv_result_t wrap_decoder_open(lv_image_decoder_t * dec, lv_image_decoder_dsc_t * dsc) {
    for (unsigned i = 0; i < dec_n_wrapped; i++) {
        if (decs[i].dec != dec) continue;
        uint64_t t0 = armed ? now_ns() : 0;
        lv_result_t r = decs[i].open(dec, dsc);
        if (armed) add_time(&pending.dec_n, &pending.dec_ns, t0);
        return r;
    }
    return LV_RESULT_INVALID;
}

static void wrap_decoders(void) {
    for (lv_image_decoder_t * d = lv_image_decoder_get_next(NULL); d; d = lv_image_decoder_get_next(d)) {
        unsigned i = 0;
        while (i < dec_n_wrapped && decs[i].dec != d) i++;
        if (i < dec_n_wrapped || dec_n_wrapped >= 8 || !d->open_cb || d->open_cb == wrap_decoder_open) continue;
        decs[dec_n_wrapped].dec = d;
        decs[dec_n_wrapped++].open = d->open_cb;
        d->open_cb = wrap_decoder_open;
    }
}

static lv_display_t * bench_disp;
static uint32_t bench_read_us, bench_write_us, bench_ram_us;

void ui_draw_trace_fb_bench(void) {
#if LV_USE_LINUX_FBDEV
    void * page = lv_linux_fbdev_get_inactive_page(bench_disp);
#else
    void * page = NULL; /* host build: no framebuffer pages to time */
#endif
    if (!page) return;
    size_t n = (size_t) lv_display_get_horizontal_resolution(bench_disp) *
               (size_t) lv_display_get_vertical_resolution(bench_disp) * 2u;
    uint8_t * a = malloc(n), * b = malloc(n);
    if (a && b) {
        for (int pass = 0; pass < 2; pass++) { /* second pass: pages and caches warm */
            uint64_t t0 = now_ns();
            memcpy(a, page, n);
            uint64_t t1 = now_ns();
            memcpy(page, a, n);
            uint64_t t2 = now_ns();
            memcpy(b, a, n);
            uint64_t t3 = now_ns();
            bench_read_us = (uint32_t) ((t1 - t0) / 1000u);
            bench_write_us = (uint32_t) ((t2 - t1) / 1000u);
            bench_ram_us = (uint32_t) ((t3 - t2) / 1000u);
        }
        printf("[FB_BENCH] %zu bytes: fb->ram %u us, ram->fb %u us, ram->ram %u us\n",
               n, bench_read_us, bench_write_us, bench_ram_us);
    }
    free(a);
    free(b);
}

void ui_draw_trace_fb_bench_results(uint32_t * read_us, uint32_t * write_us, uint32_t * ram_us) {
    *read_us = bench_read_us; *write_us = bench_write_us; *ram_us = bench_ram_us;
}

void ui_draw_trace_install(lv_display_t * disp) {
    bench_disp = disp;
    if (sw_dispatch) return;
    for (lv_draw_unit_t * u = LV_GLOBAL_DEFAULT()->draw_info.unit_head; u; u = u->next) {
        if (!u->name || (strcmp(u->name, "SW") != 0 && strcmp(u->name, "SW_ARM2D") != 0)) continue;
        sw_dispatch = u->dispatch_cb;
        u->dispatch_cb = traced_dispatch;
        return;
    }
}

void ui_draw_trace_arm(bool on) {
    if (on) {
        wrap_decoders(); /* includes any decoder registered since the last capture */
        memset(ring, 0, sizeof(ring));
        memset(&pending, 0, sizeof(pending));
        blit_code1 = blit_us = 0;
        blit_dy = 0;
        ring_next = ring_count = 0;
    }
    armed = on;
}

void ui_draw_trace_render_start(void) { memset(&pending, 0, sizeof(pending)); }

void ui_draw_trace_note_blit(uint32_t code, uint32_t dy_us) {
    blit_code1 = code + 1u;
    blit_dy = (int16_t) (dy_us >> 16);
    blit_us = dy_us & 0xffffu;
    if (code == 0) ui_blit_verify_frame();
}

void ui_draw_trace_commit(uint32_t frame_sequence, uint64_t timestamp_us) {
    if (!armed) return;
    pending.sequence = frame_sequence;
    pending.timestamp_us = timestamp_us;
    if (!blit_code1) ui_blit_verify_plain_frame();
    pending.blit = blit_code1;
    pending.blit_dy = blit_dy;
    pending.blit_us = blit_us;
    blit_code1 = blit_us = 0;
    blit_dy = 0;
    ring[ring_next] = pending;
    ring_next = (ring_next + 1u) % DRAW_CAPACITY;
    if (ring_count < DRAW_CAPACITY) ring_count++;
    memset(&pending, 0, sizeof(pending));
}

void ui_draw_trace_export_begin(void) {
    export_left = ring_count;
    export_pos = (ring_next + DRAW_CAPACITY - ring_count) % DRAW_CAPACITY;
}

bool ui_draw_trace_export_pending(void) { return export_left != 0; }

void ui_draw_trace_export_discard(void) { export_left = 0; ring_count = 0; }

/* D,seq,time then ns,tasks,px for each class in enum order (fill, fill_radius,
 * fill_grad, border, shadow, image, image_xf, image_clip, label, layer,
 * layer_opa, mask, other), then blit,dy,us: blit 0 = no scroll blit pending, else outcome+1
 * (1 shifted pixels, 2 skipped: area already invalidated, 3 net shift zero, 4 gate refused,
 * 5 display mode, 6 overlay/other content in the list rect, 7 list/ancestor style, 8 stale
 * request, 9 shift too large); dy = rows shifted, us = copy time. seq matches the F record. */
bool ui_draw_trace_export_next(FILE * f) {
    if (!export_left) return true;
    const draw_record_t * r = &ring[export_pos];
    bool ok = fprintf(f, "D,%u,%llu", r->sequence, (unsigned long long) r->timestamp_us) >= 0;
    for (unsigned i = 0; ok && i < CLS_COUNT; i++)
        ok = fprintf(f, ",%u,%u,%u", r->cls[i].ns, r->cls[i].tasks, r->cls[i].px) >= 0;
    if (ok) ok = fprintf(f, ",%u,%d,%u", r->blit, r->blit_dy, r->blit_us) >= 0;
    if (ok) ok = fprintf(f, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", r->top_cls, r->top_ns, r->top_px,
                         r->top_owner, r->top_text[0], r->top_text[1], r->top_text[2], r->top_text[3],
                         r->ttf_dsc_n, r->ttf_dsc_ns, r->ttf_bmp_n, r->ttf_bmp_ns, r->dec_n, r->dec_ns, 0u) >= 0;
    if (ok) { export_pos = (export_pos + 1u) % DRAW_CAPACITY; export_left--; }
    return ok;
}

uint32_t ui_draw_trace_record_size(void) { return (uint32_t) sizeof(draw_record_t); }
uint32_t ui_draw_trace_capacity(void) { return DRAW_CAPACITY; }
#endif
