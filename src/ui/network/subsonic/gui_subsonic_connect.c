#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic.h"
#include "gui_subsonic_internal.h"
#include "gui_subsonic_menu.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/process/worker_notify.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../network/services/subsonic_client.h"
#include "../../../library/remote/subsonic/subsonic_cache.h"
#include "../../../network/http/client/http_client.h"
#include "../../../network/wifi/wifi_status.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../text/rendering/gui_text_input.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern player_settings_t current_settings;

subsonic_server_t subsonic_server_from_settings(void) {
    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", current_settings.subsonic_url);
    snprintf(server.username, sizeof(server.username), "%s", current_settings.subsonic_username);
    snprintf(server.password, sizeof(server.password), "%s", current_settings.subsonic_password);
    server.verify_tls = current_settings.subsonic_verify_tls;
    return server;
}


typedef struct {
    subsonic_server_t server;
} subsonic_connect_request_t;

static gui_busy_handle_t subsonic_connect_token = 0;
static lv_obj_t * subsonic_entry_screen;
static lv_obj_t * subsonic_saved_servers_screen;
static lv_obj_t * subsonic_saved_servers_list;
static lv_obj_t * subsonic_new_connection_screen;

static pthread_t subsonic_connect_thread;
static http_cancel_token_t subsonic_connect_cancel;

bool subsonic_connect_active = false;
static bool subsonic_connect_timed_out = false;
static uint32_t subsonic_connect_started_at = 0;

static atomic_bool subsonic_connect_done_flag = false;

static volatile bool subsonic_connect_success_flag = false;

static char subsonic_connect_result_error[256];

static subsonic_server_t subsonic_connect_pending_server;

