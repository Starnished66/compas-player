#include "gui_network_dac_screen.h"
#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../assets/assets.h"
#include "../widgets/builders/gui_play_bar.h"
#include "gui_network.h"

static lv_obj_t * plain_column(lv_obj_t * parent) {
    lv_obj_t * column = lv_obj_create(parent);
    lv_obj_remove_style_all(column);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(column, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, BOARD_SCALE_PX(20), 0);
    return column;
}

static lv_obj_t * centered_label(lv_obj_t * parent, const char * text, gui_font_role_t role) {
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(role), 0);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    return label;
}

lv_obj_t * gui_network_dac_screen_create(const char * title, lv_obj_t ** content) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_theme_screen_bg, 0);
    build_screen_header(screen, title, generic_back_cb, NULL, NULL);
    *content = plain_column(screen);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_pos(*content, 0, top);
    lv_obj_set_height(*content, BOARD_SCREEN_HEIGHT - top - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_pad_hor(*content, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_ver(*content, BOARD_SCALE_PX(20), 0);
    finalize_screen_navigation(screen);
    return screen;
}

void gui_network_dac_screen_confirm(gui_popup_t * popup, const char * title,
                                    const char * stop_label, lv_event_cb_t confirm_cb,
                                    lv_event_cb_t dismiss_cb) {
    popup->popup = build_confirm_popup(title, LV_LABEL_LONG_WRAP, NULL, NULL, stop_label,
        lv_color_hex(GUI_BREEZE_NEGATIVE), confirm_cb, NULL, TR("Cancel"),
        accent_lv_color(), dismiss_cb, NULL, dismiss_cb, &popup->backdrop);
}

static lv_obj_t * transport_button(lv_obj_t * row, const char * asset, int direction,
                                   lv_event_cb_t callback) {
    int32_t size = BOARD_SCALE_PX(direction ? 40 : 84);
    lv_obj_t * button = lv_image_create(row);
    lv_image_set_src(button, asset_path(asset));
    lv_image_set_inner_align(button, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_style(button, &style_theme_text_primary, 0);
    lv_obj_add_style(button, &icon_press_style, LV_STATE_PRESSED);
    lv_obj_set_size(button, size, size);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, (void *) (intptr_t) direction);
    lv_obj_set_ext_click_area(button, BOARD_SCALE_PX(12));
    return button;
}

void gui_network_dac_screen_transport(gui_dac_screen_t * screen, lv_event_cb_t callback) {
    screen->transport = plain_column(screen->hero);
    lv_obj_set_flex_flow(screen->transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(screen->transport, BOARD_SCALE_PX(48), 0);
    lv_obj_set_style_pad_top(screen->transport, BOARD_SCALE_PX(16), 0);
    transport_button(screen->transport, "playing_plane/btn_prev.png", -1, callback);
    screen->play = transport_button(screen->transport, "playing_plane/btn_play.png", 0, callback);
    transport_button(screen->transport, "playing_plane/btn_next.png", 1, callback);
}

void gui_network_dac_screen_build(lv_obj_t * content, const gui_dac_screen_options_t * options,
                                  gui_dac_screen_t * out) {
    out->content = content;
    out->hero = plain_column(content);
    lv_obj_set_flex_grow(out->hero, 1);

    lv_obj_t * icon = lv_image_create(out->hero);
    lv_image_set_src(icon, asset_path(options->icon));
    lv_obj_set_size(icon, BOARD_SCALE_PX(112), BOARD_SCALE_PX(112));
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_style(icon, options->active ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    if (!options->active) lv_obj_set_style_opa(icon, LV_OPA_40, 0);

    out->title = centered_label(out->hero, options->title, GUI_FONT_ROLE_TITLE);
    lv_obj_add_style(out->title, &style_theme_text_primary, 0);
    lv_label_set_long_mode(out->title, LV_LABEL_LONG_DOT);
    lv_obj_set_height(out->title, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_TITLE)));
    if (options->show_explanation) centered_label(out->hero, options->explanation, GUI_FONT_ROLE_SUBTEXT);

    out->chip = plain_column(out->hero);
    lv_obj_set_size(out->chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(out->chip, LV_PCT(100), 0);
    lv_obj_add_style(out->chip, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(out->chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(out->chip, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_hor(out->chip, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_ver(out->chip, BOARD_SCALE_PX(8), 0);
    out->stream = centered_label(out->chip, options->waiting, GUI_FONT_ROLE_SUBTEXT);
    lv_obj_set_width(out->stream, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(out->stream, BOARD_SCREEN_WIDTH - BOARD_SCALE_PX(80), 0);
    if (!options->enabled) lv_obj_add_flag(out->chip, LV_OBJ_FLAG_HIDDEN);

    out->transport = NULL;
    out->play = NULL;
    if (options->transport) gui_network_dac_screen_transport(out, options->transport_cb);
}

void gui_network_dac_screen_action(gui_dac_screen_t * screen,
                                   const gui_dac_screen_options_t * options) {
    if (!options->enabled) {
        lv_point_t text_size;
        lv_text_get_size(&text_size, options->enable_label, gui_theme_font(GUI_FONT_ROLE_ROW),
                         0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t width = LV_MAX(BOARD_SCALE_PX(320), text_size.x + BOARD_SCALE_PX(48));
        width = LV_MIN(width, ui_list_row_width());
        gui_pill_button(screen->content, width, BOARD_SCALE_PX(56), options->enable_label,
                        true, options->enable_cb, NULL);
        return;
    }

    lv_obj_t * stop = centered_label(screen->content, options->stop_label, GUI_FONT_ROLE_BODY);
    lv_obj_set_style_text_color(stop, lv_color_hex(GUI_BREEZE_NEGATIVE), 0);
    lv_obj_set_style_pad_ver(stop, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_opa(stop, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(stop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stop, options->stop_cb, LV_EVENT_CLICKED, NULL);
}
