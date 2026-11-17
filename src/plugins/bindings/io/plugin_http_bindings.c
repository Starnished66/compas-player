#include "plugin_http_bindings.h"

#include "../../../network/http/client/http_client.h"
#include "../../../network/download/download.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../storage/plugin_storage.h"
#include "../../storage/plugin_reserved_dirs.h"
#include "plugin_archive_bindings.h"

#include "lauxlib.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLUGIN_MAX_ASYNC_HTTP 4
#define PLUGIN_ASYNC_HTTP_DEFAULT_MAX (512U * 1024U)
#define PLUGIN_ASYNC_HTTP_MAX_RESPONSE (2U * 1024U * 1024U)
#define PLUGIN_ASYNC_HTTP_MAX_REQUEST (1024U * 1024U)

typedef struct {
    bool active;
    atomic_bool done;
    /* Token tracking cancellation state and allowing socket interrupt via shutdown(). */
    http_cancel_token_t cancel;
    uint16_t generation;
    pthread_t thread;
    lua_State * L;
    int callback_ref;
    bool is_download;
    uint64_t downloaded_bytes; /* protected by plugin_download_progress_mutex */
    uint64_t total_bytes;
    char dest_path[PATH_MAX]; /* download only */
    bool verify_tls;

    char url[2048];
    http_method_t method;
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
    /* The one header value longer than a slot, owned (heap), or NULL. */
    char long_header_name[HTTP_HEADER_NAME_MAX];
    char * long_header_value;
    uint8_t * request_body;
    size_t request_body_size;
    char content_type[128];
    size_t max_response_size;
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t total_timeout_ms;
    int redirect_limit;

    /* Result fields -- the plain-request path (below) populates all of
     * these; the download path only ever uses `ok`/dest_path. */
    bool ok;
    int status;
    uint8_t * response_body;
    size_t response_body_size;
    http_header_t response_headers[HTTP_MAX_HEADERS];
    int response_header_count;
    const char * response_error; /* one of http_client.h's HTTP_ERR_* constants -- a stable static string, never malloc'd/freed */
} plugin_async_http_t;

static plugin_async_http_t plugin_async_http[PLUGIN_MAX_ASYNC_HTTP];
static pthread_mutex_t plugin_download_progress_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool plugin_async_download_progress(uint64_t downloaded, uint64_t total, void * user_data) {
    plugin_async_http_t * req = (plugin_async_http_t *) user_data;
    pthread_mutex_lock(&plugin_download_progress_mutex);
    req->downloaded_bytes = downloaded;
    req->total_bytes = total;
    pthread_mutex_unlock(&plugin_download_progress_mutex);
    return !http_cancel_token_is_cancelled(&req->cancel);
}

/* Collapses headers in place so only the last occurrence of each case-insensitive
 * header name is retained when passed to Lua callbacks. */
static void plugin_dedupe_headers_case_insensitive(http_header_t * headers, int * count) {
    int out = 0;
    for (int i = 0; i < *count; i++) {
        bool superseded = false;
        for (int j = i + 1; j < *count; j++) {
            if (strcasecmp(headers[i].name, headers[j].name) == 0) { superseded = true; break; }
        }
        if (superseded) continue;
        if (out != i) headers[out] = headers[i];
        out++;
    }
    *count = out;
}

static bool plugin_async_download(plugin_async_http_t *req) {
    /* Recheck restrictions in the worker before opening the destination parent. */
    if (plugin_storage_path_is_reserved(req->dest_path) || plugin_path_in_program_dirs(req->dest_path))
        return false;
    download_request_t request = {.url = req->url, .dest_path = req->dest_path,
        .max_bytes = UINT64_C(2) << 30, .verify_tls = req->verify_tls,
        .connect_timeout_ms = 10000U, .read_timeout_ms = 30000U};
    download_result_t result;
    download_error_t error = download_to_file(&request, &req->cancel, plugin_async_download_progress, req, &result);
    if (error == DOWNLOAD_ERR_DIR_SYNC && result.committed) {
        fprintf(stderr, "plugin: download committed but directory sync failed: %s\n", req->dest_path);
        return true;
    }
    return error == DOWNLOAD_OK;
}

