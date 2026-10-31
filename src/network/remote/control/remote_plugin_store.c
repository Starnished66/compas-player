#include "remote_plugin_store.h"

#include "../../../network/store/plugins/plugin_store.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The native store retains a mutex-protected result snapshot. Copy it on the
 * UI thread, then build JSON without holding any native store locks. */
static const char * state_name(plugin_store_state_t state) {
    switch (state) {
        case PLUGIN_STORE_IDLE: return "idle";
        case PLUGIN_STORE_REFRESHING: return "refreshing";
        case PLUGIN_STORE_INSTALLING: return "installing";
        case PLUGIN_STORE_UPDATING: return "updating";
        case PLUGIN_STORE_UPDATING_ALL: return "updating_all";
        case PLUGIN_STORE_UNINSTALLING: return "uninstalling";
        case PLUGIN_STORE_READY: return "ready";
        case PLUGIN_STORE_NEEDS_CONFIRM: return "needs_confirm";
        case PLUGIN_STORE_FAILED: return "failed";
        default: return "failed";
    }
}

static const char * plugin_state_name(plugin_store_plugin_state_t state) {
    switch (state) {
        case PLUGIN_STORE_PLUGIN_AVAILABLE: return "available";
        case PLUGIN_STORE_PLUGIN_INSTALLED: return "installed";
        case PLUGIN_STORE_PLUGIN_UPDATE: return "update";
        case PLUGIN_STORE_PLUGIN_MANUAL: return "manual";
        case PLUGIN_STORE_PLUGIN_INCOMPATIBLE: return "incompatible";
        case PLUGIN_STORE_PLUGIN_REMOVED: return "removed";
        default: return "available";
    }
}

