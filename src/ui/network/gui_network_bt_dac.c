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
#include "gui_network_dac_screen.h"
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
static bool bt_enable_pending;
static bool showing_enabled;
static bool showing_receiving;
static int showing_stream_state;
static int showing_controls;
static char showing_phone[sizeof(((bt_dac_stream_info_t *) 0)->device_name)];

void disable_bt_dac_mode(void) {
    bt_enable_pending = false;
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

void enable_bt_dac_mode(void) {
    gui_shell_bt_dac_remember(false);
    if (!bt_is_powered_cached) {
        bt_enable_pending = false;
        show_error_toast(TR("Enable Bluetooth in Wireless or the Quick Drawer to use BT DAC mode"));
        return;
    }
    if (current_settings.wifi_dac_mode_enabled || gui_network_airplay_enable_pending())
        disable_airplay_mode();
    /* The newest DAC choice wins; the session waits for the port to leave USB DAC. */
    if (current_settings.usb_dac_mode_enabled || current_settings.usb_mode == USB_MODE_DAC ||
        gui_network_usb_dac_pending())
        disable_usb_dac_mode();
    if (gui_network_airplay_stop_pending()) {
        bt_enable_pending = true;
        return;
    }
    bt_enable_pending = false;
    gui_shell_cancel_bt_reconnect();
    if (gui_shell_is_bt_audio_connected()) gui_player_stop_for_external_session();
    current_settings.bt_dac_mode_enabled = true;
    settings_save_async(&current_settings);
    start_bt_apply_output_settings(true, current_settings.bt_volume_sync_enabled);
    populate_bt_dac_screen();
    gui_network_extend_bt_pairing_window();
}

void gui_network_bt_dac_poll_enable(void) {
    if (bt_enable_pending && !gui_network_airplay_stop_pending()) enable_bt_dac_mode();
}

/* On and not paused by a Bluetooth power-off or the sleep timer. */
bool gui_network_bt_dac_active(void) {
    return bt_enable_pending || (current_settings.bt_dac_mode_enabled && !gui_shell_bt_dac_suspended());
}

bool gui_network_toggle_bt_dac(void) {
    if (gui_network_bt_dac_active()) disable_bt_dac_mode();
    else enable_bt_dac_mode();
    gui_shell_refresh_quick_drawer_expansion_toggles();
    return gui_network_bt_dac_active();
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

void gui_network_bt_dac_refresh(void) {
    if (!bt_dac_content) return;
    if (showing_enabled != gui_network_bt_dac_active()) populate_bt_dac_screen();
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

void populate_bt_dac_screen(void) {
    if (!bt_dac_content) return;
    lv_obj_clean(bt_dac_content);
    stream_label = stream_chip = phone_label = play_button = transport_row = NULL;
    showing_enabled = gui_network_bt_dac_active();
    showing_stream_state = showing_controls = -1;
    showing_receiving = false;
    showing_phone[0] = '\0';
    char breeze[96];
    const char * icon = breeze_row_icon("wireless/list_bt.png", breeze, sizeof(breeze))
        ? breeze : row_icon_or("wireless/list_bt.png", "bt/bt.png");
    gui_dac_screen_options_t options = {
        .icon = icon,
        .title = TR("Bluetooth DAC"),
        .explanation = TR("Stream audio from another device over Bluetooth."),
        .waiting = showing_enabled ? TR("Waiting for a device") : TR("Playback error: audio output failed"),
        .enable_label = TR("Enable Bluetooth DAC"),
        .stop_label = TR("Stop Bluetooth DAC"),
        .enabled = showing_enabled,
        .active = showing_enabled,
        .show_explanation = !showing_enabled,
        .enable_cb = enable_cb,
        .stop_cb = stop_cb,
        .transport = showing_enabled,
        .transport_cb = transport_cb,
    };
    gui_dac_screen_t built;
    gui_network_dac_screen_build(bt_dac_content, &options, &built);
    stream_label = built.stream;
    stream_chip = built.chip;
    phone_label = built.title;
    gui_network_dac_screen_action(&built, &options);
    transport_row = built.transport;
    play_button = built.play;
    gui_network_bt_dac_refresh();
}

void bt_dac_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!bt_is_powered_cached) {
        bt_enable_pending = false;
        show_error_toast(TR("Enable Bluetooth in Wireless or the Quick Drawer to use BT DAC mode"));
        return;
    }
    gui_network_open_bt_dac();
}

void gui_network_bt_dac_build(void) {
    bt_dac_screen = gui_network_dac_screen_create(TR("Bluetooth DAC"), &bt_dac_content);
    gui_network_dac_screen_confirm(&stop_popup, TR("Leave Bluetooth DAC mode?"),
        TR("Stop Bluetooth DAC"), confirm_cb, dismiss_cb);
    populate_bt_dac_screen();
}

void gui_network_bt_dac_teardown(void) {
    gui_popup_teardown(&stop_popup);
    network_delete_screen(&bt_dac_screen);
    bt_dac_content = stream_label = stream_chip = phone_label = play_button = transport_row = NULL;
}

lv_obj_t * gui_network_get_bt_dac_screen(void) { return bt_dac_screen; }
