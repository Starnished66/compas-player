/* Album scrub track. It shares the covers' fractional position and renderer. */
#include "cover_flow_internal.h"
#include "cover_flow_cap.h"
#include "../../appearance/gui_theme.h"
#include "../../library/library_az_table.h"
#include "../../shell/gui_shell.h"
#include "../builders/screen_builders.h"
#include <stdlib.h>

/* The cap is drawn at its design size and stretched to this width. */
#define GRIP_W BOARD_SCALE_PX(72)
#define GRIP_H BOARD_SCALE_PX(COVER_FLOW_CAP_HEIGHT)
#define TRACK_H BOARD_SCALE_PX(8)
#define TRACK_INSET BOARD_SCALE_PX(16)
#define FADE_MS 150
#define HIDE_DELAY_MS 1500
#if defined(BOARD_TEMPOTEC_V1) || defined(BOARD_R3II_2025)
#define BUBBLE_FONT (&lv_font_montserrat_20)
#else
#define BUBBLE_FONT (&lv_font_montserrat_28)
#endif

int32_t cf_az_height(void) { return BOARD_SCALE_PX(58); }
int32_t cf_az_track_center(void) { return (cf_az_height() - GRIP_H) / 2 + BOARD_SCALE_PX(17); }

static lv_obj_t * rect(lv_obj_t * parent, int w, int h, lv_color_t color, int radius) {
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

void cf_az_update(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    if (!az->touch) return;
    int32_t x = cover_flow_scrub_x(cf->pos, az->left, az->width, cf->count);
    lv_obj_set_x(az->grip, x - GRIP_W / 2);
    lv_obj_set_width(az->fill, x - TRACK_INSET);
    lv_obj_set_x(az->bubble, x - BOARD_SCALE_PX(24));
    if (!az->scrubbing || cf->count <= 0) return;
    int letter = library_az_letter(cf->items[cf->base].title);
    if (letter == az->shown) return;
    az->shown = letter;
    az->text[0] = letter < 26 ? (char) ('A' + letter) : '#';
    lv_label_set_text_static(az->bubble_label, az->text);
    lv_obj_center(az->bubble_label);
}

static void set_opa(void * obj, int32_t opa) { lv_obj_set_style_opa(obj, (lv_opa_t) opa, 0); }

/* The track stays hidden until touched, then fades out a moment after release. */
static void fade(cf_az_t * az, bool in) {
    lv_anim_delete(az->touch, set_opa);
    az->visible = in;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, az->touch);
    lv_anim_set_exec_cb(&a, set_opa);
    lv_anim_set_values(&a, lv_obj_get_style_opa(az->touch, 0), in ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_MS);
    if (!in) lv_anim_set_delay(&a, HIDE_DELAY_MS);
    lv_anim_start(&a);
}

static void cancel_scrub(cf_az_t * az) {
    az->scrubbing = false;
    az->pointer = NULL;
    az->shown = -1;
    if (az->poll) lv_timer_pause(az->poll);
    if (az->bubble) lv_obj_add_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
}

/* Covers swiped: show the track while they move, hide it once they settle. */
void cf_az_show(cover_flow_t * cf, bool show) {
    if (cf->az.touch && !cf->az.scrubbing) fade(&cf->az, show);
}

/* Leaving the screen: stop now and come back to a hidden track. */
void cf_az_cancel(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    cancel_scrub(az);
    if (!az->touch) return;
    lv_anim_delete(az->touch, set_opa);
    lv_obj_set_style_opa(az->touch, LV_OPA_TRANSP, 0);
    az->visible = false;
}

void cf_az_content(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    cancel_scrub(az);
    cf_az_update(cf);
}

static void end_scrub(cover_flow_t * cf, bool lost) {
    cf_az_t * az = &cf->az;
    if (!az->scrubbing) return;
    int target = (cf->pos + CF_ONE / 2) / CF_ONE;
    /* A tap on the hidden track only reveals it; a tap on the shown one jumps. */
    if (!az->moved && !lost && !az->tap_reveals)
        target = (cover_flow_scrub_pos(az->start_x - az->offset, az->left, az->width, cf->count) + CF_ONE / 2) / CF_ONE;
    cancel_scrub(az);
    fade(az, false);
    cf_scroll_to(target);
    cf_pump_kick();
}

static void scrub_at(cover_flow_t * cf, int32_t x) {
    cf_az_t * az = &cf->az;
    if (!az->moved && abs(x - az->start_x) < BOARD_SCALE_PX(3)) return;
    az->moved = true;
    int32_t pos = cover_flow_scrub_pos(x - az->offset, az->left, az->width, cf->count);
    if (pos == cf->pos) return;
    cf->pos = pos;
    cf_layout_update();
}

static void poll_cb(lv_timer_t * timer) {
    cover_flow_t * cf = lv_timer_get_user_data(timer);
    cf_az_t * az = &cf->az;
    if (!az->scrubbing || !az->pointer) return;
    lv_point_t p;
    lv_indev_get_point(az->pointer, &p);
    scrub_at(cf, p.x);
    if (lv_indev_get_state(az->pointer) == LV_INDEV_STATE_RELEASED) end_scrub(cf, false);
}

