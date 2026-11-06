/* Cover flow screen: open and close, the header and labels, and freeing the
 * screen once it leaves navigation. Layout lives in cover_flow_layout.c, touch
 * in cover_flow_input.c, faces in cover_flow_faces.c. */
#include "cover_flow_internal.h"
#include "../../appearance/gui_theme.h"
#include "../../plugins/gui_plugins.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_shell.h"
#include "../builders/screen_builders.h"
#include "../../input/gesture_detector.h" /* HOME_SWIPE_HIT_EXTRA_PX */
#include "../../../core/text/utf8_util.h"
#include <stdlib.h>
#include <string.h>

cover_flow_t * cf_instance;

/* ---- Items ---- */

static size_t text_cap(const char * s) {
    return (s ? strnlen(s, CF_TEXT_MAX - 1) : 0) + 1;
}

static char * put_text(char ** at, const char * s) {
    char * out = *at;
    size_t cap = text_cap(s);
    utf8_truncate_safe(out, s, cap);
    utf8_sanitize(out);
    *at += cap;
    return out;
}

/* Refs are opaque ids: copied as they are, up to COVER_FLOW_REF_MAX. */
static size_t ref_cap(const char * s) {
    return (s ? strnlen(s, COVER_FLOW_REF_MAX - 1) : 0) + 1;
}

static char * put_ref(char ** at, const char * s) {
    char * out = *at;
    size_t cap = ref_cap(s);
    if (s) memcpy(out, s, cap - 1);
    out[cap - 1] = '\0';
    *at += cap;
    return out;
}

/* One block: the item array, then every string. */
static bool copy_items(cover_flow_t * cf, const cover_flow_params_t * p) {
    int n = p->count > COVER_FLOW_MAX_ITEMS ? COVER_FLOW_MAX_ITEMS : p->count;
    size_t chars = 0;
    for (int i = 0; i < n; i++)
        chars += text_cap(p->items[i].title) + text_cap(p->items[i].subtitle) + ref_cap(p->items[i].ref) +
                 ref_cap(p->items[i].cover_ref);
    uint8_t * block = malloc((size_t) n * sizeof(cover_flow_item_t) + chars);
    if (!block) return false;
    cover_flow_item_t * items = (cover_flow_item_t *) block;
    char * at = (char *) (items + n);
    for (int i = 0; i < n; i++) {
        items[i].title = put_text(&at, p->items[i].title);
        items[i].subtitle = put_text(&at, p->items[i].subtitle);
        items[i].song_id = p->items[i].song_id;
        items[i].ref = put_ref(&at, p->items[i].ref);
        items[i].cover_ref = put_ref(&at, p->items[i].cover_ref);
    }
    free(cf->items_block);
    cf->items_block = block;
    cf->items = items;
    cf->count = n;
    return true;
}

/* ---- Screen ---- */

static void compute_geometry(cover_flow_t * cf) {
    lv_display_t * disp = lv_display_get_default();
    cf_geometry_t * g = &cf->g;
    g->screen_w = lv_display_get_horizontal_resolution(disp);
    g->cx = g->screen_w / 2;
    g->area_top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t floor_y = lv_display_get_vertical_resolution(disp) - HOME_INDICATOR_CONTENT_INSET;
    g->strip_h = cf->az_on ? cf_az_height() : 0;
    g->strip_y = floor_y - g->strip_h;
    g->area_bottom = g->strip_y; /* covers and labels sit above the A-Z strip */
    int32_t labels = row_label_bounded_height(gui_theme_font(GUI_FONT_ROLE_ROW)) +
                     row_label_bounded_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    int32_t row_h = cf->warps.tilt->out_h; /* cover and reflection */
    int32_t block = row_h + BOARD_SCALE_PX(8) + labels;
    int32_t spare = g->area_bottom - g->area_top - block;
    g->cover_y = g->area_top + (spare > 0 ? spare / 2 : 0);
    g->label_y = g->cover_y + row_h + BOARD_SCALE_PX(8);
    if (!cf->az_on) return;
    /* The track sits halfway between the subtitle's text (its label box keeps
     * a margin below it) and the home indicator pill, centred in its band. */
    int32_t pill_top = lv_display_get_vertical_resolution(disp) -
                       (HOME_INDICATOR_BAND_HEIGHT + HOME_SWIPE_HIT_EXTRA_PX) / 2 - HOME_INDICATOR_PILL_HEIGHT / 2;
    int32_t text_bottom = g->label_y + labels - row_label_descender_margin(gui_theme_font(GUI_FONT_ROLE_SUBTEXT));
    g->strip_y = (text_bottom + pill_top) / 2 - cf_az_track_center();
    g->area_bottom = g->strip_y;
}

