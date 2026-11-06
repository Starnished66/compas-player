/* Slot bookkeeping and the per-frame layout: which image each slot shows and
 * where. Faces and their warps travel with their item when the
 * column shifts by one. */
#include "cover_flow_internal.h"
#include "../../diagnostics/ui_frame_trace.h"
#include <stdlib.h>
#include <string.h>

bool cf_slots_create(cover_flow_t * cf) {
    for (int i = 0; i < CF_SLOTS; i++) {
        cf->slot[i] = calloc(1, sizeof(cf_content_t));
        if (!cf->slot[i]) return false;
        cf->view[i].img = lv_image_create(cf->screen);
        lv_obj_remove_flag(cf->view[i].img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cf->view[i].img, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

void cf_slots_destroy(cover_flow_t * cf) {
    for (int i = 0; i < CF_SLOTS; i++) {
        if (cf->slot[i]) cf_content_clear(cf->slot[i], 0);
        free(cf->slot[i]);
        cf->slot[i] = NULL;
    }
}

void cf_slots_reset(cover_flow_t * cf, int base) {
    cf->base = base;
    for (int i = 0; i < CF_SLOTS; i++) {
        if (cf->slot[i]) cf_content_clear(cf->slot[i], base + i - CF_CENTRE);
        cf->view[i].data = NULL;
    }
}

/* The intermediate steps are only reachable by the slots next to the centre, and only in the
 * part of the range their distance allows. Step 0 belongs to the face and stays. */
static bool step_cacheable(int slot, int step) {
    int o = slot - CF_CENTRE;
    if (o == 0) return step <= CF_HALF_STEP;
    return (o == 1 || o == -1) && step >= CF_HALF_STEP;
}

static void trim_caches(cover_flow_t * cf) {
    for (int i = 0; i < CF_SLOTS; i++)
        for (int s = 1; s < COVER_FLOW_TILT_STEPS; s++)
            if (!step_cacheable(i, s)) cf_warp_free(&cf->slot[i]->warp[s], s);
}

/* The column moved by `shift` items: slot contents rotate with their items and
 * the ones that wrapped around are rebound to the new edge items. */
static void shift_slots(cover_flow_t * cf, int base) {
    int shift = base - cf->base;
    if (shift >= CF_SLOTS || shift <= -CF_SLOTS) {
        cf_slots_reset(cf, base);
        return;
    }
    cf_content_t * old[CF_SLOTS];
    memcpy(old, cf->slot, sizeof(old));
    for (int i = 0; i < CF_SLOTS; i++) {
        cf->slot[i] = old[((i + shift) % CF_SLOTS + CF_SLOTS) % CF_SLOTS];
        cf->view[i].data = NULL;
        bool wrapped = shift > 0 ? i >= CF_SLOTS - shift : i < -shift;
        if (wrapped) cf_content_clear(cf->slot[i], base + i - CF_CENTRE);
    }
    cf->base = base;
    trim_caches(cf);
}

static bool warp_ready(const cf_warp_t * w, int side) {
    return w->buf && w->side == side;
}

/* The nearest already built step to `step`, or NULL. */
static const uint16_t * nearest_ready(const cf_content_t * c, int step, int side, int * picked) {
    for (int d = 1; d < COVER_FLOW_TILT_STEPS; d++) {
        int lo = step - d, hi = step + d;
        if (lo >= 1 && warp_ready(&c->warp[lo], side)) { *picked = lo; return c->warp[lo].buf; }
        if (hi < COVER_FLOW_TILT_STEPS + 1 && warp_ready(&c->warp[hi], side)) { *picked = hi; return c->warp[hi].buf; }
    }
    return NULL;
}

/* A plain RGB565 image of `w` columns read from `data`, whose rows are `stride` bytes apart. */
static void cf_dsc_init(lv_image_dsc_t * d, int w, int h, uint32_t stride, const void * data) {
    *d = (lv_image_dsc_t) { 0 };
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = (uint32_t) w;
    d->header.h = (uint32_t) h;
    d->header.stride = stride;
    d->data = data;
    d->data_size = stride * (uint32_t) h; /* LVGL checks stride * h; a clipped view reads less */
}

/* Pixels a build writes. The flat warp is the face itself, so only its reflection counts. */
static int32_t warp_cost(const cover_flow_tilt_t * t, int step) {
    return t->steps[step].width * (step ? t->out_h : t->out_h - t->face_px);
}

/* Choose the cached tilt before clipping, so fallback geometry determines the visible span. */
static cf_content_t * pick_content(cover_flow_t * cf, int i, int step, int side, int * picked) {
    cf_content_t * c = cf->slot[i];
    if (!c->face) c = &cf->ph[side > 0];
    *picked = step;
    if (!warp_ready(&c->warp[step], side) && warp_cost(cf->warps.tilt, step) > cf->warp_budget) {
        if (nearest_ready(c, step, side, picked)) cf->relayout = true;
    }
    if (!warp_ready(&c->warp[*picked], side)) cf->warp_budget -= warp_cost(cf->warps.tilt, *picked);
    return c;
}

/* One slot's place in this pass, before the covers in front clip it. */
typedef struct {
    cf_content_t * content;    /* NULL: nothing to show */
    cover_flow_span_t ext;      /* its columns on screen */
    int step, side;
} cf_placed_t;

/* The descriptor may point at pixels that get freed: it is set again when the view shows. */
static void hide_view(cf_view_t * v) {
    if (!v->shown) return;
    lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
    v->data = NULL;
    v->shown = false;
}

static void place_slot(cover_flow_t * cf, int i, cf_placed_t * out) {
    int item = cf->base + i - CF_CENTRE;
    *out = (cf_placed_t) { 0 };
    if (item < 0 || item >= cf->count) return;
    int32_t d = item * CF_ONE - cf->pos;
    int32_t ad = d < 0 ? -d : d;
    int step = cover_flow_step_for(ad), side = step ? (d < 0 ? -1 : 1) : 0;
    out->content = pick_content(cf, i, step, side, &step);
    int width = cf->warps.tilt->steps[step].width;
    int32_t off = cover_flow_offset_for(ad, CF_OFF1, CF_PITCH_FAR);
    int32_t x = cf->g.cx + (d < 0 ? -off : off) - width / 2;
    out->ext = (cover_flow_span_t) { x, x + width };
    out->step = step;
    out->side = step ? side : 0;
}

/* Build only columns exposed by the covers in front, retaining them as they move. */
static const uint16_t * visible_pixels(cover_flow_t * cf, const cf_placed_t * pl, cover_flow_span_t vis) {
    cf_warp_t * w = &pl->content->warp[pl->step];
    UI_TRACE_SPAN_BEGIN(perf_warp);
    int columns = cf_warp_require(&cf->warps, w, pl->step, pl->side, pl->content->face,
                                 vis.from - pl->ext.from, vis.to - pl->ext.from);
    if (columns > 0) {
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_WARP, perf_warp);
        if (!pl->step) UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_FLAT, perf_warp);
    }
    if (columns < 0) { cf->relayout = true; return NULL; }
    int height = pl->step ? cf->warps.tilt->out_h : cf->warps.tilt->out_h - cf->warps.tilt->face_px;
    cf->warp_budget -= columns * height;
    return w->buf + (vis.from - pl->ext.from);
}

/* Shows columns `vis` of the placed bitmap: a descriptor over the same pixels, no copy. */
static void show_view(cover_flow_t * cf, int i, const cf_placed_t * pl, cover_flow_span_t vis) {
    cf_view_t * v = &cf->view[i];
    if (vis.to <= vis.from) { hide_view(v); return; }
    const void * data = visible_pixels(cf, pl, vis);
    if (!data) { hide_view(v); return; }
    uint32_t w = (uint32_t) (vis.to - vis.from), stride = (uint32_t) (pl->ext.to - pl->ext.from) * 2;
    lv_obj_set_pos(v->img, vis.from, cf->g.cover_y); /* before the source, so that only the final place is invalidated */
    if (v->data != data || v->side != pl->side || v->dsc.header.w != w || v->dsc.header.stride != stride) {
        cf_dsc_init(&v->dsc, (int) w, cf->warps.tilt->out_h, stride, data);
        lv_image_set_src(v->img, &v->dsc);
        v->data = data;
    }
    if (!v->shown) { lv_obj_remove_flag(v->img, LV_OBJ_FLAG_HIDDEN); v->shown = true; }
    v->x = vis.from;
    v->clip = vis.from - pl->ext.from; v->step = pl->step; v->side = pl->side;
}

/* Each shown cover keeps only the columns the ones nearer the centre leave open. */
static void apply_visible(cover_flow_t * cf, const cf_placed_t * pl) {
    cover_flow_span_t ext[CF_SLOTS], vis[CF_SLOTS];
    for (int i = 0; i < CF_SLOTS; i++) ext[i] = pl[i].ext; /* empty for a slot with nothing to show */
    cover_flow_visible(ext, CF_SLOTS, CF_CENTRE, vis);
    for (int k = 0; k < CF_SLOTS; k++) {
        int i = cf_centre_out(k);
        if (vis[i].from < 0) vis[i].from = 0;
        if (vis[i].to > cf->g.screen_w) vis[i].to = cf->g.screen_w;
        show_view(cf, i, &pl[i], vis[i]);
    }
}

static void trim_steps(cf_content_t * c) {
    for (int s = 1; s < COVER_FLOW_TILT_STEPS; s++) cf_warp_free(&c->warp[s], s);
}

void cf_trim_idle(cover_flow_t * cf) {
    for (int i = 0; i < CF_SLOTS; i++) /* a view still on an in-between step would lose its pixels */
        if (cf->view[i].shown && cf->view[i].step % COVER_FLOW_TILT_STEPS) return;
    for (int i = 0; i < CF_SLOTS; i++) trim_steps(cf->slot[i]);
    trim_steps(&cf->ph[0]);
    trim_steps(&cf->ph[1]);
}

/* Refilled at most once per half refresh period. The tick is frozen during a timer
 * handler pass, so every layout pass of one loop pass shares the same budget. */
static void refill_budget(cover_flow_t * cf) {
    if (lv_tick_elaps(cf->budget_tick) < LV_DEF_REFR_PERIOD / 2) return;
    cf->budget_tick = lv_tick_get();
    cf->warp_budget = CF_WARP_PX_PER_FRAME;
}

void cf_layout_update(void) {
    cover_flow_t * cf = cf_instance;
    if (!cf || cf->count <= 0) return;
    UI_TRACE_SPAN_BEGIN(perf_layout);
    int32_t max_pos = (cf->count - 1) * CF_ONE;
    if (cf->pos < 0) cf->pos = 0;
    if (cf->pos > max_pos) cf->pos = max_pos;
    int base = (cf->pos + CF_ONE / 2) / CF_ONE;
    if (base != cf->base) {
        UI_TRACE_SPAN_BEGIN(perf_shift);
        shift_slots(cf, base);
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_SHIFT, perf_shift);
        UI_TRACE_SPAN_BEGIN(perf_labels);
        cf_update_labels();
        UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_LABELS, perf_labels);
        cf_pump_kick();
    }
    cf->relayout = false;
    refill_budget(cf);
    int32_t budget = cf->warp_budget;
    cf_placed_t placed[CF_SLOTS];
    for (int k = 0; k < CF_SLOTS; k++) { /* centre outward, so the budget goes to what is seen first */
        int i = cf_centre_out(k);
        place_slot(cf, i, &placed[i]);
    }
    cf->warp_budget = budget;
    apply_visible(cf, placed);
    cf_az_update(cf);
    if (cf->relayout) cf_pump_kick();
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_LAYOUT, perf_layout);
}

int cf_slot_at(int32_t x, int32_t y) {
    cover_flow_t * cf = cf_instance;
    if (!cf) return -1;
    for (int i = 0; i < CF_SLOTS; i++) { /* the shown parts never overlap */
        const cf_view_t * v = &cf->view[i];
        if (!v->shown || x < v->x || x >= v->x + (int32_t) v->dsc.header.w) continue;
        int col = v->clip + x - v->x;
        const cover_flow_tilt_step_t * st = &cf->warps.tilt->steps[v->step];
        if (v->side >= 0) col = st->width - 1 - col;
        const cover_flow_tilt_col_t * c = &st->cols[col];
        int row = y - cf->g.cover_y;
        if (row >= c->top && row < c->top + c->height) return i;
    }
    return -1;
}
