#include "../application/gui.h"
#include "../text/i18n/i18n.h"
#include "../text/rendering/gui_text_input.h"
#include "../widgets/builders/screen_builders.h"
#include <stdio.h>
#include <string.h>
#include "gui_network_internal.h"


/* ---- Wi-Fi settings screen ----
 *
 * Enable/disable toggle at top; everything else (info/manual entry/DNS,
 * memorized networks, available networks) only appears once Wi-Fi is on,
 * rebuilt into the one dynamic wifi_list container by populate_wifi_screen()
 * -- reachable both from the Wireless submenu's Wi-Fi tile and from a
 * long-press on the quick-access drawer's wifi icon (see
 * quick_drawer_wifi_long_press_cb near build_quick_drawer). Scan/connect/
 * disconnect/forget all share the same background-thread-plus-polled-flag
 * shape; connect and connect-saved show the shared gui_busy overlay while
 * they run. */

/* Screens owned by this module */
lv_obj_t * wifi_screen;
lv_obj_t * wifi_info_screen;
static lv_obj_t * wifi_dns_screen;

/* ---- Wi-Fi saved-network action popup ------------------------------------
 * Presents options to Connect or Forget for a selected memorized network,
 * matching bt_action_popup's overlay and backdrop pattern. */
static gui_popup_t wifi_action_popup;
static lv_obj_t * wifi_action_popup_title;
static lv_obj_t * wifi_action_primary_label;
static int wifi_action_popup_network_id = -1;
static char wifi_action_popup_network_ssid[WIFI_MAX_SSID_LEN];
static bool wifi_action_disconnect_requested;

static void hide_wifi_action_popup(void) {
    gui_popup_hide(&wifi_action_popup);
    wifi_action_popup_network_id = -1;
    wifi_action_popup_network_ssid[0] = '\0';
    wifi_action_disconnect_requested = false;
}

static void wifi_action_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_wifi_action_popup();
}

static bool wifi_saved_profile_is_connected(int index) {
    if (index < 0 || index >= wifi_saved_result_count || !wifi_cached_info_is_current() ||
        !wifi_cached_info_connected) return false;
    if (wifi_saved_results[index].current)
        return strcmp(wifi_saved_results[index].ssid, wifi_cached_info.ssid) == 0;
    for (int i = 0; i < wifi_saved_result_count; ++i)
        if (wifi_saved_results[i].current) return false;
    return strcmp(wifi_saved_results[index].ssid, wifi_cached_info.ssid) == 0 &&
           wifi_saved_display_index(index) == index;
}

static int wifi_action_popup_resolve_network(void) {
    if (wifi_action_popup_network_id < 0 || wifi_action_popup_network_ssid[0] == '\0') return -1;
    for (int i = 0; i < wifi_saved_result_count; ++i) {
        if (wifi_saved_results[i].id == wifi_action_popup_network_id &&
            strcmp(wifi_saved_results[i].ssid, wifi_action_popup_network_ssid) == 0) return i;
    }
    return -1;
}

static void wifi_action_primary_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = wifi_action_popup_resolve_network();
    if (index < 0) {
        hide_wifi_action_popup();
        return;
    }
    if (wifi_link_op_busy()) {
        hide_wifi_action_popup();
        show_info_toast(TR("Wi-Fi is busy"));
        return;
    }
    wifi_saved_network_t * net = &wifi_saved_results[index];
    int id = net->id;
    char ssid[WIFI_MAX_SSID_LEN];
    snprintf(ssid, sizeof(ssid), "%s", net->ssid);
    bool connected = wifi_saved_profile_is_connected(index);
    bool disconnect_requested = wifi_action_disconnect_requested;
    hide_wifi_action_popup();
    if (disconnect_requested != connected) return;
    if (disconnect_requested) start_wifi_disconnect();
    else start_wifi_connect_saved(id, ssid);
}

static void wifi_action_forget_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = wifi_action_popup_resolve_network();
    if (index < 0) {
        hide_wifi_action_popup();
        return;
    }
    int id = wifi_saved_results[index].id;
    hide_wifi_action_popup();
    start_wifi_forget(id);
}

