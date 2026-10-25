#include "plugin_http_bindings.h"

#include "http_client.h"
#include "plugin_lua_runtime.h"
#include "plugin_storage.h"

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

static void * plugin_async_http_thread_func(void * arg) {
    plugin_async_http_t * req = (plugin_async_http_t *) arg;
    if (req->is_download) {
        char temp_path[PATH_MAX];
        /* Defense in depth: l_plugin_download_file_async() already
         * rejected a reserved dest_path before ever starting this thread,
         * but re-check here too, immediately before touching the
         * filesystem, rather than trusting that the only path into this
         * branch is the one already-guarded call site -- the same
         * discipline this codebase already applies to bounds/validity
         * checks that matter (re-verify at the point of use, not just at
         * the call site three frames up). Must NOT return early here --
         * req->done still needs to be set below, via the function's
         * normal shared tail, or plugin_manager_poll() would never reap
         * this slot and it would leak permanently as "active". */
        int n;
        if (plugin_storage_path_is_reserved(req->dest_path)) {
            n = -1; /* forces the "invalid" branch below without touching the filesystem */
        } else {
            n = snprintf(temp_path, sizeof(temp_path), "%s.part.XXXXXX", req->dest_path);
        }
        if (n <= 0 || (size_t) n >= sizeof(temp_path)) {
            req->ok = false;
        } else {
            int fd = mkstemp(temp_path);
            if (fd < 0) {
                req->ok = false;
            } else {
                close(fd);
                /* Review finding: http_get_to_file() had no size bound at
                 * all for its file-sink write path -- http_get_to_file_
                 * bounded() (0 = a generous 2 GiB built-in default) closes
                 * that gap for plugin-driven downloads specifically,
                 * without touching http_get_to_file() itself (DLNA/cover-
                 * art downloads keep their original unlimited behavior). */
                req->ok = http_get_to_file_bounded(req->url, req->verify_tls, temp_path, 0,
                                                    plugin_async_download_progress, req);
                bool cancelled = http_cancel_token_is_cancelled(&req->cancel);
                if (req->ok && !cancelled) {
                    req->ok = rename(temp_path, req->dest_path) == 0;
                }
                if (!req->ok || cancelled) remove(temp_path);
            }
        }
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

int l_plugin_http_request(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        if (!plugin_async_http[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return plugin_lua_push_error(L, "too many active HTTP requests");
    }

    lua_getfield(L, 1, "url");
    const char * url = luaL_checkstring(L, -1);
    if (strlen(url) >= sizeof(plugin_async_http[slot].url)) {
        return luaL_error(L, "plugin.http_request: URL is too long");
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "method");
    const char * method_str = luaL_optstring(L, -1, "GET");
    http_method_t method;
    if (strcasecmp(method_str, "GET") == 0) method = HTTP_METHOD_GET;
    else if (strcasecmp(method_str, "POST") == 0) method = HTTP_METHOD_POST;
    else if (strcasecmp(method_str, "PUT") == 0) method = HTTP_METHOD_PUT;
    else if (strcasecmp(method_str, "PATCH") == 0) method = HTTP_METHOD_PATCH;
    else if (strcasecmp(method_str, "DELETE") == 0) method = HTTP_METHOD_DELETE;
    else if (strcasecmp(method_str, "HEAD") == 0) method = HTTP_METHOD_HEAD;
    else return luaL_error(L, "plugin.http_request: unknown method '%s'", method_str);
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");
    size_t body_size = 0;
    const char * body = lua_isnil(L, -1) ? NULL : luaL_checklstring(L, -1, &body_size);
    if (body_size > PLUGIN_ASYNC_HTTP_MAX_REQUEST) {
        return luaL_error(L, "plugin.http_request: request body exceeds %u bytes", PLUGIN_ASYNC_HTTP_MAX_REQUEST);
    }
    uint8_t * body_copy = NULL;
    if (body_size > 0) {
        body_copy = malloc(body_size);
        if (!body_copy) return luaL_error(L, "plugin.http_request: out of memory");
        memcpy(body_copy, body, body_size);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "content_type");
    const char * content_type = luaL_optstring(L, -1, "application/x-www-form-urlencoded");
    if (strlen(content_type) >= sizeof(plugin_async_http[slot].content_type)) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: content_type is too long");
    }
    lua_pop(L, 1);

    /* Arbitrary request headers -- {Name = "value", ...}, bounded to
     * HTTP_MAX_HEADERS entries and HTTP_HEADER_NAME_MAX/VALUE_MAX per
     * entry (silently truncated past that, same as content_type/url
     * above being length-checked rather than ever overflowing). */
    http_header_t headers[HTTP_MAX_HEADERS];
    int header_count = 0;
    /* One value may exceed a slot (a long bearer token); see
     * HTTP_LONG_HEADER_VALUE_MAX in http_client.h. The Lua string stays
     * reachable through options.headers for this whole call, so it is only
     * copied to the heap after the last call below that can raise. */
    char long_header_name[HTTP_HEADER_NAME_MAX] = "";
    const char * long_value_src = NULL;
    size_t long_value_len = 0;
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        /* Every entry is visited: more than HTTP_MAX_HEADERS short headers
         * (or a second long one) is an error, never a silently dropped
         * header whose fate depends on table traversal order. */
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                /* Review finding: snprintf() alone silently truncated an
                 * oversized name/value, and validation afterward (the
                 * CRLF/invalid-character check further down this
                 * function) then validated the TRUNCATED copy, not what
                 * the plugin actually passed -- a header could silently
                 * become a different, shorter one on the wire instead of
                 * failing loudly. Check the real length (via
                 * lua_tolstring(), which also catches an embedded NUL a
                 * plain lua_tostring()+strlen() would miss) and reject
                 * with a clear Lua error, exactly like an oversized
                 * url/content_type already does elsewhere in this
                 * function, instead of truncating. */
                size_t name_len = 0, value_len = 0;
                const char * name = lua_tolstring(L, -2, &name_len);
                const char * value = lua_tolstring(L, -1, &value_len); /* non-string values (numbers) still convert fine via lua_tolstring */
                if (name && value) {
                    bool long_value = value_len >= sizeof(headers[0].value);
                    if (strlen(name) != name_len || strlen(value) != value_len) {
                        free(body_copy);
                        return luaL_error(L, "plugin.http_request: header '%s' contains a NUL byte", name);
                    }
                    if (name_len >= sizeof(headers[0].name) ||
                        (long_value && (long_value_src || value_len >= HTTP_LONG_HEADER_VALUE_MAX ||
                                        lua_type(L, -1) != LUA_TSTRING))) {
                        free(body_copy);
                        return luaL_error(L, "plugin.http_request: header '%s' name/value exceeds %d/%d bytes "
                                          "(one value may be up to %d)",
                                          name, (int) sizeof(headers[0].name) - 1, (int) sizeof(headers[0].value) - 1,
                                          HTTP_LONG_HEADER_VALUE_MAX - 1);
                    }
                    if (!long_value && header_count >= HTTP_MAX_HEADERS) {
                        free(body_copy);
                        return luaL_error(L, "plugin.http_request: at most %d headers", HTTP_MAX_HEADERS);
                    }
                    if (long_value) {
                        long_value_src = value;
                        long_value_len = value_len;
                        memcpy(long_header_name, name, name_len + 1);
                    } else {
                        memcpy(headers[header_count].name, name, name_len + 1);
                        memcpy(headers[header_count].value, value, value_len + 1);
                        header_count++;
                    }
                }
            }
            lua_pop(L, 1); /* pop value, keep key for lua_next */
        }
    }
    /* The long value itself is pushed and stays on the stack until the call
     * returns: a later options metamethod could remove it from the headers
     * table and collect it, so the table alone does not keep it alive. */
    if (long_value_src) {
        lua_pushstring(L, long_header_name);
        lua_rawget(L, -2);
        size_t len = 0;
        long_value_src = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : NULL;
        if (!long_value_src || len != long_value_len) {
            free(body_copy);
            return luaL_error(L, "plugin.http_request: header '%s' changed while reading", long_header_name);
        }
    }

    lua_getfield(L, 1, "verify_tls");
    bool verify_tls = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "max_response_bytes");
    lua_Integer requested_max = luaL_optinteger(L, -1, PLUGIN_ASYNC_HTTP_DEFAULT_MAX);
    lua_pop(L, 1);
    if (requested_max < 1 || requested_max > PLUGIN_ASYNC_HTTP_MAX_RESPONSE) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: max_response_bytes must be 1..%u",
                          PLUGIN_ASYNC_HTTP_MAX_RESPONSE);
    }

    lua_getfield(L, 1, "connect_timeout_ms");
    lua_Integer connect_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "read_timeout_ms");
    lua_Integer read_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "total_timeout_ms");
    lua_Integer total_timeout_ms = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    lua_getfield(L, 1, "redirect_limit");
    lua_Integer redirect_limit = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    if (connect_timeout_ms < 0 || read_timeout_ms < 0 || total_timeout_ms < 0 || redirect_limit < 0) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: timeout/redirect_limit fields must not be negative");
    }
    /* Review finding: values were only checked for being non-negative,
     * then narrowed to uint32_t/int with no upper bound at all. A
     * timeout above UINT32_MAX would wrap; more seriously, a value
     * between INT_MAX+1 and UINT32_MAX becomes NEGATIVE once narrowed to
     * the plain int poll() itself takes, which poll() treats as "wait
     * indefinitely" -- the opposite of what a timeout is for. An
     * enormous redirect_limit could also drive the redirect-following
     * recursion deep enough to exhaust the C stack. Reject outright
     * rather than silently clamp/truncate, so a plugin passing a garbage
     * value finds out immediately, not via mysterious hangs later. These
     * limits match http_client.c's own separate, defensive caps
     * (HTTP_REQUEST_MAX_TIMEOUT_MS/HTTP_REQUEST_MAX_REDIRECTS) -- kept as
     * two independent checks deliberately (belt and suspenders), not
     * because either alone would be insufficient. */