static void start_scrub(cover_flow_t * cf, lv_indev_t * pointer, int32_t x) {
    cf_az_t * az = &cf->az;
    cf_input_stop();
    cover_flow_loader_drop(&cf->loader);
    int32_t handle = cover_flow_scrub_x(cf->pos, az->left, az->width, cf->count);
    az->offset = abs(x - handle) <= GRIP_W / 2 ? x - handle : 0;
    az->start_x = x;
    az->moved = false;
    az->pointer = pointer;
    az->scrubbing = true;
    az->tap_reveals = !az->visible;
    fade(az, true);
    az->shown = -1;
    cf_az_update(cf);
    lv_obj_remove_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(az->poll);
}

static void strip_cb(lv_event_t * e) {
    cover_flow_t * cf = cf_instance;
    if (!cf || !cf->az_on || cf->count <= 0) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (code == LV_EVENT_RELEASED && cf->az.pointer) {
            lv_point_t p;
            lv_indev_get_point(cf->az.pointer, &p);
            scrub_at(cf, p.x);
        }
        end_scrub(cf, code == LV_EVENT_PRESS_LOST);
        return;
    }
    lv_indev_t * pointer = lv_indev_active();
    if (!pointer) return;
    lv_point_t p;
    lv_indev_get_point(pointer, &p);
    if (code == LV_EVENT_PRESSED) start_scrub(cf, pointer, p.x);
    else if (code == LV_EVENT_PRESSING && cf->az.scrubbing) scrub_at(cf, p.x);
}

static void make_grip(cf_az_t * az, int y) {
    az->cap = lv_draw_buf_create(GRIP_W, GRIP_H, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!az->cap) {
        az->grip = rect(az->touch, GRIP_W, BOARD_SCALE_PX(28), lv_color_hex(0x26282d), BOARD_SCALE_PX(5));
        lv_obj_set_y(az->grip, y + BOARD_SCALE_PX(3));
        return;
    }
    az->grip = lv_image_create(az->touch);
    lv_obj_remove_flag(az->grip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    cover_flow_cap_render(az->cap->data, GRIP_W, GRIP_H, az->cap->header.stride);
    lv_image_set_src(az->grip, az->cap);
    lv_obj_set_y(az->grip, y);
}

static void make_track(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    int32_t y = (cf->g.strip_h - GRIP_H) / 2;
    int32_t track_y = cf_az_track_center() - TRACK_H / 2;
    int inset = TRACK_INSET;
    az->left = inset + GRIP_W / 2;
    az->width = cf->g.screen_w - 2 * az->left;
    az->track = rect(az->touch, cf->g.screen_w - 2 * inset, TRACK_H, lv_color_hex(0x3a3e46), LV_RADIUS_CIRCLE);
    lv_obj_set_pos(az->track, inset, track_y);
    az->fill = rect(az->touch, 0, TRACK_H, lv_color_hex(0x848890), LV_RADIUS_CIRCLE);
    lv_obj_set_pos(az->fill, inset, track_y);
    make_grip(az, y);
}

static void build(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    az->touch = lv_obj_create(cf->screen);
    lv_obj_remove_style_all(az->touch);
    lv_obj_set_pos(az->touch, 0, cf->g.strip_y);
    lv_obj_set_size(az->touch, cf->g.screen_w, cf->g.strip_h);
    lv_obj_remove_flag(az->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(az->touch, LV_OBJ_FLAG_CLICKABLE);
    register_swipe_dead_zone(az->touch);
    lv_obj_add_event_cb(az->touch, strip_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_opa(az->touch, LV_OPA_TRANSP, 0);
    make_track(cf);
    az->bubble = rect(cf->screen, BOARD_SCALE_PX(48), BOARD_SCALE_PX(48), lv_color_hex(0x2c2f36), BOARD_SCALE_PX(14));
    lv_obj_set_style_border_width(az->bubble, BOARD_SCALE_PX(1), 0);
    lv_obj_set_style_border_color(az->bubble, lv_color_hex(0x969aa2), 0);
    lv_obj_set_y(az->bubble, cf->g.strip_y - BOARD_SCALE_PX(48));
    az->bubble_label = lv_label_create(az->bubble);
    lv_obj_add_style(az->bubble_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_color(az->bubble_label, lv_color_hex(0xf0f0f2), 0);
    lv_obj_set_style_text_font(az->bubble_label, BUBBLE_FONT, 0);
    lv_obj_add_flag(az->bubble, LV_OBJ_FLAG_HIDDEN);
    az->poll = lv_timer_create(poll_cb, CF_PUMP_MS, cf);
    lv_timer_pause(az->poll);
}

void cf_az_destroy(cover_flow_t * cf) {
    cf_az_t * az = &cf->az;
    if (!az->touch) return;
    cancel_scrub(az);
    lv_timer_delete(az->poll);
    unregister_swipe_dead_zone(az->touch);
    lv_obj_delete(az->touch);
    lv_obj_delete(az->bubble);
    if (az->cap) lv_draw_buf_destroy(az->cap);
    *az = (cf_az_t) { .shown = -1 };
}

void cf_az_sync(cover_flow_t * cf) {
    if (cf->az_on && !cf->az.touch) build(cf);
    else if (!cf->az_on) cf_az_destroy(cf);
}
