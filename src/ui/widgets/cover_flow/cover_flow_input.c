/* Touch: drag, fling and snap, tap and long press on the covers. */
#include "cover_flow_internal.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_shell.h"
#include <stdlib.h>

#define FLING_MIN_MS 160
#define FLING_MAX_MS 700
#define FLING_MAX_ITEMS 12
#define SAMPLE_WINDOW_MS 100 /* velocity is read over the last stretch of the drag */
#define MAX_PX_PER_S 8000
#define FINGER_STILL_MS 80   /* a release this long after the last move starts no fling */

static void anim_exec_cb(void * var, int32_t v) {
    cover_flow_t * cf = var;
    cf->pos = v;
    cf_layout_update();
}

static void anim_done_cb(lv_anim_t * a) {
    cover_flow_t * cf = a->var;
    cf->animating = false;
    cf_az_show(cf, false);
    cf_pump_kick();
}

static void stop_anim(cover_flow_t * cf) {
    lv_anim_delete(cf, anim_exec_cb);
    cf->animating = false;
}

bool cf_settled(void) {
    return cf_instance && !cf_instance->pressed && !cf_instance->animating && !cf_instance->az.scrubbing;
}

void cf_input_stop(void) {
    cover_flow_t * cf = cf_instance;
    if (!cf) return;
    stop_anim(cf);
    cf->pressed = cf->dragging = false;
}

