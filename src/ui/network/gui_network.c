#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../widgets/builders/screen_builders.h"
#include "gui_network_internal.h"

static lv_obj_t * wireless_screen;

void configure_network_row_line(lv_obj_t * label, int32_t width, const lv_font_t * font) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, font, 0);
    row_label_apply_bounded_height(label, font);
}

static int32_t network_row_text_stack_height(const lv_font_t * const fonts[], int count) {
    if (!fonts || count <= 0 || count > 3) return 0;
    int32_t heights[3];
    int32_t gap = BOARD_SCALE_PX(2);
    int32_t total_height = gap * (count - 1);
    for (int i = 0; i < count; ++i) {
        heights[i] = row_label_bounded_height(fonts[i]);
        total_height += heights[i];
    }
    return total_height;
}

/* Keep the standard scaled row height unless the selected font tier needs
 * more room for the full bounded line stack and its top/bottom breathing room. */
int32_t network_row_text_height(int32_t base_height, const lv_font_t * const fonts[], int count) {
    int32_t needed = network_row_text_stack_height(fonts, count) + BOARD_SCALE_PX(16);
    return needed > base_height ? needed : base_height;
}

/* Center the visible title/status (and optional codec) stack in the row's
 * content box. Use bounded label heights rather than raw font line heights
 * so descender clearance is included on every board. */
void align_network_row_text_stack(lv_obj_t * row, lv_obj_t * const labels[],
                                  const lv_font_t * const fonts[], int count, int32_t left) {
    if (!row || !labels || !fonts || count <= 0 || count > 3) return;
    int32_t heights[3];
    int32_t gap = BOARD_SCALE_PX(2);
    int32_t total_height = network_row_text_stack_height(fonts, count);
    for (int i = 0; i < count; ++i) heights[i] = row_label_bounded_height(fonts[i]);

    int32_t cursor = -(total_height / 2);
    for (int i = 0; i < count; ++i) {
        int32_t y = cursor + heights[i] / 2;
        lv_obj_align(labels[i], LV_ALIGN_LEFT_MID, left, y);
        cursor += heights[i] + gap;
    }
}

