#include "gui_notifications.h"
#include "../text/rendering/ui_label_text.h"
#include "../text/rendering/fallback_font.h"
#include "gui_navigation.h"
#include "../appearance/gui_theme.h"
#include "../application/gui.h"
#include "../widgets/builders/screen_builders.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    lv_obj_t * obj;
    lv_obj_t * label;
    lv_timer_t * timer;
} toast_t;

static toast_t error_toast = { 0 };
static toast_t info_toast = { 0 };

/* Busy overlay */
static uint32_t gui_busy_current_token = 0;
static lv_obj_t * gui_busy_screen = NULL;
#if defined(BOARD_TEMPOTEC_V1)
static lv_obj_t * gui_busy_content = NULL;
#define GUI_BUSY_PARENT gui_busy_content
#else
#define GUI_BUSY_PARENT gui_busy_screen
#endif
static lv_obj_t * gui_busy_label = NULL;
static lv_obj_t * gui_busy_progress_bar = NULL;
static lv_obj_t * gui_busy_detail_label = NULL;

static void toast_hide_timer_cb(lv_timer_t * timer) {
    toast_t * toast = lv_timer_get_user_data(timer);
    if (!toast) return;
    if (toast->obj) lv_obj_add_flag(toast->obj, LV_OBJ_FLAG_HIDDEN);
    if (toast->timer) lv_timer_pause(toast->timer);
}

/* Shared by both toasts, which differ only in colours, width, placement and
 * how long they linger. Height follows the message in both directions, so a
 * wrapped message grows and a short one stays compact. A message long enough
 * to reach the screen-bounded maximum is clipped, since the toast does not
 * scroll. */
