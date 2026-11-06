/* Integration check with the host build's real LVGL display and image widgets. */
#include "../cover_flow_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

cover_flow_t * cf_instance;
lv_font_t lv_font_montserrat_16; /* the integration creates no text widgets */
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
static int labels, az_updates;
void cf_update_labels(void) { labels++; }
void cf_pump_kick(void) { }
void cf_az_update(cover_flow_t * cf) { (void) cf; az_updates++; }

static void check_immediate(cover_flow_t * cf, lv_display_t * display) {
    for (int i = 1; i <= 3; i++) {
        cf->pos = i * CF_ONE;
        cf_layout_update();
        assert(cf->base == i && labels == i && az_updates == i);
        assert(cf->view[CF_CENTRE].shown);
        assert(cf->view[CF_CENTRE].x == cf->g.cx - CF_FACE_PX / 2);
    }
    cf->pos = 3 * CF_ONE + CF_ONE / 4;
    cf_layout_update();
    assert(cf->base == 3 && labels == 3 && az_updates == 4);
    assert(cf->view[CF_CENTRE].shown);
    assert(cf->view[CF_CENTRE].x != cf->g.cx - CF_FACE_PX / 2);
    cf->pos = -CF_ONE;
    cf_layout_update();
    assert(cf->pos == 0 && cf->base == 0);
    cf->pos = cf->count * CF_ONE;
    cf_layout_update();
    assert(cf->pos == (cf->count - 1) * CF_ONE && cf->base == cf->count - 1);
    assert(labels == 5 && az_updates == 6);
    lv_display_send_event(display, LV_EVENT_REFR_START, NULL);
    assert(labels == 5 && az_updates == 6); /* refresh does no deferred layout */
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(480, 800);
    cover_flow_t cf = { .count = 10, .warp_budget = CF_WARP_PX_PER_FRAME };
    const cover_flow_tilt_params_t params = { 22, 80, 75, 35, 35, 40 };
    cf_instance = &cf;
    cf.screen = lv_obj_create(NULL);
    cf.g.cx = 240;
    cf.g.screen_w = 480;
    cf.warps.tilt = cover_flow_tilt_create(CF_FACE_PX, &params);
    assert(cf.warps.tilt && cf_slots_create(&cf));
    cf_slots_reset(&cf, 0);
    cf.ph[0].face = calloc((size_t) CF_FACE_PX * cf.warps.tilt->out_h, 2);
    assert(cf.ph[0].face);
    cf.ph[1].face = cf.ph[0].face;
    check_immediate(&cf, display);
    uint32_t callbacks = lv_display_get_event_count(display);
    cf_slots_destroy(&cf);
    assert(lv_display_get_event_count(display) == callbacks);
    cf_instance = NULL;
    lv_display_send_event(display, LV_EVENT_REFR_START, NULL);
    cf_layout_update(); /* no queued callback or live instance can visit destroyed slots */
    assert(labels == 5 && az_updates == 6);
    cf.ph[1].face = NULL;
    cf_content_clear(&cf.ph[0], 0);
    cf_content_clear(&cf.ph[1], 0);
    cover_flow_tilt_destroy(cf.warps.tilt);
    lv_obj_delete(cf.screen);
    lv_display_delete(display);
    lv_deinit();
    puts("cover_flow_refresh: PASS");
    return 0;
}