#define PLUGIN_HTTP_MAX_TIMEOUT_MS (5 * 60 * 1000) /* 5 minutes */
#define PLUGIN_HTTP_MAX_REDIRECTS 10
    if (connect_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS || read_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS ||
        total_timeout_ms > PLUGIN_HTTP_MAX_TIMEOUT_MS) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: connect_timeout_ms/read_timeout_ms/total_timeout_ms must not exceed %d",
                          PLUGIN_HTTP_MAX_TIMEOUT_MS);
    }
    if (redirect_limit > PLUGIN_HTTP_MAX_REDIRECTS) {
        free(body_copy);
        return luaL_error(L, "plugin.http_request: redirect_limit must not exceed %d", PLUGIN_HTTP_MAX_REDIRECTS);
    }

    /* Registered before anything native is allocated or the slot is
     * claimed: luaL_ref() can raise. */
    lua_pushvalue(L, 2);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    char * long_header_value = NULL;
    if (long_value_src) {
        long_header_value = malloc(long_value_len + 1);
        if (!long_header_value) {
            luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
            free(body_copy);
            return plugin_lua_push_error(L, "out of memory");
        }
        memcpy(long_header_value, long_value_src, long_value_len + 1);
    }

    lua_State * owner = plugin_lua_main_state(L);
    plugin_async_http_t * req = &plugin_async_http[slot];
    uint16_t generation = (uint16_t) (req->generation + 1);
    if (generation == 0) generation = 1;
    memset(req, 0, sizeof(*req));
    atomic_init(&req->done, false);
    http_cancel_token_init(&req->cancel);
    req->generation = generation;
    req->L = owner;
    req->method = method;
    memcpy(req->headers, headers, sizeof(req->headers));
    req->header_count = header_count;
    memcpy(req->long_header_name, long_header_name, sizeof(req->long_header_name));
    req->long_header_value = long_header_value;
    req->verify_tls = verify_tls;
    req->request_body = body_copy;
    req->request_body_size = body_size;
    req->max_response_size = (size_t) requested_max;
    req->connect_timeout_ms = (uint32_t) connect_timeout_ms;
    req->read_timeout_ms = (uint32_t) read_timeout_ms;
    req->total_timeout_ms = (uint32_t) total_timeout_ms;
    req->redirect_limit = (int) redirect_limit;
    snprintf(req->url, sizeof(req->url), "%s", url);
    snprintf(req->content_type, sizeof(req->content_type), "%s", content_type);
    req->callback_ref = callback_ref;

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
    int handle = ((int) generation << 8) | (slot + 1);
    lua_pushinteger(L, handle);
    return 1;
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

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        if (!plugin_async_http[i].active) { slot = i; break; }
    }
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


void plugin_http_bindings_clear_owner(lua_State * owner, bool aborted) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active || req->L != owner) continue;
        http_cancel_token_cancel(&req->cancel);
        pthread_join(req->thread, NULL);
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
void plugin_http_bindings_cancel_all(void) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active) continue;
        http_cancel_token_cancel(&req->cancel);
        pthread_join(req->thread, NULL);
        luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
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

void plugin_http_bindings_poll(plugin_http_guarded_call_fn guarded_call) {
    for (int i = 0; i < PLUGIN_MAX_ASYNC_HTTP; i++) {
        plugin_async_http_t * req = &plugin_async_http[i];
        if (!req->active || !atomic_load(&req->done)) continue;
        pthread_join(req->thread, NULL);

        if (!http_cancel_token_is_cancelled(&req->cancel)) {
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
            if (guarded_call(req->L, callback_args, 0, 0) != LUA_OK) {
                const char * err = lua_tostring(req->L, -1);
                fprintf(stderr, "[plugins] HTTP callback error: %s\n", err ? err : "unknown error");
                lua_pop(req->L, 1);
            }
        }

        luaL_unref(req->L, LUA_REGISTRYINDEX, req->callback_ref);
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
