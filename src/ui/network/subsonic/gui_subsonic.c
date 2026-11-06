#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_covers.h"
#include "gui_subsonic_menu.h"
#include "gui_subsonic_sections.h"
#include "../../../network/wifi/wifi_status.h"
#include "../../../network/services/subsonic_client.h"
#include "../../../library/remote/subsonic/subsonic_cache.h"
#include <stdio.h>

/* Subsonic talks to a remote server, so the radio being on is not enough: this
 * asks for a real association. New connections and servers without a cache need
 * it; the entry screen and cached libraries open without. */
bool gui_subsonic_wifi_connected_guard(void) {
    int level;
    if (wifi_get_status(&level)) return true;
    show_error_toast(TR("Connect to Wi-Fi first"));
    return false;
}

void subsonic_copy_request_error(char * dest, size_t size, const char * fallback) {
    const char * error = subsonic_last_error();
    snprintf(dest, size, "%s", error && error[0] ? error : fallback);
}

/* A foreground request owns the network and the busy screen. */
bool subsonic_foreground_busy(void) {
    if (!subsonic_browse_active && !subsonic_connect_active) return false;
    show_info_toast(TR("Previous request still finishing"));
    return true;
}

/* A result can push a new screen over busy before the request is dismissed. */
void subsonic_dismiss_busy(int depth_before, gui_busy_handle_t token) {
    if (gui_navigation_get_depth() > depth_before) nav_remove_stack_slot(depth_before - 1);
    else gui_busy_hide(token);
}

void subsonic_active_server_key(char out[SUBSONIC_CACHE_KEY_SIZE]) {
    subsonic_server_t server = subsonic_server_from_settings();
    subsonic_cache_server_key(&server, out);
}

static void build_subsonic_menu(void) {
    const gui_subsonic_menu_actions_t actions = {
        subsonic_menu_artists_row_cb, gui_subsonic_albums_row_cb, gui_subsonic_genres_row_cb, gui_subsonic_random_row_cb,
        subsonic_menu_playlists_row_cb, gui_subsonic_favorites_row_cb, subsonic_menu_cover_flow_row_cb,
        gui_subsonic_settings_open_cb,
    };
    gui_subsonic_menu_init(&actions);
}

void gui_subsonic_init(void) {
    gui_subsonic_connect_build();

    build_subsonic_menu();
    gui_subsonic_sections_init();
    gui_subsonic_settings_build();

    gui_subsonic_views_build();

    gui_subsonic_download_confirm_build();

    gui_subsonic_views_build_headers();
}

/* For gui_reload.c's in-process UI reload -- deletes every screen this
 * module owns so gui_subsonic_init() can rebuild them from a clean slate
 * without leaking the old objects. subsonic_download_confirm_popup and its
 * backdrop are built directly on lv_layer_top() (see build_confirm_popup()'s
 * own comment), not as children of any of these screens, so they need their
 * own explicit deletion. re-running gui_subsonic_init() also re-runs
 * register_search() for the two virtualized lists below, which already
 * frees its own prior state on re-registration (gui_library.c) -- nothing
 * extra needed here for that. */
void gui_subsonic_teardown(void) {
    gui_subsonic_sync_reset();
    gui_subsonic_download_confirm_teardown();
    gui_subsonic_connect_delete();
    gui_subsonic_menu_teardown();
    gui_subsonic_sections_teardown(); /* before the lists go: it detaches favorites views */
    gui_subsonic_settings_delete();
    subsonic_delete_browse_screens();
    gui_subsonic_covers_shutdown(); /* the lists it drew into are gone */
    /* The screens that borrowed these labels are gone. */
    subsonic_reset_browse_data();
}

bool gui_subsonic_has_background_work(void) {
    return gui_subsonic_downloads_active() || subsonic_connect_active || subsonic_browse_active ||
           gui_subsonic_sync_busy();
}

void gui_subsonic_handle_wifi_disabled(void) {
    /* The activity flags remain set until the normal poll functions join
     * their workers and dismiss their UI. Cancellation only interrupts the
     * socket here; it never blocks the GUI thread waiting for DNS/TCP/TLS. */
    gui_subsonic_connect_cancel();
    gui_subsonic_browse_cancel();
    gui_subsonic_downloads_cancel();
    gui_subsonic_sync_stop_network();
}

void gui_subsonic_cancel_background_work(void) {
    gui_subsonic_handle_wifi_disabled();
    gui_subsonic_sync_shutdown();
    gui_subsonic_connect_join();
    gui_subsonic_downloads_join();
    gui_subsonic_browse_join();
}

