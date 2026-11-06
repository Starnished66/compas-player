/* Cover faces: the shared placeholder, and the timer that feeds the decode
 * worker and takes its results. */
#include "cover_flow_internal.h"
#include "../../library/library_thumbnail_placeholder.h"
#include "../builders/screen_builders.h"
#include <stdlib.h>
#include "../../diagnostics/ui_frame_trace.h"

/* Blends the library's album glyph, centred, over the surface colour. */
static void paint_placeholder(uint16_t * px, int face) {
    lv_style_value_t v = { 0 };
    lv_color_t bg = lv_color_hex(0x292C30);
    if (lv_style_get_prop(&style_theme_card_bg, LV_STYLE_BG_COLOR, &v) == LV_STYLE_RES_FOUND) bg = v.color;
    uint16_t fill = lv_color_to_u16(bg);
    for (int i = 0; i < face * face; i++) px[i] = fill;

    thumbnail_placeholder_t glyph;
    if (!thumbnail_placeholder_get(&glyph) || glyph.dsc->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    int gw = (int) glyph.dsc->header.w, gh = (int) glyph.dsc->header.h;
    int stride = (int) glyph.dsc->header.stride;
    if (gw > face || gh > face) return;
    int x0 = (face - gw) / 2, y0 = (face - gh) / 2;
    for (int y = 0; y < gh; y++) {
        const uint8_t * s = glyph.dsc->data + (size_t) y * (size_t) stride;
        uint16_t * d = px + (size_t) (y0 + y) * (size_t) face + x0;
        for (int x = 0; x < gw; x++, s += 4) { /* ARGB8888 in memory: B, G, R, A */
            uint32_t a = s[3];
            d[x] = lv_color_to_u16(lv_color_make((uint8_t) ((s[2] * a + bg.red * (255 - a)) / 255),
                                                 (uint8_t) ((s[1] * a + bg.green * (255 - a)) / 255),
                                                 (uint8_t) ((s[0] * a + bg.blue * (255 - a)) / 255)));
        }
    }
}

/* The shared placeholder keeps its face, so its warps can be built when a step is first needed;
 * the flat and the two full tilts are ready from the start. */
bool cf_faces_create(cover_flow_t * cf) {
    uint16_t * src = malloc((size_t) cf->warps.tilt->out_h * CF_FACE_PX * 2);
    if (!src) return false;
    paint_placeholder(src, CF_FACE_PX);
    cf->ph[0].face = cf->ph[1].face = src; /* [0] owns it */
    bool ok = cf_warp_build(&cf->warps, &cf->ph[0].warp[0], 0, 0, src);
    for (int side = 0; ok && side < 2; side++)
        ok = cf_warp_build(&cf->warps, &cf->ph[side].warp[COVER_FLOW_TILT_STEPS], COVER_FLOW_TILT_STEPS, side ? 1 : -1, src);
    return ok;
}

void cf_faces_destroy(cover_flow_t * cf) {
    cover_flow_loader_drop(&cf->loader);
    cf->ph[1].face = NULL; /* the same pixels as ph[0]'s */
    cf_content_clear(&cf->ph[0], 0);
    cf_content_clear(&cf->ph[1], 0);
}

void cf_pump_kick(void) {
    if (!cf_instance || !cf_instance->pump) return;
    cf_instance->quiet_since = lv_tick_get();
    lv_timer_resume(cf_instance->pump);
}

void cover_flow_retry_faces(void) {
    if (!cf_instance || !cf_instance->face) return; /* the library's own faces do not depend on the network */
    cover_flow_loader_drop(&cf_instance->loader); /* one started before the network came back would fail for good */
    for (int i = 0; i < CF_SLOTS; i++)
        if (cf_instance->slot[i] && cf_instance->slot[i]->failed == 3) cf_instance->slot[i]->failed = 0;
    cf_pump_kick();
}

/* A decoded face arrived for `item`: hand it to the slot still showing that item. */
static void face_arrived(cover_flow_t * cf, const cover_flow_loader_result_t * r) {
    for (int i = 0; i < CF_SLOTS; i++) {
        cf_content_t * c = cf->slot[i];
        if (c->item != r->item) continue;
        if (c->face) break; /* one face per item: its warps were built from it */
        if (r->pixels) {
            c->face = r->pixels;
            cf->view[i].data = NULL;
            cf->relayout = true;
        } else {
            c->failed = !r->allow_generate ? 1 : cf->face ? 3 : 2;
        }
        return;
    }
    free(r->pixels); /* the column moved on */
}

static void reap(cover_flow_t * cf) {
    cover_flow_loader_result_t r;
    if (cf->loader && cover_flow_loader_take(&cf->loader, &r)) face_arrived(cf, &r);
}

/* Next face worth decoding, the centre first and then outward; -1 if none. */
static int next_wanted(const cover_flow_t * cf) {
    for (int k = 0; k < CF_SLOTS; k++) {
        int i = cf_centre_out(k);
        const cf_content_t * c = cf->slot[i];
        if (c->item < 0 || c->item >= cf->count || c->face) continue;
        if (c->failed >= 2 || (c->failed == 1 && i != CF_CENTRE)) continue;
        return i;
    }
    return -1;
}

/* Starts decoding slot `i`'s face. False if nothing was started. */
static bool submit(cover_flow_t * cf, int i) {
    cf_content_t * c = cf->slot[i];
    const cover_flow_item_t * it = &cf->items[c->item];
    if (cf->face ? !it->cover_ref[0] : it->song_id <= 0) { c->failed = 2; return false; }
    cf->loader = cover_flow_loader_start(c->item, it->song_id, it->cover_ref, cf->face, i == CF_CENTRE,
                                         CF_FACE_PX, cf->warps.tilt->out_h);
    if (!cf->loader) c->failed = 2; /* no thread: do not retry on every tick */
    return cf->loader != NULL;
}

static void pump(cover_flow_t * cf) {
    reap(cf);
    if (cf->relayout && !cf->animating) cf_layout_update(); /* an animation lays out every frame */
    bool settled = cf_settled(), busy = cf->loader != NULL;
    int wanted = next_wanted(cf);
    if (settled && !busy && wanted >= 0) busy = submit(cf, wanted);
    if (busy || cf->relayout) { /* a decode or a pass is pending */
        cf->quiet_since = lv_tick_get();
        return;
    }
    if (!settled) lv_timer_pause(cf->pump); /* the end of the gesture wakes it */
    else if (wanted < 0 && lv_tick_elaps(cf->quiet_since) >= CF_TRIM_MS) {
        cf_trim_idle(cf);
        lv_timer_pause(cf->pump);
    }
}

void cf_pump_cb(lv_timer_t * timer) {
    UI_TRACE_SPAN_BEGIN(perf_pump);
    pump(lv_timer_get_user_data(timer));
    UI_TRACE_SPAN_END(UI_TRACE_SPAN_COVER_FLOW_PUMP, perf_pump);
}
