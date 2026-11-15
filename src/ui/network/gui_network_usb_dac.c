#include <stdio.h>
#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../text/rendering/ui_label_text.h"
#include "../widgets/builders/screen_builders.h"
#include "../assets/assets.h"
#include "../../hardware/usb/usb_dac_bridge.h"
#include "gui_network.h"
#include "gui_network_internal.h"
#include "gui_network_dac_screen.h"

static lv_obj_t * usb_dac_screen;
static lv_obj_t * usb_dac_content;
static lv_obj_t * usb_dac_stream_label;
static lv_obj_t * usb_dac_stream_chip;
static bool usb_dac_showing_intent;
static bool usb_dac_showing_active;
static bool usb_dac_streaming_state;
static gui_popup_t usb_dac_stop_popup;

static void dismiss_stop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_hide(&usb_dac_stop_popup);
}

static void confirm_stop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&usb_dac_stop_popup);
    disable_usb_dac_mode();
}

static void stop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) gui_popup_show(&usb_dac_stop_popup);
}

static void enable_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) enable_usb_dac_mode();
}

static void populate_usb_dac_screen(void) {
    if (!usb_dac_content) return;
    lv_obj_clean(usb_dac_content);
    usb_dac_showing_intent = current_settings.usb_dac_mode_enabled;
    usb_dac_showing_active = gui_network_usb_dac_active();
    char breeze[96];
    const char * icon = breeze_row_icon("submenu/usb.png", breeze, sizeof(breeze)) ? breeze : "submenu/usb.png";
    gui_dac_screen_options_t options = {
        .icon = icon,
        .title = TR("USB DAC"),
        .explanation = TR("Stream audio from a computer over USB."),
        .waiting = TR("Waiting for USB audio…"),
        .enabled = usb_dac_showing_intent,
        .active = usb_dac_showing_active,
        .show_explanation = !usb_dac_showing_intent,
        .enable_label = TR("Enable USB DAC"),
        .stop_label = TR("Stop USB DAC"),
        .enable_cb = enable_cb,
        .stop_cb = stop_cb,
    };
    gui_dac_screen_t built;
    gui_network_dac_screen_build(usb_dac_content, &options, &built);
    usb_dac_stream_label = built.stream;
    usb_dac_stream_chip = built.chip;
    usb_dac_streaming_state = false;
    gui_network_dac_screen_action(&built, &options);
    gui_network_usb_dac_refresh();
}

void gui_network_open_usb_dac(void) {
    gui_network_usb_dac_build();
    populate_usb_dac_screen();
    nav_push(usb_dac_screen);
}

void gui_network_usb_dac_build(void) {
    if (usb_dac_screen) return;
    usb_dac_screen = gui_network_dac_screen_create(TR("USB DAC"), &usb_dac_content);
    gui_network_dac_screen_confirm(&usb_dac_stop_popup, TR("Leave USB DAC mode?"),
        TR("Stop USB DAC"), confirm_stop_cb, dismiss_stop_cb);
    populate_usb_dac_screen();
}

void gui_network_usb_dac_refresh(void) {
    if (!usb_dac_content) return;
    if (usb_dac_showing_intent != current_settings.usb_dac_mode_enabled ||
        usb_dac_showing_active != gui_network_usb_dac_active()) {
        populate_usb_dac_screen();
        return;
    }
    if (!usb_dac_stream_label) return;
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    if (!usb_dac_showing_intent) {
        lv_obj_add_flag(usb_dac_stream_chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    char text[128];
    if (info.streaming != usb_dac_streaming_state) {
        usb_dac_streaming_state = info.streaming;
        if (info.streaming) lv_obj_add_style(usb_dac_stream_label, gui_theme_accent_style(), 0);
        else lv_obj_remove_style(usb_dac_stream_label, gui_theme_accent_style(), 0);
    }
    if (info.output_failed) {
        snprintf(text, sizeof(text), "%s", TR("Playback error: audio output failed"));
    } else if (!info.streaming) {
        snprintf(text, sizeof(text), "%s", TR("Waiting for USB audio…"));
    } else {
        char rate[32];
        format_rate(rate, sizeof(rate), info.input_sample_rate);
        snprintf(text, sizeof(text), "%s · %s · %u-bit", TR("Receiving"), rate, info.input_bit_depth);
    }
    if (!ui_label_text_matches(usb_dac_stream_label, text)) lv_label_set_text(usb_dac_stream_label, text);
}

void gui_network_usb_dac_teardown(void) {
    gui_popup_teardown(&usb_dac_stop_popup);
    network_delete_screen(&usb_dac_screen);
    usb_dac_content = usb_dac_stream_label = usb_dac_stream_chip = NULL;
}
