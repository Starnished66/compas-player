#include "screen_builders.h"
#include "screen_builders_internal.h"
#include "../../text/i18n/i18n.h"
#include "../../assets/assets.h"

#include <string.h>

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
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_set_style_pad_row(popup, GUI_ROW_GAP, 0);
    lv_obj_set_scroll_dir(popup, LV_DIR_VER);
    screen_builders_configure_content_scrollbar(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(popup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
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
    lv_obj_set_style_text_color(label, color, 0);
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

    lv_obj_t * cancel_row = add_popup_action_row(popup, cancel_text,
        popup_text_is_cancel(cancel_text) ? lv_color_make(255, 120, 120) : cancel_color, cancel_cb, NULL);
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
    lv_obj_set_style_text_color(cancel_label, lv_color_make(255, 120, 120), 0);
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
    if (p->backdrop) lv_obj_remove_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
    if (p->popup) lv_obj_remove_flag(p->popup, LV_OBJ_FLAG_HIDDEN);
    if (p->backdrop) lv_obj_move_foreground(p->backdrop);
    if (p->popup) lv_obj_move_foreground(p->popup);
}

void gui_popup_hide(gui_popup_t * p) {
    if (!p) return;
    if (p->visible) {
        p->visible = false;
        if (visible_popup_count > 0) visible_popup_count--;
    }
    if (p->backdrop) lv_obj_add_flag(p->backdrop, LV_OBJ_FLAG_HIDDEN);
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