static void build_wifi_action_popup(void) {
    wifi_action_popup.popup = build_confirm_popup_with_labels(
                                                   "", LV_LABEL_LONG_DOT, &wifi_action_popup_title,
                                                   NULL, NULL, TR("Connect"), &wifi_action_primary_label,
                                                   accent_lv_color(), wifi_action_primary_cb, NULL, TR("Forget"),
                                                   lv_color_hex(GUI_BREEZE_NEGATIVE), wifi_action_forget_cb, NULL,
                                                   wifi_action_popup_backdrop_cb, &wifi_action_popup.backdrop);
}

static void show_wifi_action_popup(int index) {
    if (index < 0 || index >= wifi_saved_result_count) return;
    wifi_action_popup_network_id = wifi_saved_results[index].id;
    snprintf(wifi_action_popup_network_ssid, sizeof(wifi_action_popup_network_ssid), "%s",
             wifi_saved_results[index].ssid);
    lv_label_set_text(wifi_action_popup_title, wifi_saved_results[index].ssid);
    wifi_action_disconnect_requested = wifi_saved_profile_is_connected(index);
    lv_label_set_text(wifi_action_primary_label, wifi_action_disconnect_requested
                      ? TR("Disconnect") : TR("Connect"));
    gui_popup_show(&wifi_action_popup);
}

static void wifi_saved_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    show_wifi_action_popup(index);
}

/* ---- Wi-Fi Info screen (read-only) ---- */

static lv_obj_t * wifi_info_list;

void populate_wifi_info_screen(void) {
    lv_obj_clean(wifi_info_list);

    wifi_settings_snapshot_start();
    if (!wifi_cached_info_is_current() || !wifi_cached_info_connected) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, wifi_settings_snapshot_active ? TR("Loading...") : TR("Not connected"));
        lv_obj_add_style(label, &style_theme_text_muted, 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        return;
    }

    wifi_info_t info = wifi_cached_info;
    static const char * signal_labels[] = { N_("Weak"), N_("Fair"), N_("Good"), N_("Excellent") };
    int signal_index = (info.signal_level >= 0 && info.signal_level <= 3) ? info.signal_level : 0;
    char lines[5][160];
    snprintf(lines[0], sizeof(lines[0]), TR("SSID: %s"), info.ssid);
    snprintf(lines[1], sizeof(lines[1]), TR("IP Address: %s"), info.ip[0] ? info.ip : "-");
    snprintf(lines[2], sizeof(lines[2]), TR("Gateway: %s"), info.gateway[0] ? info.gateway : "-");
    snprintf(lines[3], sizeof(lines[3]), TR("MAC Address: %s"), info.mac[0] ? info.mac : "-");
    snprintf(lines[4], sizeof(lines[4]), TR("Signal: %s"), TR(signal_labels[signal_index]));

    for (int i = 0; i < 5; i++) {
        lv_obj_t * label = lv_label_create(wifi_info_list);
        lv_label_set_text(label, lines[i]);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_pad_left(label, BOARD_SCALE_PX(24), 0);
        lv_obj_set_style_pad_right(label, BOARD_SCALE_PX(24), 0);
        lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(12), 0);
    }
}

static void wifi_info_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wifi_settings_snapshot_invalidate();
    wifi_settings_snapshot_enabled = true;
    populate_wifi_info_screen();
    nav_push(wifi_info_screen);
}

/* ---- Manual SSID entry -- chains the same show_text_entry() flow scan
 * results already use for a secured network's password, just starting from
 * a typed SSID instead of a tapped scan row. An empty password submission
 * connects as an open network (start_wifi_connect() already treats an empty
 * password as "no password"), so there's no separate "is this secured?"
 * question to ask up front. ---- */

static void wifi_manual_ssid_entered_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!wifi_pending_ssid_set(text)) return;
    show_text_entry(TR("Wi-Fi Password"), NULL, true, false, wifi_password_entered_cb, NULL);
}

static void wifi_manual_entry_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Network Name (SSID)"), NULL, false, false, wifi_manual_ssid_entered_cb, NULL);
}