static void * plugin_async_http_thread_func(void * arg) {
    plugin_async_http_t * req = (plugin_async_http_t *) arg;
    if (req->is_download) {
        req->ok = plugin_async_download(req);
    } else {
        /* plugin.http_request()'s plain (non-download) path -- shares the
         * extended http_request_ex() (headers, methods beyond GET/POST,
         * timeouts, redirects, response headers, real cancellation) with
         * the future authenticated audio-stream reader, per this app's
         * own remote-provider design (see http_client.h's own comment on
         * why this is separate, new code rather than a rework of the
         * do_get()/do_post()-based functions used elsewhere). */
        http_request_t hreq;
        memset(&hreq, 0, sizeof(hreq));
        snprintf(hreq.url, sizeof(hreq.url), "%s", req->url);
        hreq.method = req->method;
        memcpy(hreq.headers, req->headers, sizeof(hreq.headers));
        hreq.header_count = req->header_count;
        if (req->long_header_value) {
            memcpy(hreq.long_header_name, req->long_header_name, sizeof(hreq.long_header_name));
            hreq.long_header_value = req->long_header_value;
        }
        /* Backward-compat note: the OLD GET path (http_get_to_buffer_
         * limited()) never received request_body at all -- a plugin's
         * "body" field was parsed and allocated regardless of method, but
         * silently dropped (never sent, never even passed to the GET
         * function) unless method was POST. Likewise, do_get() never
         * built a Content-Type header into the request either, even
         * though the Lua-facing content_type field already defaulted to a
         * non-empty string regardless of method. Preserve BOTH exactly:
         * only pass body and content_type through for methods that
         * conventionally carry one, so an existing GET-only plugin's
         * request is byte-for-byte the same on the wire as before -- not
         * a new (harmless to most servers, but still a real behavior
         * change) body/Content-Length/Content-Type appearing on a GET for
         * the first time just because the field happened to be set. */
        bool method_has_body = req->method == HTTP_METHOD_POST || req->method == HTTP_METHOD_PUT ||
                                req->method == HTTP_METHOD_PATCH;
        hreq.body = method_has_body ? req->request_body : NULL;
        hreq.body_len = method_has_body ? req->request_body_size : 0;
        hreq.content_type = (method_has_body && req->content_type[0]) ? req->content_type : NULL;
        hreq.verify_tls = req->verify_tls;
        hreq.connect_timeout_ms = req->connect_timeout_ms;
        hreq.read_timeout_ms = req->read_timeout_ms;
        hreq.total_timeout_ms = req->total_timeout_ms;
        hreq.max_response_bytes = req->max_response_size;
        hreq.redirect_limit = req->redirect_limit;

        http_response_t hresp;
        req->ok = http_request_ex(&hreq, &req->cancel, &hresp);
        req->status = hresp.status;
        req->response_body = hresp.body; /* ownership transferred -- do NOT http_response_free() this; freed via plugin_manager_poll()'s existing free(req->response_body) */
        req->response_body_size = hresp.body_len;
        memcpy(req->response_headers, hresp.headers, sizeof(req->response_headers));
        req->response_header_count = hresp.header_count;
        plugin_dedupe_headers_case_insensitive(req->response_headers, &req->response_header_count);
        req->response_error = hresp.error;
    }
    free(req->request_body);
    req->request_body = NULL;
    req->request_body_size = 0;
    free(req->long_header_value);
    req->long_header_value = NULL;
    atomic_store(&req->done, true);
    return NULL;
}

typedef struct {
    char url_copy[sizeof(plugin_async_http[0].url)];
    char content_type_copy[sizeof(plugin_async_http[0].content_type)];
    http_method_t method;
    size_t body_size;
    int body_root;
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count;
    char long_header_name[HTTP_HEADER_NAME_MAX];
    const char *long_value_src;
    size_t long_value_len;
    bool verify_tls;
    lua_Integer requested_max;
    lua_Integer connect_timeout_ms;
    lua_Integer read_timeout_ms;
    lua_Integer total_timeout_ms;
    lua_Integer redirect_limit;
} http_options_t;

static int find_free_slot(void) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++)
        if (!plugin_async_http[i].active) return i;
    return -1;
}