static void populate_indexed_list(lv_obj_t * list, int count, const char * (*label_of)(int), lv_event_cb_t click_cb) {
    lv_obj_clean(list);
    for (int i = 0; i < count; i++) {
        /* One lv_label via the shared list_row_style, not a container +
         * child label each with their own local style properties -- see
         * list_row_style's own doc comment (screen_builders.h). */
        lv_obj_t * row = lv_label_create(list);
        lv_obj_add_style(row, &list_row_style, 0);
        lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
        row_label_enable_marquee(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_label_set_text(row, label_of(i));

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    }
}

static void * subsonic_connect_thread_func(void * arg) {
    subsonic_connect_request_t * req = (subsonic_connect_request_t *) arg;

    /* Connecting only authenticates. Artists load when Artists is opened, so a
     * Navidrome account with an empty artist index is still a valid connection. */
    bool authenticated = subsonic_ping(&req->server, &subsonic_connect_cancel);
    if (!authenticated) {
        subsonic_copy_request_error(subsonic_connect_result_error, sizeof(subsonic_connect_result_error),
                                    TR("Failed to connect to server"));
    }

    subsonic_connect_success_flag = authenticated;
    atomic_store_explicit(&subsonic_connect_done_flag, true, memory_order_release); /* written last -- poll_subsonic_connect only checks this flag */
    worker_notify_ui();
    free(req);
    return NULL;
}

/* Becomes the connection everything else in Subsonic browsing and downloading
 * reads via subsonic_server_from_settings(). */
static void subsonic_set_active_server(const subsonic_server_t * server) {
    snprintf(current_settings.subsonic_url, sizeof(current_settings.subsonic_url), "%s", server->base_url);
    snprintf(current_settings.subsonic_username, sizeof(current_settings.subsonic_username), "%s", server->username);
    snprintf(current_settings.subsonic_password, sizeof(current_settings.subsonic_password), "%s", server->password);
    current_settings.subsonic_verify_tls = server->verify_tls;
}

/* A timeout is reported once; the worker is collected only on a later poll. */
static bool subsonic_connect_poll_waiting(void) {
    if (!subsonic_connect_timed_out &&
        lv_tick_elaps(subsonic_connect_started_at) >= SUBSONIC_REQUEST_TIMEOUT_MS) {
        subsonic_connect_timed_out = true;
        http_cancel_token_cancel(&subsonic_connect_cancel);
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
        show_error_toast(TR("Connection timed out after 30 seconds"));
        return true; /* Do not join a request still blocked in the network. */
    }
    return !atomic_load_explicit(&subsonic_connect_done_flag, memory_order_acquire);
}

static void subsonic_connect_collect(void) {
    subsonic_connect_active = false;
    pthread_join(subsonic_connect_thread, NULL);
    http_cancel_token_destroy(&subsonic_connect_cancel);
}

/* Saved Servers and New Connection both adopt and persist through this path. */
static void subsonic_adopt_connected_server(void) {
    subsonic_set_active_server(&subsonic_connect_pending_server);
    metadata_db_subsonic_server_save(subsonic_connect_pending_server.base_url, subsonic_connect_pending_server.username,
                                      subsonic_connect_pending_server.password, subsonic_connect_pending_server.verify_tls);
    settings_subsonic_server_upsert(&current_settings, subsonic_connect_pending_server.base_url,
                                    subsonic_connect_pending_server.username, subsonic_connect_pending_server.password,
                                    subsonic_connect_pending_server.verify_tls);
    settings_save(&current_settings);
    /* A new connection starts with empty browse caches. */
    subsonic_clear_browse_caches();
    nav_push(gui_subsonic_menu_screen());
}

void poll_subsonic_connect(void) {
    if (!subsonic_connect_active || subsonic_connect_poll_waiting()) return;
    subsonic_connect_collect();
    if (subsonic_connect_timed_out) {
        return; /* Discard even successful late authentication without navigation. */
    }
    bool success = subsonic_connect_success_flag;
    /* Adoption can navigate over busy; remove its old stack slot in that case. */
    int depth_before = gui_navigation_get_depth();
    if (success) subsonic_adopt_connected_server();
    subsonic_dismiss_busy(depth_before, subsonic_connect_token);
    subsonic_connect_token = 0;
    if (!success) show_error_toast(subsonic_connect_result_error);
}

static void start_subsonic_connect(const subsonic_server_t * server) {
    if (subsonic_foreground_busy()) return;
    if (!gui_subsonic_wifi_connected_guard()) return;
    subsonic_connect_pending_server = *server;

    subsonic_connect_request_t * req = malloc(sizeof(*req));
    if (!req) {
        show_error_toast(TR("Not enough memory to connect"));
        return;
    }
    req->server = *server;

    atomic_store_explicit(&subsonic_connect_done_flag, false, memory_order_relaxed);
    subsonic_connect_success_flag = false;
    subsonic_connect_timed_out = false;
    subsonic_connect_started_at = lv_tick_get();
    subsonic_connect_result_error[0] = '\0';
    http_cancel_token_init(&subsonic_connect_cancel);
    subsonic_connect_active = true;

    subsonic_connect_token = gui_busy_show(TR("Connecting to server..."), "");

    if (pthread_create(&subsonic_connect_thread, NULL, subsonic_connect_thread_func, req) != 0) {
        free(req);
        subsonic_connect_active = false;
        http_cancel_token_destroy(&subsonic_connect_cancel);
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
        show_error_toast(TR("Failed to start connection"));
    }
}

static bool subsonic_is_active_server(const subsonic_server_t * server) {
    char a[SUBSONIC_CACHE_KEY_SIZE], b[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_active_server_key(b);
    subsonic_cache_server_key(server, a);
    return strcmp(a, b) == 0;
}

/* A saved server with a cached library opens its menu at once; the check that
 * the server answers runs in the background and reports only a failure. */
static void subsonic_open_cached_server(const subsonic_server_t * server) {
    bool same = subsonic_is_active_server(server);
    subsonic_set_active_server(server);
    if (!same) {
        subsonic_clear_browse_caches();
        settings_save_async(&current_settings);
    }
    nav_push(gui_subsonic_menu_screen());
    subsonic_ping_or_queue();
}

static subsonic_server_row_t * subsonic_saved_servers = NULL;

static int subsonic_saved_server_count = 0;

static void subsonic_saved_server_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    subsonic_server_row_t * row = &subsonic_saved_servers[index];

    subsonic_server_t server;
    snprintf(server.base_url, sizeof(server.base_url), "%s", row->url);
    snprintf(server.username, sizeof(server.username), "%s", row->username);
    snprintf(server.password, sizeof(server.password), "%s", row->password);
    server.verify_tls = row->verify_tls;
    if (!subsonic_connect_active && !subsonic_browse_active && subsonic_cache_server_known(&server))
        subsonic_open_cached_server(&server);
    else
        start_subsonic_connect(&server);
}

static const char * subsonic_saved_server_label_of(int i) { return subsonic_saved_servers[i].url; }

static void populate_subsonic_saved_servers_screen(void) {
    free(subsonic_saved_servers);
    subsonic_saved_servers = NULL;
    subsonic_saved_server_count = 0;
    metadata_db_load_subsonic_servers(&subsonic_saved_servers, &subsonic_saved_server_count);

    lv_obj_clean(subsonic_saved_servers_list);
    if (subsonic_saved_server_count == 0) {
        lv_obj_t * title = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(title, TR("No saved servers"));
        lv_obj_add_style(title, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(title, gui_theme_font(GUI_FONT_ROLE_ROW), 0);
        lv_obj_set_width(title, lv_pct(100));
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t * hint = lv_label_create(subsonic_saved_servers_list);
        lv_label_set_text(hint, TR("Go back and choose New Connection to add one."));
        lv_obj_add_style(hint, &style_theme_text_muted, 0);
        lv_obj_set_style_text_font(hint, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
        lv_obj_set_width(hint, lv_pct(88));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    populate_indexed_list(subsonic_saved_servers_list, subsonic_saved_server_count, subsonic_saved_server_label_of,
                          subsonic_saved_server_row_cb);
}

static lv_obj_t * build_subsonic_saved_servers_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("Saved Servers"), &title_label, &subsonic_saved_servers_list);
}

static void subsonic_saved_servers_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    populate_subsonic_saved_servers_screen();
    nav_push(subsonic_saved_servers_screen);
}

static subsonic_server_t subsonic_new_conn_form = { .verify_tls = true };

static lv_obj_t * subsonic_new_connection_list;

static void populate_subsonic_new_connection_screen(void); /* defined below, after the row callbacks it references */

static void subsonic_new_conn_url_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.base_url, sizeof(subsonic_new_conn_form.base_url), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_url_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char title[96];
    snprintf(title, sizeof(title), TR("Server URL (e.g. %s)"), "https://myserver:4040");
    show_text_entry(title, subsonic_new_conn_form.base_url, false, false,
                    subsonic_new_conn_url_entry_done, NULL);
}

