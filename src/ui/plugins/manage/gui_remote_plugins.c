#define _POSIX_C_SOURCE 200809L
#include "gui_remote_plugins.h"

#include "../../../network/remote/control/remote_plugins.h"
#include "../../../network/remote/control/remote_plugin_store.h"
#include "../../../plugins/manager/plugin_manager.h"
#include "../../../plugins/lifecycle/plugin_disabled_list.h"
#include "../../../network/store/plugins/plugin_store.h"
#include "gui_plugin_store.h"
#include "../../player/layout/player_layouts.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../shell/gui_navigation.h"
#include "../../shell/gui_reload.h"
#include "../../settings/setup/gui_setup_plugins.h"
#include "../../shell/gui_notifications.h"
#include "../../../core/persistence/settings.h"
#include "gui_remote_plugin_images.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define REMOTE_PLUGIN_SCAN_MAX 400
#define REMOTE_PAGE_DEFAULT 50

static bool pending_soft_reload;
static uint64_t next_soft_retry_ms;

static bool support_module(const char * filename) {
    return filename && strcasecmp(filename, "AsyncHttp.lua") == 0;
}

static bool native_owner_busy(void) {
    return plugin_store_busy() || gui_plugin_store_operation_active() ||
           gui_setup_plugins_busy() || gui_reload_pending();
}

static bool ui_owner_busy(void) {
    return native_owner_busy() || pending_soft_reload;
}

static void fail(remote_plugins_result_t * result, int status, const char * code) {
    remote_plugins_error(result, status, code);
}

static bool add_string(cJSON * o, const char * key, const char * value) {
    return o && cJSON_AddStringToObject(o, key, value ? value : "") != NULL;
}

static bool add_number(cJSON * o, const char * key, double value) {
    return o && cJSON_AddNumberToObject(o, key, value) != NULL;
}

static bool add_bool(cJSON * o, const char * key, bool value) {
    return o && cJSON_AddBoolToObject(o, key, value) != NULL;
}

static bool serialize(remote_plugins_result_t * result, cJSON * root, int status) {
    if (!root) { fail(result, 500, "out_of_memory"); return false; }
    char * body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { fail(result, 500, "out_of_memory"); return false; }
    if (strlen(body) > REMOTE_PLUGINS_RESPONSE_MAX) {
        free(body);
        fail(result, 500, "response_too_large");
        return false;
    }
    result->status = status;
    result->body = body;
    return true;
}

static int page_limit(int limit) {
    if (limit <= 0) return REMOTE_PAGE_DEFAULT;
    return limit > REMOTE_PLUGINS_PAGE_MAX ? REMOTE_PLUGINS_PAGE_MAX : limit;
}

static bool scan_plugins(plugin_available_entry_t ** out, int * out_count) {
    plugin_available_entry_t * entries = calloc(REMOTE_PLUGIN_SCAN_MAX, sizeof(*entries));
    if (!entries) return false;
    int count = plugin_manager_scan_available(entries, REMOTE_PLUGIN_SCAN_MAX);
    if (count < 0) count = 0;
    if (count > REMOTE_PLUGIN_SCAN_MAX) count = REMOTE_PLUGIN_SCAN_MAX;
    *out = entries;
    *out_count = count;
    return true;
}