static int read_http_strings(lua_State *L, http_options_t *opts) {

    lua_getfield(L, 1, "url");
    const char * url = luaL_checkstring(L, -1);
    size_t url_len = strlen(url);
    if (url_len >= sizeof(opts->url_copy)) {
        return luaL_error(L, "plugin.http_request: URL is too long");
    }
    memcpy(opts->url_copy, url, url_len + 1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "method");
    const char * method_str = luaL_optstring(L, -1, "GET");

    if (strcasecmp(method_str, "GET") == 0) opts->method = HTTP_METHOD_GET;
    else if (strcasecmp(method_str, "POST") == 0) opts->method = HTTP_METHOD_POST;
    else if (strcasecmp(method_str, "PUT") == 0) opts->method = HTTP_METHOD_PUT;
    else if (strcasecmp(method_str, "PATCH") == 0) opts->method = HTTP_METHOD_PATCH;
    else if (strcasecmp(method_str, "DELETE") == 0) opts->method = HTTP_METHOD_DELETE;
    else if (strcasecmp(method_str, "HEAD") == 0) opts->method = HTTP_METHOD_HEAD;
    else return luaL_error(L, "plugin.http_request: unknown method '%s'", method_str);
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");

    const char * body = lua_isnil(L, -1) ? NULL : luaL_checklstring(L, -1, &opts->body_size);
    if (opts->body_size > PLUGIN_ASYNC_HTTP_MAX_REQUEST) {
        return luaL_error(L, "plugin.http_request: request body exceeds %u bytes", PLUGIN_ASYNC_HTTP_MAX_REQUEST);
    }

    opts->body_root = body ? lua_gettop(L) : 0;
    if (!body) lua_pop(L, 1);

    lua_getfield(L, 1, "content_type");
    const char * content_type = luaL_optstring(L, -1, "application/x-www-form-urlencoded");

    size_t content_type_len = strlen(content_type);
    if (content_type_len >= sizeof(opts->content_type_copy)) {
        return luaL_error(L, "plugin.http_request: content_type is too long");
    }
    memcpy(opts->content_type_copy, content_type, content_type_len + 1);
    lua_pop(L, 1);

    return 0;
}

static int read_http_headers(lua_State *L, http_options_t *opts) {
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);

        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {

                size_t name_len = 0, value_len = 0;
                const char * name = lua_tolstring(L, -2, &name_len);
                const char * value = lua_tolstring(L, -1, &value_len);
                if (name && value) {
                    bool long_value = value_len >= sizeof(opts->headers[0].value);
                    if (strlen(name) != name_len || strlen(value) != value_len) {
                        return luaL_error(L, "plugin.http_request: header '%s' contains a NUL byte", name);
                    }
                    if (name_len >= sizeof(opts->headers[0].name) ||
                        (long_value && (opts->long_value_src || value_len >= HTTP_LONG_HEADER_VALUE_MAX ||
                                        lua_type(L, -1) != LUA_TSTRING))) {
                        return luaL_error(L, "plugin.http_request: header '%s' name/value exceeds %d/%d bytes "
                                          "(one value may be up to %d)",
                                          name, (int) sizeof(opts->headers[0].name) - 1, (int) sizeof(opts->headers[0].value) - 1,
                                          HTTP_LONG_HEADER_VALUE_MAX - 1);
                    }
                    if (!long_value && opts->header_count >= HTTP_MAX_HEADERS) {
                        return luaL_error(L, "plugin.http_request: at most %d headers", HTTP_MAX_HEADERS);
                    }
                    if (long_value) {
                        opts->long_value_src = value;
                        opts->long_value_len = value_len;
                        memcpy(opts->long_header_name, name, name_len + 1);
                    } else {
                        memcpy(opts->headers[opts->header_count].name, name, name_len + 1);
                        memcpy(opts->headers[opts->header_count].value, value, value_len + 1);
                        opts->header_count++;
                    }
                }
            }
            lua_pop(L, 1);
        }
    }

    if (opts->long_value_src) {
        lua_pushstring(L, opts->long_header_name);
        lua_rawget(L, -2);
        size_t len = 0;
        opts->long_value_src = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
        if (!opts->long_value_src || len != opts->long_value_len) {
            return luaL_error(L, "plugin.http_request: header '%s' changed while reading", opts->long_header_name);
        }
    }

    return 0;
}

static int read_http_scalar_options(lua_State *L, http_options_t *opts) {
    lua_getfield(L, 1, "verify_tls");
    opts->verify_tls = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "max_response_bytes");
    opts->requested_max = luaL_optinteger(L, -1, PLUGIN_ASYNC_HTTP_DEFAULT_MAX);
    lua_pop(L, 1);
    if (opts->requested_max < 1 || opts->requested_max > PLUGIN_ASYNC_HTTP_MAX_RESPONSE) {
        return luaL_error(L, "plugin.http_request: max_response_bytes must be 1..%u",
                          PLUGIN_ASYNC_HTTP_MAX_RESPONSE);
    }

    lua_getfield(L, 1, "connect_timeout_ms");
    opts->connect_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "read_timeout_ms");
    opts->read_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "total_timeout_ms");
    opts->total_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "redirect_limit");
    opts->redirect_limit = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    if (opts->connect_timeout_ms < 0 || opts->read_timeout_ms < 0 || opts->total_timeout_ms < 0 || opts->redirect_limit < 0) {
        return luaL_error(L, "plugin.http_request: timeout/redirect_limit fields must not be negative");
    }

