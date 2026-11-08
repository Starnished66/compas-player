#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "gui_network.h"
#include "../widgets/builders/screen_builders.h"
#include "../../network/bluetooth/control/bt_pairing_agent.h"
#include "../../network/bluetooth/reconnect/bluetooth_reconnect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "gui_network_internal.h"

extern bt_device_t bt_scan_results[];
extern int bt_scan_result_count;

static lv_obj_t * bt_screen;
static lv_obj_t * bt_codec_screen;
static lv_obj_t * bt_advanced_screen;
static lv_obj_t * bt_rate_screen;
static void start_bt_scan(bool inquiry);

/* ---- Bluetooth settings screen ----
 *
 * Same shape as the Wi-Fi screen: full scan + pair/connect, reachable
 * from the Wireless submenu's Bluetooth tile and from a long-press on the
 * drawer's bt icon. bt_control_scan() blocks for its whole scan window
 * itself (bluetoothctl's own --timeout), so the scan thread has no separate
 * sleep to add, unlike the Wi-Fi scan thread. */

static lv_obj_t * bt_list;


static pthread_t bt_connect_thread;
static bool bt_connect_active = false;
static atomic_bool bt_connect_done_flag = false;
static volatile bool bt_connect_succeeded = false;

/* Which device's row (see add_bt_device_row()) should show inline
 * "Connecting..."/"Failed to connect" status -- an earlier version of this
 * blocked the whole screen with a "Pairing & connecting..." overlay
 * instead, same class of issue as the scan/toggle overlays removed
 * elsewhere in this file (see start_wifi_scan()'s comment): running fully
 * in the background with inline per-row feedback, matching the stock
 * player, means there's no screen to get stuck on at all. */
/* How long the pairing agent accepts requests after the user's last Bluetooth action. */
#define BT_PAIRING_WINDOW_MS 60000

static char bt_connecting_mac[18] = "";
static char bt_connect_failed_mac[18] = "";

static void bt_apply_connection_result_to_snapshot(const char * mac, bool succeeded) {
    if (!mac || !mac[0]) return;
    for (int i = 0; i < bt_scan_result_count; ++i) {
        if (strcasecmp(bt_scan_results[i].mac, mac) != 0) continue;
        bt_scan_results[i].paired = succeeded || bt_scan_results[i].paired;
        bt_scan_results[i].connected = succeeded;
    }
}

static void bt_clear_snapshot_state_for_mac(const char * mac) {
    if (!mac || !mac[0]) return;
    for (int i = 0; i < bt_scan_result_count; ++i) {
        if (strcasecmp(bt_scan_results[i].mac, mac) != 0) continue;
        bt_scan_results[i].paired = false;
        bt_scan_results[i].connected = false;
    }
}

typedef struct {
    char mac[18];
    unsigned int rate;
} bt_connect_request_t;

static void * bt_connect_thread_func(void * arg) {
    bt_connect_request_t * req = (bt_connect_request_t *) arg;
    bt_connect_succeeded = bt_control_connect_with_rate(req->mac, req->rate);
    free(req);
    atomic_store_explicit(&bt_connect_done_flag, true, memory_order_release); /* written last -- poll_bt_connect only checks this flag */
    return NULL;
}

static void start_bt_connect(const char * mac) {
    if (current_settings.bt_dac_mode_enabled) return;
    if (bt_connect_active) return; /* one at a time -- bt_connect_thread/succeeded are single shared slots */

    bt_connect_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    req->rate = settings_bt_rate_for(&current_settings, req->mac);

    atomic_store_explicit(&bt_connect_done_flag, false, memory_order_relaxed);
    bt_connect_active = true;
    bt_pairing_agent_allow_pairing_for(BT_PAIRING_WINDOW_MS); /* connecting an unpaired device pairs it */
    snprintf(bt_connecting_mac, sizeof(bt_connecting_mac), "%s", mac);
    bt_connect_failed_mac[0] = '\0';
    populate_bt_screen(); /* immediate inline "Connecting..." on that row */

        if (pthread_create(&bt_connect_thread, NULL, bt_connect_thread_func, req) != 0) {
        bt_connect_active = false;
        free(req);
        bt_connecting_mac[0] = '\0'; /* rows prefer this text, so drop the stale "Connecting..." */
        populate_bt_screen();
    }
}

void poll_bt_connect(void) {
    if (!bt_connect_active || !atomic_load_explicit(&bt_connect_done_flag, memory_order_acquire)) return;

    bt_connect_active = false;
    char connected_mac[18];
    snprintf(connected_mac, sizeof(connected_mac), "%s", bt_connecting_mac);
    pthread_join(bt_connect_thread, NULL);
    if (!bt_connect_succeeded) {
        snprintf(bt_connect_failed_mac, sizeof(bt_connect_failed_mac), "%s", connected_mac);
    }
    bt_apply_connection_result_to_snapshot(connected_mac, bt_connect_succeeded);
    bt_connecting_mac[0] = '\0';
    populate_bt_screen();
}

static pthread_t bt_forget_thread;
static bool bt_forget_active = false;
static atomic_bool bt_forget_done_flag = false;

typedef struct {
    char mac[18];
    bool succeeded;
} bt_forget_request_t;

/* Kept alive past the thread completing (freed in poll_bt_forget() instead
 * of the thread func itself) so the completion handler still knows which
 * device to update in bt_scan_results -- see poll_bt_forget()'s own
 * comment for why. */
static bt_forget_request_t * bt_forget_pending_req = NULL;

static void * bt_forget_thread_func(void * arg) {
    bt_forget_request_t * req = (bt_forget_request_t *) arg;
    req->succeeded = bt_control_forget(req->mac);
    atomic_store_explicit(&bt_forget_done_flag, true, memory_order_release); /* written last -- poll_bt_forget only checks this flag */
    return NULL;
}