/* ---- DNS settings screen -- view/edit the 2 servers currently in
 * /etc/resolv.conf (see wifi_control_get_dns()'s own doc comment for why
 * this is a "until the next DHCP renewal" override, not a permanent one).
 * ---- */

static lv_obj_t * wifi_dns_list;
static char wifi_dns_servers[2][16];
static int wifi_dns_server_count = 0;

static void wifi_dns_row_cb(lv_event_t * e);

static void populate_wifi_dns_screen(void) {
    lv_obj_clean(wifi_dns_list);

    char current[2][16];
    int count = wifi_control_get_dns(current, 2);
    for (int i = 0; i < 2; i++) {
        snprintf(wifi_dns_servers[i], sizeof(wifi_dns_servers[i]), "%s", i < count ? current[i] : "");
    }
    wifi_dns_server_count = count;

    static const char * slot_labels[2] = { N_("Primary DNS"), N_("Secondary DNS") };
    for (int i = 0; i < 2; i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s: %s", TR(slot_labels[i]), wifi_dns_servers[i][0] ? wifi_dns_servers[i] : TR("Not set"));
        lv_obj_t * row = add_pill_chevron_row(wifi_dns_list, text, NULL);
        lv_obj_add_event_cb(row, wifi_dns_row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void wifi_dns_entered_cb(const char * text, void * user_data) {
    int slot = (int) (intptr_t) user_data;
    snprintf(wifi_dns_servers[slot], sizeof(wifi_dns_servers[slot]), "%s", text);
    if (slot + 1 > wifi_dns_server_count) wifi_dns_server_count = slot + 1;

    const char * servers[2] = { wifi_dns_servers[0], wifi_dns_servers[1] };
    wifi_control_set_dns(servers, wifi_dns_server_count);
    populate_wifi_dns_screen();
}

static void wifi_dns_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int slot = (int) (intptr_t) lv_event_get_user_data(e);
    show_text_entry(slot == 0 ? TR("Primary DNS") : TR("Secondary DNS"), wifi_dns_servers[slot], false, false, wifi_dns_entered_cb,
                    (void *) (intptr_t) slot);
}

static void wifi_dns_settings_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_wifi_dns_screen();
    nav_push(wifi_dns_screen);
}

/* ---- Advanced screen: settings that are rarely changed. Info and DNS need
 * a live radio, so they show only while Wi-Fi is on. ---- */

static lv_obj_t * wifi_advanced_screen;
static lv_obj_t * wifi_advanced_list;

static void wifi_restore_at_startup_toggle_cb(lv_event_t * e);

static void populate_wifi_advanced_screen(void) {
    lv_obj_clean(wifi_advanced_list);
    add_pill_toggle_row(wifi_advanced_list, TR("Restore at Startup"), current_settings.wifi_restore_at_startup,
                        wifi_restore_at_startup_toggle_cb);
    if (!gui_shell_wifi_effective_enabled()) return;
    add_pill_chevron_row(wifi_advanced_list, TR("Wi-Fi Info"), wifi_info_row_cb);
    add_pill_chevron_row(wifi_advanced_list, TR("DNS Settings"), wifi_dns_settings_row_cb);
}

static void wifi_restore_at_startup_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    current_settings.wifi_restore_at_startup = !current_settings.wifi_restore_at_startup;
    if (current_settings.wifi_restore_at_startup)
        current_settings.wifi_last_on = gui_shell_wifi_effective_enabled();
    settings_save_async(&current_settings);
    populate_wifi_advanced_screen();
}

static void wifi_advanced_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_wifi_advanced_screen();
    nav_push(wifi_advanced_screen);
}

/* ---- Master rebuild for the Wi-Fi screen's one dynamic list: toggle row,
 * the Advanced row, and (only while enabled) manual entry, memorized
 * networks and available networks -- called after every state change
 * (toggle, scan, connect, disconnect, forget) rather than patched
 * incrementally, same full-rebuild convention as every other list in this
 * file. ---- */
/* enabled is passed in rather than read via wifi_control_is_enabled()
 * internally so the pending/completed toggle hooks
 * (gui_network_show_wifi_toggle_pending, gui_network_wifi_toggle_completed)
 * can pass the requested target state instead of the real (not-yet-caught-up)
 * one. Every other caller passes wifi_control_is_enabled(). */