static lv_obj_t * build_toast(lv_obj_t ** out_label, int32_t width, int32_t y_offset) {
    lv_obj_t * toast = lv_obj_create(lv_layer_top());
    lv_obj_set_width(toast, width);
    lv_obj_set_height(toast, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(toast, BOARD_SCALE_PX(56), 0);
    lv_obj_set_style_max_height(toast,
        lv_display_get_vertical_resolution(lv_display_get_default()) - 2 * STATUS_BAR_CLEARANCE, 0);
    lv_obj_set_style_pad_all(toast, BOARD_SCALE_PX(16), 0);
    lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, y_offset);
    lv_obj_set_style_radius(toast, 16, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * label = lv_label_create(toast);
    lv_obj_set_width(label, lv_pct(100)); /* the toast's own padding is the inset */
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    *out_label = label;
    return toast;
}

static void build_error_toast(void) {
    error_toast.obj = build_toast(&error_toast.label, BOARD_SCALE_PX(400), -BOARD_SCALE_PX(180));
    lv_obj_set_style_bg_color(error_toast.obj, lv_color_make(40, 20, 20), 0);
    lv_obj_set_style_bg_opa(error_toast.obj, LV_OPA_80, 0);
    lv_obj_set_style_text_color(error_toast.label, lv_color_make(255, 200, 200), 0);

    error_toast.timer = lv_timer_create(toast_hide_timer_cb, 2500, &error_toast);
    lv_timer_pause(error_toast.timer);
}

static void show_toast(toast_t * toast, const char * msg, uint32_t duration_ms) {
    if (!toast->label) return;
    lv_label_set_text(toast->label, msg);
    lv_obj_remove_flag(toast->obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast->obj);
    if (duration_ms > 0) lv_timer_set_period(toast->timer, duration_ms);
    lv_timer_reset(toast->timer);
    lv_timer_resume(toast->timer);
}

void show_error_toast(const char * msg) {
    show_toast(&error_toast, msg, 0);
}

static void build_info_toast(void) {
    info_toast.obj = build_toast(&info_toast.label, BOARD_SCALE_PX(420), -BOARD_SCALE_PX(160));
    lv_obj_add_style(info_toast.obj, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(info_toast.obj, LV_OPA_80, 0);
    lv_obj_add_style(info_toast.label, &style_theme_text_primary, 0);

    info_toast.timer = lv_timer_create(toast_hide_timer_cb, 5000, &info_toast);
    lv_timer_pause(info_toast.timer);
}

void show_info_toast(const char * msg) {
    show_info_toast_for(msg, 5000);
}

void show_info_toast_for(const char * msg, uint32_t duration_ms) {
    if (duration_ms < 100) duration_ms = 100;
    if (duration_ms > 30000) duration_ms = 30000;
    show_toast(&info_toast, msg, duration_ms);
}

bool gui_notifications_toast_visible(void) {
    return (error_toast.obj && !lv_obj_has_flag(error_toast.obj, LV_OBJ_FLAG_HIDDEN)) ||
           (info_toast.obj && !lv_obj_has_flag(info_toast.obj, LV_OBJ_FLAG_HIDDEN));
}

gui_busy_handle_t gui_busy_show(const char * title, const char * msg) {
    gui_busy_current_token++;
    if (!gui_busy_screen) {
        gui_busy_screen = lv_obj_create(NULL);
        lv_obj_add_style(gui_busy_screen, &style_theme_screen_bg, 0);

#if defined(BOARD_TEMPOTEC_V1)
        /* V1's 320px height cannot safely use fixed center offsets when the
         * wrapped title, message, and optional progress lines grow. Keep the
         * content within the status/home safe area and let flex reflow hidden
         * or newly shown rows; long operations remain vertically scrollable. */
        gui_busy_content = lv_obj_create(gui_busy_screen);
        lv_obj_set_width(gui_busy_content, LV_PCT(100));
        lv_obj_set_height(gui_busy_content, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(gui_busy_content,
                                    BOARD_SCREEN_HEIGHT - STATUS_BAR_CLEARANCE - HOME_INDICATOR_CONTENT_INSET, 0);
        lv_obj_align(gui_busy_content, LV_ALIGN_CENTER, 0,
                     (STATUS_BAR_CLEARANCE - HOME_INDICATOR_CONTENT_INSET) / 2);
        lv_obj_set_style_bg_opa(gui_busy_content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(gui_busy_content, 0, 0);
        lv_obj_set_style_pad_all(gui_busy_content, BOARD_SCALE_PX(8), 0);
        lv_obj_set_style_pad_row(gui_busy_content, BOARD_SCALE_PY(10), 0);
        lv_obj_set_flex_flow(gui_busy_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(gui_busy_content, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollbar_mode(gui_busy_content, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(gui_busy_content, LV_DIR_VER);
        lv_obj_add_flag(gui_busy_content, LV_OBJ_FLAG_SCROLLABLE);

#endif

        gui_busy_label = lv_label_create(GUI_BUSY_PARENT);
        lv_obj_add_style(gui_busy_label, &style_theme_text_primary, 0);
        /* Same reason as the detail label below: the theme styles carry colours
         * only, so the title inherited LV_FONT_DEFAULT -- the compiled
         * montserrat_16 bitmap without a non-Latin fallback chain, which turned every
         * translated title ("checking for updates", ...) into boxes.
         * fallback_font_fixed(16) keeps that exact size and only adds the chain,
         * so the dialog layout and its progress-bar spacing do not move. */
        lv_obj_set_style_text_font(gui_busy_label, fallback_font_fixed(16), 0);
        lv_obj_set_width(gui_busy_label, LV_PCT(90));
        lv_label_set_long_mode(gui_busy_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(gui_busy_label, LV_TEXT_ALIGN_CENTER, 0);
#if !defined(BOARD_TEMPOTEC_V1)
        lv_obj_align(gui_busy_label, LV_ALIGN_CENTER, 0, -BOARD_SCALE_PX(20));
#endif

        gui_busy_progress_bar = lv_bar_create(GUI_BUSY_PARENT);
        lv_obj_set_size(gui_busy_progress_bar, BOARD_SCALE_PX(280), BOARD_SCALE_PX(14));
#if !defined(BOARD_TEMPOTEC_V1)
        lv_obj_align(gui_busy_progress_bar, LV_ALIGN_CENTER, 0, BOARD_SCALE_PX(30));
#endif
        lv_bar_set_range(gui_busy_progress_bar, 0, 100);
        lv_obj_add_style(gui_busy_progress_bar, gui_theme_accent_style(), LV_PART_INDICATOR);

        /* Optional status line under the bar (e.g. scan phase and counts). */
        gui_busy_detail_label = lv_label_create(GUI_BUSY_PARENT);
        lv_obj_add_style(gui_busy_detail_label, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(gui_busy_detail_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_style_text_align(gui_busy_detail_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(gui_busy_detail_label, BOARD_SCALE_PX(280));
        lv_label_set_long_mode(gui_busy_detail_label, LV_LABEL_LONG_WRAP);
#if !defined(BOARD_TEMPOTEC_V1)
        lv_obj_align_to(gui_busy_detail_label, gui_busy_progress_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, BOARD_SCALE_PX(16));
#endif
    }
    
    if (msg && msg[0] != '\0') {
        lv_label_set_text_fmt(gui_busy_label, "%s\n%s", title, msg);
    } else {
        lv_label_set_text(gui_busy_label, title);
    }
    lv_obj_add_flag(gui_busy_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(gui_busy_detail_label, "");
    lv_obj_add_flag(gui_busy_detail_label, LV_OBJ_FLAG_HIDDEN);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_scroll_to_y(gui_busy_content, 0, LV_ANIM_OFF);
#endif
    
    if (lv_screen_active() != gui_busy_screen) {
        nav_push(gui_busy_screen);
    }
    return gui_busy_current_token;
}

void gui_busy_set_progress(gui_busy_handle_t handle, int percent) {
    if (handle != gui_busy_current_token || !gui_busy_screen) return;
    lv_obj_remove_flag(gui_busy_progress_bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(gui_busy_progress_bar, percent, LV_ANIM_OFF);
}

void gui_busy_set_detail(gui_busy_handle_t handle, const char * text) {
    if (handle != gui_busy_current_token || !gui_busy_screen) return;
    if (!text || text[0] == '\0') {
        lv_obj_add_flag(gui_busy_detail_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (!ui_label_text_matches(gui_busy_detail_label, text)) lv_label_set_text(gui_busy_detail_label, text);
    lv_obj_remove_flag(gui_busy_detail_label, LV_OBJ_FLAG_HIDDEN);
}

static void gui_busy_hide_internal(gui_busy_handle_t handle, bool immediate) {
    if (handle != gui_busy_current_token || !gui_busy_screen) return;
    if (lv_screen_active() == gui_busy_screen) {
        if (immediate) nav_pop_immediate();
        else nav_pop();
    } else if (!gui_navigation_transition_in_progress()) {
        /* A lock screen or another destination can cover the interstitial.
         * Drop its stack entry without disturbing the current screen or a
         * transition's captured destination. Folder jobs defer dismissal
         * until any transition has settled. */
        lv_obj_t * screens[] = { gui_busy_screen };
        gui_navigation_remove_screen_instances(screens, 1);
    }
    gui_busy_current_token++; /* invalidate token */
}

void gui_busy_hide(gui_busy_handle_t handle) {
    gui_busy_hide_internal(handle, false);
}

void gui_busy_hide_immediate(gui_busy_handle_t handle) {
    gui_busy_hide_internal(handle, true);
}

void gui_notifications_init(void) {
    build_error_toast();
    build_info_toast();
}

lv_obj_t * gui_busy_get_screen(void) {
    return gui_busy_screen;
}

lv_obj_t * build_menu_popup(const menu_popup_row_t * rows, int row_count, lv_event_cb_t backdrop_cb,
                            lv_obj_t ** out_backdrop) {
    lv_obj_t * popup = build_popup_surface(backdrop_cb, out_backdrop);

    for (int i = 0; i < row_count; i++) {
        lv_obj_t * row = lv_obj_create(popup);
        style_popup_action_row(row);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, rows[i].cb, LV_EVENT_CLICKED, rows[i].user_data);

        lv_obj_t * label = lv_label_create(row);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_text(label, rows[i].label);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        if (rows[i].destructive) lv_obj_set_style_text_color(label, lv_color_hex(GUI_BREEZE_NEGATIVE), 0);
        else if (rows[i].cancel) lv_obj_set_style_text_color(label, lv_color_make(255, 120, 120), 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    return popup;
}