static void start_bt_forget(const char * mac) {
    if (bt_forget_active) return;
    gui_shell_cancel_bt_reconnect();
    bt_forget_request_t * req = malloc(sizeof(*req));
    if (!req) return;
    snprintf(req->mac, sizeof(req->mac), "%s", mac);
    bt_forget_pending_req = req;

    atomic_store_explicit(&bt_forget_done_flag, false, memory_order_relaxed);
    bt_forget_active = true;
        if (pthread_create(&bt_forget_thread, NULL, bt_forget_thread_func, req) != 0) {
        bt_forget_active = false;
        bt_forget_pending_req = NULL;
        free(req);
    }
}

/* Updates bt_scan_results locally and redraws the Bluetooth screen upon
 * forgetting a device without running a full background scan. */
void poll_bt_forget(void) {
    if (!bt_forget_active || !atomic_load_explicit(&bt_forget_done_flag, memory_order_acquire)) return;
    bt_forget_active = false;
    pthread_join(bt_forget_thread, NULL);

    if (bt_forget_pending_req->succeeded) {
        /* A refresh launched after the initial manual cancellation may still
         * have observed the device before Forget completed. Invalidate that
         * positive snapshot before changing the remembered preference. */
        gui_shell_cancel_bt_reconnect();
        bt_clear_snapshot_state_for_mac(bt_forget_pending_req->mac);
        if (strcasecmp(current_settings.bt_last_output_mac, bt_forget_pending_req->mac) == 0) {
            current_settings.bt_last_output_mac[0] = '\0';
            settings_save_async(&current_settings);
        }
    }
    free(bt_forget_pending_req);
    bt_forget_pending_req = NULL;
    populate_bt_screen();
}

/* ---- Bluetooth device action popup --------------------------------------
 * Displays an action popup offering Connect or Forget based on the device's
 * current paired/connected state. */
static gui_popup_t bt_action_popup;
static lv_obj_t * bt_action_popup_title;
static lv_obj_t * bt_action_connect_row;
static lv_obj_t * bt_action_forget_row;
static char bt_action_popup_device_mac[18] = "";

static void hide_bt_action_popup(void) {
    gui_popup_hide(&bt_action_popup);
    bt_action_popup_device_mac[0] = '\0';
}

static void bt_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_bt_action_popup();
}

/* Copy the popup's device MAC into out and close the popup. Returns false
 * (after closing) when no valid MAC is selected. */
static bool bt_action_take_mac(char out[18]) {
    if (strlen(bt_action_popup_device_mac) != 17) {
        hide_bt_action_popup();
        return false;
    }
    snprintf(out, 18, "%s", bt_action_popup_device_mac);
    hide_bt_action_popup();
    return true;
}

static void bt_action_connect_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char mac[18];
    if (!bt_action_take_mac(mac)) return;
    gui_shell_cancel_bt_reconnect();
    start_bt_connect(mac);
}

static void bt_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char mac[18];
    if (!bt_action_take_mac(mac)) return;
    start_bt_forget(mac);
}

static void build_bt_action_popup(void) {
    bt_action_popup.popup = build_confirm_popup("", LV_LABEL_LONG_DOT, &bt_action_popup_title, NULL, TR("Connect"),
                                                 accent_lv_color(), bt_action_connect_cb, &bt_action_connect_row, TR("Forget"),
                                                 lv_color_hex(GUI_BREEZE_NEGATIVE), bt_action_forget_cb, &bt_action_forget_row,
                                                 bt_action_popup_backdrop_cb, &bt_action_popup.backdrop);
}

static void show_bt_action_popup(int index) {
    if (index < 0 || index >= bt_scan_result_count) return;
    bt_device_t * dev = &bt_scan_results[index];
    snprintf(bt_action_popup_device_mac, sizeof(bt_action_popup_device_mac), "%s", dev->mac);

    lv_label_set_text(bt_action_popup_title, dev->name[0] != '\0' ? dev->name : dev->mac);

    /* Connect makes sense unless already connected. Forget makes sense for
     * anything paired OR connected (the "OR connected" is just a safety net
     * -- a connected-but-somehow-not-paired device shouldn't be able to land
     * on a popup with neither action available). */
    bool show_connect = !dev->connected;
    bool show_forget = dev->paired || dev->connected;
    /* No manual reposition needed -- bt_action_popup's rows are flex-column
     * children now (build_confirm_popup()), so hiding one lets the other
     * reflow up to take its place automatically. */
    if (show_connect) lv_obj_remove_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_connect_row, LV_OBJ_FLAG_HIDDEN);
    if (show_forget) lv_obj_remove_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(bt_action_forget_row, LV_OBJ_FLAG_HIDDEN);

    gui_popup_show(&bt_action_popup);
}

/* The device each row was built for: a rebuild deferred under a finger can leave a row in
 * place after bt_scan_results was replaced, so a click resolves the row's device by MAC. */
static char bt_row_macs[BT_MAX_RESULTS][18];

static void bt_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || index >= BT_MAX_RESULTS) return;
    const char * mac = bt_row_macs[index];
    if (index >= bt_scan_result_count || strcasecmp(bt_scan_results[index].mac, mac) != 0) {
        index = -1;
        for (int i = 0; i < bt_scan_result_count; i++)
            if (strcasecmp(bt_scan_results[i].mac, mac) == 0) { index = i; break; }
    }
    show_bt_action_popup(index);
}

