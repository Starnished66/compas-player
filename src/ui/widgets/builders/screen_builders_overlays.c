#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../../text/i18n/i18n.h"
#include "../../assets/assets.h"
#include "../../effects/transition_compositor.h"
#include "lvgl/src/core/lv_obj_event_private.h"
#include "lvgl/src/misc/lv_area_private.h"

#include <string.h>
#ifndef HOST_BUILD
#include "lvgl/src/drivers/display/fb/lv_linux_fbdev.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/core/lv_refr_private.h"
#endif

/* Share one frozen, dimmed page so opaque popups can skip rendering the screen below. */
static lv_draw_buf_t * popup_backdrop_buf;
static unsigned popup_backdrop_images;
static char popup_backdrop_marker;

/* Keep the pair independent of callers' object user data. */
static void popup_pair_delete_cb(lv_event_t * e) {
    lv_obj_t * peer = lv_event_get_user_data(e);
    lv_obj_remove_event_cb_with_user_data(peer, popup_pair_delete_cb, lv_event_get_target(e));
}

static lv_obj_t * popup_backdrop_card(lv_obj_t * backdrop) {
    for (uint32_t i = 0; i < lv_obj_get_event_count(backdrop); ++i) {
        lv_event_dsc_t * dsc = lv_obj_get_event_dsc(backdrop, i);
        if (lv_event_dsc_get_cb(dsc) == popup_pair_delete_cb)
            return lv_event_dsc_get_user_data(dsc);
    }
    return NULL;
}