static bool valid_id(const char * id) {
    if (!id) return false;
    size_t n = strlen(id);
    if (n < 1 || n > 63 || !((*id >= 'a' && *id <= 'z') || (*id >= '0' && *id <= '9'))) return false;
    for (size_t i = 1; i < n; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

static bool add_string(cJSON * object, const char * key, const char * value) {
    return cJSON_AddStringToObject(object, key, value ? value : "") != NULL;
}

static bool add_number(cJSON * object, const char * key, double value) {
    return cJSON_AddNumberToObject(object, key, value) != NULL;
}

static bool add_bool(cJSON * object, const char * key, bool value) {
    return cJSON_AddBoolToObject(object, key, value) != NULL;
}

static void free_status_rows(plugin_store_result_t * rows) { free(rows); }

static bool make_state_body(const plugin_store_status_t * status,
                            const plugin_store_result_t * rows, size_t count,
                            int offset, int limit, bool accepted,
                            remote_plugins_result_t * result) {
    cJSON * root = cJSON_CreateObject();
    cJSON * plugins = cJSON_CreateArray();
    if (!root || !plugins) goto fail;
    if (!add_string(root, "state", state_name(status->state)) ||
        !add_number(root, "percent", status->percent < 0 ? 0 : status->percent > 100 ? 100 : status->percent) ||
        !add_string(root, "current_plugin", status->current_plugin)) goto fail;
    if (status->error[0]) {
        if (!add_string(root, "error", status->error)) goto fail;
    } else if (!cJSON_AddNullToObject(root, "error")) goto fail;
    if (!add_bool(root, "changed", status->changed) ||
        !add_bool(root, "refresh_required", status->refresh_required) ||
        !add_bool(root, "confirmation_required", status->state == PLUGIN_STORE_NEEDS_CONFIRM) ||
        (accepted && !add_bool(root, "accepted", true)) ||
        !add_number(root, "offset", offset) || !add_number(root, "limit", limit) ||
        !add_number(root, "total", (double)count)) goto fail;
    if (!cJSON_AddItemToObject(root, "plugins", plugins)) {
        cJSON_Delete(plugins);
        plugins = NULL;
        goto fail;
    }
    plugins = NULL; /* root owns the array now */
    plugins = cJSON_GetObjectItemCaseSensitive(root, "plugins");

    size_t end = (size_t)offset + (size_t)limit;
    if (end > count) end = count;
    for (size_t i = (size_t)offset; i < end; ++i) {
        const plugin_store_result_t * row = &rows[i];
        plugin_store_details_t details = {0};
        (void)plugin_store_get_details(row->id, &details);
        cJSON * item = cJSON_CreateObject();
        if (!item) goto fail;
        if (!cJSON_AddItemToArray(plugins, item)) {
            cJSON_Delete(item);
            goto fail;
        }
        if (!add_string(item, "id", row->id) || !add_string(item, "name", row->name) ||
            !add_string(item, "version", row->version) ||
            !add_string(item, "state", plugin_state_name(row->state))) goto fail;
        if (!add_bool(item, "incompatible", row->incompatible) ||
            !add_bool(item, "needs_confirmation", row->needs_confirmation) ||
            !add_bool(item, "player_layout", row->player_layout) ||
            !add_bool(item, "theme_pack", row->theme_pack)) goto fail;
        if (!add_string(item, "description", details.description) ||
            !add_string(item, "category", details.category) ||
            !add_string(item, "author", details.author) ||
            !add_number(item, "size", details.size)) goto fail;
    }

    char * printed = cJSON_PrintUnformatted(root);
    if (!printed) goto fail;
    if (strlen(printed) > REMOTE_PLUGINS_RESPONSE_MAX) {
        free(printed);
        cJSON_Delete(root);
        remote_plugins_error(result, 503, "response_too_large");
        return false;
    }
    cJSON_Delete(root);
    result->status = 200;
    result->body = printed;
    return true;
fail:
    if (plugins && (!root || !cJSON_GetObjectItemCaseSensitive(root, "plugins"))) cJSON_Delete(plugins);
    cJSON_Delete(root);
    return false;
}

static bool snapshot(remote_plugins_result_t * result, int offset, int limit, bool accepted) {
    plugin_store_status_t status = {0};
    plugin_store_result_t * rows = calloc(PLUGIN_STORE_MAX_RESULTS, sizeof(*rows));
    if (!rows) return false;
    plugin_store_get_status(&status, rows, PLUGIN_STORE_MAX_RESULTS);
    size_t count = status.result_count;
    if (count > PLUGIN_STORE_MAX_RESULTS) count = PLUGIN_STORE_MAX_RESULTS;
    bool ok = make_state_body(&status, rows, count, offset, limit, accepted, result);
    free_status_rows(rows);
    return ok;
}

static bool add_action_response(remote_plugins_result_t * result) {
    if (snapshot(result, 0, 0, true)) {
        result->status = 202;
        return true;
    }
    return false;
}

static void accepted_fallback(remote_plugins_result_t * result) {
    static const char body[] = "{\"accepted\":true}";
    free(result->body);
    result->body = malloc(sizeof(body));
    if (result->body) memcpy(result->body, body, sizeof(body));
    result->status = 202;
}

void remote_plugin_store_execute(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    if (!job || !result) return;
    result->status = 500;
    result->body = NULL;
    if (job->operation == REMOTE_PLUGINS_STORE_STATE) {
        int offset = job->offset < 0 ? 0 : job->offset;
        int limit = job->limit < 0 ? 0 : job->limit;
        if (limit > REMOTE_PLUGINS_PAGE_MAX) limit = REMOTE_PLUGINS_PAGE_MAX;
        if (offset > PLUGIN_STORE_MAX_RESULTS) offset = PLUGIN_STORE_MAX_RESULTS;
        if (!snapshot(result, offset, limit, false) && !result->body)
            remote_plugins_error(result, 503, "store_unavailable");
        return;
    }
    if (job->operation != REMOTE_PLUGINS_STORE_REFRESH &&
        job->operation != REMOTE_PLUGINS_STORE_INSTALL &&
        job->operation != REMOTE_PLUGINS_STORE_UPDATE &&
        job->operation != REMOTE_PLUGINS_STORE_UPDATE_ALL &&
        job->operation != REMOTE_PLUGINS_STORE_UNINSTALL) {
        remote_plugins_error(result, 400, "unsupported_operation");
        return;
    }
    if (plugin_store_busy()) {
        remote_plugins_error(result, 409, "store_busy");
        return;
    }
    if (job->operation != REMOTE_PLUGINS_STORE_REFRESH) {
        plugin_store_status_t status = {0};
        plugin_store_get_status(&status, NULL, 0);
        if (status.result_count == 0 &&
            (status.state == PLUGIN_STORE_IDLE || status.state == PLUGIN_STORE_FAILED)) {
            remote_plugins_error(result, 409, "catalog_required");
            return;
        }
    }
    bool accepted = false;
    if (job->operation == REMOTE_PLUGINS_STORE_REFRESH) {
        accepted = plugin_store_refresh();
    } else if (job->operation == REMOTE_PLUGINS_STORE_UPDATE_ALL) {
        accepted = plugin_store_update_all();
    } else {
        if (!valid_id(job->id)) {
            remote_plugins_error(result, 400, "invalid_plugin_id");
            return;
        }
        if (job->operation == REMOTE_PLUGINS_STORE_UNINSTALL) {
            plugin_store_status_t status = {0};
            plugin_store_result_t * rows = calloc(PLUGIN_STORE_MAX_RESULTS, sizeof(*rows));
            if (!rows) { remote_plugins_error(result, 503, "store_unavailable"); return; }
            plugin_store_get_status(&status, rows, PLUGIN_STORE_MAX_RESULTS);
            size_t count = status.result_count > PLUGIN_STORE_MAX_RESULTS ? PLUGIN_STORE_MAX_RESULTS : status.result_count;
            bool found = false;
            for (size_t i = 0; i < count; ++i) if (!strcmp(rows[i].id, job->id)) { found = true; break; }
            free(rows);
            if (!found) { remote_plugins_error(result, 404, "plugin_not_found"); return; }
            accepted = plugin_store_uninstall(job->id);
        } else {
            plugin_store_details_t details = {0};
            if (!plugin_store_get_details(job->id, &details)) {
                remote_plugins_error(result, 404, "plugin_not_found");
                return;
            }
            accepted = job->operation == REMOTE_PLUGINS_STORE_INSTALL
                ? plugin_store_install(job->id, job->force)
                : plugin_store_update(job->id, job->force);
        }
    }
    if (!accepted) {
        if (plugin_store_busy()) remote_plugins_error(result, 409, "store_busy");
        else remote_plugins_error(result, 503, "store_start_failed");
        return;
    }
    if (!add_action_response(result)) accepted_fallback(result);
}