static void bt_rescan_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* bt_scan_results contains both paired devices and transient discovery
     * results. Preserve the former (pairing is persistent state), but drop
     * every old Available Devices entry before beginning a new discovery
     * window. The completed scan replaces this compacted set as usual. */
    int paired_count = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (!bt_scan_results[i].paired) continue;
        if (paired_count != i) bt_scan_results[paired_count] = bt_scan_results[i];
        paired_count++;
    }
    bt_scan_result_count = paired_count;
    bt_pairing_agent_allow_pairing_for(BT_PAIRING_WINDOW_MS);
    populate_bt_screen();
    start_bt_scan(true);
}

static lv_obj_t * bt_rescan_btn;

static void set_bt_rescan_active(bool active) {
    if (!bt_rescan_btn) return;
    set_header_refresh_action_busy(bt_rescan_btn, active);
}

/* Checks if the device name is a MAC address placeholder (e.g. "5D-C4-96-7E-56-88")
 * substituted by bluetoothctl when no advertised name is present. */
static bool bt_name_is_mac_placeholder(const bt_device_t * dev) {
    if (dev->name[0] == '\0') return true;
    size_t mac_len = strlen(dev->mac);
    if (strlen(dev->name) != mac_len) return false;
    for (size_t i = 0; i < mac_len; i++) {
        char m = dev->mac[i];
        char n = dev->name[i];
        if (m == ':') { if (n != '-' && n != ':') return false; }
        else if (m != n) return false;
    }
    return true;
}

static int bt_device_state_rank(const bt_device_t * dev) {
    return dev->connected ? 2 : dev->paired ? 1 : 0;
}

/* bluetoothctl's device output can contain the same MAC more than once.
 * Choose one row identity without changing the backend snapshot or the
 * callback index, preferring live connection state, then pairing state,
 * then a useful advertised name. */
static int bt_preferred_device_index(int index) {
    const bt_device_t * target = &bt_scan_results[index];
    int preferred = index;
    for (int i = 0; i < bt_scan_result_count; ++i) {
        if (strcasecmp(bt_scan_results[i].mac, target->mac) != 0) continue;
        int candidate_rank = bt_device_state_rank(&bt_scan_results[i]);
        int preferred_rank = bt_device_state_rank(&bt_scan_results[preferred]);
        if (candidate_rank > preferred_rank ||
            (candidate_rank == preferred_rank && bt_name_is_mac_placeholder(&bt_scan_results[preferred]) &&
             !bt_name_is_mac_placeholder(&bt_scan_results[i])) ||
            (candidate_rank == preferred_rank &&
             bt_name_is_mac_placeholder(&bt_scan_results[preferred]) ==
             bt_name_is_mac_placeholder(&bt_scan_results[i]) && i < preferred)) {
            preferred = i;
        }
    }
    return preferred;
}

static void bt_merged_device_state(const char * mac, bool * paired, bool * connected) {
    *paired = false;
    *connected = false;
    for (int i = 0; i < bt_scan_result_count; ++i) {
        if (strcasecmp(bt_scan_results[i].mac, mac) != 0) continue;
        *paired |= bt_scan_results[i].paired;
        *connected |= bt_scan_results[i].connected;
    }
}

/* Extra row height (beyond LIST_ROW_WIDTH's normal LIST_ROW_HEIGHT) when a
 * row shows the codec subtitle line -- one more small-font line plus a
 * little breathing room, not a full second LIST_ROW_HEIGHT line. */
#define BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT BOARD_SCALE_PX(40)

static void add_bt_device_row(lv_obj_t * parent, int index) {
    bt_device_t * dev = &bt_scan_results[index];
    if (index >= 0 && index < BT_MAX_RESULTS)
        snprintf(bt_row_macs[index], sizeof(bt_row_macs[index]), "%s", dev->mac);

    /* Codec is only meaningful for the ONE device actually carrying A2DP
     * audio right now -- dev->connected alone isn't enough (a paired,
     * link-level-connected non-audio BLE peripheral would also read
     * connected=true here), so this also checks the row's own MAC against
     * bt_connected_mac_cached, the accessory bt_control_get_connected_
     * device_mac()/_codec() actually queried. Both come from the same
     * background poll as the rest of this screen's live state (see
     * refresh_bt_icon_thread_func()) -- never a synchronous bluealsactl
     * call here on the UI thread. */
    bool device_paired, device_connected;
    bt_merged_device_state(dev->mac, &device_paired, &device_connected);
    bool show_codec = device_connected && bt_connected_codec_cached[0] != '\0' &&
                       strcasecmp(dev->mac, bt_connected_mac_cached) == 0;
    const lv_font_t * body_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    const lv_font_t * subtext_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    const lv_font_t * text_fonts[3] = {body_font, subtext_font, subtext_font};
    int text_line_count = show_codec ? 3 : 2;

    lv_obj_t * row = network_row_create(parent,
        network_row_text_height(ui_list_row_height() +
                                 (show_codec ? BT_DEVICE_ROW_CODEC_EXTRA_HEIGHT : 0),
                                 text_fonts, text_line_count),
        bt_row_click_cb, (void *) (intptr_t) index);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, dev->name[0] != '\0' ? dev->name : dev->mac);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    configure_network_row_line(label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET, body_font);

    lv_obj_t * status_label = lv_label_create(row);
    const char * status = strcasecmp(dev->mac, bt_connecting_mac) == 0 ? TR("Connecting") :
                          strcasecmp(dev->mac, bt_connect_failed_mac) == 0 ? TR("Connection failed") :
                          device_connected ? TR("Connected") : device_paired ? TR("Paired") : TR("Available");
    lv_label_set_text(status_label, status);
    lv_obj_add_style(status_label, device_connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
    configure_network_row_line(status_label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET, subtext_font);

    lv_obj_t * text_lines[3] = {label, status_label, NULL};
    if (show_codec) {
        lv_obj_t * codec_label = lv_label_create(row);
        /* Codec and rate together: the negotiated link is not necessarily the
         * codec preference in Settings, and the frequency is the other half of
         * what tells you which link you actually got. Rate is omitted rather
         * than guessed when bluealsa does not report one. */
        char codec_text[96]; /* codec (32) + separator + formatted rate */
        if (bt_connected_rate_cached) {
            char rate[32];
            format_rate(rate, sizeof(rate), bt_connected_rate_cached);
            snprintf(codec_text, sizeof(codec_text), "%s · %s", bt_connected_codec_cached, rate);
        } else {
            snprintf(codec_text, sizeof(codec_text), "%s", bt_connected_codec_cached);
        }
        lv_label_set_text(codec_label, codec_text);
        lv_obj_add_style(codec_label, &style_theme_text_muted, 0);
        configure_network_row_line(codec_label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET, subtext_font);
        text_lines[2] = codec_label;
    }

    align_network_row_text_stack(row, text_lines, text_fonts, text_line_count,
                                 LIST_ROW_LABEL_INSET);
}

