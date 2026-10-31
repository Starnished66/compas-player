#define _POSIX_C_SOURCE 200809L
#include "remote_plugins.h"
#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define BODY_TIMEOUT_MS 4000
#ifndef REMOTE_PLUGINS_WAIT_MS
#define REMOTE_PLUGINS_WAIT_MS 10000
#endif

typedef enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING, JOB_DONE } job_state_t;
static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond;
static pthread_once_t cond_once = PTHREAD_ONCE_INIT;
static bool cond_ready;
static job_state_t job_state;
static bool job_abandoned;
/* The dispatch caller owns its stack job until this request is consumed or
 * cancelled. The UI copies it while holding job_mutex before the caller can
 * return, so no payload-sized global queue is needed. */
static const remote_plugins_job_t * queued_job;
static atomic_bool job_pending;
static remote_plugins_result_t completed_job;

static void init_cond(void) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0) return;
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
        pthread_cond_init(&job_cond, &attr) == 0) cond_ready = true;
    pthread_condattr_destroy(&attr);
}

void remote_plugins_error(remote_plugins_result_t * result, int status, const char * code) {
    result->status = status;
    cJSON * json = cJSON_CreateObject();
    if (json && cJSON_AddStringToObject(json, "error", "plugin_api_error") &&
        cJSON_AddStringToObject(json, "code", code)) result->body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
}

bool remote_plugins_consume_job(remote_plugins_job_t * out) {
    if (!out) return false;
    if (!atomic_load_explicit(&job_pending, memory_order_acquire)) return false;
    pthread_mutex_lock(&job_mutex);
    bool found = job_state == JOB_QUEUED && queued_job != NULL;
    if (found) {
        *out = *queued_job;
        queued_job = NULL;
        job_state = JOB_RUNNING;
    }
    atomic_store_explicit(&job_pending, false, memory_order_release);
    pthread_mutex_unlock(&job_mutex);
    return found;
}

void remote_plugins_finish_job(remote_plugins_result_t * result) {
    if (!result) return;
    pthread_mutex_lock(&job_mutex);
    if (job_state == JOB_RUNNING && !job_abandoned) {
        completed_job = *result;
        result->body = NULL;
        job_state = JOB_DONE;
        pthread_cond_signal(&job_cond);
    } else {
        free(result->body);
        result->body = NULL;
        if (job_state == JOB_RUNNING) job_state = JOB_IDLE;
    }
    pthread_mutex_unlock(&job_mutex);
}

static void dispatch(const remote_plugins_job_t * job, remote_plugins_result_t * result) {
    pthread_once(&cond_once, init_cond);
    struct timespec deadline;
    if (!cond_ready || clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        remote_plugins_error(result, 503, "ui_unavailable");
        return;
    }
    deadline.tv_sec += REMOTE_PLUGINS_WAIT_MS / 1000;
    deadline.tv_nsec += (REMOTE_PLUGINS_WAIT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&job_mutex);
    if (job_state != JOB_IDLE) {
        pthread_mutex_unlock(&job_mutex);
        remote_plugins_error(result, 409, "busy");
        return;
    }
    queued_job = job;
    job_abandoned = false;
    job_state = JOB_QUEUED;
    atomic_store_explicit(&job_pending, true, memory_order_release);
    while (job_state != JOB_DONE) {
        int rc = pthread_cond_timedwait(&job_cond, &job_mutex, &deadline);
        if (rc != 0 && job_state != JOB_DONE) {
            bool running = job_state == JOB_RUNNING;
            if (running) {
                job_abandoned = true;
            } else {
                job_state = JOB_IDLE; /* cancelled before the UI could execute it */
                queued_job = NULL;
                atomic_store_explicit(&job_pending, false, memory_order_release);
            }
            pthread_mutex_unlock(&job_mutex);
            remote_plugins_error(result, rc == ETIMEDOUT ? 504 : 503,
                                 running ? "operation_in_progress" : "ui_timeout");
            return;
        }
    }
    *result = completed_job;
    completed_job = (remote_plugins_result_t){0};
    job_state = JOB_IDLE;
    pthread_mutex_unlock(&job_mutex);
}

static const char * status_line(int status) {
    switch (status) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 400: return "400 Bad Request";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";
    case 415: return "415 Unsupported Media Type";
    case 503: return "503 Service Unavailable";
    case 504: return "504 Gateway Timeout";
    default: return "500 Internal Server Error";
    }
}

