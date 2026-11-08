#include <stdio.h>
#include <string.h>
#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../text/rendering/ui_label_text.h"
#include "../widgets/builders/screen_builders.h"
#include "../widgets/builders/gui_play_bar.h"
#include "../assets/assets.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#ifndef HOST_BUILD
#include "../../network/bluetooth/media/bt_media_player.h"
#endif

static lv_obj_t * bt_dac_screen;
static lv_obj_t * bt_dac_content;
static lv_obj_t * stream_label;
static lv_obj_t * stream_chip;
static lv_obj_t * phone_label;
static lv_obj_t * play_button;
static lv_obj_t * transport_row;
static gui_popup_t stop_popup;
static bool showing_enabled;
static bool showing_receiving;
static int showing_stream_state;
static int showing_controls;
static char showing_phone[sizeof(((bt_dac_stream_info_t *) 0)->device_name)];

void disable_bt_dac_mode(void) {
#ifndef HOST_BUILD
    bt_media_player_set_phone_control_enabled(false);
#endif
    gui_shell_bt_dac_release();
    gui_shell_bt_dac_remember(false);
    current_settings.bt_dac_mode_enabled = false;
    settings_save_async(&current_settings);
    start_bt_apply_output_settings(false, current_settings.bt_volume_sync_enabled);
    populate_bt_dac_screen();
}

static void enable_bt_dac_mode(void) {
    gui_shell_bt_dac_remember(false);
    if (!bt_is_powered_cached) {
        show_error_toast(TR("Enable Bluetooth in settings to use BT DAC mode"));
        return;
    }
    if (current_settings.usb_mode == USB_MODE_DAC || gui_network_usb_dac_pending()) {
        show_error_toast(TR("Exit USB DAC mode to play music on this device"));
        return;
    }
    if (current_settings.wifi_dac_mode_enabled || gui_network_airplay_enable_pending())
        disable_airplay_mode();
    gui_shell_cancel_bt_reconnect();
    if (gui_shell_is_bt_audio_connected()) gui_player_stop_for_external_session();
    current_settings.bt_dac_mode_enabled = true;
    settings_save_async(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    populate_bt_dac_screen();
    gui_network_extend_bt_pairing_window();
}

bool gui_network_toggle_bt_dac(void) {
    if (current_settings.bt_dac_mode_enabled) disable_bt_dac_mode();
    else enable_bt_dac_mode();
    gui_shell_refresh_quick_drawer_expansion_toggles();
    return current_settings.bt_dac_mode_enabled;
}

static void enable_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) enable_bt_dac_mode();
}

void gui_network_open_bt_dac(void) {
    populate_bt_dac_screen();
    nav_push(bt_dac_screen);
    gui_network_extend_bt_pairing_window();
}

static void dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&stop_popup);
}

static void confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&stop_popup);
    disable_bt_dac_mode();
}

static void stop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_show(&stop_popup);
}

static void transport_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bt_dac_stream_info_t info;
    bt_control_get_dac_stream_info(&info);
    if (!info.mac[0] || !gui_shell_bt_dac_profile_ready()) return;
#ifndef HOST_BUILD
    bt_media_player_control_phone((int) (intptr_t) lv_event_get_user_data(e));
#endif
}

static lv_obj_t * transport_button(lv_obj_t * row, const char * asset, int direction) {
    int32_t size = BOARD_SCALE_PX(direction ? 40 : 84);
    lv_obj_t * button = lv_image_create(row);
    lv_image_set_src(button, asset_path(asset));
    lv_image_set_inner_align(button, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_style(button, &style_theme_text_primary, 0);
    lv_obj_add_style(button, &icon_press_style, LV_STATE_PRESSED);
    lv_obj_set_size(button, size, size);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, transport_cb, LV_EVENT_CLICKED, (void *) (intptr_t) direction);
    lv_obj_set_ext_click_area(button, BOARD_SCALE_PX(12));
    return button;
}