static pthread_t bt_scan_thread;
static bool bt_scan_active = false;
static atomic_bool bt_scan_done_flag = false;
static bt_device_t bt_scan_pending_results[BT_MAX_RESULTS];
static int bt_scan_pending_result_count = 0;

/* Set before the thread starts, so the write happens-before the read. */
static bool bt_scan_want_inquiry = true;

static void * bt_scan_thread_func(void * arg) {
    (void) arg;
    bt_scan_pending_result_count = bt_scan_want_inquiry
        ? bt_control_scan(6, bt_scan_pending_results, BT_MAX_RESULTS)
        : bt_control_list_devices(bt_scan_pending_results, BT_MAX_RESULTS);
    atomic_store_explicit(&bt_scan_done_flag, true, memory_order_release); /* written last -- poll_bt_scan only checks this flag */
    return NULL;
}

/* Runs Bluetooth scan in a background thread without navigation or busy screen.
 * With inquiry false it only re-reads the devices BlueZ already knows, which
 * leaves an active A2DP stream alone. */
static void start_bt_scan(bool inquiry) {
    if (bt_scan_active) return;

    bt_scan_want_inquiry = inquiry;

    atomic_store_explicit(&bt_scan_done_flag, false, memory_order_relaxed);
    bt_scan_active = true;
    set_bt_rescan_active(true);

    if (pthread_create(&bt_scan_thread, NULL, bt_scan_thread_func, NULL) != 0) {
        bt_scan_active = false;
        set_bt_rescan_active(false);
    }
}

void poll_bt_scan(void) {
    if (!bt_scan_active || !atomic_load_explicit(&bt_scan_done_flag, memory_order_acquire)) return;

    bt_scan_active = false;
    pthread_join(bt_scan_thread, NULL);
    set_bt_rescan_active(false);
    if (bt_scan_pending_result_count >= 0 && bt_scan_pending_result_count <= BT_MAX_RESULTS) {
        memcpy(bt_scan_results, bt_scan_pending_results,
               (size_t) bt_scan_pending_result_count * sizeof(bt_scan_results[0]));
        bt_scan_result_count = bt_scan_pending_result_count;
    }
    populate_bt_screen(); /* refresh the list either way -- worth keeping current even if the user already backed out */
}

/* ---- Codec selection screen -- single-select list, current choice shown
 * via an accent-colored border rather than a checkmark glyph (same
 * indicator build_accent_color_screen's swatches use; safer than a unicode
 * checkmark that this project's subset LVGL fonts may not actually have a
 * glyph for). The preference is applied through the BlueALSA 5 source daemon
 * reconciliation path. ---- */

typedef struct {
    const char * value; /* matches player_settings_t.bt_codec / bt_control_set_codec()'s argument */
    const char * label;
} bt_codec_option_t;

static const bt_codec_option_t bt_codec_options[] = {
    { "auto", N_("Auto") },   { "ldac_hq", N_("LDAC Quality") }, { "ldac_sq", N_("LDAC Standard") },
    { "ssc", "SSC" },        { "aptx", "aptX" },                { "aac", "AAC" },
    { "sbc", "SBC" },        { "sbc_xq", "SBC-XQ" },
};
#define BT_CODEC_OPTION_COUNT (sizeof(bt_codec_options) / sizeof(bt_codec_options[0]))

static lv_obj_t * bt_codec_list;

static void bt_codec_option_row_cb(lv_event_t * e);

static void populate_bt_codec_screen(void) {
    lv_obj_clean(bt_codec_list);
    for (size_t i = 0; i < BT_CODEC_OPTION_COUNT; i++) {
        bool selected = strcmp(current_settings.bt_codec, bt_codec_options[i].value) == 0;
        add_pill_option_row(bt_codec_list, TR(bt_codec_options[i].label),
                            selected, bt_codec_option_row_cb, (void *) (intptr_t) i);
    }
}

static void bt_codec_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    if (index < 0 || (size_t) index >= BT_CODEC_OPTION_COUNT) return;
    const char * codec = bt_codec_options[index].value;
    bool codec_changed = strcmp(current_settings.bt_codec, codec) != 0;
    if (!bt_control_set_codec(codec)) {
        show_error_toast(TR("Could not save Bluetooth codec"));
        return;
    }
    snprintf(current_settings.bt_codec, sizeof(current_settings.bt_codec), "%s", codec);
    settings_save_async(&current_settings);
    populate_bt_codec_screen();
    if (codec_changed) show_info_toast(TR("Turn Bluetooth off and on to apply"));
}

/* ---- Bluetooth > Advanced ---------------------------------------------
 * Settings that are set once and rarely revisited, kept off the Bluetooth
 * screen so the device list stays the focus there. ---- */
static lv_obj_t * bt_advanced_list;
static lv_obj_t * bt_rate_list;