static void list_plugins(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    plugin_available_entry_t * entries = NULL;
    int count = 0;
    if (!scan_plugins(&entries, &count)) { fail(result, 500, "out_of_memory"); return; }
    int offset = job->offset < 0 ? 0 : job->offset;
    int limit = page_limit(job->limit);
    int visible_total = 0;
    for (int i = 0; i < count; ++i) if (!support_module(entries[i].filename)) visible_total++;
    cJSON * root = cJSON_CreateObject();
    cJSON * rows = cJSON_CreateArray();
    if (!root || !rows) {
        cJSON_Delete(root); cJSON_Delete(rows); free(entries);
        fail(result, 500, "out_of_memory"); return;
    }
    bool ok = add_number(root, "offset", offset) && add_number(root, "limit", limit) &&
              add_number(root, "total", visible_total) &&
              add_bool(root, "truncated", count == REMOTE_PLUGIN_SCAN_MAX) &&
              add_bool(root, "refresh_pending", pending_soft_reload || gui_reload_pending());
    int visible_index = 0;
    for (int i = 0; ok && i < count; ++i) {
        plugin_available_entry_t * entry = &entries[i];
        if (support_module(entry->filename)) continue;
        int row_index = visible_index++;
        if (row_index < offset) continue;
        if (row_index - offset >= limit) break;
        cJSON * row = cJSON_CreateObject();
        if (!row) { ok = false; break; }
        bool row_ok = add_string(row, "id", entry->filename) &&
                      add_string(row, "name", entry->display_name) &&
                      add_bool(row, "enabled", !entry->disabled) &&
                      add_bool(row, "loaded", entry->loaded) &&
                      add_bool(row, "can_open_settings", plugin_manager_file_has_settings(entry->filename));
        if (row_ok) {
            if (entry->loaded || entry->disabled) row_ok = cJSON_AddNullToObject(row, "error") != NULL;
            else row_ok = add_string(row, "error", entry->over_limit ? "load_limit_reached" : "load_failed");
        }
        row_ok = row_ok && add_bool(row, "player_layout", entry->player_layout) &&
                 add_bool(row, "theme_package", entry->theme_package);
        if (!row_ok || !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            ok = false;
        }
    }
    free(entries);
    if (!ok || !cJSON_AddItemToObject(root, "plugins", rows)) {
        cJSON_Delete(rows); cJSON_Delete(root); fail(result, 500, "out_of_memory"); return;
    }
    (void) serialize(result, root, 200);
}

static bool valid_filename(const char * s) {
    if (!s || !*s || strlen(s) >= sizeof(((plugin_available_entry_t *) 0)->filename) ||
        strcmp(s, "..") == 0) return false;
    for (const unsigned char * p = (const unsigned char *) s; *p; ++p)
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return false;
    return true;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

static void soft_reload_try(void) {
    if (!pending_soft_reload) return;
    /* A deferred refresh may outlive the request that queued it. Keep it
     * pending while the store worker can still be changing plugin files. */
    if (native_owner_busy()) return;
    uint64_t now = monotonic_ms();
    if (now < next_soft_retry_ms) return;
    next_soft_retry_ms = now + 1000u;
    if (!gui_reload_in_progress() && gui_reload_request_preserving_plugin_manager_page()) {
        pending_soft_reload = false;
        next_soft_retry_ms = 0;
    }
}

static void soft_reload_schedule(void) {
    pending_soft_reload = true;
    soft_reload_try();
}

static void set_enabled(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    if (plugin_store_busy() || gui_reload_in_progress()) { fail(result, 409, "busy"); return; }
    if (!valid_filename(job->id)) { fail(result, 404, "not_found"); return; }
    plugin_available_entry_t * entries = NULL;
    int count = 0;
    if (!scan_plugins(&entries, &count)) { fail(result, 500, "out_of_memory"); return; }
    plugin_available_entry_t * found = NULL;
    for (int i = 0; i < count; ++i)
        if (strcmp(entries[i].filename, job->id) == 0 && !support_module(entries[i].filename)) { found = &entries[i]; break; }
    if (!found) { free(entries); fail(result, 404, "not_found"); return; }
    bool saved = plugin_disabled_list_set(found->filename, !job->enabled);
    free(entries);
    if (!saved) { fail(result, 503, "storage_write_failed"); return; }
    soft_reload_schedule();
    cJSON * root = cJSON_CreateObject();
    if (!root || !add_bool(root, "accepted", true) || !add_bool(root, "refresh_requested", true)) {
        cJSON_Delete(root); fail(result, 500, "out_of_memory"); return;
    }
    (void) serialize(result, root, 202);
}

static void refresh_plugins(remote_plugins_result_t * result) {
    if (plugin_store_busy() || gui_reload_in_progress()) { fail(result, 409, "busy"); return; }
    soft_reload_schedule();
    cJSON * root = cJSON_CreateObject();
    if (!root || !add_bool(root, "accepted", true) || !add_bool(root, "refresh_requested", true)) {
        cJSON_Delete(root); fail(result, 500, "out_of_memory"); return;
    }
    (void) serialize(result, root, 202);
}

static const char * layout_source(player_layout_kind_t kind) {
    switch (kind) {
        case PLAYER_LAYOUT_BUILTIN_C: return "builtin_c";
        case PLAYER_LAYOUT_EXPORTED_C: return "exported_c";
        default: return "xml_file";
    }
}

static void list_layouts(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    /* Match the Settings picker, but don't race a store install/removal that
     * may currently be changing the registered XML layout files. */
    if (!ui_owner_busy()) player_layouts_rescan();
    int count = player_layouts_count();
    int offset = job->offset < 0 ? 0 : job->offset;
    int limit = page_limit(job->limit);
    cJSON * root = cJSON_CreateObject();
    cJSON * rows = cJSON_CreateArray();
    if (!root || !rows) { cJSON_Delete(root); cJSON_Delete(rows); fail(result, 500, "out_of_memory"); return; }
    bool ok = add_string(root, "current", player_layouts_effective_id()) &&
              add_number(root, "offset", offset) && add_number(root, "limit", limit) &&
              add_number(root, "total", count);
    for (int i = 0; ok && i < count; ++i) {
        if (i < offset) continue;
        if (i - offset >= limit) break;
        const player_layout_info_t * info = player_layouts_get(i);
        if (!info) { ok = false; break; }
        char preview[512];
        bool has_preview = player_layouts_get_preview(i, preview, sizeof(preview));
        cJSON * row = cJSON_CreateObject();
        if (!row) { ok = false; break; }
        bool row_ok = add_string(row, "id", info->id) && add_string(row, "name", info->name) &&
                      add_string(row, "source", layout_source(info->kind)) &&
                      add_bool(row, "has_preview", has_preview);
        if (!row_ok || !cJSON_AddItemToArray(rows, row)) { cJSON_Delete(row); ok = false; }
    }
    if (!ok || !cJSON_AddItemToObject(root, "layouts", rows)) {
        cJSON_Delete(rows); cJSON_Delete(root); fail(result, 500, "out_of_memory"); return;
    }
    (void) serialize(result, root, 200);
}

static void select_layout(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    if (ui_owner_busy()) { fail(result, 409, "busy"); return; }
    player_layouts_rescan();
    int index = player_layouts_find(job->id);
    const player_layout_info_t * info = index >= 0 ? player_layouts_get(index) : NULL;
    if (!info) { fail(result, 404, "not_found"); return; }
    player_layouts_session_clear_selection();
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s",
             strcmp(info->id, PLAYER_LAYOUT_ID_DEFAULT) == 0 ? "" : info->id);
    settings_save(&current_settings);
    gui_player_layout_reload_request();
    cJSON * root = cJSON_CreateObject();
    if (!root || !add_bool(root, "accepted", true) || !add_string(root, "current", info->id)) {
        cJSON_Delete(root); fail(result, 500, "out_of_memory"); return;
    }
    (void) serialize(result, root, 202);
}