/* Muted, wrapped description line above/below a settings toggle. */
lv_obj_t * add_network_explanation(lv_obj_t * list, const char * text) {
    lv_obj_t * explanation = lv_label_create(list);
    lv_label_set_text(explanation, text);
    lv_obj_set_width(explanation, lv_pct(90));
    lv_label_set_long_mode(explanation, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(explanation, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(explanation, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_left(explanation, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(explanation, BOARD_SCALE_PX(12), 0);
    return explanation;
}

/* Settings-style list screen (header, back, flex list); the title never
 * changes after the build, so only the list is returned through the pointer. */
lv_obj_t * network_list_screen(const char * title, lv_obj_t ** list) {
    lv_obj_t * title_label;
    return build_subsonic_list_screen(title, &title_label, list);
}

/* One pill row: scaled size, pill background, clickable with cb. The caller
 * fills in the text labels. */
lv_obj_t * network_row_create(lv_obj_t * parent, int32_t height, lv_event_cb_t cb, void * user_data) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_ROW_WIDTH, height);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_set_style_radius(row, LIST_ROW_RADIUS, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    return row;
}

/* True while a finger is down or the list (if any) is scrolling: rebuilding
 * then would destroy the row being pressed and lose the tap. */
bool network_list_touch_active(lv_obj_t * list) {
    if (list && lv_obj_is_scrolling(list)) return true;
    for (lv_indev_t * indev = lv_indev_get_next(NULL); indev; indev = lv_indev_get_next(indev))
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

/* Deletes a screen this module built and clears its pointer. */
void network_delete_screen(lv_obj_t ** screen) {
    if (!*screen) return;
    lv_obj_delete(*screen);
    *screen = NULL;
}

/* Display order that moves the item at `front` (if any) to the top and keeps
 * the rest in their original order. Returns the source index for `position`. */
int rotate_to_front(int position, int front) {
    if (front < 0) return position;
    if (position == 0) return front;
    return position <= front ? position - 1 : position;
}

/* ---- Shared Wi-Fi dependency guard for AirPlay/DLNA --
 * these require the Wi-Fi radio/interface to be enabled
 * (NOT association with an access point; individual screens keep showing
 * their own existing "connect first" state for that). Checks gui_shell_wifi.c's
 * effective state (an in-flight disable counts as already off, see that
 * function's own comment) rather than wifi_control_is_enabled() directly,
 * shows the exact required toast, and returns false -- callers must bail
 * out without navigating, touching a setting, or starting anything. Shared
 * by the tile taps below (screen not open yet) and each feature's own
 * enable/toggle path (screen may already be open when Wi-Fi goes away). */
bool wifi_feature_guard(void) {
    if (gui_shell_wifi_effective_enabled()) return true;
    show_error_toast(TR("Enable WiFi to access"));
    return false;
}

static lv_obj_t * build_wireless_screen(void) {
    static icon_grid_item_t items[5];
    /* No .bg_image here: the stock per-tile background pill isn't present in
     * every board's own firmware (missing on R1, so the tile silently rendered
     * without one there) and measurably added render/decode cost on R3 Pro II
     * for a background that isn't load-bearing to this screen. */
    items[0] = (icon_grid_item_t){ "wireless/list_wifi.png", NULL, TR("Wi-Fi"), wifi_tile_cb, NULL };
    items[1] = (icon_grid_item_t){ "wireless/list_bt.png", NULL, TR("Bluetooth"), bt_tile_cb, NULL };
    items[2] = (icon_grid_item_t){ "wireless/list_airplay.png", NULL, TR("AirPlay"), airplay_tile_cb, NULL };
    items[3] = (icon_grid_item_t){ "wireless/list_dlna.png", NULL, TR("DLNA"), dlna_tile_cb, NULL };
    items[4] = (icon_grid_item_t){ "wireless/list_remote.png", NULL, TR("Remote"), remote_control_tile_cb, NULL };
    lv_obj_t * scr = build_category_menu_screen(TR("Wireless"), generic_back_cb, items, 5,
                                                &launcher_layout_config.wireless);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_network_refresh_wireless_screen(void) {
    lv_obj_t * old = wireless_screen;
    lv_obj_t * fresh = build_wireless_screen();
    if (!fresh) return;
    wireless_screen = fresh;
    gui_navigation_replace_static_screen(3, old, fresh);
    if (old) lv_obj_delete(old);
}

void gui_network_init(void) {
    gui_network_bt_build();
    gui_network_pairing_build();
    gui_network_wifi_screen_build();
    gui_network_choice_settings_build();
    gui_network_usb_build();
    gui_network_receivers_build();
    gui_network_services_build();
    wireless_screen = build_wireless_screen();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen/popup this
 * module owns so gui_network_init() can rebuild them from a clean slate
 * without leaking the old objects. Deliberately does NOT touch
 * dac_stream_labels_timer (already guarded/reused correctly by gui_network_
 * init() itself). Popups are built directly on lv_layer_top() (see
 * build_confirm_popup()'s own comment), not as children of any screen, so
 * each area's teardown deletes its own popups explicitly. */
void gui_network_teardown(void) {
    gui_network_wifi_teardown();
    gui_network_bt_teardown();
    gui_network_pairing_teardown();
    gui_network_wifi_screen_teardown();
    gui_network_receivers_teardown();
    gui_network_services_teardown();
    gui_network_choice_settings_teardown();
    gui_network_usb_teardown();
    network_delete_screen(&wireless_screen);
}

bool gui_network_has_background_work(void) {
    return gui_network_wifi_has_work() || gui_network_bt_has_work() ||
           gui_network_usb_has_work() || gui_network_services_has_work();
}

void gui_network_cancel_background_work(void) {
    /* The detached bridge restart owns no UI objects and may be stuck in
     * start() indefinitely. Leave its guard intact; never join it here. */
    gui_network_wifi_cancel();
    gui_network_bt_cancel();
    gui_network_usb_cancel();
    gui_network_services_cancel();
}

lv_obj_t * gui_network_get_wifi_screen(void) { return wifi_screen; }
lv_obj_t * gui_network_get_wireless_screen(void) { return wireless_screen; }