static void bt_rate_option_row_cb(lv_event_t * e);

/* Rates offered by the connected accessory for the codec in use. Queried
 * when the screen opens, because the set depends on both the accessory and
 * the negotiated codec. */
#define BT_RATE_MAX_OPTIONS 12
static unsigned int bt_rate_options[BT_RATE_MAX_OPTIONS];
static int bt_rate_option_count;
/* True while the background query is in flight, so the list can say so. */
static bool bt_rate_query_active = false;

/* Rate shown and edited is the connected accessory's own, falling back to the
 * global default when nothing is connected. */
static unsigned int bt_rate_selected(void) {
    return settings_bt_rate_for(&current_settings, bt_connected_mac_cached);
}

static void populate_bt_rate_screen(void) {
    lv_obj_clean(bt_rate_list);
    add_pill_option_row(bt_rate_list, TR("Auto"), bt_rate_selected() == 0,
                        bt_rate_option_row_cb, (void *) (intptr_t) 0);

    /* Nothing connected means nothing to query, and only 44.1 can be applied
     * without an accessory present to negotiate with. */
    if (bt_rate_option_count == 0) {
        add_pill_option_row(bt_rate_list, "44.1 kHz", bt_rate_selected() == 44100,
                            bt_rate_option_row_cb, (void *) (intptr_t) 44100);
        add_section_header(bt_rate_list, bt_rate_query_active ? TR("Reading supported rates...")
                                                              : TR("Connect a device to see its supported rates"));
        return;
    }
    for (int i = 0; i < bt_rate_option_count; i++) {
        char label[32];
        format_rate(label, sizeof(label), bt_rate_options[i]);
        add_pill_option_row(bt_rate_list, label, bt_rate_selected() == bt_rate_options[i],
                            bt_rate_option_row_cb, (void *) (intptr_t) bt_rate_options[i]);
    }
}

static void populate_bt_advanced_screen(void);
/* Defined with the Bluetooth screen further down; the Advanced screen owns
 * the rows now but the callbacks stay next to the state they touch. */
static void bt_volume_sync_toggle_cb(lv_event_t * e);
static void bt_hide_unnamed_toggle_cb(lv_event_t * e);
static void bt_restore_at_startup_toggle_cb(lv_event_t * e);
static void bt_codec_settings_row_cb(lv_event_t * e);

/* Cycling the accessory's link blocks for seconds, so it runs on a worker
 * while the screen stays live. */
static pthread_t bt_rate_apply_thread;
static bool bt_rate_apply_active = false;
static atomic_bool bt_rate_apply_done_flag = false;
/* Captured at launch so a status refresh cannot change what is being applied
 * half way through. */
static unsigned int bt_rate_apply_target = 0;