static bool family_path(const char * path, const char * prefix) {
    size_t n = strlen(prefix);
    return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

typedef struct { const char * path; const char * method; remote_plugins_operation_t op; unsigned fields; } route_t;
enum { FIELD_ID = 1, FIELD_ENABLED = 2, FIELD_FORCE = 4 };
static const route_t routes[] = {
    {"/api/plugin-store", "GET", REMOTE_PLUGINS_STORE_STATE, 0},
    {"/api/plugin-store/refresh", "POST", REMOTE_PLUGINS_STORE_REFRESH, 0},
    {"/api/plugin-store/install", "POST", REMOTE_PLUGINS_STORE_INSTALL, FIELD_ID | FIELD_FORCE},
    {"/api/plugin-store/update", "POST", REMOTE_PLUGINS_STORE_UPDATE, FIELD_ID | FIELD_FORCE},
    {"/api/plugin-store/update-all", "POST", REMOTE_PLUGINS_STORE_UPDATE_ALL, 0},
    {"/api/plugin-store/uninstall", "POST", REMOTE_PLUGINS_STORE_UNINSTALL, FIELD_ID},
    {"/api/plugins", "GET", REMOTE_PLUGINS_LIST, 0},
    {"/api/plugins/set-enabled", "POST", REMOTE_PLUGINS_SET_ENABLED, FIELD_ID | FIELD_ENABLED},
    {"/api/plugins/refresh", "POST", REMOTE_PLUGINS_REFRESH, 0},
    {"/api/player-layouts", "GET", REMOTE_PLUGINS_LAYOUTS, 0},
    {"/api/player-layouts/select", "POST", REMOTE_PLUGINS_SELECT_LAYOUT, FIELD_ID},
    {"/api/plugin-store/preview", "GET", REMOTE_PLUGINS_STORE_PREVIEW, 0},
    {"/api/player-layouts/preview", "GET", REMOTE_PLUGINS_LAYOUT_PREVIEW, 0},
    {"/api/plugins/open-settings", "POST", REMOTE_PLUGINS_OPEN_SETTINGS, FIELD_ID},
};

static bool pagination(const char * path, remote_plugins_job_t * job) {
    job->offset = 0; job->limit = 50;
    const char * q = strchr(path, '?');
    unsigned seen = 0;
    while (q && *++q) {
        const char * end = strchr(q, '&');
        if (!end) end = q + strlen(q);
        const char * eq = memchr(q, '=', (size_t)(end - q));
        if (!eq || eq + 1 == end) return false;
        unsigned bit;
        int * value;
        if (eq - q == 6 && strncmp(q, "offset", 6) == 0) { bit = 1; value = &job->offset; }
        else if (eq - q == 5 && strncmp(q, "limit", 5) == 0) { bit = 2; value = &job->limit; }
        else return false;
        if (seen & bit) return false;
        seen |= bit;
        unsigned parsed = 0;
        for (const char * p = eq + 1; p < end; p++) {
            if (*p < '0' || *p > '9' || parsed > ((unsigned)INT_MAX - (unsigned)(*p - '0')) / 10u) return false;
            parsed = parsed * 10u + (unsigned)(*p - '0');
        }
        *value = (int)parsed;
        if (*end == '\0') break;
        q = end;
        if (!q[1]) return false;
    }
    return job->limit > 0 && job->limit <= REMOTE_PLUGINS_PAGE_MAX;
}

static char * read_body(int fd, size_t length) {
    char * text = malloc(length + 1);
    if (!text) return NULL;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) { free(text); return NULL; }
    size_t got = 0;
    while (got < length) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) break;
        int64_t elapsed = (int64_t)(now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed >= BODY_TIMEOUT_MS) break;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, (int)(BODY_TIMEOUT_MS - elapsed));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        ssize_t count = read(fd, text + got, length - got);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        got += (size_t)count;
    }
    if (got != length) { free(text); return NULL; }
    text[length] = '\0';
    return text;
}

static bool bounded_json(const char * text, size_t length) {
    if (memchr(text, '\0', length) || strstr(text, "\\u0000")) return false;
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < length; i++) {
        char ch = text[i];
        if (quoted) {
            if (ch == '\\' && i + 1 < length) i++;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if ((ch == '{' || ch == '[') && ++depth > 4) return false;
        else if (ch == '}' || ch == ']') depth--;
    }
    return true;
}

static bool id_valid(const remote_plugins_job_t * job) {
    const unsigned char * p = (const unsigned char *)job->id;
    size_t n = strlen(job->id);
    if (!n) return false;
    if (job->operation == REMOTE_PLUGINS_SET_ENABLED) {
        if (n < 5 || strcasecmp(job->id + n - 4, ".lua") != 0) return false;
        for (; *p; p++) if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
        return true;
    }
    bool store = job->operation == REMOTE_PLUGINS_STORE_INSTALL || job->operation == REMOTE_PLUGINS_STORE_UPDATE ||
                 job->operation == REMOTE_PLUGINS_STORE_UNINSTALL || job->operation == REMOTE_PLUGINS_STORE_PREVIEW;
    if (job->operation == REMOTE_PLUGINS_OPEN_SETTINGS) {
        if (n < 5) return false;
        if (strcasecmp(job->id + n - 4, ".lua") != 0) return false;
        for (; *p; p++) if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
        return true;
    }
    if (n > 63 || (store && !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))) return false;
    for (; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-') continue;
        if (!store && *p >= 'A' && *p <= 'Z') continue;
        return false;
    }
    return true;
}