static void animate_to(cover_flow_t * cf, int32_t target, uint32_t ms) {
    stop_anim(cf);
    if (target == cf->pos) {
        cf_pump_kick();
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cf);
    lv_anim_set_values(&a, cf->pos, target);
    lv_anim_set_duration(&a, gui_anim_ms(ms));
    lv_anim_set_exec_cb(&a, anim_exec_cb);
    lv_anim_set_completed_cb(&a, anim_done_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    cf->animating = true;
    lv_anim_start(&a);
}

/* Animates the scroll position to `item`. */
void cf_scroll_to(int item) {
    cover_flow_t * cf = cf_instance;
    if (!cf || item < 0 || item >= cf->count) return;
    int32_t dist = abs(item * CF_ONE - cf->pos) / CF_ONE;
    uint32_t ms = FLING_MIN_MS + (uint32_t) dist * 60;
    animate_to(cf, item * CF_ONE, ms > FLING_MAX_MS ? FLING_MAX_MS : ms);
}

static void add_sample(cover_flow_t * cf, int32_t x) {
    if (cf->sample_n == 4) {
        for (int i = 1; i < 4; i++) cf->sample[i - 1] = cf->sample[i];
        cf->sample_n = 3;
    }
    cf->sample[cf->sample_n].x = x;
    cf->sample[cf->sample_n].t = lv_tick_get();
    cf->sample_n++;
}

/* Release velocity in items per second, 16.16, positive towards higher items. */
static int32_t release_velocity(const cover_flow_t * cf) {
    if (cf->sample_n < 2) return 0;
    const cf_sample_t * last = &cf->sample[cf->sample_n - 1];
    if (lv_tick_elaps(last->t) > FINGER_STILL_MS) return 0;
    const cf_sample_t * first = last;
    for (int i = cf->sample_n - 2; i >= 0; i--) {
        if (last->t - cf->sample[i].t > SAMPLE_WINDOW_MS) break;
        first = &cf->sample[i];
    }
    uint32_t dt = last->t - first->t;
    if (dt == 0) return 0;
    int32_t px_per_s = (last->x - first->x) * 1000 / (int32_t) dt;
    if (px_per_s > MAX_PX_PER_S) px_per_s = MAX_PX_PER_S;
    if (px_per_s < -MAX_PX_PER_S) px_per_s = -MAX_PX_PER_S;
    return -(px_per_s * CF_ONE) / CF_STEP_PX; /* dragging left moves to higher items */
}

/* Coasts with the release velocity, then lands on an item. */
static void fling(cover_flow_t *cf) {
    int32_t v = release_velocity(cf);
    int32_t coast = v * CF_COAST_MS / 1000; /* 16.16 items */
    int32_t target = (cf->pos + coast + CF_ONE / 2) / CF_ONE;
    int32_t from = (cf->pos + CF_ONE / 2) / CF_ONE;
    if (target > from + FLING_MAX_ITEMS) target = from + FLING_MAX_ITEMS;
    if (target < from - FLING_MAX_ITEMS) target = from - FLING_MAX_ITEMS;
    if (target < 0) target = 0;
    if (target > cf->count - 1) target = cf->count - 1;
    cf_scroll_to(target);
}

static void on_pressed(cover_flow_t * cf, lv_point_t p) {
    cf->was_animating = cf->animating;
    stop_anim(cf);
    cf->pressed = true;
    cf->dragging = false;
    cf->long_fired = false;
    cf->start = p;
    cf->sample_n = 0;
    add_sample(cf, p.x);
}

static void on_pressing(cover_flow_t * cf, lv_point_t p) {
    if (!cf->pressed) return;
    add_sample(cf, p.x);
    if (!cf->dragging) {
        if (abs(p.x - cf->start.x) < NAV_SWIPE_DEADZONE_PX) return;
        cf->dragging = true;
        cf_az_show(cf, true);
        cf->grab_x = p.x; /* follow from here, so crossing the dead zone does not jump */
        cf->grab_pos = cf->pos;
    }
    int32_t pos = cf->grab_pos - (p.x - cf->grab_x) * CF_ONE / CF_STEP_PX;
    if (pos == cf->pos && !cf->relayout) return;
    cf->pos = pos;
    cf_layout_update();
}

/* A tap on a cover: the centre one is chosen, a side one is scrolled to. */
static void on_tap(cover_flow_t * cf, lv_point_t p) {
    if (cf->was_animating) { /* the tap only stopped a fling */
        cf_scroll_to(cf->base);
        return;
    }
    int i = cf_slot_at(p.x, p.y);
    if (i < 0) return;
    int item = cf->slot[i]->item;
    int32_t d = abs(item * CF_ONE - cf->pos);
    if (item == cf->base && d < CF_ONE / 4) cf->on_select(item); /* may replace this screen: last */
    else cf_scroll_to(item);
}

static void on_released(cover_flow_t * cf, lv_point_t p, bool lost) {
    if (!cf->pressed) return;
    cf->pressed = false;
    if (cf->dragging) {
        cf->dragging = false;
        fling(cf);
        if (!cf->animating) cf_az_show(cf, false);
    } else if (!lost && !cf->long_fired) {
        on_tap(cf, p);
    } else {
        cf_scroll_to(cf->base);
    }
    cf_pump_kick();
}

static void on_long_press(cover_flow_t * cf, lv_point_t p) {
    if (!cf->pressed || cf->dragging || !cf->on_hold) return;
    int i = cf_slot_at(p.x, p.y);
    if (i < 0 || cf->slot[i]->item != cf->base) return;
    cf->long_fired = true;
    cf->on_hold(cf->base);
}

static void touch_cb(lv_event_t * e) {
    cover_flow_t * cf = cf_instance;
    if (!cf) return;
    lv_point_t p = { 0, 0 };
    lv_indev_t * indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED: on_pressed(cf, p); break;
    case LV_EVENT_PRESSING: on_pressing(cf, p); break;
    case LV_EVENT_LONG_PRESSED: on_long_press(cf, p); break;
    case LV_EVENT_RELEASED: on_released(cf, p, false); break;
    case LV_EVENT_PRESS_LOST: on_released(cf, p, true); break;
    default: break;
    }
}

void cf_input_attach(lv_obj_t * touch) {
    lv_obj_add_event_cb(touch, touch_cb, LV_EVENT_ALL, NULL);
}