static void * bt_rate_apply_thread_func(void * arg) {
    (void) arg;
    (void) bt_control_reconnect_for_rate_change(bt_rate_apply_target);
    atomic_store_explicit(&bt_rate_apply_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

void poll_bt_rate_apply(void) {
    if (!bt_rate_apply_active || !atomic_load_explicit(&bt_rate_apply_done_flag, memory_order_acquire)) return;
    bt_rate_apply_active = false;
    pthread_join(bt_rate_apply_thread, NULL);
    /* No outcome toast: the accessory may still need reconnecting by hand,
     * so "applied" or "failed" here would contradict what the user is about
     * to do. The redrawn list shows the rate that actually took. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
}

static void bt_rate_option_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    unsigned int rate = (unsigned int) (intptr_t) lv_event_get_user_data(e);
    if (bt_rate_apply_active || bt_reconnect_busy()) {
        show_info_toast(TR("Service is busy"));
        return;
    }
    bool changed = bt_rate_selected() != rate;
    /* Remembered against the accessory when one is connected, so each keeps
     * its own; with nothing connected this sets the default for accessories
     * that have no entry yet. */
    if (bt_connected_mac_cached[0]) settings_bt_set_rate_for(&current_settings, bt_connected_mac_cached, rate);
    else current_settings.bt_sample_rate = rate;
    settings_save_async(&current_settings);
    bt_control_choose_sample_rate(rate);
    populate_bt_rate_screen();
    if (!changed) return;

    /* The rate is fixed when the A2DP configuration is negotiated, and these
     * accessories refuse to renegotiate a live transport, so the link has to
     * be re-established for the choice to take effect. Only the link cycles;
     * the radio stays on. With nothing connected there is nothing to cycle
     * and the choice simply applies on the next connection. */
    if (!gui_shell_is_bt_audio_connected()) return;
    atomic_store_explicit(&bt_rate_apply_done_flag, false, memory_order_relaxed);
    bt_rate_apply_target = rate;
    bt_rate_apply_active = true;
    if (pthread_create(&bt_rate_apply_thread, NULL, bt_rate_apply_thread_func, NULL) != 0) {
        bt_rate_apply_active = false;
        show_info_toast(TR("Turn Bluetooth off and on to apply"));
        return;
    }
    show_info_toast(TR("Headset may disconnect, manual reconnection might be required"));
}

/* The query forks bluealsactl, which must never happen on the UI thread:
 * this screen would otherwise freeze for as long as the call takes, the same
 * stall populate_bt_screen() documents for bt_control_is_powered(). The
 * screen opens with what is already known and fills in when the worker
 * finishes. */
static pthread_t bt_rate_query_thread;
static atomic_bool bt_rate_query_done_flag = false;
static unsigned int bt_rate_query_results[BT_RATE_MAX_OPTIONS];
static int bt_rate_query_result_count;
/* Bumped whenever the connected accessory changes; a query that finishes under
 * an older value belongs to the previous accessory and must not be shown. */
static unsigned int bt_rate_device_generation;
static unsigned int bt_rate_query_generation;

static void * bt_rate_query_thread_func(void * arg) {
    (void) arg;
    bt_rate_query_result_count = bt_control_get_available_rates(bt_rate_query_results, BT_RATE_MAX_OPTIONS);
    atomic_store_explicit(&bt_rate_query_done_flag, true, memory_order_release); /* written last */
    return NULL;
}

/* Set when the accessory changed while the rate screen was buried under
 * another screen; the query is started once it is on top again. */
static bool bt_rate_requery_pending;

/* The screen shows one accessory's rates and edits that accessory's entry,
 * so a device changing underneath it has to redraw (wherever it sits in the
 * stack) or Back would show, and edit with, the previous accessory's rows. */
void gui_network_notify_bt_device_changed(void) {
    bt_rate_device_generation++;
    bt_rate_option_count = 0;
    if (!bt_rate_screen || !bt_rate_list) return;
    populate_bt_rate_screen();
    bt_rate_requery_pending = true;
}

static void start_bt_rate_query(void) {
    if (!gui_shell_is_bt_audio_connected() || bt_rate_query_active) return;
    atomic_store_explicit(&bt_rate_query_done_flag, false, memory_order_relaxed);
    bt_rate_query_generation = bt_rate_device_generation;
    bt_rate_query_active = true;
    if (pthread_create(&bt_rate_query_thread, NULL, bt_rate_query_thread_func, NULL) != 0)
        bt_rate_query_active = false;
}

void poll_bt_rate_query(void) {
    if (bt_rate_requery_pending && !bt_rate_query_active && bt_rate_screen && gui_navigation_is_top(bt_rate_screen)) {
        bt_rate_requery_pending = false;
        start_bt_rate_query();
        populate_bt_rate_screen(); /* "Reading..." instead of the stale hint */
    }
    if (!bt_rate_query_active || !atomic_load_explicit(&bt_rate_query_done_flag, memory_order_acquire)) return;
    bt_rate_query_active = false;
    pthread_join(bt_rate_query_thread, NULL);
    bool stale = bt_rate_query_generation != bt_rate_device_generation;
    if (!stale && bt_rate_query_result_count > 0) {
        memcpy(bt_rate_options, bt_rate_query_results, sizeof(bt_rate_options));
        bt_rate_option_count = bt_rate_query_result_count;
    }
    /* The accessory changed while this ran: ask the new one instead, but only
     * if its screen is still up (reopening it starts a query anyway). */
    if (stale && bt_rate_screen && gui_navigation_is_top(bt_rate_screen)) {
        bt_rate_requery_pending = false;
        start_bt_rate_query();
    }
    /* Repainted even when the query found nothing, so the "reading" note is
     * replaced rather than left up until the screen is reopened. Finishing
     * under another screen leaves the rows unpainted, so redo it on return. */
    if (gui_navigation_is_top(bt_rate_screen)) populate_bt_rate_screen();
    else if (bt_rate_screen && bt_rate_list) bt_rate_requery_pending = true;
}

static void bt_rate_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bt_rate_option_count = 0;
    populate_bt_rate_screen();
    nav_push(bt_rate_screen);
    bt_rate_requery_pending = false;
    start_bt_rate_query();
}

static void bt_speexrate_toggle_cb(lv_event_t * e) {
    /* add_pill_toggle_row() makes the switch itself non-clickable and fires
     * CLICKED on the row, so the state is flipped here rather than read back
     * off the widget. */
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_speexrate_enabled = !current_settings.bt_speexrate_enabled;
    settings_save_async(&current_settings);
    bt_control_set_speexrate_enabled(current_settings.bt_speexrate_enabled);
    populate_bt_advanced_screen();
}

static void populate_bt_advanced_screen(void) {
    lv_obj_clean(bt_advanced_list);
    add_pill_toggle_row(bt_advanced_list, TR("Bluetooth Volume Sync"),
                        current_settings.bt_volume_sync_enabled, bt_volume_sync_toggle_cb);
    add_pill_chevron_row(bt_advanced_list, TR("Codec"), bt_codec_settings_row_cb);
    add_pill_chevron_row(bt_advanced_list, TR("Sample Rate"), bt_rate_settings_row_cb);
    /* Only does anything for a track whose rate differs from the negotiated
     * transport rate; with Sample Rate at 44.1 kHz most tracks never reach
     * the resampler at all. */
    add_pill_toggle_row(bt_advanced_list, TR("Speex Resampling"),
                        current_settings.bt_speexrate_enabled, bt_speexrate_toggle_cb);
    add_pill_toggle_row(bt_advanced_list, TR("Hide Unnamed Devices"),
                        current_settings.bt_hide_unnamed_devices, bt_hide_unnamed_toggle_cb);
    add_pill_toggle_row(bt_advanced_list, TR("Restore at Startup"),
                        current_settings.bt_restore_at_startup, bt_restore_at_startup_toggle_cb);
}

static void bt_advanced_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_bt_advanced_screen();
    nav_push(bt_advanced_screen);
}

static void bt_codec_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_bt_codec_screen();
    nav_push(bt_codec_screen);
}

static void bt_volume_sync_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_volume_sync_enabled = !current_settings.bt_volume_sync_enabled;
    settings_save_async(&current_settings);
    start_bt_apply_output_settings(current_settings.bt_dac_mode_enabled, current_settings.bt_volume_sync_enabled);
    populate_bt_advanced_screen(); /* the row lives on Advanced now */
}

/* Display filter toggle to hide unnamed Bluetooth devices (e.g. BLE beacons)
 * from the Available Devices list. Paired devices are never hidden. */
