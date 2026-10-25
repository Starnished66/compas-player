#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "app_version.h"
#include "firmware_update.h"
#include "firmware_ota.h"
#include "gui.h"
#include "gui_network.h"
#include "gui_notifications.h"
#include "gui_settings.h"
#include "gui_shell.h"
#include "gui_theme.h"
#include "i18n.h"
#include "player_update.h"
#include "screen_builders.h"

lv_style_t style_theme_text_muted;

#ifdef UPDATE_SPEC_FINAL
#include "gui_updates.h"
#include "gui_updates.c"
static void update_test_build_popups(void) { gui_updates_build_popups(); }
static void update_test_teardown(void) { gui_updates_teardown(); }
static void update_test_player_row_cb(lv_event_t * event) { gui_updates_player_row_cb(event); }
#else
#include "update_baseline_fixture.c"
#endif

static char trace[16384];
static size_t trace_length;
static lv_obj_t * buttons[16];
static const char * button_names[16];
static int button_count;
static lv_obj_t * menus[2];
static lv_obj_t * menu_backdrops[2];
static int menu_count;
static int busy_next = 1;
static int busy_show_count;
static int busy_hide_count;
static gui_busy_handle_t current_busy;
static int popup_build_count;
static int scan_count;
static int firmware_start_count;
static int ota_check_count;
static int ota_download_count;
static int ota_reset_count;
static int player_check_count;
static int player_online_count;
static int player_sd_count;
static int player_reset_count;
static int error_count;
static int info_count;
static bool usb_blocked;
static bool wifi_enabled;
static bool firmware_busy_backend;
static bool ota_busy_backend;
static bool firmware_start_ok = true;
static bool ota_check_ok = true;
static bool ota_download_ok = true;
static bool player_check_ok = true;
static bool player_online_ok = true;
static firmware_update_status_t firmware_status;
static firmware_ota_status_t ota_status;
static player_update_status_t player_status;

static void tracef(const char * format, ...);

#include <stdarg.h>
static void tracef(const char * format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(trace + trace_length, sizeof(trace) - trace_length, format, args);
    va_end(args);
    assert(n >= 0 && (size_t)n < sizeof(trace) - trace_length);
    trace_length += (size_t)n;
}