static void send_preview(const char * path, bool png_only, remote_plugins_result_t * result) {
    result->body = gui_remote_plugin_preview_json(path, png_only);
    if (!result->body) { fail(result, 503, "preview_unavailable"); return; }
    result->status = 200;
}

static void pending_preview(remote_plugins_result_t * result) {
    cJSON * root = cJSON_CreateObject();
    if (!root || !add_bool(root, "pending", true)) {
        cJSON_Delete(root);
        fail(result, 503, "out_of_memory");
        return;
    }
    (void) serialize(result, root, 202);
}

static void store_preview(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    plugin_store_preview_state_t state = plugin_store_preview_state(job->id);
    if (state == PLUGIN_STORE_PREVIEW_UNKNOWN) { fail(result, 404, "plugin_not_found"); return; }
    if (state == PLUGIN_STORE_PREVIEW_NONE) { fail(result, 404, "preview_not_found"); return; }
    if (state == PLUGIN_STORE_PREVIEW_FAILED) { fail(result, 503, "preview_unavailable"); return; }
    if (state == PLUGIN_STORE_PREVIEW_PENDING) { pending_preview(result); return; }
    char path[512];
    if (state == PLUGIN_STORE_PREVIEW_READY) {
        if (!plugin_store_get_preview_jpeg(job->id, path, sizeof(path))) { fail(result, 503, "preview_unavailable"); return; }
        send_preview(path, false, result);
        return;
    }
    /* A ready cache remains readable while another owner is busy. Starting a
     * new preview is allowed only when store/setup/reload state is idle. */
    if (ui_owner_busy()) { fail(result, 409, "busy"); return; }
    const char * ids[] = { job->id };
    (void) plugin_store_prepare_previews(ids, 1);
    state = plugin_store_preview_state(job->id);
    if (state == PLUGIN_STORE_PREVIEW_READY && plugin_store_get_preview_jpeg(job->id, path, sizeof(path))) {
        send_preview(path, false, result);
    } else if (state == PLUGIN_STORE_PREVIEW_PENDING) {
        pending_preview(result);
    } else if (state == PLUGIN_STORE_PREVIEW_NONE) {
        fail(result, 404, "preview_not_found");
    } else if (state == PLUGIN_STORE_PREVIEW_UNKNOWN) {
        fail(result, 404, "plugin_not_found");
    } else {
        fail(result, 503, "preview_unavailable");
    }
}