static bool popup_visible_except(lv_obj_t * skip) {
    lv_obj_t * top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        lv_obj_t * other = lv_obj_get_child(top, i);
        lv_obj_t * image = lv_obj_get_child(other, 0);
        if (other != skip && image && lv_obj_get_user_data(image) == &popup_backdrop_marker &&
            !lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

bool gui_popup_backdrop_visible(void) {
    return popup_visible_except(NULL);
}

/* Closes the top popup the way tapping outside it does (the back swipe). */
bool gui_popup_dismiss_top(void) {
    lv_obj_t * top = lv_layer_top();
    for (int32_t i = (int32_t)lv_obj_get_child_count(top) - 1; i >= 0; --i) {
        lv_obj_t * backdrop = lv_obj_get_child(top, i);
        lv_obj_t * image = lv_obj_get_child(backdrop, 0);
        if (!image || lv_obj_get_user_data(image) != &popup_backdrop_marker ||
            lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_obj_send_event(backdrop, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}

#ifndef HOST_BUILD
static lv_draw_buf_t * popup_restore_buf;
static lv_obj_t * popup_restore_owner;
static lv_obj_t * popup_restore_screen;
static lv_display_t * popup_restore_display;
static lv_display_rotation_t popup_restore_rotation;
static lv_area_t popup_dirty;
static bool popup_dirty_valid;
static bool popup_collect_overlays;

static void popup_restore_reset(void) {
    popup_restore_owner = NULL;
    lv_refr_set_invalidation_cb(NULL);
}

static void popup_track_dirty(lv_display_t * display, lv_obj_t * obj, const lv_area_t * area) {
    if (!popup_restore_owner || display != popup_restore_display) return;
    if (!area) { popup_restore_reset(); return; }
    if (obj && !popup_collect_overlays) {
        lv_obj_t * top = lv_display_get_layer_top(display);
        lv_obj_t * root = obj;
        while (lv_obj_get_parent(root) && lv_obj_get_parent(root) != top)
            root = lv_obj_get_parent(root);
        if (lv_obj_get_parent(root) == top &&
            (root == popup_backdrop_card(popup_restore_owner) ||
             lv_obj_get_index(root) >= lv_obj_get_index(popup_restore_owner))) return;
    }
    if (popup_dirty_valid) lv_area_join(&popup_dirty, &popup_dirty, area);
    else { popup_dirty = *area; popup_dirty_valid = true; }
}

/* Include descendants with overflow, shadows and transforms using LVGL's own bounds. */
static void popup_invalidate_tree(lv_obj_t * obj) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_invalidate(obj);
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        popup_invalidate_tree(lv_obj_get_child(obj, i));
}

static void popup_invalidate_overlays(lv_display_t * display) {
    lv_obj_t * layers[] = {lv_display_get_layer_top(display), lv_display_get_layer_sys(display)};
    for (unsigned n = 0; n < 2; ++n)
        for (uint32_t i = 0; i < lv_obj_get_child_count(layers[n]); ++i)
            popup_invalidate_tree(lv_obj_get_child(layers[n], i));
}
#endif

static bool popup_backdrop_capture(lv_obj_t * backdrop) {
#ifdef HOST_BUILD
    (void)backdrop;
    return false;
#else
    popup_restore_reset();
    lv_display_t * display = lv_obj_get_display(backdrop);
    if (!display || transition_compositor_is_active() || lv_linux_fbdev_ram_stage_active(display) ||
        lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565) return false;
    if (popup_visible_except(backdrop) || lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_0 ||
        !lv_display_is_invalidation_enabled(display) || display->rendering_in_progress || display->flushing)
        return false;
    /* Flush recent hides and screen changes before copying the scanned page. */
    lv_refr_now(display);
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    const uint8_t * page = lv_linux_fbdev_get_active_page(display);
    uint32_t stride = lv_linux_fbdev_get_stride(display);
    if (!page || !stride || width <= 0 || height <= 0 || stride < (uint32_t) width * 2U) return false;
    if (!popup_backdrop_buf || popup_backdrop_buf->header.w != (uint32_t) width ||
        popup_backdrop_buf->header.h != (uint32_t) height) {
        if (popup_backdrop_buf && popup_backdrop_images) return false;
        if (popup_backdrop_buf) lv_draw_buf_destroy(popup_backdrop_buf);
        if (popup_restore_buf) lv_draw_buf_destroy(popup_restore_buf);
        popup_restore_buf = NULL;
        popup_backdrop_buf = lv_draw_buf_create((uint32_t) width, (uint32_t) height,
                                                LV_COLOR_FORMAT_RGB565, 0);
    }
    if (!popup_backdrop_buf || !popup_backdrop_buf->data) return false;
    if (!popup_restore_buf) popup_restore_buf = lv_draw_buf_create((uint32_t) width, (uint32_t) height,
                                                                  LV_COLOR_FORMAT_RGB565, 0);
    if (!popup_restore_buf || !popup_restore_buf->data) return false;
    for (int32_t y = 0; y < height; ++y) {
        const uint16_t * src = (const uint16_t *)(page + (size_t) y * stride);
        uint16_t * dst = (uint16_t *)(popup_backdrop_buf->data + (size_t) y * popup_backdrop_buf->header.stride);
        memcpy(popup_restore_buf->data + (size_t)y * popup_restore_buf->header.stride, src, (size_t)width * 2U);
        /* Halve each RGB565 channel without leaking bits into its neighbour. */
        for (int32_t x = 0; x < width; ++x) dst[x] = (uint16_t)((src[x] >> 1) & 0x7BEF);
    }
    popup_restore_owner = backdrop;
    popup_restore_screen = lv_display_get_screen_active(display);
    popup_restore_display = display;
    popup_restore_rotation = lv_display_get_rotation(display);
    popup_dirty_valid = false;
    lv_refr_set_invalidation_cb(popup_track_dirty);
    /* Captured overlays may disappear or move while the popup is open. */
    popup_collect_overlays = true;
    popup_invalidate_overlays(display);
    popup_collect_overlays = false;
    return true;
#endif
}

static void popup_backdrop_image_delete_cb(lv_event_t * e) {
#ifndef HOST_BUILD
    if (lv_obj_get_parent(lv_event_get_target(e)) == popup_restore_owner) popup_restore_reset();
#else
    (void)e;
#endif
    if (popup_backdrop_images) popup_backdrop_images--;
    /* Hidden reusable popups still own image references to this buffer. */
    if (!popup_backdrop_images && popup_backdrop_buf) {
        lv_draw_buf_destroy(popup_backdrop_buf);
        popup_backdrop_buf = NULL;
#ifndef HOST_BUILD
        if (popup_restore_buf) lv_draw_buf_destroy(popup_restore_buf);
        popup_restore_buf = NULL;
        popup_restore_reset();
#endif
    }
}

/* lv_image never claims to cover (its own background is transparent), so
 * without this the screen under the opaque snapshot is still redrawn. */
static void popup_backdrop_cover_cb(lv_event_t * e) {
    lv_cover_check_info_t * info = lv_event_get_param(e);
    lv_obj_t * image = lv_event_get_target(e);
    lv_area_t coords;
    lv_obj_get_coords(image, &coords);
    if (info->res != LV_COVER_RES_MASKED && lv_image_get_src(image) &&
        lv_area_is_in(info->area, &coords, 0)) info->res = LV_COVER_RES_COVER;
}

static void popup_backdrop_refresh(lv_obj_t * backdrop) {
    if (!backdrop) return;
    lv_obj_t * image = lv_obj_get_child(backdrop, 0);
    if (!image || lv_obj_get_user_data(image) != &popup_backdrop_marker) return;
    if (!popup_backdrop_capture(backdrop)) {
        /* The translucent parent remains the fallback when capture is unavailable. */
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_image_set_src(image, popup_backdrop_buf);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(image);
}

void gui_popup_backdrop_show(lv_obj_t * backdrop) {
    if (!backdrop) return;
    /* Reordering a hidden pair changes no pixels; avoid a full top-layer redraw. */
    lv_obj_t * card = popup_backdrop_card(backdrop);
    lv_display_t * display = lv_obj_get_display(backdrop);
    bool hidden = lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN) &&
                  (!card || lv_obj_has_flag(card, LV_OBJ_FLAG_HIDDEN));
    if (hidden) lv_display_enable_invalidation(display, false);
    lv_obj_move_foreground(backdrop);
    if (card) lv_obj_move_foreground(card);
    if (hidden) lv_display_enable_invalidation(display, true);
    if (lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN)) popup_backdrop_refresh(backdrop);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
}

#ifndef HOST_BUILD
static void popup_restore_copy(lv_display_t * display) {
    uint8_t * page = lv_linux_fbdev_get_inactive_page(display);
    uint32_t stride = lv_linux_fbdev_get_stride(display);
    for (uint32_t y = 0; y < popup_restore_buf->header.h; ++y)
        memcpy(page + (size_t)y * stride,
               popup_restore_buf->data + (size_t)y * popup_restore_buf->header.stride,
               (size_t)popup_restore_buf->header.w * 2U);
}

static uint8_t * popup_follow_page;
static bool popup_follow_registered;

/* Once the restored page is on screen, the other page still shows the popup:
 * copy the restore there too, before LVGL syncs the last frame's areas into it.
 * Skipped when another page loaded meanwhile (its first frame is a full redraw). */
static void popup_restore_follow_cb(lv_event_t * e) {
    lv_display_t * display = lv_event_get_current_target(e);
    if (!popup_follow_page || lv_linux_fbdev_get_inactive_page(display) == popup_follow_page) return;
    popup_follow_page = NULL;
    if (popup_restore_buf && lv_display_get_screen_active(display) == popup_restore_screen &&
        !transition_compositor_is_active() && !lv_linux_fbdev_ram_stage_active(display) &&
        display->buf_act && display->buf_act->data == lv_linux_fbdev_get_inactive_page(display))
        popup_restore_copy(display);
}

static bool popup_backdrop_restore(lv_obj_t * backdrop, lv_obj_t * card) {
    lv_display_t * display = lv_obj_get_display(backdrop);
    int32_t width = lv_display_get_horizontal_resolution(display);
    int32_t height = lv_display_get_vertical_resolution(display);
    if (popup_restore_owner != backdrop || !popup_restore_buf || !card ||
        popup_visible_except(backdrop) || display != popup_restore_display ||
        lv_display_get_screen_active(display) != popup_restore_screen ||
        lv_display_get_rotation(display) != popup_restore_rotation ||
        width != (int32_t)popup_restore_buf->header.w || height != (int32_t)popup_restore_buf->header.h ||
        lv_display_get_color_format(display) != LV_COLOR_FORMAT_RGB565 ||
        transition_compositor_is_active() || lv_linux_fbdev_ram_stage_active(display) ||
        !lv_linux_fbdev_get_active_page(display) || !lv_linux_fbdev_get_inactive_page(display) ||
        lv_linux_fbdev_get_stride(display) < (uint32_t)width * 2U ||
        !lv_display_is_invalidation_enabled(display) || display->rendering_in_progress || display->flushing ||
        (popup_dirty_valid && (uint64_t)lv_area_get_size(&popup_dirty) * 4 >= (uint64_t)width * height * 3))
        return false;

    uint8_t * page = lv_linux_fbdev_get_inactive_page(display);
    if (!display->buf_act || display->buf_act->data != page) return false;

    /* Write only the hidden page and let LVGL's next frame show it. A page
     * opened by the same tap then replaces the popup directly instead of
     * Now Playing flashing in between. */
    popup_restore_reset();
    lv_refr_scroll_blit_reset();
    lv_display_enable_invalidation(display, false);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_display_enable_invalidation(display, true);
    popup_restore_copy(display);
    /* Areas drawn while the popup was up would copy popup pixels back over the restore. */
    lv_ll_clear(&display->sync_areas);
    if (!popup_follow_registered) {
        lv_display_add_event_cb(display, popup_restore_follow_cb, LV_EVENT_REFR_READY, NULL);
        popup_follow_registered = true;
    }
    popup_follow_page = page;
    if (popup_dirty_valid) lv_inv_area(display, &popup_dirty);
    popup_invalidate_overlays(display);
    lv_area_t pixel = {0, 0, 0, 0};
    lv_inv_area(display, &pixel); /* always present the restored page, even with nothing else queued */
    return true;
}
#endif

void gui_popup_backdrop_hide(lv_obj_t * backdrop) {
    if (!backdrop) return;
    lv_obj_t * card = popup_backdrop_card(backdrop);
#ifndef HOST_BUILD
    if (!lv_obj_has_flag(backdrop, LV_OBJ_FLAG_HIDDEN) && popup_backdrop_restore(backdrop, card)) return;
    if (popup_restore_owner == backdrop) popup_restore_reset();
#endif
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
}

/* Overlays: popup surfaces, confirmation and titled popups, popup visibility
 * tracking, the Power Hub panel and the font-change mask. */

/* Shared 2-button confirmation popup builder (backdrop, card, wrapped title,
 * and confirm/cancel buttons) using LV_SIZE_CONTENT and flex layout to
 * accommodate varying font sizes. */
lv_obj_t * build_popup_surface(lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    lv_obj_t * top = lv_layer_top();
    lv_obj_t * backdrop = lv_obj_create(top);
    lv_obj_set_size(backdrop, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_50, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * image = lv_image_create(backdrop);
    lv_obj_set_pos(image, 0, 0);
    lv_obj_set_size(image, lv_pct(100), lv_pct(100));
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(image, &popup_backdrop_marker);
    lv_obj_add_event_cb(image, popup_backdrop_image_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(image, popup_backdrop_cover_cb, LV_EVENT_COVER_CHECK, NULL);
    popup_backdrop_images++;
    if (backdrop_cb) lv_obj_add_event_cb(backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * popup = lv_obj_create(top);
    lv_obj_set_width(popup, lv_pct(92));
    lv_obj_set_height(popup, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(popup,
        lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * STATUS_BAR_CLEARANCE, 0);
    lv_obj_align(popup, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(popup, LIST_ROW_RADIUS, 0);
    /* The card's Breeze frame (its border color follows the theme). */
    lv_obj_add_style(popup, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, ICON_GRID_TILE_BORDER, 0);
    lv_obj_set_style_pad_all(popup, BOARD_SCALE_PX(12), 0);
    ui_marquee_list_enable(popup);
    lv_obj_set_style_pad_row(popup, GUI_ROW_GAP, 0);
    lv_obj_set_scroll_dir(popup, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(backdrop, popup_pair_delete_cb, LV_EVENT_DELETE, popup);
    lv_obj_add_event_cb(popup, popup_pair_delete_cb, LV_EVENT_DELETE, backdrop);
    *out_backdrop = backdrop;
    return popup;
}

void style_popup_action_row(lv_obj_t * row) {
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, BOARD_SCALE_PX(88), 0);
    lv_obj_set_style_pad_all(row, BOARD_SCALE_PX(14), 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_set_style_radius(row, BOARD_SCALE_PX(12), 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    /* Flex gives wrapped labels a real content height and centers short
     * labels without reducing the surrounding touch target. */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

static bool popup_text_is_cancel(const char * text) {
    return text && (strcmp(text, "Cancel") == 0 || strcmp(text, TR("Cancel")) == 0);
}

/* A full-width, centred popup text label. */
static lv_obj_t * popup_label(lv_obj_t * parent, const char * text, lv_label_long_mode_t long_mode,
                              lv_style_t * color_style, gui_font_role_t font_role) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, long_mode);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(label, color_style, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(font_role), 0);
    return label;
}

lv_obj_t * add_popup_action_row(lv_obj_t * popup, const char * text, lv_color_t color,
                                lv_event_cb_t cb, void * user_data) {
    lv_obj_t * row = lv_obj_create(popup);
    style_popup_action_row(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t * label = lv_label_create(row);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    if (popup_text_is_cancel(text)) lv_obj_add_style(label, &style_theme_text_primary, 0);
    else lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void build_titled_popup(gui_popup_t * popup, lv_event_cb_t backdrop_cb, lv_obj_t ** title_out,
                        lv_obj_t ** body_out) {
    popup->popup = build_popup_surface(backdrop_cb, &popup->backdrop);

    lv_obj_t * title = popup_label(popup->popup, "", LV_LABEL_LONG_WRAP, &style_theme_text_primary,
                                   GUI_FONT_ROLE_ROW);
    if (title_out) *title_out = title;

    if (body_out) {
        /* A long description scrolls inside the popup instead of pushing the
         * buttons off a small screen. */
        lv_obj_t * scroll = lv_obj_create(popup->popup);
        lv_obj_remove_style_all(scroll);
        lv_obj_set_width(scroll, lv_pct(100));
        lv_obj_set_height(scroll, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(scroll, BOARD_SCALE_PX(180), 0);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
        *body_out = popup_label(scroll, "", LV_LABEL_LONG_WRAP, &style_theme_text_muted,
                                GUI_FONT_ROLE_SUBTEXT);
        lv_obj_remove_flag(popup->popup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(popup->popup, LV_DIR_NONE);
    }
}

lv_obj_t * build_confirm_popup_with_labels(const char * title_text, lv_label_long_mode_t title_long_mode,
                                           lv_obj_t ** out_title, const char * body_text, lv_obj_t ** out_body,
                                           const char * confirm_text, lv_obj_t ** out_confirm_label,
                                           lv_color_t confirm_color, lv_event_cb_t confirm_cb,
                                           lv_obj_t ** out_confirm_row, const char * cancel_text,
                                           lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                                           lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb,
                                           lv_obj_t ** out_backdrop) {
    lv_obj_t * popup = build_popup_surface(backdrop_cb, out_backdrop);
    lv_obj_set_style_pad_row(popup, BOARD_SCALE_PX(12), 0);

    lv_obj_t * title = popup_label(popup, title_text, title_long_mode, &style_theme_text_primary,
                                   GUI_FONT_ROLE_ROW);
    if (out_title) *out_title = title;

    if (body_text) {
        lv_obj_t * body = popup_label(popup, body_text, LV_LABEL_LONG_WRAP, &style_theme_text_muted,
                                      GUI_FONT_ROLE_SUBTEXT);
        if (out_body) *out_body = body;
    } else if (out_body) {
        *out_body = NULL;
    }

    lv_obj_t * confirm_row = add_popup_action_row(popup, confirm_text, confirm_color, confirm_cb, NULL);
    if (out_confirm_label) *out_confirm_label = lv_obj_get_child(confirm_row, 0);
    if (out_confirm_row) *out_confirm_row = confirm_row;

    lv_obj_t * cancel_row = add_popup_action_row(popup, cancel_text, cancel_color, cancel_cb, NULL);
    if (out_cancel_row) *out_cancel_row = cancel_row;

    return popup;
}

lv_obj_t * build_confirm_popup(const char * title_text, lv_label_long_mode_t title_long_mode,
                               lv_obj_t ** out_title, const char * body_text, const char * confirm_text,
                               lv_color_t confirm_color, lv_event_cb_t confirm_cb, lv_obj_t ** out_confirm_row,
                               const char * cancel_text, lv_color_t cancel_color, lv_event_cb_t cancel_cb,
                               lv_obj_t ** out_cancel_row, lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    return build_confirm_popup_with_labels(title_text, title_long_mode, out_title, body_text, NULL,
                                           confirm_text, NULL, confirm_color, confirm_cb, out_confirm_row,
                                           cancel_text, cancel_color, cancel_cb, out_cancel_row,
                                           backdrop_cb, out_backdrop);
}

static lv_obj_t * power_hub_card(lv_obj_t * parent, const char * title,
                                 const char * icon_path, const char * fallback,
                                 lv_event_cb_t cb, int index) {
    lv_obj_t * card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(48));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, BOARD_SCALE_PX(132), 0);
    lv_obj_set_style_radius(card, BOARD_SCALE_PX(16), 0);
    lv_obj_add_style(card, &pill_row_bg_style, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, BOARD_SCALE_PX(8), 0);
    lv_obj_add_style(card, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, BOARD_SCALE_PX(8), 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    char breeze[96];
    const char * source = asset_path(breeze_row_icon(icon_path, breeze, sizeof(breeze)) ? breeze : icon_path);
    if (source) {
        lv_obj_t * icon = lv_image_create(card);
        lv_image_set_src(icon, source);
        /* Fit installed assets of any size into the same icon box. */
        lv_obj_set_size(icon, BOARD_SCALE_PX(48), BOARD_SCALE_PX(48));
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
        lv_obj_add_style(icon, gui_theme_accent_style(), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_t * icon = lv_label_create(card);
        lv_label_set_text(icon, fallback);
        lv_obj_set_style_text_font(icon, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
        lv_obj_add_style(icon, gui_theme_accent_style(), 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t * label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t * build_power_hub_panel(lv_obj_t * parent, lv_event_cb_t action_cb,
                                 lv_obj_t ** out_status, lv_obj_t ** out_actions) {
    static const char * titles[] = {N_("Power off"), N_("Restart"), N_("Screen off"), N_("Sleep timer")};
    static const char * icons[] = {"power_action/power.png", "power_action/reboot.png",
                                   "power_action/screen_off.png", "power_action/sleep_timer.png"};
    static const char * symbols[] = {LV_SYMBOL_POWER, LV_SYMBOL_REFRESH, LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_SETTINGS};
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_width(panel, lv_pct(100));
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel,
        lv_display_get_vertical_resolution(lv_display_get_default()), 0);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_style(panel, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_hor(panel, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_top(panel, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_bottom(panel, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_row(panel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * title = lv_label_create(panel);
    lv_label_set_text(title, TR("Power controls"));
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_style(title, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * subtitle = lv_label_create(panel);
    lv_label_set_text(subtitle, TR("Playback and device actions"));
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_add_style(subtitle, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(subtitle, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_remove_flag(subtitle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * grid = lv_obj_create(panel);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(grid, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_pad_column(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int row_index = 0; row_index < 2; ++row_index) {
        lv_obj_t * row = lv_obj_create(grid);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        for (int column = 0; column < 2; ++column) {
            int i = row_index * 2 + column;
            lv_obj_t * card = power_hub_card(row, TR(titles[i]), icons[i], symbols[i], action_cb, i);
            if (out_actions) out_actions[i] = card;
        }
    }

    lv_obj_t * cancel = lv_obj_create(panel);
    lv_obj_set_width(cancel, lv_pct(100));
    lv_obj_set_height(cancel, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(cancel, LV_MAX(44, BOARD_SCALE_PX(64)), 0);
    lv_obj_set_style_radius(cancel, BOARD_SCALE_PX(14), 0);
    lv_obj_add_style(cancel, &pill_row_bg_style, 0);
    lv_obj_add_style(cancel, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(cancel, BOARD_SCALE_PX(12), 0);
    lv_obj_set_flex_flow(cancel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cancel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cancel, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)4);
    lv_obj_t * cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, TR("Cancel"));
    lv_obj_set_width(cancel_label, lv_pct(100));
    lv_obj_set_style_text_align(cancel_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(cancel_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(cancel_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_remove_flag(cancel_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_actions) out_actions[4] = cancel;

    lv_obj_t * status = lv_label_create(panel);
    lv_label_set_text(status, "");
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_style(status, gui_theme_accent_style(), 0);
    lv_obj_set_style_text_font(status, gui_theme_font(GUI_FONT_ROLE_STATUS), 0);
    lv_obj_remove_flag(status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (out_status) *out_status = status;
    return panel;
}

static unsigned visible_popup_count;

void gui_popup_show(gui_popup_t * p) {
    if (!p) return;
    if (!p->visible && (p->popup || p->backdrop)) {
        p->visible = true;
        visible_popup_count++;
    }
    if (p->backdrop) gui_popup_backdrop_show(p->backdrop);
    if (p->popup) {
        lv_obj_remove_flag(p->popup, LV_OBJ_FLAG_HIDDEN);
        if (!p->backdrop) lv_obj_move_foreground(p->popup);
    }
}

void gui_popup_hide(gui_popup_t * p) {
    if (!p) return;
    if (p->visible) {
        p->visible = false;
        if (visible_popup_count > 0) visible_popup_count--;
    }
    if (p->backdrop) gui_popup_backdrop_hide(p->backdrop);
    if (p->popup) lv_obj_add_flag(p->popup, LV_OBJ_FLAG_HIDDEN);
}

void gui_popup_teardown(gui_popup_t * p) {
    if (!p) return;
    gui_popup_hide(p);
    if (p->popup) {
        lv_obj_delete(p->popup);
        p->popup = NULL;
    }
    if (p->backdrop) {
        lv_obj_delete(p->backdrop);
        p->backdrop = NULL;
    }
}

bool gui_popup_is_visible(const gui_popup_t * p) {
    return p && p->visible;
}

unsigned gui_popup_visible_count(void) {
    return visible_popup_count;
}

static void popup_confirm_hide_cb(lv_event_t * e) {
    gui_popup_hide(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

/* A yes/no question, built once into p. Cancel and the backdrop close it;
 * confirm_cb runs on the red action and hides p itself. */
void gui_popup_confirm(gui_popup_t * p, const char * title, const char * body,
                       const char * confirm_text, lv_event_cb_t confirm_cb) {
    if (!p->popup) {
        lv_obj_t * cancel_row;
        p->popup = build_confirm_popup(title, LV_LABEL_LONG_WRAP, NULL, body, confirm_text,
                                       lv_color_hex(GUI_BREEZE_NEGATIVE), confirm_cb, NULL, TR("Cancel"),
                                       accent_lv_color(), popup_confirm_hide_cb, &cancel_row,
                                       popup_confirm_hide_cb, &p->backdrop);
        lv_obj_set_user_data(cancel_row, p);
        lv_obj_set_user_data(p->backdrop, p);
    }
    gui_popup_show(p);
}

lv_obj_t * build_font_change_mask(int target) {
    lv_obj_t * mask = lv_obj_create(lv_layer_sys());
    lv_obj_set_user_data(mask, (void *)(intptr_t)(target + 1));
    lv_display_t * display = lv_display_get_default();
    lv_obj_set_size(mask,
                    lv_display_get_horizontal_resolution(display),
                    lv_display_get_vertical_resolution(display));
    lv_obj_align(mask, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_set_style_pad_all(mask, 0, 0);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(mask);
    lv_obj_invalidate(mask);
    return mask;
}

/* One transient option menu, owned by its setting row. */
typedef struct {
    lv_obj_t * row, * summary;
    char * options;
    unsigned selected;
    gui_option_selected_cb_t callback;
} option_menu_row_t;
static gui_popup_t option_popup;
static option_menu_row_t * option_owner;

bool gui_option_menu_close(void) {
    if (!option_owner) return false;
    option_owner = NULL;
    gui_popup_teardown(&option_popup);
    return true;
}
static void option_backdrop(lv_event_t * e) {
    (void)e;
    gui_option_menu_close();
}
static const char * option_text(option_menu_row_t * ctx, unsigned index, size_t * len) {
    const char * start = ctx->options;
    while (index-- && *start) {
        const char * next = strchr(start, '\n');
        if (!next) break;
        start = next + 1;
    }
    const char * end = strchr(start, '\n');
    *len = end ? (size_t)(end - start) : strlen(start);
    return start;
}
void gui_option_menu_set_selected(lv_obj_t * row, unsigned selected) {
    if (!row) return;
    option_menu_row_t * ctx = lv_obj_get_user_data(row);
    ctx->selected = selected;
    size_t len;
    const char * text = option_text(ctx, selected, &len);
    lv_label_set_text_fmt(ctx->summary, "%.*s", (int)len, text);
}
static void option_selected(lv_event_t * e) {
    unsigned selected = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    gui_option_selected_cb_t callback = option_owner->callback;
    /* Close before the setter: native pages rebuild their rows on change. */
    gui_option_menu_close();
    if (callback) callback(selected);
}
static void option_label_resized(lv_event_t * e) {
    lv_obj_t * label = lv_event_get_target(e);
    lv_obj_set_height(lv_obj_get_parent(label),
        LV_MAX(ui_list_row_height(), lv_obj_get_height(label) + BOARD_SCALE_PX(32)));
}
void gui_option_menu_open(lv_obj_t * row) {
    option_menu_row_t * ctx = lv_obj_get_user_data(row);
    if (lv_obj_has_state(ctx->row, LV_STATE_DISABLED)) return;
    gui_option_menu_close();
    option_owner = ctx;
    lv_obj_t * title;
    build_titled_popup(&option_popup, option_backdrop, &title, NULL);
    lv_label_set_text(title, lv_label_get_text(lv_obj_get_child(ctx->row, 0)));
    unsigned count = 1;
    for (const char * p = ctx->options; *p; ++p) if (*p == '\n') ++count;
    lv_obj_t * checked = NULL;
    for (unsigned i = 0; i < count; ++i) {
        size_t len;
        const char * text = option_text(ctx, i, &len);
        char * label = lv_malloc(len + 1);
        if (!label) continue;
        memcpy(label, text, len); label[len] = 0;
        lv_obj_t * row = add_pill_option_row(option_popup.popup, label, i == ctx->selected,
                                            option_selected, (void *)(uintptr_t)i);
        lv_obj_set_width(row, lv_pct(100));
        /* Popup rows are narrower than full-screen lists; wrap long choices. */
        lv_obj_t * option_label = lv_obj_get_child(row, 0);
        lv_label_set_long_mode(option_label, LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(option_label, LV_OBJ_FLAG_USER_3);
        lv_obj_set_width(option_label, lv_pct(78));
        lv_obj_set_height(option_label, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(option_label, 0, 0);
        lv_obj_add_event_cb(option_label, option_label_resized, LV_EVENT_SIZE_CHANGED, NULL);
        lv_free(label);
        if (i == ctx->selected) checked = row;
    }
    gui_popup_show(&option_popup);
    lv_obj_update_layout(option_popup.popup);
    if (checked) lv_obj_scroll_to_view(checked, LV_ANIM_OFF);
}
static void option_open(lv_event_t * e) {
    gui_option_menu_open(lv_event_get_current_target(e));
}
static void option_row_deleted(lv_event_t * e) {
    option_menu_row_t * ctx = lv_event_get_user_data(e);
    if (option_owner == ctx) gui_option_menu_close();
    lv_free(ctx->options);
    lv_free(ctx);
}
lv_obj_t * add_pill_option_menu_row(lv_obj_t * parent, const char * title,
    const char * options, unsigned selected, gui_option_selected_cb_t callback) {
    option_menu_row_t * ctx = lv_malloc(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->options = lv_malloc(strlen(options) + 1);
    if (!ctx->options) { lv_free(ctx); return NULL; }
    strcpy(ctx->options, options);
    ctx->callback = callback;
    ctx->row = add_pill_chevron_row(parent, title, NULL);
    ctx->summary = add_pill_summary_label(ctx->row);
    lv_obj_set_user_data(ctx->row, ctx);
    gui_option_menu_set_selected(ctx->row, selected);
    lv_obj_add_event_cb(ctx->row, option_open, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(ctx->row, option_row_deleted, LV_EVENT_DELETE, ctx);
    return ctx->row;
}