static bool query_id(const char * path, remote_plugins_job_t * job) {
    const char * query = strchr(path, '?');
    if (!query || strncmp(query + 1, "id=", 3) != 0) return false;
    const char * value = query + 4;
    size_t length = strlen(value);
    if (!length || length >= sizeof(job->id) || length > 63) return false;
    memcpy(job->id, value, length + 1);
    return id_valid(job);
}

static bool parse_json_body(const char * text, size_t length, const route_t * route, remote_plugins_job_t * job) {
    if (!bounded_json(text, length)) return false;
    const char * end;
    cJSON * json = cJSON_ParseWithLengthOpts(text, length + 1, &end, 1);
    if (!json || !cJSON_IsObject(json)) { cJSON_Delete(json); return false; }
    unsigned seen = 0;
    bool valid = true;
    const cJSON * item;
    cJSON_ArrayForEach(item, json) {
        unsigned bit = 0;
        if (!item->string) { valid = false; break; }
        if (strcmp(item->string, "id") == 0) bit = FIELD_ID;
        else if (strcmp(item->string, "enabled") == 0) bit = FIELD_ENABLED;
        else if (strcmp(item->string, "force") == 0) bit = FIELD_FORCE;
        if (!bit || !(route->fields & bit) || (seen & bit)) { valid = false; break; }
        seen |= bit;
        if (bit == FIELD_ID) {
            if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizeof(job->id)) { valid = false; break; }
            snprintf(job->id, sizeof(job->id), "%s", item->valuestring);
        } else {
            if (!cJSON_IsBool(item)) { valid = false; break; }
            if (bit == FIELD_ENABLED) job->enabled = cJSON_IsTrue(item);
            else job->force = cJSON_IsTrue(item);
        }
    }
    unsigned required = route->fields & ~FIELD_FORCE;
    valid = valid && (seen & required) == required && (!(seen & FIELD_ID) || id_valid(job));
    cJSON_Delete(json);
    return valid;
}

bool remote_plugins_handle(int fd, const char * method, const char * path_only, const char * path,
                            const remote_plugins_http_request_t * request,
                            char * status, size_t status_size, char ** body) {
    if (!family_path(path_only, "/api/plugin-store") && !family_path(path_only, "/api/plugins") &&
        !family_path(path_only, "/api/player-layouts")) return false;
    remote_plugins_result_t result = {0};
    bool body_read = false;
    remote_plugins_job_t job = {.offset = 0, .limit = 50};
    const route_t * route = NULL;
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        if (strcmp(path_only, routes[i].path) == 0) { route = &routes[i]; break; }
    if (!route) remote_plugins_error(&result, 404, "not_found");
    else if (strcmp(method, route->method) != 0) remote_plugins_error(&result, 405, "method_not_allowed");
    else {
        job.operation = route->op;
        if (strcmp(method, "GET") == 0) {
            bool valid_query = route->op == REMOTE_PLUGINS_STORE_PREVIEW || route->op == REMOTE_PLUGINS_LAYOUT_PREVIEW
                ? query_id(path, &job)
                : pagination(path, &job);
            if (!valid_query || request->content_length > 0) remote_plugins_error(&result, 400, "invalid_query");
            else dispatch(&job, &result);
        } else if (!request->header_auth) remote_plugins_error(&result, 403, "pin_header_required");
        else if (!request->json_content_type) remote_plugins_error(&result, 415, "json_required");
        else if (request->content_length > REMOTE_PLUGINS_BODY_MAX) remote_plugins_error(&result, 413, "body_too_large");
        else if (request->content_length <= 0) remote_plugins_error(&result, 400, "body_required");
        else if (strchr(path, '?')) remote_plugins_error(&result, 400, "invalid_query");
        else {
            body_read = true;
            char * text = read_body(fd, (size_t)request->content_length);
            if (!text) remote_plugins_error(&result, 400, "body_unreadable");
            else {
                bool valid = parse_json_body(text, (size_t)request->content_length, route, &job);
                free(text);
                if (!valid) remote_plugins_error(&result, 400, "invalid_body");
                else dispatch(&job, &result);
            }
        }
    }
    /* A TCP close with unread body bytes can reset the connection and hide
     * an early validation error. Drain only bounded payloads, using the same
     * total deadline as normal body reads. */
    if (!body_read && request->content_length > 0 && request->content_length <= REMOTE_PLUGINS_BODY_MAX) {
        char * discarded = read_body(fd, (size_t)request->content_length);
        free(discarded);
    }
    if (!result.body) {
        result.status = 500;
        result.body = strdup("{\"error\":\"plugin_api_error\",\"code\":\"out_of_memory\"}");
    }
    snprintf(status, status_size, "%s", status_line(result.status));
    *body = result.body;
    return true;
}