const char * i18n_tr(const char * text) { return text; }
const char * app_version_label(void) { return "test-build"; }
const lv_font_t * gui_theme_font(gui_font_role_t role) { (void)role; return &lv_font_montserrat_16; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
lv_color_t accent_lv_color(void) { return lv_color_hex(0x55aacc); }
bool gui_shell_wifi_effective_enabled(void) { return wifi_enabled; }
bool gui_network_firmware_update_usb_blocked(void) { return usb_blocked; }

void show_error_toast(const char * message) {
    error_count++;
    tracef("error:%s\n", message ? message : "");
}
void show_info_toast(const char * message) {
    info_count++;
    tracef("info:%s\n", message ? message : "");
}

gui_busy_handle_t gui_busy_show(const char * title, const char * message) {
    current_busy = (gui_busy_handle_t)busy_next++;
    busy_show_count++;
    tracef("busy-show:%u:%s:%s\n", current_busy, title ? title : "", message ? message : "");
    return current_busy;
}
void gui_busy_set_progress(gui_busy_handle_t handle, int percent) {
    tracef("busy-progress:%u:%d\n", handle, percent);
}
void gui_busy_set_detail(gui_busy_handle_t handle, const char * text) {
    tracef("busy-detail:%u:%s\n", handle, text ? text : "");
}
void gui_busy_hide(gui_busy_handle_t handle) {
    busy_hide_count++;
    tracef("busy-hide:%u\n", handle);
}

static const char * popup_name(const gui_popup_t * popup) {
    if (popup == &firmware_update_popup) return "manual";
    if (popup == &ota_offer_popup) return "ota-offer";
    if (popup == &ota_install_popup) return "ota-install";
    if (popup == &player_update_offer_popup) return "player-offer";
    if (popup == &player_update_confirm_popup) return "player-confirm";
    return "other";
}
void gui_popup_show(gui_popup_t * popup) {
    popup->visible = true;
    tracef("popup-show:%s\n", popup_name(popup));
}
void gui_popup_hide(gui_popup_t * popup) {
    popup->visible = false;
    tracef("popup-hide:%s\n", popup_name(popup));
}
void gui_popup_teardown(gui_popup_t * popup) {
    tracef("popup-teardown:%s\n", popup_name(popup));
    if (popup->popup) lv_obj_delete(popup->popup);
    if (popup->backdrop) lv_obj_delete(popup->backdrop);
    popup->popup = popup->backdrop = NULL;
    popup->visible = false;
}

lv_obj_t * build_confirm_popup(const char * title, lv_label_long_mode_t mode,
                               lv_obj_t ** out_title, const char * body,
                               const char * confirm, lv_color_t confirm_color,
                               lv_event_cb_t confirm_cb, lv_obj_t ** confirm_row,
                               const char * cancel, lv_color_t cancel_color,
                               lv_event_cb_t cancel_cb, lv_obj_t ** cancel_row,
                               lv_event_cb_t backdrop_cb, lv_obj_t ** backdrop) {
    (void)mode; (void)body; (void)confirm_color; (void)cancel_color;
    lv_obj_t * popup = lv_obj_create(lv_layer_top());
    lv_obj_t * label = lv_label_create(popup);
    lv_label_set_text(label, title ? title : "");
    if (out_title) *out_title = label;
    lv_obj_t * yes = lv_btn_create(popup);
    lv_obj_t * no = lv_btn_create(popup);
    lv_obj_add_event_cb(yes, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * yes_label = lv_label_create(yes);
    lv_label_set_text(yes_label, confirm ? confirm : "");
    lv_obj_t * no_label = lv_label_create(no);
    lv_label_set_text(no_label, cancel ? cancel : "");
    if (confirm_row) *confirm_row = yes;
    if (cancel_row) *cancel_row = no;
    if (button_count < 16) {
        buttons[button_count] = yes;
        button_names[button_count++] = confirm;
        buttons[button_count] = no;
        button_names[button_count++] = cancel;
    }
    if (backdrop) {
        *backdrop = lv_obj_create(lv_layer_top());
        if (backdrop_cb) lv_obj_add_event_cb(*backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);
    }
    popup_build_count++;
    tracef("confirm-build:%s|%s\n", confirm ? confirm : "", cancel ? cancel : "");
    return popup;
}

lv_obj_t * build_menu_popup(const menu_popup_row_t * rows, int row_count,
                            lv_event_cb_t backdrop_cb, lv_obj_t ** out_backdrop) {
    lv_obj_t * menu = lv_obj_create(lv_layer_top());
    lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    int menu_index = menu_count++;
    assert(menu_index < (int)(sizeof(menus) / sizeof(menus[0])));
    menus[menu_index] = menu;
    for (int i = 0; i < row_count; ++i) {
        lv_obj_t * button = lv_btn_create(menu);
        lv_obj_add_event_cb(button, rows[i].cb, LV_EVENT_ALL, rows[i].user_data);
        lv_obj_t * label = lv_label_create(button);
        lv_label_set_text(label, rows[i].label);
    }
    if (out_backdrop) {
        *out_backdrop = lv_obj_create(lv_layer_top());
        lv_obj_add_flag(*out_backdrop, LV_OBJ_FLAG_HIDDEN);
        if (backdrop_cb) lv_obj_add_event_cb(*out_backdrop, backdrop_cb, LV_EVENT_CLICKED, NULL);
        menu_backdrops[menu_index] = *out_backdrop;
    }
    tracef("menu-build:%d\n", row_count);
    return menu;
}

static lv_obj_t * named_button(const char * label, int occurrence) {
    for (int i = 0, seen = 0; i < button_count; ++i) {
        if (strcmp(button_names[i], label) == 0 && seen++ == occurrence) return buttons[i];
    }
    assert(!"requested action button was not built");
    return NULL;
}

bool firmware_update_scan_checked(char * path, size_t path_size, char * error, size_t error_size) {
    scan_count++;
    tracef("backend:firmware-scan\n");
    snprintf(path, path_size, "/sdcard/firmware.upt");
    if (error_size) error[0] = 0;
    return true;
}
bool firmware_update_start(const char * path, char * error, size_t error_size) {
    firmware_start_count++;
    tracef("backend:firmware-start:%s\n", path);
    if (error_size) error[0] = 0;
    return firmware_start_ok;
}
void firmware_update_get_status(firmware_update_status_t * out) { *out = firmware_status; }
bool firmware_update_busy(void) { return firmware_busy_backend; }

bool firmware_ota_start_check(void) {
    ota_check_count++;
    tracef("backend:ota-check\n");
    return ota_check_ok && !ota_busy_backend;
}
bool firmware_ota_start_download(void) { ota_download_count++; tracef("backend:ota-download\n"); return ota_download_ok; }
void firmware_ota_get_status(firmware_ota_status_t * out) { *out = ota_status; }
bool firmware_ota_busy(void) { return ota_busy_backend; }
void firmware_ota_reset(void) { ota_reset_count++; memset(&ota_status, 0, sizeof(ota_status)); tracef("backend:ota-reset\n"); }
firmware_ota_pending_t firmware_ota_pending(firmware_ota_release_t * out) {
    (void)out; return FIRMWARE_OTA_PENDING_NONE;
}
bool firmware_ota_start_install(char * error, size_t error_size) {
    (void)error; (void)error_size; assert(!"OTA install must never be invoked by this UI test"); return false;
}

bool player_update_start_check(void) { player_check_count++; tracef("backend:player-check\n"); return player_check_ok; }
bool player_update_start_online_install(char * error, size_t error_size) {
    player_online_count++;
    tracef("backend:player-online-install-stub\n");
    if (error_size) error[0] = 0;
    return player_online_ok;
}
bool player_update_start_sd_install(char * error, size_t error_size) {
    (void)error; (void)error_size; player_sd_count++;
    assert(!"SD install must never be invoked by this UI test");
    return false;
}
void player_update_get_status(player_update_status_t * out) { *out = player_status; }
void player_update_reset(void) {
    player_reset_count++;
    memset(&player_status, 0, sizeof(player_status));
    tracef("backend:player-reset\n");
}

static void build_ui(void) { update_test_build_popups(); }
static void teardown_ui(void) { update_test_teardown(); }
static void click(lv_obj_t * object, lv_event_code_t code) { lv_obj_send_event(object, code, NULL); }

static void open_firmware_menu(void) {
    lv_obj_t * row = lv_btn_create(lv_layer_top());
    lv_obj_add_event_cb(row, firmware_update_row_cb, LV_EVENT_ALL, NULL);
    click(row, LV_EVENT_VALUE_CHANGED);
    assert(lv_obj_has_flag(firmware_source_menu, LV_OBJ_FLAG_HIDDEN));
    click(row, LV_EVENT_CLICKED);
    assert(!lv_obj_has_flag(firmware_source_menu, LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(row);
}

static void run_firmware_manual_and_ota(void) {
    open_firmware_menu();
    usb_blocked = true;
    click(lv_obj_get_child(firmware_source_menu, 2), LV_EVENT_CLICKED);
    assert(scan_count == 0 && error_count == 1);
    open_firmware_menu();
    usb_blocked = false;
    wifi_enabled = false;
    click(lv_obj_get_child(firmware_source_menu, 1), LV_EVENT_CLICKED);
    assert(ota_check_count == 0 && error_count == 2);
    open_firmware_menu();
    wifi_enabled = true;
    ota_busy_backend = true;
    click(lv_obj_get_child(firmware_source_menu, 1), LV_EVENT_CLICKED);
    assert(ota_check_count == 1 && error_count == 3);
    ota_busy_backend = false;

    open_firmware_menu();
    click(lv_obj_get_child(firmware_source_menu, 2), LV_EVENT_CLICKED);
    assert(scan_count == 1 && firmware_update_popup.visible);
    click(named_button("Update & Reboot", 0), LV_EVENT_CLICKED);
    assert(firmware_start_count == 1 && manual_update_ui_active && manual_update_busy);
    firmware_status.busy = true;
    firmware_status.phase = FIRMWARE_UPDATE_BOOTFLAG;
    poll_firmware_ota();
    firmware_status.busy = false;
    firmware_status.phase = FIRMWARE_UPDATE_FAILED;
    snprintf(firmware_status.error, sizeof(firmware_status.error), "mock recovery failure");
    poll_firmware_ota();
    assert(!manual_update_ui_active && error_count == 4);

    open_firmware_menu();
    click(lv_obj_get_child(firmware_source_menu, 1), LV_EVENT_CLICKED);
    assert(ota_ui_active && ota_check_count == 2);
    ota_status.state = FIRMWARE_OTA_CHECKING;
    poll_firmware_ota();
    ota_status.state = FIRMWARE_OTA_CHECKED;
    ota_status.newer = true;
    snprintf(ota_status.release.label, sizeof(ota_status.release.label), "v9.8.7");
    snprintf(ota_status.installed, sizeof(ota_status.installed), "v1.0");
    poll_firmware_ota();
    assert(!ota_ui_active && ota_offer_popup.visible);
    click(named_button("Download", 0), LV_EVENT_CLICKED);
    assert(ota_ui_active && ota_download_count == 1);
    ota_status.state = FIRMWARE_OTA_DOWNLOADING;
    ota_status.percent = 42;
    poll_firmware_ota();
    ota_status.state = FIRMWARE_OTA_READY;
    poll_firmware_ota();
    assert(!ota_ui_active && ota_install_popup.visible);
    click(named_button("Later", 0), LV_EVENT_CLICKED);
    assert(!ota_install_popup.visible && info_count == 1);

    open_firmware_menu();
    click(lv_obj_get_child(firmware_source_menu, 1), LV_EVENT_CLICKED);
    ota_status.state = FIRMWARE_OTA_FAILED;
    snprintf(ota_status.error, sizeof(ota_status.error), "mock OTA failure");
    poll_firmware_ota();
    assert(!ota_ui_active && error_count == 5);
}

static void open_player_menu(void) {
    lv_obj_t * row = lv_btn_create(lv_layer_top());
    lv_obj_add_event_cb(row, update_test_player_row_cb, LV_EVENT_ALL, NULL);
    click(row, LV_EVENT_VALUE_CHANGED);
    assert(lv_obj_has_flag(player_update_menu, LV_OBJ_FLAG_HIDDEN));
    click(row, LV_EVENT_CLICKED);
    assert(!lv_obj_has_flag(player_update_menu, LV_OBJ_FLAG_HIDDEN));
    lv_obj_delete(row);
}

static void start_player_check(void) {
    click(lv_obj_get_child(player_update_menu, 1), LV_EVENT_CLICKED);
    assert(player_update_ui_active && player_check_count > 0 && player_update_busy);
}

static void run_player_lifecycle_and_results(void) {
    open_player_menu();
    firmware_busy_backend = true;
    click(lv_obj_get_child(player_update_menu, 1), LV_EVENT_CLICKED);
    assert(player_check_count == 0 && error_count == 6);
    firmware_busy_backend = false;
    start_player_check();
    player_status.state = PLAYER_UPDATE_CHECKING;
    poll_firmware_ota();
    gui_busy_handle_t original_busy = player_update_busy;
    int resets_before = player_reset_count;
    teardown_ui();
    assert(player_update_ui_active && player_update_busy == 0);
    assert(player_reset_count == resets_before);
    assert(player_update_menu == NULL && !player_update_offer_popup.popup);
    button_count = 0;
    menu_count = 0;
    build_ui();
    poll_firmware_ota();
    assert(player_update_ui_active && player_update_busy && player_update_busy != original_busy);

    player_status.state = PLAYER_UPDATE_CHECKED;
    snprintf(player_status.release.label, sizeof(player_status.release.label), "player-v2");
    poll_firmware_ota();
    assert(!player_update_ui_active && player_update_offer_popup.visible);
    click(named_button("Download & Install", 0), LV_EVENT_CLICKED);
    assert(player_update_ui_active && player_online_count == 1);
    player_status.state = PLAYER_UPDATE_DOWNLOADING;
    player_status.percent = 67;
    poll_firmware_ota();
    player_status.state = PLAYER_UPDATE_DONE;
    poll_firmware_ota();
    assert(!player_update_ui_active && player_reset_count == resets_before + 1 && info_count == 2);

    open_player_menu();
    start_player_check();
    player_status.state = PLAYER_UPDATE_CHECKED;
    snprintf(player_status.release.label, sizeof(player_status.release.label), "player-v3");
    poll_firmware_ota();
    click(named_button("Download & Install", 0), LV_EVENT_CLICKED);
    player_status.state = PLAYER_UPDATE_FAILED;
    snprintf(player_status.error, sizeof(player_status.error), "mock player failure");
    poll_firmware_ota();
    assert(!player_update_ui_active && player_reset_count == resets_before + 2);
    assert(error_count == 7 && player_sd_count == 0);
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(240, 320);
    assert(display);
    static uint8_t display_buffer[240 * 16 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);

    build_ui();
    assert(popup_build_count == 5 && menu_count == 2);
    run_firmware_manual_and_ota();
    run_player_lifecycle_and_results();
    teardown_ui();
    assert(!player_update_ui_active && player_update_busy == 0);
    assert(firmware_update_popup.popup == NULL && ota_offer_popup.popup == NULL && player_update_offer_popup.popup == NULL);

    printf("update-ui-effects:\n%scounts:build=%d;scan=%d;firmware-start=%d;ota-check=%d;ota-download=%d;player-check=%d;player-online=%d;player-reset=%d;busy-show=%d;busy-hide=%d;errors=%d;infos=%d\n",
           trace, popup_build_count, scan_count, firmware_start_count, ota_check_count,
           ota_download_count, player_check_count, player_online_count, player_reset_count,
           busy_show_count, busy_hide_count, error_count, info_count);
    lv_display_delete(display);
    lv_deinit();
    return 0;
}