static void subsonic_new_conn_username_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.username, sizeof(subsonic_new_conn_form.username), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_username_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Username"), subsonic_new_conn_form.username, false, false, subsonic_new_conn_username_entry_done, NULL);
}

static void subsonic_new_conn_password_entry_done(const char * text, void * user_data) {
    (void) user_data;
    snprintf(subsonic_new_conn_form.password, sizeof(subsonic_new_conn_form.password), "%s", text);
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_password_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_text_entry(TR("Password"), subsonic_new_conn_form.password, true, false, subsonic_new_conn_password_entry_done, NULL);
}

static void subsonic_new_conn_verify_tls_toggle_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_new_conn_form.verify_tls = !subsonic_new_conn_form.verify_tls;
    populate_subsonic_new_connection_screen();
}

static void subsonic_new_conn_connect_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    start_subsonic_connect(&subsonic_new_conn_form);
}

static void populate_subsonic_new_connection_screen(void) {
    lv_obj_clean(subsonic_new_connection_list);

    char url_text[300];
    snprintf(url_text, sizeof(url_text), TR("Server URL: %s"),
             subsonic_new_conn_form.base_url[0] ? subsonic_new_conn_form.base_url : TR("Not set"));
    add_pill_chevron_row(subsonic_new_connection_list, url_text, subsonic_new_conn_url_row_cb);

    add_pill_toggle_row(subsonic_new_connection_list, TR("Verify server certificate"), subsonic_new_conn_form.verify_tls,
                        subsonic_new_conn_verify_tls_toggle_cb);

    char username_text[160];
    snprintf(username_text, sizeof(username_text), TR("Username: %s"),
             subsonic_new_conn_form.username[0] ? subsonic_new_conn_form.username : TR("Not set"));
    add_pill_chevron_row(subsonic_new_connection_list, username_text, subsonic_new_conn_username_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, subsonic_new_conn_form.password[0] ? TR("Password: Set") : TR("Password: Not set"),
                         subsonic_new_conn_password_row_cb);

    add_pill_chevron_row(subsonic_new_connection_list, TR("Connect & Browse"), subsonic_new_conn_connect_row_cb);
}