void gui_network_bt_dac_refresh(void) {
    if (!bt_dac_content) return;
    if (showing_enabled != current_settings.bt_dac_mode_enabled) populate_bt_dac_screen();
    if (!stream_label) return;
    if (!showing_enabled) {
        lv_obj_set_flag(stream_chip, LV_OBJ_FLAG_HIDDEN, !gui_shell_bt_dac_failed());
        return;
    }
    bt_dac_stream_info_t info;
    bt_control_get_dac_stream_info(&info);
    bool controls_enabled = info.mac[0] && gui_shell_bt_dac_profile_ready();
    if (showing_controls != controls_enabled) {
        showing_controls = controls_enabled;
        for (uint32_t i = 0; i < lv_obj_get_child_count(transport_row); ++i) {
            lv_obj_t * button = lv_obj_get_child(transport_row, i);
            lv_obj_set_state(button, LV_STATE_DISABLED, !controls_enabled);
            lv_obj_set_style_opa(button, controls_enabled ? LV_OPA_COVER : LV_OPA_40, 0);
        }
    }
    int state = gui_shell_bt_dac_failed() ? 2 : gui_shell_bt_dac_receiving() ? 1 : 0;
    if (showing_stream_state != state) {
        showing_stream_state = state;
        lv_obj_remove_style(stream_label, gui_theme_accent_style(), 0);
        if (state == 1) lv_obj_add_style(stream_label, gui_theme_accent_style(), 0);
    }
    char text[160];
    if (state == 2) {
        snprintf(text, sizeof(text), "%s", TR("Playback error: audio output failed"));
    } else if (state == 0) {
        snprintf(text, sizeof(text), "%s", TR("Waiting for a device"));
    } else {
        char rate[32];
        format_rate(rate, sizeof(rate), info.sample_rate);
        snprintf(text, sizeof(text), "%s · %s · %s", TR("Receiving"),
                 info.codec[0] ? info.codec : TR("Unknown codec"), rate);
    }
    if (!ui_label_text_matches(stream_label, text)) lv_label_set_text(stream_label, text);
    /* LONG_DOT mutates the label buffer; compare the unabridged device name. */
    if (strcmp(showing_phone, info.device_name) != 0) {
        snprintf(showing_phone, sizeof(showing_phone), "%s", info.device_name);
        lv_label_set_text(phone_label, showing_phone[0] ? showing_phone : TR("Bluetooth DAC"));
    }
    bool receiving = info.running && gui_shell_bt_dac_owns_output();
    if (receiving != showing_receiving) {
        showing_receiving = receiving;
        lv_image_set_src(play_button, asset_path(receiving ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png"));
    }
}

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

static void build_hero(lv_obj_t * hero) {
    char breeze[96];
    const char * icon_asset = breeze_row_icon("wireless/list_bt.png", breeze, sizeof(breeze))
        ? breeze : row_icon_or("wireless/list_bt.png", "bt/bt.png");
    lv_obj_t * icon = lv_image_create(hero);
    lv_image_set_src(icon, asset_path(icon_asset));
    lv_obj_set_size(icon, BOARD_SCALE_PX(112), BOARD_SCALE_PX(112));
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_add_style(icon, showing_enabled ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    if (!showing_enabled) lv_obj_set_style_opa(icon, LV_OPA_40, 0);
    phone_label = centered_label(hero, TR("Bluetooth DAC"), GUI_FONT_ROLE_TITLE);
    lv_obj_add_style(phone_label, &style_theme_text_primary, 0);
    lv_label_set_long_mode(phone_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(phone_label, lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_TITLE)));
}

static void build_stream_chip(lv_obj_t * hero) {
    stream_chip = plain_column(hero);
    lv_obj_set_size(stream_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(stream_chip, LV_PCT(100), 0);
    lv_obj_add_style(stream_chip, &style_theme_card_bg, 0);
    lv_obj_set_style_bg_opa(stream_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stream_chip, BOARD_SCALE_PX(20), 0);
    lv_obj_set_style_pad_hor(stream_chip, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_pad_ver(stream_chip, BOARD_SCALE_PX(8), 0);
    stream_label = centered_label(stream_chip, showing_enabled ? TR("Waiting for a device")
        : TR("Playback error: audio output failed"), GUI_FONT_ROLE_SUBTEXT);
    lv_obj_set_width(stream_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(stream_label, BOARD_SCREEN_WIDTH - BOARD_SCALE_PX(80), 0);
}

static void build_transport(lv_obj_t * hero) {
    transport_row = plain_column(hero);
    lv_obj_set_flex_flow(transport_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(transport_row, BOARD_SCALE_PX(48), 0);
    lv_obj_set_style_pad_top(transport_row, BOARD_SCALE_PX(16), 0);
    transport_button(transport_row, "playing_plane/btn_prev.png", -1);
    play_button = transport_button(transport_row, "playing_plane/btn_play.png", 0);
    transport_button(transport_row, "playing_plane/btn_next.png", 1);
    showing_receiving = false;
}

static void build_action(void) {
    if (!showing_enabled) {
        gui_pill_button(bt_dac_content, BOARD_SCALE_PX(320), BOARD_SCALE_PX(56),
                        TR("Enable Bluetooth DAC"), true, enable_cb, NULL);
        return;
    }
    lv_obj_t * stop = centered_label(bt_dac_content, TR("Stop Bluetooth DAC"), GUI_FONT_ROLE_BODY);
    lv_obj_set_style_text_color(stop, lv_color_hex(GUI_BREEZE_NEGATIVE), 0);
    lv_obj_set_style_pad_ver(stop, BOARD_SCALE_PX(16), 0);
    lv_obj_set_style_opa(stop, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(stop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_CLICKED, NULL);
}

void populate_bt_dac_screen(void) {
    if (!bt_dac_content) return;
    lv_obj_clean(bt_dac_content);
    stream_label = stream_chip = phone_label = play_button = transport_row = NULL;
    showing_enabled = current_settings.bt_dac_mode_enabled;
    showing_stream_state = showing_controls = -1;
    showing_phone[0] = '\0';
    lv_obj_t * hero = plain_column(bt_dac_content);
    lv_obj_set_flex_grow(hero, 1);
    build_hero(hero);
    if (!showing_enabled) {
        centered_label(hero, TR("Stream audio from another device over Bluetooth."), GUI_FONT_ROLE_SUBTEXT);
    }
    build_stream_chip(hero);
    if (showing_enabled) build_transport(hero);
    build_action();
    gui_network_bt_dac_refresh();
}

void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!bt_is_powered_cached) {
        show_error_toast(TR("Enable Bluetooth in settings to use BT DAC mode"));
        return;
    }
    gui_network_open_bt_dac();
}

void gui_network_bt_dac_build(void) {
    bt_dac_screen = lv_obj_create(NULL);
    lv_obj_add_style(bt_dac_screen, &style_theme_screen_bg, 0);
    build_screen_header(bt_dac_screen, TR("Bluetooth DAC"), generic_back_cb, NULL, NULL);
    bt_dac_content = plain_column(bt_dac_screen);
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    lv_obj_set_pos(bt_dac_content, 0, top);
    lv_obj_set_height(bt_dac_content, BOARD_SCREEN_HEIGHT - top - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_pad_hor(bt_dac_content, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_ver(bt_dac_content, BOARD_SCALE_PX(20), 0);
    finalize_screen_navigation(bt_dac_screen);
    stop_popup.popup = build_confirm_popup(TR("Leave Bluetooth DAC mode?"), LV_LABEL_LONG_WRAP,
        NULL, NULL, TR("Stop Bluetooth DAC"), lv_color_hex(GUI_BREEZE_NEGATIVE), confirm_cb,
        NULL, TR("Cancel"), accent_lv_color(), dismiss_cb, NULL, dismiss_cb, &stop_popup.backdrop);
    populate_bt_dac_screen();
}

void gui_network_bt_dac_teardown(void) {
    gui_popup_teardown(&stop_popup);
    network_delete_screen(&bt_dac_screen);
    bt_dac_content = stream_label = stream_chip = phone_label = play_button = transport_row = NULL;
}

lv_obj_t * gui_network_get_bt_dac_screen(void) { return bt_dac_screen; }