static void layout_preview(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    int index = player_layouts_find(job->id);
    if (index < 0) { fail(result, 404, "layout_not_found"); return; }
    char path[512];
    if (!player_layouts_get_preview(index, path, sizeof(path))) { fail(result, 404, "preview_not_found"); return; }
    send_preview(path, true, result);
}

static void open_plugin_settings(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    if (!current_settings.setup_complete || gui_navigation_transition_in_progress() ||
        gui_lock_screen_is_showing() || gui_lock_screen_unlock_pending() ||
        lv_screen_active() == gui_busy_get_screen()) {
        fail(result, 409, "ui_not_ready");
        return;
    }
    plugin_available_entry_t * entries = NULL;
    int count = 0;
    if (!scan_plugins(&entries, &count)) { fail(result, 503, "out_of_memory"); return; }
    bool found = false;
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].filename, job->id) == 0) { found = true; break; }
    }
    free(entries);
    if (!found || !plugin_manager_file_has_settings(job->id)) { fail(result, 404, "settings_not_found"); return; }
    cJSON * root = cJSON_CreateObject();
    if (!root || !add_bool(root, "accepted", true)) {
        cJSON_Delete(root); fail(result, 503, "out_of_memory"); return;
    }
    char * body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) { fail(result, 503, "out_of_memory"); return; }
    if (!plugin_manager_open_file_settings(job->id)) {
        free(body);
        fail(result, 404, "settings_not_found");
        return;
    }
    result->status = 202;
    result->body = body;
}

void gui_remote_plugins_poll(void) {
    remote_plugins_job_t job;
    remote_plugins_result_t result = {0};
    if (remote_plugins_consume_job(&job)) {
        bool is_mutation = job.operation >= REMOTE_PLUGINS_STORE_REFRESH &&
                           job.operation <= REMOTE_PLUGINS_STORE_UNINSTALL;
        is_mutation = is_mutation || job.operation == REMOTE_PLUGINS_SET_ENABLED ||
                      job.operation == REMOTE_PLUGINS_REFRESH ||
                      job.operation == REMOTE_PLUGINS_SELECT_LAYOUT ||
                      job.operation == REMOTE_PLUGINS_OPEN_SETTINGS;
        if (is_mutation && ui_owner_busy()) {
            fail(&result, 409, "busy");
        } else if (job.operation <= REMOTE_PLUGINS_STORE_UNINSTALL) {
            remote_plugin_store_execute(&job, &result);
        } else switch (job.operation) {
            case REMOTE_PLUGINS_LIST: list_plugins(&job, &result); break;
            case REMOTE_PLUGINS_SET_ENABLED: set_enabled(&job, &result); break;
            case REMOTE_PLUGINS_REFRESH: refresh_plugins(&result); break;
            case REMOTE_PLUGINS_LAYOUTS: list_layouts(&job, &result); break;
            case REMOTE_PLUGINS_SELECT_LAYOUT: select_layout(&job, &result); break;
            case REMOTE_PLUGINS_STORE_PREVIEW: store_preview(&job, &result); break;
            case REMOTE_PLUGINS_LAYOUT_PREVIEW: layout_preview(&job, &result); break;
            case REMOTE_PLUGINS_OPEN_SETTINGS: open_plugin_settings(&job, &result); break;
            default: fail(&result, 400, "invalid_operation"); break;
        }
        remote_plugins_finish_job(&result);
    }
    soft_reload_try();
}