static void bt_hide_unnamed_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_hide_unnamed_devices = !current_settings.bt_hide_unnamed_devices;
    settings_save_async(&current_settings);
    populate_bt_advanced_screen(); /* moves the switch the user just tapped */
    populate_bt_screen();          /* and the device list it filters, for the way back */
}

static void bt_restore_at_startup_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.bt_restore_at_startup = !current_settings.bt_restore_at_startup;
    if (current_settings.bt_restore_at_startup)
        current_settings.bt_last_on = bt_is_powered_cached;
    settings_save_async(&current_settings);
    populate_bt_advanced_screen();
}

static uint64_t bt_sig_add(uint64_t h, const void * data, size_t len) {
    const unsigned char * p = (const unsigned char *) data;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ULL; /* FNV-1a */
    return h;
}

static uint64_t bt_sig_add_str(uint64_t h, const char * s) {
    return bt_sig_add(h, s, strlen(s) + 1);
}

/* Hash of everything populate_bt_screen() renders, so an unchanged list is
 * left alone. Any input of add_bt_device_row() or the section layout belongs here. */
static uint64_t bt_screen_signature(bool powered) {
    uint64_t h = 14695981039346656037ULL;
    int32_t row_h = ui_list_row_height();
    const lv_font_t * fonts[2] = {gui_theme_font(GUI_FONT_ROLE_BODY), gui_theme_font(GUI_FONT_ROLE_SUBTEXT)};
    h = bt_sig_add(h, &powered, sizeof(powered));
    h = bt_sig_add(h, &row_h, sizeof(row_h));
    h = bt_sig_add(h, fonts, sizeof(fonts));
    /* A font size change rewrites the descriptors in place, so the addresses alone miss it. */
    for (int i = 0; i < 2; i++) {
        int32_t metrics[2] = {fonts[i] ? lv_font_get_line_height(fonts[i]) : 0, fonts[i] ? fonts[i]->base_line : 0};
        h = bt_sig_add(h, metrics, sizeof(metrics));
    }
    if (!powered) return h;

    bool hide_unnamed = current_settings.bt_hide_unnamed_devices;
    h = bt_sig_add(h, &hide_unnamed, sizeof(hide_unnamed));
    h = bt_sig_add_str(h, bt_connecting_mac);
    h = bt_sig_add_str(h, bt_connect_failed_mac);
    h = bt_sig_add_str(h, bt_connected_mac_cached);
    h = bt_sig_add_str(h, bt_connected_codec_cached);
    h = bt_sig_add(h, &bt_connected_rate_cached, sizeof(bt_connected_rate_cached));
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_preferred_device_index(i) != i) continue;
        bool paired, connected;
        bt_merged_device_state(bt_scan_results[i].mac, &paired, &connected);
        unsigned char flags = (unsigned char) ((paired ? 1 : 0) | (connected ? 2 : 0) |
                                               (bt_name_is_mac_placeholder(&bt_scan_results[i]) ? 4 : 0));
        h = bt_sig_add(h, &i, sizeof(i)); /* the row's click callback carries this index */
        h = bt_sig_add_str(h, bt_scan_results[i].mac);
        h = bt_sig_add_str(h, bt_scan_results[i].name);
        h = bt_sig_add(h, &flags, sizeof(flags));
    }
    return h;
}

/* Adds the merged-device rows of one section (paired, or available and not
 * paired; the latter honours the hide-unnamed filter). Returns rows added. */
static int add_bt_device_section(bool paired_section) {
    int shown = 0;
    for (int i = 0; i < bt_scan_result_count; i++) {
        if (bt_preferred_device_index(i) != i) continue;
        bool paired, connected;
        bt_merged_device_state(bt_scan_results[i].mac, &paired, &connected);
        (void) connected;
        if (paired != paired_section) continue;
        if (!paired_section && current_settings.bt_hide_unnamed_devices &&
            bt_name_is_mac_placeholder(&bt_scan_results[i])) continue;
        add_bt_device_row(bt_list, i);
        shown++;
    }
    return shown;
}

/* ---- Master rebuild for the Bluetooth screen's one dynamic list: toggle
 * row, then (only while powered) volume sync/DAC/Codec rows, Paired
 * Devices, and Available Devices -- same full-rebuild-on-any-change
 * convention as populate_wifi_screen() above. bt_scan_results already holds
 * both paired and freshly-discovered devices from one bt_control_scan()
 * call; this just splits that single list into the two sections by
 * ->paired rather than needing two separate scans. ---- */