/* Where the carousel's touch area and the labels go for the current geometry. */
/* Moves the carousel and labels to the geometry in cf->g. */
static void cf_place_chrome(cover_flow_t * cf) {
    lv_obj_set_pos(cf->touch, CF_EDGE_PX, cf->g.area_top);
    lv_obj_set_size(cf->touch, cf->g.screen_w - CF_EDGE_PX, cf->g.area_bottom - cf->g.area_top);
    lv_obj_set_y(cf->title_label, cf->g.label_y);
    lv_obj_set_y(cf->sub_label, cf->g.label_y + row_label_bounded_height(gui_theme_font(GUI_FONT_ROLE_ROW)));
}

static lv_obj_t * make_label(cover_flow_t * cf, lv_style_t * color, gui_font_role_t role) {
    lv_obj_t * label = lv_label_create(cf->screen);
    lv_obj_add_style(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    lv_label_set_text(label, "");
    configure_scrolling_row_label(label, cf->g.screen_w - 2 * GUI_TEXT_INSET);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0); /* centred while it fits */
    lv_obj_set_x(label, GUI_TEXT_INSET);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

void cf_update_labels(void) {
    cover_flow_t * cf = cf_instance;
    if (!cf || cf->label_item == cf->base || cf->base < 0 || cf->base >= cf->count) return;
    cf->label_item = cf->base;
    lv_label_set_text(cf->title_label, cf->items[cf->base].title);
    lv_label_set_text(cf->sub_label, cf->items[cf->base].subtitle);
}

/* One flat colour from the theme's screen background, no image or gradient: the
 * covers carry that colour in their pixels and are drawn as plain copies. */
static void solid_background(cover_flow_t * cf) {
    lv_style_value_t v;
    lv_color_t color = lv_color_black();
    if (lv_style_get_prop(&style_theme_screen_bg, LV_STYLE_BG_COLOR, &v) == LV_STYLE_RES_FOUND) color = v.color;
    cf->warps.bg = lv_color_to_u16(color);
    lv_obj_set_style_bg_color(cf->screen, color, 0);
    lv_obj_set_style_bg_opa(cf->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(cf->screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_image_src(cf->screen, NULL, 0);
}

static void build_screen(cover_flow_t * cf, const char * title) {
    cf->screen = lv_obj_create(NULL);
    lv_obj_add_style(cf->screen, &style_theme_screen_bg, 0);
    solid_background(cf);
    cf->header_title = build_screen_header(cf->screen, title ? title : "", generic_back_cb, NULL, NULL);
    compute_geometry(cf);

    /* Everything under the header except a strip at the left edge, which stays with the
     * shell's back swipe. The shell skips its own swipes for a press that starts in a
     * registered dead zone, so this is also what lets the carousel take horizontal drags. */
    cf->touch = lv_obj_create(cf->screen);
    lv_obj_remove_style_all(cf->touch);
    lv_obj_remove_flag(cf->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cf->touch, LV_OBJ_FLAG_CLICKABLE);
    register_swipe_dead_zone(cf->touch);
    cf_input_attach(cf->touch);

    cf->title_label = make_label(cf, &style_theme_text_primary, GUI_FONT_ROLE_ROW);
    cf->sub_label = make_label(cf, &style_theme_text_muted, GUI_FONT_ROLE_SUBTEXT);
    cf_place_chrome(cf);
}

/* ---- Lifecycle ---- */

static void destroy_async(void * arg);

static void destroy(cover_flow_t * cf) {
    if (cf->pump) lv_timer_delete(cf->pump);
    cf_input_stop();
    cf_faces_destroy(cf); /* drops the pending decode before anything it feeds goes away */
    cf_slots_destroy(cf);
    cf_az_destroy(cf);
    if (cf->touch) unregister_swipe_dead_zone(cf->touch);
    if (cf->screen) {
        gui_navigation_remove_screen_instances(&cf->screen, 1);
        lv_obj_delete(cf->screen);
    }
    cover_flow_tilt_destroy(cf->warps.tilt);
    free(cf->items_block);
    if (cf_instance == cf) cf_instance = NULL;
    /* Last: removing the screen from navigation above can queue another destroy of this instance. */
    lv_async_call_cancel(destroy_async, cf);
    free(cf);
}

static void destroy_async(void * arg) {
    destroy(arg);
}

/* A covered screen that navigation dropped is already unloaded, so no further unload event
 * comes: the navigation calls this when it removes entries. Freeing is left to the next loop
 * pass, since the screen cannot be deleted inside its own event. */
void cover_flow_note_nav_removed(void) {
    cover_flow_t * cf = cf_instance;
    if (cf && lv_screen_active() != cf->screen && !gui_navigation_contains(cf->screen))
        lv_async_call(destroy_async, cf);
}

static void unloaded_cb(lv_event_t * e) {
    cf_az_cancel(lv_event_get_user_data(e));
    cover_flow_note_nav_removed();
}

/* Takes the caller's callbacks and starts at the selected item. The items are copied and the slots exist. */
static void apply_params(cover_flow_t * cf, const cover_flow_params_t * p) {
    int sel = p->selected < 0 ? 0 : p->selected >= cf->count ? cf->count - 1 : p->selected;
    cf->on_select = p->on_select;
    cf->on_hold = p->on_hold;
    cf->face = p->face;
    cf->label_item = -1;
    cf->pos = sel * CF_ONE;
    cf_slots_reset(cf, sel);
    cf_layout_update();
    cf_update_labels();
}

/* Exercise deferred text sizing and marquee setup before the screen opens.
 * Scratch labels leave the selected item's text and animation untouched. */
static void warm_labels(cover_flow_t * cf) {
    const char * title = "", * subtitle = "";
    size_t title_len = 0, subtitle_len = 0;
    for (int i = 0; i < cf->count; i++) {
        size_t n = strlen(cf->items[i].title);
        if (n > title_len) { title = cf->items[i].title; title_len = n; }
        n = strlen(cf->items[i].subtitle);
        if (n > subtitle_len) { subtitle = cf->items[i].subtitle; subtitle_len = n; }
    }
    lv_obj_t * heading = make_label(cf, &style_theme_text_primary, GUI_FONT_ROLE_ROW);
    lv_obj_t * detail = make_label(cf, &style_theme_text_muted, GUI_FONT_ROLE_SUBTEXT);
    lv_label_set_text(heading, title);
    lv_label_set_text(detail, subtitle);
    lv_obj_update_layout(cf->screen);
    lv_obj_delete(heading);
    lv_obj_delete(detail);
}

static bool create(const cover_flow_params_t * p) {
    static const cover_flow_tilt_params_t tilt = { CF_FAR_WIDTH_PCT,  CF_SIDE_HEIGHT_PCT, CF_FAR_HEIGHT_PCT,
                                                   CF_FAR_SHADE_PCT,  CF_REFLECT_PCT,     CF_REFLECT_ALPHA_PCT };
    cover_flow_t * cf = calloc(1, sizeof(*cf));
    if (!cf) return false;
    cf_instance = cf;
    cf->warp_budget = CF_WARP_PX_PER_FRAME;
    cf->az_on = p->az_index;
    cf->az.shown = -1;
    cf->warps.tilt = cover_flow_tilt_create(CF_FACE_PX, &tilt);
    if (!cf->warps.tilt || !copy_items(cf, p)) goto fail;
    build_screen(cf, p->title);
    if (!cf_slots_create(cf)) goto fail; /* before the A-Z strip, so its bubble stacks above the covers */
    cf_az_sync(cf);
    cf_az_content(cf);
    if (!cf_faces_create(cf)) goto fail;
    apply_params(cf, p);
    warm_labels(cf);
    finalize_screen_navigation(cf->screen);
    cf->pump = lv_timer_create(cf_pump_cb, CF_PUMP_MS, cf);
    if (!cf->pump) goto fail;
    lv_obj_add_event_cb(cf->screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, cf);
    nav_push(cf->screen);
    return true;
fail:
    destroy(cf);
    return false;
}

/* A second show while open: new content on the same screen. */
static bool replace(cover_flow_t * cf, const cover_flow_params_t * p) {
    cover_flow_loader_drop(&cf->loader); /* the decode of an old item */
    cf_input_stop();
    if (!copy_items(cf, p)) return false;
    if (cf->az_on != p->az_index) { /* the strip comes or goes: the rows move */
        cf->az_on = p->az_index;
        compute_geometry(cf);
        cf_place_chrome(cf);
        cf_az_sync(cf);
    }
    cf_az_content(cf);
    lv_label_set_text(cf->header_title, p->title ? p->title : "");
    screen_title_fit(cf->header_title);
    apply_params(cf, p);
    warm_labels(cf);
    cf_pump_kick();
    return true;
}

bool cover_flow_show(const cover_flow_params_t * p) {
    if (!p || !p->items || p->count <= 0 || !p->on_select) return false;
    cover_flow_t * cf = cf_instance;
    if (cf && gui_navigation_contains(cf->screen)) return replace(cf, p);
    /* A push during a slide can be lost when the slide loads its destination. */
    if (gui_navigation_transition_in_progress()) return false;
    if (cf) destroy(cf); /* left the stack but not yet reaped */
    return create(p);
}

const cover_flow_item_t * cover_flow_item(int index) {
    return cf_instance && index >= 0 && index < cf_instance->count ? &cf_instance->items[index] : NULL;
}

void cover_flow_shutdown(void) {
    if (cf_instance) destroy(cf_instance);
}
