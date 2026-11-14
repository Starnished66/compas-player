#include "gui_play_bar.h"
#include "screen_builders.h"
#include "../../appearance/gui_theme.h"
#include "../../text/i18n/i18n.h"
#include "../../../core/system/runtime/board_config.h"
#include <stdio.h>

static int play_bar_marker;
static int play_buttons_marker;

static lv_color_t play_label_color(void) {
    return lv_color_brightness(accent_lv_color()) > 160 ? lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT) : lv_color_white();
}

static int32_t pill_height(void) {
    int32_t bh = lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_ROW)) + BOARD_SCALE_PX(20);
    return bh < BOARD_SCALE_PX(56) ? BOARD_SCALE_PX(56) : bh;
}

static lv_obj_t * plain(lv_obj_t * parent) {
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* Room for a pill's label; pills are always given a fixed pixel width. */
static int32_t pill_label_room(lv_obj_t * pill) {
    return lv_obj_get_style_width(pill, 0) - 2 * BOARD_SCALE_PX(12);
}

static int32_t text_width(const char * text, const lv_font_t * font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Fits the labels of side-by-side pills with one shared font: the row font, or
 * the largest smaller one every label fits in, so a long translation shrinks
 * the pair together; an ellipsis only if even the smallest is too wide. */
static void pills_fit(lv_obj_t * const * pills, uint32_t n) {
    static const gui_font_role_t roles[] = { GUI_FONT_ROLE_ROW, GUI_FONT_ROLE_BODY, GUI_FONT_ROLE_SUBTEXT };
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    for (size_t r = 0; r < sizeof(roles) / sizeof(roles[0]); r++) {
        const lv_font_t * candidate = gui_theme_font(roles[r]);
        bool fits = true;
        for (uint32_t i = 0; i < n && fits; i++) {
            lv_obj_t * label = lv_obj_get_child(pills[i], 0);
            fits = !label || text_width(lv_label_get_text(label), candidate) <= pill_label_room(pills[i]);
        }
        if (fits) {
            font = candidate;
            break;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t * label = lv_obj_get_child(pills[i], 0);
        if (!label) continue;
        lv_obj_set_style_text_font(label, font, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_size(label, LV_MIN(text_width(lv_label_get_text(label), font), pill_label_room(pills[i])),
                        lv_font_get_line_height(font));
        lv_obj_center(label);
    }
}

static lv_obj_t * pill_button_create(lv_obj_t * parent, int32_t w, int32_t h, const char * text, bool filled,
                                     lv_event_cb_t cb, void * user_data, bool fit_label) {
    lv_obj_t * button = plain(parent);
    lv_obj_set_size(button, w, h);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t * label = lv_label_create(button);
    lv_label_set_text(label, text);
    if (filled) {
        lv_obj_add_style(button, gui_theme_accent_style(), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, play_label_color(), 0);
    } else {
        lv_obj_add_style(button, &style_theme_card_bg, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(button, BOARD_SCALE_PX(2), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
    }
    if (fit_label) pills_fit(&button, 1);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
    return button;
}

lv_obj_t * gui_pill_button(lv_obj_t * parent, int32_t w, int32_t h, const char * text, bool filled,
                           lv_event_cb_t cb, void * user_data) {
    return pill_button_create(parent, w, h, text, filled, cb, user_data, true);
}

lv_obj_t * gui_play_buttons_build(lv_obj_t * parent, int32_t row_w, lv_event_cb_t play_cb, void * play_data,
                                  lv_event_cb_t shuffle_cb, void * shuffle_data) {
    int32_t bgap = BOARD_SCALE_PX(12), bw = (row_w - bgap) / 2;
    int32_t bh = pill_height();
    lv_obj_t * buttons = plain(parent);
    lv_obj_set_user_data(buttons, &play_buttons_marker);
    lv_obj_set_size(buttons, row_w, bh);
    lv_obj_set_style_pad_column(buttons, bgap, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    char play[64], shuffle[64];
    snprintf(play, sizeof(play), LV_SYMBOL_PLAY "  %s", TR("Play"));
    snprintf(shuffle, sizeof(shuffle), LV_SYMBOL_SHUFFLE "  %s", TR("Shuffle"));
    lv_obj_t * pills[] = { pill_button_create(buttons, bw, bh, play, true, play_cb, play_data, false),
                           pill_button_create(buttons, bw, bh, shuffle, false, shuffle_cb, shuffle_data, false) };
    pills_fit(pills, 2);
    return buttons;
}

lv_obj_t * gui_play_bar_build(lv_obj_t * parent, int32_t row_w, const char * count_text,
                              lv_event_cb_t play_cb, lv_event_cb_t shuffle_cb) {
    lv_obj_t * root = plain(parent);
    lv_obj_set_user_data(root, &play_bar_marker);
    lv_obj_set_size(root, row_w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(root, BOARD_SCALE_PX(8), 0);
    lv_obj_set_style_pad_top(root, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_bottom(root, BOARD_SCALE_PX(10), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_BODY);
    lv_obj_t * label = lv_label_create(root);
    lv_label_set_text(label, count_text);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, row_w, lv_font_get_line_height(font));
    gui_play_buttons_build(root, row_w, play_cb, NULL, shuffle_cb, NULL);
    return root;
}

bool gui_play_bar_is(const lv_obj_t * obj) {
    if (!obj) return false;
    void * marker = lv_obj_get_user_data((lv_obj_t *) obj);
    return marker == &play_bar_marker || marker == &play_buttons_marker;
}

static void play_buttons_restyle(lv_obj_t * buttons) {
    if (!buttons || lv_obj_get_child_count(buttons) < 1) return;
    int32_t bh = pill_height();
    lv_obj_set_height(buttons, bh);
    lv_obj_t * pills[2];
    uint32_t n = LV_MIN(lv_obj_get_child_count(buttons), 2);
    for (uint32_t i = 0; i < n; i++) {
        pills[i] = lv_obj_get_child(buttons, i);
        lv_obj_set_height(pills[i], bh);
    }
    pills_fit(pills, n);
    lv_obj_t * play_label = n ? lv_obj_get_child(pills[0], 0) : NULL;
    if (play_label) lv_obj_set_style_text_color(play_label, play_label_color(), 0);
    lv_obj_t * parent = lv_obj_get_parent(buttons);
    if (parent) lv_obj_update_layout(parent);
}

void gui_play_bar_restyle(lv_obj_t * root) {
    if (!gui_play_bar_is(root)) return;
    if (lv_obj_get_user_data(root) == &play_buttons_marker) {
        play_buttons_restyle(root);
        return;
    }
    if (lv_obj_get_child_count(root) < 2) return;
    lv_obj_t * count = lv_obj_get_child(root, 0);
    lv_obj_t * buttons = lv_obj_get_child(root, 1);
    const lv_font_t * body = gui_theme_font(GUI_FONT_ROLE_BODY);
    lv_obj_set_style_text_font(count, body, 0);
    lv_obj_set_height(count, lv_font_get_line_height(body));
    play_buttons_restyle(buttons);
}