void populate_bt_screen(void) {
    static uint64_t last_signature;
    static lv_obj_t * last_list;
    static uint32_t last_child_count;

    /* bt_is_powered_cached, not bt_control_is_powered() -- same reasoning
     * as external_dac_block_reason()'s own use of the cache (see its
     * comment): calling bt_control_is_powered() directly here forked
     * bluetoothctl show synchronously on the UI thread every time this
     * screen opened, which on a real device produced a multi-second freeze
     * entering the Bluetooth submenu (a milder version of the same
     * pre-existing hang class start_refresh_bt_icon()'s own comment
     * documents). The cache is kept fresh in the background regardless. */
    bool powered = bt_is_powered_cached;
    if (bt_rescan_btn) {
        if (powered) lv_obj_remove_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bt_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* Nothing changed: keep the rows (and any press on them) in place. A
     * different or emptied list always rebuilds. A touch in progress defers
     * the rebuild; the signature stays stale so the next refresh does it. */
    uint64_t signature = bt_screen_signature(powered);
    bool same_list = bt_list == last_list && lv_obj_get_child_count(bt_list) == last_child_count;
    if (same_list && signature == last_signature) return;
    if (same_list && network_list_touch_active(bt_list)) return;

    /* Preserve scroll position across periodic screen rebuilds so the view
     * does not snap back to the top while browsing devices. */
    int32_t saved_scroll_y = lv_obj_get_scroll_y(bt_list);

    lv_obj_clean(bt_list);
    /* quick_drawer_bt_event_cb is the SAME real toggle-thread trigger the
     * drawer's own bt icon uses -- see populate_wifi_screen()'s identical
     * reasoning for wifi. */
    add_pill_toggle_row(bt_list, TR("Bluetooth"), powered, quick_drawer_bt_event_cb);

    if (!powered) {
        build_list_message(bt_list, TR("Bluetooth is off"), TR("Turn on Bluetooth to see paired and nearby devices."));
        goto finish;
    }

    add_pill_chevron_row(bt_list, TR("Bluetooth DAC"), bt_dac_settings_row_cb);
    add_pill_chevron_row(bt_list, TR("Advanced"), bt_advanced_settings_row_cb);

    add_section_header(bt_list, TR("Paired Devices"));
    if (add_bt_device_section(true) == 0) {
        build_list_message(bt_list, TR("No paired devices"), TR("Previously connected accessories will appear here."));
    }

    add_section_header(bt_list, TR("Available Devices"));
    if (add_bt_device_section(false) == 0) {
        build_list_message(bt_list, TR("No nearby devices"), TR("Make an accessory discoverable, then tap Rescan."));
    }

finish:
    lv_obj_scroll_to_y(bt_list, saved_scroll_y, LV_ANIM_OFF);
    last_list = bt_list;
    last_child_count = lv_obj_get_child_count(bt_list);
    last_signature = signature;
}

static lv_obj_t * build_bluetooth_screen(void) {
    lv_obj_t * scr = network_list_screen(TR("Bluetooth"), &bt_list);

    bt_rescan_btn = build_header_refresh_action(scr, bt_rescan_btn_cb);

    return scr;
}

void open_bluetooth_screen(void) {
    bt_pairing_agent_allow_pairing_for(BT_PAIRING_WINDOW_MS);
    populate_bt_screen();
    nav_push(bt_screen);
    /* Auto-refresh on open -- cached power state, not a fresh
     * bt_control_is_powered() call, same reasoning as populate_bt_screen().
     * An inquiry is skipped while an accessory is connected: discovery makes
     * the controller interleave with the A2DP link and the music audibly
     * stutters for the whole scan window. The known-device list still
     * refreshes, and Rescan is there when discovery is actually wanted. */
    if (bt_is_powered_cached) start_bt_scan(!gui_shell_is_bt_audio_connected());
}

/* Called from the periodic refresh and when a pairing screen opens: pairing
 * stays open while a screen a phone pairs from is shown: Bluetooth, the
 * Bluetooth DAC screen, and Remote Control while it is enabled. */
void gui_network_extend_bt_pairing_window(void) {
    lv_obj_t * dac_screen = gui_network_get_bt_dac_screen();
    if ((bt_screen && gui_navigation_is_top(bt_screen)) ||
        (dac_screen && gui_navigation_is_top(dac_screen)) ||
        (remote_control_screen && current_settings.remote_control_enabled &&
         gui_navigation_is_top(remote_control_screen)))
        bt_pairing_agent_allow_pairing_for(BT_PAIRING_WINDOW_MS);
}

/* Bluetooth just came on: load BlueZ's known devices now so Paired Devices
 * is not empty until Rescan. No inquiry; an active scan already refreshes. */
void gui_network_bt_powered_on(void) {
    if (!bt_screen || !gui_navigation_is_top(bt_screen)) return;
    start_bt_scan(false);
}

void bt_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_bluetooth_screen();
}

void gui_network_bt_build(void) {
    bt_screen = build_bluetooth_screen();
    bt_codec_screen = network_list_screen(TR("Codec"), &bt_codec_list);
    bt_advanced_screen = network_list_screen(TR("Advanced"), &bt_advanced_list);
    bt_rate_screen = network_list_screen(TR("Sample Rate"), &bt_rate_list);
    build_bt_action_popup();
}

void gui_network_bt_teardown(void) {
    gui_popup_teardown(&bt_action_popup);
    network_delete_screen(&bt_screen);
    network_delete_screen(&bt_codec_screen);
    network_delete_screen(&bt_advanced_screen);
    network_delete_screen(&bt_rate_screen);
    bt_list = NULL;
    bt_rescan_btn = NULL;
    bt_codec_list = NULL;
    bt_advanced_list = NULL;
    bt_rate_list = NULL;
    bt_rate_requery_pending = false;
}

bool gui_network_bt_has_work(void) {
    return bt_connect_active || bt_forget_active || bt_rate_apply_active || bt_rate_query_active || bt_scan_active;
}

bool gui_network_bt_user_operation_active(void) {
    return bt_connect_active || bt_forget_active || bt_rate_apply_active;
}

void gui_network_bt_cancel(void) {
    if (bt_connect_active) {
        bt_pairing_agent_cancel_prompts(); /* a pair waiting on its popup could not be answered now */
        pthread_join(bt_connect_thread, NULL);
        bt_connect_active = false;
    }
    if (bt_forget_active) {
        pthread_join(bt_forget_thread, NULL);
        bt_forget_active = false;
        free(bt_forget_pending_req); /* poll_bt_forget() will never consume it now */
        bt_forget_pending_req = NULL;
    }
    if (bt_rate_query_active) {
        pthread_join(bt_rate_query_thread, NULL);
        bt_rate_query_active = false;
    }
    if (bt_rate_apply_active) {
        pthread_join(bt_rate_apply_thread, NULL);
        bt_rate_apply_active = false;
    }
    if (bt_scan_active) {
        pthread_join(bt_scan_thread, NULL);
        bt_scan_active = false;
    }
}

lv_obj_t * gui_network_get_bt_screen(void) { return bt_screen; }