#define PLUGIN_HTTP_MAX_TIMEOUT_MS (5 * 60 * 1000)
#define PLUGIN_HTTP_MAX_REDIRECTS 10
    if (opts->connect_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS || opts->read_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS ||
        opts->total_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS) {
        return luaL_error(L, "plugin.http_request: connect_timeout_ms/read_timeout_ms/total_timeout_ms must not exceed %d",
                          PLUGIN_HTTP_MAX_TIMEOUT_MS);
    }
    if (opts->redirect_limit > PLUGIN_HTTP_MAX_REDIRECTS) {
        return luaL_error(L, "plugin.http_request: redirect_limit must not exceed %d", PLUGIN_HTTP_MAX_REDIRECTS);
    }

    return 0;
}

static plugin_async_http_t *prepare_http_slot(lua_State *L, int slot, http_options_t *opts,
    int callback_ref, uint8_t *body_copy, char *long_header_value) {
    plugin_async_http_t * req = &plugin_async_http[slot];
    uint16_t generation = (uint16_t) (req->generation + 1);
    if (generation == 0) generation = 1;
    memset(req, 0, sizeof(*req));
    atomic_init(&req->done, false);
    http_cancel_token_init(&req->cancel);
    req->generation = generation;
    req->L = plugin_lua_main_state(L);
    req->method = opts->method;
    memcpy(req->headers, opts->headers, sizeof(req->headers));
    req->header_count = opts->header_count;
    memcpy(req->long_header_name, opts->long_header_name, sizeof(req->long_header_name));
    req->long_header_value = long_header_value;
    req->verify_tls = opts->verify_tls;
    req->request_body = body_copy;
    req->request_body_size = opts->body_size;
    req->max_response_size = (size_t) opts->requested_max;
    req->connect_timeout_ms = (uint32_t) opts->connect_timeout_ms;
    req->read_timeout_ms = (uint32_t) opts->read_timeout_ms;
    req->total_timeout_ms = (uint32_t) opts->total_timeout_ms;
    req->redirect_limit = (int) opts->redirect_limit;
    snprintf(req->url, sizeof(req->url), "%s", opts->url_copy);
    snprintf(req->content_type, sizeof(req->content_type), "%s", opts->content_type_copy);
    req->callback_ref = callback_ref;

    return req;
}

static int start_http_request(lua_State *L, int slot, http_options_t *opts) {
    uint8_t *body_copy = NULL;
    lua_pushvalue(L, 2);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (opts->body_size > 0) {
        body_copy = malloc(opts->body_size);
        if (!body_copy) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
            return plugin_lua_push_error(L, "out of memory");
        }
        memcpy(body_copy, lua_tostring(L, opts->body_root), opts->body_size);
    }
    if (opts->body_root) lua_remove(L, opts->body_root);

    char * long_header_value = NULL;
    if (opts->long_value_src) {
        long_header_value = malloc(opts->long_value_len + 1);
        if (!long_header_value) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
            free(body_copy);
            return plugin_lua_push_error(L, "out of memory");
        }
        memcpy(long_header_value, opts->long_value_src, opts->long_value_len + 1);
    }

    plugin_async_http_t *req = prepare_http_slot(L, slot, opts, callback_ref, body_copy, long_header_value);
    if (pthread_create(&req->thread, NULL, plugin_async_http_thread_func, req) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->request_body);
        req->request_body = NULL;
        free(req->long_header_value);
        req->long_header_value = NULL;
        req->active = false;
        http_cancel_token_destroy(&req->cancel);
        return plugin_lua_push_error(L, "could not start HTTP worker");
    }

    req->active = true;
    int handle = ((int) req->generation << 8) | (slot + 1);
    lua_pushinteger(L, handle);
    return 1;
}

int l_plugin_http_request(lua_State *L) {
    int initial_top = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    http_options_t opts = {0};
    read_http_strings(L, &opts);
    read_http_headers(L, &opts);
    read_http_scalar_options(L, &opts);
    int slot = find_free_slot();
    if (slot < 0) {
        lua_settop(L, initial_top);
        return plugin_lua_push_error(L, "too many active HTTP requests");
    }
    return start_http_request(L, slot, &opts);
}