void populate_wifi_screen(bool enabled) {
    lv_coord_t scroll_y = wifi_list ? lv_obj_get_scroll_y(wifi_list) : 0;
    lv_obj_clean(wifi_list);
    wifi_settings_snapshot_enabled = enabled;

    if (wifi_rescan_btn) {
        if (enabled) lv_obj_remove_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(wifi_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* quick_drawer_wifi_event_cb is the SAME real toggle-thread trigger the
     * drawer's own wifi icon uses -- no drawer-specific logic in it, so
     * it's reused directly here rather than duplicating the thread-kickoff
     * boilerplate. poll_wifi_toggle() repopulates this screen once the
     * toggle completes. */
    add_pill_toggle_row(wifi_list, TR("Wi-Fi"), enabled, quick_drawer_wifi_event_cb);
    if (enabled) add_pill_chevron_row(wifi_list, TR("Manual SSID Entry"), wifi_manual_entry_row_cb);
    /* Stays reachable while Wi-Fi is off: Restore at Startup lives there. */
    add_pill_chevron_row(wifi_list, TR("Advanced"), wifi_advanced_row_cb);

    if (!enabled) goto restore_scroll;

    wifi_settings_snapshot_start();

    const lv_font_t * body_font = gui_theme_font(GUI_FONT_ROLE_BODY);
    const lv_font_t * subtext_font = gui_theme_font(GUI_FONT_ROLE_SUBTEXT);
    const lv_font_t * text_fonts[] = {body_font, subtext_font};

    add_section_header(wifi_list, TR("Memorized Networks"));
    /* During an optimistic cold enable there is no wpa_supplicant control
     * socket yet. Keep the last cached saved-network rows instead of
     * launching synchronous wpa_cli subprocesses that cannot succeed and
     * would undermine the immediate feedback this state exists to provide. */
    bool have_settings_snapshot = wifi_cached_info_is_current();
    if (wifi_saved_result_count == 0) {
        if (have_settings_snapshot)
            build_list_message(wifi_list, TR("No memorized networks"), TR("Networks you connect to will appear here."));
        else
            build_list_message(wifi_list, TR("Loading Wi-Fi settings"), TR("Saved network details are being checked."));
    }
    /* Cross-reference wpa_cli status against saved network SSIDs to indicate
     * which saved network is currently connected. */
    bool wifi_currently_connected = have_settings_snapshot && wifi_cached_info_connected;
    int connected_saved_index = -1;
    if (wifi_currently_connected) {
        connected_saved_index = wifi_saved_current_index(wifi_cached_info.ssid);
        if (connected_saved_index < 0) {
            for (int i = 0; i < wifi_saved_result_count; ++i) {
                if (strcmp(wifi_saved_results[i].ssid, wifi_cached_info.ssid) == 0) {
                    connected_saved_index = wifi_saved_display_index(i);
                    break;
                }
            }
        }
    }
    for (int position = 0; position < wifi_saved_result_count; position++) {
        int i = rotate_to_front(position, connected_saved_index);
        if (wifi_saved_display_index(i) != i) continue;
        lv_obj_t * row = network_row_create(wifi_list,
            network_row_text_height(ui_list_row_height(), text_fonts, 2),
            wifi_saved_row_click_cb, (void *) (intptr_t) i);

        bool is_connected = wifi_saved_profile_is_connected(i);
        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, wifi_saved_results[i].ssid);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        configure_network_row_line(label, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                                   body_font);

        lv_obj_t * status = lv_label_create(row);
        lv_label_set_text(status, is_connected ? TR("Connected") : TR("Saved network"));
        lv_obj_add_style(status, is_connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
        configure_network_row_line(status, LIST_ROW_WIDTH - 2 * LIST_ROW_LABEL_INSET,
                                   subtext_font);
        lv_obj_t * text_lines[] = {label, status};
        align_network_row_text_stack(row, text_lines, text_fonts, 2, LIST_ROW_LABEL_INSET);
    }

    add_section_header(wifi_list, TR("Available Networks"));
    int current_scan_index = wifi_scan_connected_index();
    int available_networks_shown = 0;
    for (int position = 0; position < wifi_scan_result_count; position++) {
        int i = rotate_to_front(position, current_scan_index);
        wifi_network_t * net = &wifi_scan_results[i];
        /* Saved profiles already have persistent action rows above. Do not
         * repeat the same SSID in the transient scan results section. */
        if (wifi_ssid_is_saved(net->ssid)) continue;
        available_networks_shown++;
        lv_obj_t * row = network_row_create(wifi_list,
            network_row_text_height(ui_list_row_height(), text_fonts, 2),
            wifi_row_click_cb, (void *) (intptr_t) i);

        lv_obj_t * icon = lv_image_create(row);
        lv_image_set_src(icon, wifi_signal_icon(net->signal_level));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, BOARD_SCALE_PX(20), 0);

        lv_obj_t * label = lv_label_create(row);
        lv_label_set_text(label, net->ssid);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        configure_network_row_line(label, LIST_ROW_WIDTH - BOARD_SCALE_PX(88),
                                   body_font);

        lv_obj_t * status = lv_label_create(row);
        bool is_connected = wifi_network_is_connected(net);
        lv_label_set_text(status, wifi_network_status_text(net, is_connected, false));
        lv_obj_add_style(status, is_connected ? gui_theme_accent_style() : &style_theme_text_muted, 0);
        configure_network_row_line(status, LIST_ROW_WIDTH - BOARD_SCALE_PX(88),
                                   subtext_font);
        lv_obj_t * text_lines[] = {label, status};
        align_network_row_text_stack(row, text_lines, text_fonts, 2, BOARD_SCALE_PX(64));
    }
    if (available_networks_shown == 0) {
        if (wifi_enable_pending_feedback)
            build_list_message(wifi_list, TR("Scanning for networks"), TR("Nearby Wi-Fi networks will appear here."));
        else if (wifi_scan_result_count > 0)
            build_list_message(wifi_list, TR("No other networks found"),
                               TR("All visible networks are already saved."));
        else
            build_list_message(wifi_list, TR("No networks found"), TR("Check that Wi-Fi is enabled, then rescan."));
    }
restore_scroll:
    if (wifi_list) {
        lv_obj_update_layout(wifi_list);
        lv_obj_scroll_to_y(wifi_list, scroll_y, LV_ANIM_OFF);
    }
}

static lv_obj_t * build_wifi_screen(void) {
    lv_obj_t * scr = network_list_screen(TR("Wi-Fi"), &wifi_list);

    wifi_rescan_btn = build_header_refresh_action(scr, wifi_rescan_btn_cb);

    return scr;
}

void open_wifi_screen(void) {
    bool enabled = wifi_control_is_enabled();
    wifi_settings_snapshot_invalidate();
    populate_wifi_screen(enabled);
    lv_obj_scroll_to_y(wifi_list, 0, LV_ANIM_OFF);
    nav_push(wifi_screen);
    if (enabled) start_wifi_scan(); /* auto-refresh -- matches the old always-scan-on-open behavior, but only when there's a radio to scan with */
}

void wifi_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    open_wifi_screen();
}

void gui_network_wifi_screen_build(void) {
    wifi_screen = build_wifi_screen();
    wifi_info_screen = network_list_screen(TR("Wi-Fi Info"), &wifi_info_list);
    wifi_dns_screen = network_list_screen(TR("DNS Settings"), &wifi_dns_list);
    wifi_advanced_screen = network_list_screen(TR("Advanced"), &wifi_advanced_list);
    build_wifi_action_popup();
}

void gui_network_wifi_screen_teardown(void) {
    gui_popup_teardown(&wifi_action_popup);
    network_delete_screen(&wifi_screen);
    network_delete_screen(&wifi_info_screen);
    network_delete_screen(&wifi_dns_screen);
    network_delete_screen(&wifi_advanced_screen);
    wifi_list = NULL;
    wifi_rescan_btn = NULL;
    wifi_info_list = NULL;
    wifi_dns_list = NULL;
    wifi_advanced_list = NULL;
}