static lv_obj_t * build_subsonic_new_connection_screen(void) {
    lv_obj_t * title_label; /* unused after build -- title never changes */
    return build_subsonic_list_screen(TR("New Connection"), &title_label, &subsonic_new_connection_list);
}

static void subsonic_new_connection_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !gui_subsonic_wifi_connected_guard()) return;
    /* Only the text fields reset -- verify_tls deliberately carries over
     * between visits (no widget-desync risk here anymore, unlike the old
     * build_pill_list_screen()-based version -- this screen's rows are
     * fully rebuilt by populate_subsonic_new_connection_screen() below on
     * every visit, so there's nothing stale left to desync from). */
    subsonic_new_conn_form.base_url[0] = '\0';
    subsonic_new_conn_form.username[0] = '\0';
    subsonic_new_conn_form.password[0] = '\0';
    populate_subsonic_new_connection_screen();
    nav_push(subsonic_new_connection_screen);
}

static lv_obj_t * build_subsonic_entry_screen(void) {
    static pill_list_item_t items[2];
    items[0] = (pill_list_item_t){ TR("Saved Servers"), PILL_ACCESSORY_CHEVRON, false, subsonic_saved_servers_row_cb, NULL, NULL };
    items[1] = (pill_list_item_t){ TR("New Connection"), PILL_ACCESSORY_CHEVRON, false, subsonic_new_connection_row_cb, NULL, NULL };
    lv_obj_t * scr = build_pill_list_screen(TR("Subsonic"), generic_back_cb, items, 2, gui_theme_accent_style(), GUI_ROW_GAP, 100);
    finalize_screen_navigation(scr);
    return scr;
}

void subsonic_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    nav_push(subsonic_entry_screen);
}


void gui_subsonic_connect_build(void) {
    subsonic_entry_screen = build_subsonic_entry_screen();
    subsonic_saved_servers_screen = build_subsonic_saved_servers_screen();
    subsonic_new_connection_screen = build_subsonic_new_connection_screen();
}

void gui_subsonic_connect_delete(void) {
    if (subsonic_entry_screen) { lv_obj_delete(subsonic_entry_screen); subsonic_entry_screen = NULL; }
    if (subsonic_saved_servers_screen) { lv_obj_delete(subsonic_saved_servers_screen); subsonic_saved_servers_screen = NULL; }
    if (subsonic_new_connection_screen) { lv_obj_delete(subsonic_new_connection_screen); subsonic_new_connection_screen = NULL; }
}

void gui_subsonic_connect_cancel(void) {
    if (subsonic_connect_active) http_cancel_token_cancel(&subsonic_connect_cancel);
}

void gui_subsonic_connect_join(void) {
    if (subsonic_connect_active) {
        pthread_join(subsonic_connect_thread, NULL);
        subsonic_connect_active = false;
        http_cancel_token_destroy(&subsonic_connect_cancel);
        gui_busy_hide(subsonic_connect_token);
        subsonic_connect_token = 0;
    }
}