/* plugin.download_file_async(url, dest_path [, verify_tls], callback)
 * -> handle | nil, error. Streams into a unique sibling temporary file on
 * the native HTTP worker, then atomically renames it over dest_path only
 * after a complete 2xx response. callback(dest_path, error) runs later on
 * the Lua/UI thread, matching http_request's callback discipline. */
int l_plugin_download_file_async(lua_State * L) {
    const char * url = luaL_checkstring(L, 1);
    const char * dest_path = luaL_checkstring(L, 2);
    int callback_index = lua_isfunction(L, 3) ? 3 : 4;
    bool verify_tls = callback_index == 3 ? true : lua_toboolean(L, 3);
    luaL_checktype(L, callback_index, LUA_TFUNCTION);

    if (strlen(url) >= sizeof(plugin_async_http[0].url))
        return luaL_error(L, "plugin.download_file_async: URL is too long");
    if (dest_path[0] == '\0' || strlen(dest_path) >= sizeof(plugin_async_http[0].dest_path) - 12)
        return luaL_error(L, "plugin.download_file_async: destination path is invalid or too long");
    if (plugin_storage_path_is_reserved(dest_path))
        return luaL_error(L, "plugin.download_file_async: path is reserved for plugin.storage/plugin.secrets");
    if (plugin_path_in_program_dirs(dest_path))
        return luaL_error(L, "plugin.download_file_async: the program folders (.apps, .apps-data, .emulators) are reserved");

    int slot = find_free_slot();
    if (slot < 0) {
        return plugin_lua_push_error(L, "too many active HTTP requests");
    }

    lua_pushvalue(L, callback_index);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_async_http_t * req = &plugin_async_http[slot];
    uint16_t generation = (uint16_t) (req->generation + 1);
    if (generation == 0) generation = 1;
    memset(req, 0, sizeof(*req));
    atomic_init(&req->done, false);
    http_cancel_token_init(&req->cancel);
    req->generation = generation;
    req->L = owner;
    req->is_download = true;
    req->verify_tls = verify_tls;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->dest_path, sizeof(req->dest_path), "%s", dest_path);
    req->callback_ref = callback_ref;

    if (pthread_create(&req->thread, NULL, plugin_async_http_thread_func, req) != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, req->callback_ref);
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
        http_cancel_token_destroy(&req->cancel);
        return plugin_lua_push_error(L, "could not start HTTP worker");
    }
    req->active = true;

    lua_pushinteger(L, ((int) generation << 8) | (slot + 1));
    return 1;
}

/* API 16: poll a coherent byte snapshot without calling Lua from the
 * download worker. Unknown Content-Length is represented by total = 0. */
int l_plugin_get_download_progress(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    if (handle <= 0 || handle > 0xffffff)
        return plugin_lua_push_error(L, "invalid download handle");
    int slot = ((int) handle & 0xff) - 1;
    uint16_t generation = (uint16_t) ((unsigned int) handle >> 8);
    if (slot < 0 || slot >= PLUGIN_MAX_ASYNC_HTTP)
        return plugin_lua_push_error(L, "invalid download handle");
    plugin_async_http_t * req = &plugin_async_http[slot];
    if (!req->active || !req->is_download || req->generation != generation
        || req->L != plugin_lua_main_state(L)
        || http_cancel_token_is_cancelled(&req->cancel))
        return plugin_lua_push_error(L, "download is not active");
    pthread_mutex_lock(&plugin_download_progress_mutex);
    uint64_t downloaded = req->downloaded_bytes;
    uint64_t total = req->total_bytes;
    pthread_mutex_unlock(&plugin_download_progress_mutex);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer) downloaded); lua_setfield(L, -2, "downloaded");
    lua_pushinteger(L, (lua_Integer) total); lua_setfield(L, -2, "total");
    return 1;
}

int l_plugin_cancel(lua_State * L) {
    int handle = (int) luaL_checkinteger(L, 1);
    int slot = (handle & 0xFF) - 1;
    uint16_t generation = (uint16_t) ((unsigned int) handle >> 8);
    bool cancelled = false;
    if (slot >= 0 && slot < PLUGIN_MAX_ASYNC_HTTP) {
        plugin_async_http_t * req = &plugin_async_http[slot];
        if (req->active && req->generation == generation) {
            /* Real cancellation now, not just callback suppression --
             * http_cancel_token_cancel() shuts down the connection's fd,
             * forcing a blocked connect()/recv() to return immediately
             * (see http_client.h's own comment on http_cancel_token_t). */
            http_cancel_token_cancel(&req->cancel);
            cancelled = true;
        }
    }
    lua_pushboolean(L, cancelled);
    return 1;
}

/* A finished download has already replaced dest_path, even when its
 * callback is then skipped (cancelled, owner gone), so the cached image of
 * that path is refreshed on every join. */
static void join_request(plugin_async_http_t * req) {
    pthread_join(req->thread, NULL);
    if (req->is_download && req->ok) plugin_image_cache_forget(req->dest_path);
}

void plugin_http_bindings_clear_owner(lua_State * owner, bool aborted) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active || req->L != owner) continue;
        http_cancel_token_cancel(&req->cancel);
        join_request(req);
        if (!aborted) luaL_unref(owner, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->response_body);
        req->response_body = NULL;
        req->response_body_size = 0;
        req->active = false;
        atomic_store(&req->done, false);
        http_cancel_token_destroy(&req->cancel);
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
    }
}
void plugin_http_bindings_cancel_all(plugin_http_owner_aborted_fn owner_is_aborted) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active) continue;
        http_cancel_token_cancel(&req->cancel);
        join_request(req);
        if (!owner_is_aborted(req->L)) luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->response_body);
        req->response_body = NULL;
        req->response_body_size = 0;
        req->active = false;
        atomic_store(&req->done, false);
        http_cancel_token_destroy(&req->cancel);
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
    }
}

void plugin_http_bindings_poll(plugin_http_guarded_call_fn guarded_call,
                               plugin_http_owner_aborted_fn owner_is_aborted) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active || !atomic_load(&req->done)) continue;
        join_request(req);

        /* A hard-aborted owner is frozen: no callback, no registry access. */
        bool owner_aborted = owner_is_aborted(req->L);
        if (!http_cancel_token_is_cancelled(&req->cancel) && !owner_aborted) {
            lua_rawgeti(req->L, LUA_REGISTRYINDEX, req->callback_ref);
            int callback_args;
            if (req->is_download) {
                if (req->ok) {
                    lua_pushstring(req->L, req->dest_path);
                    lua_pushnil(req->L);
                } else {
                    lua_pushnil(req->L);
                    lua_pushstring(req->L, "download failed");
                }
                callback_args = 2;
            } else if (req->ok) {
                lua_pushinteger(req->L, req->status);
                lua_pushlstring(req->L, req->response_body ? (const char *) req->response_body : "",
                                req->response_body_size);
                lua_pushnil(req->L);
                /* New 4th argument: {Name = "value", ...} response
                 * headers. An existing 3-parameter callback is completely
                 * unaffected -- Lua silently drops extra arguments a
                 * function doesn't declare. A repeated header name keeps
                 * only the last occurrence (documented in PLUGINS.md). */
                lua_newtable(req->L);
                for (int h = 0; h < req->response_header_count; h++) {
                    lua_pushstring(req->L, req->response_headers[h].value);
                    lua_setfield(req->L, -2, req->response_headers[h].name);
                }
                callback_args = 4;
            } else {
                lua_pushnil(req->L);
                lua_pushnil(req->L);
                /* req->response_error is one of http_client.h's stable
                 * HTTP_ERR_* strings (e.g. "connect_timeout", "cancelled",
                 * "response_too_large") when http_request_ex() set it --
                 * more specific than the previous single generic message.
                 * Still falls back to that generic message for the rare
                 * case nothing set it (shouldn't happen, but a plugin
                 * should never see a NULL/empty error string here). */
                lua_pushstring(req->L, (req->response_error && req->response_error[0])
                                           ? req->response_error : "network error or response limit exceeded");
                callback_args = 3;
            }
            if (guarded_call(req->L, callback_args, 0, 0) != LUA_OK && !owner_is_aborted(req->L)) {
                const char * err = lua_tostring(req->L, -1);
                fprintf(stderr, "[plugins] HTTP callback error: %s\n", err ? err : "unknown error");
                lua_pop(req->L, 1);
            }
        }

        if (!owner_is_aborted(req->L)) luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
        free(req->response_body);
        req->response_body = NULL;
        req->response_body_size = 0;
        req->active = false;
        atomic_store(&req->done, false);
        http_cancel_token_destroy(&req->cancel);
        req->L = NULL;
        req->callback_ref = LUA_NOREF;
    }
}

bool plugin_http_bindings_has_work(void) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++)
        if (plugin_async_http[i].active) return true;
    return false;
}
