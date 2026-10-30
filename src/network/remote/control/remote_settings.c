#include "remote_settings.h"
#include "../../../core/persistence/settings_registry.h"
#include "cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_WAIT_MS 1500
#define APPLY_WAIT_MS 10000
#define ERRORS_MAX 16
#define BODY_DEADLINE_MS 4000
#define BODY_NESTING_MAX 4 /* {"changes":{...},"acknowledge":[...]} needs 2 */

static bool (*rescan_busy_probe)(void);

typedef enum { JOB_IDLE, JOB_QUEUED, JOB_RUNNING, JOB_DONE } job_state_t;

static pthread_mutex_t job_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond;
static pthread_once_t job_cond_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t post_mutex = PTHREAD_MUTEX_INITIALIZER; /* one change request at a time, check to apply */
static job_state_t job_state = JOB_IDLE;
static bool job_abandoned;
static remote_settings_job_t job_data;
static uint32_t job_revision;
static bool rescan_pending;

typedef struct {
    char key[64];
    setv_result_t code;
    const setting_desc_t * desc;
} field_error_t;

/* Value of ?key=digits or &key=digits. */
static uint32_t query_uint(const char * path, const char * key, bool * present) {
    char needle[24];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char * q = strchr(path, '?');
    *present = false;
    while (q) {
        q++;
        if (strncmp(q, needle, strlen(needle)) == 0 && q[strlen(needle)] >= '0' && q[strlen(needle)] <= '9') {
            *present = true;
            return (uint32_t) strtoul(q + strlen(needle), NULL, 10);
        }
        q = strchr(q, '&');
    }
    return 0;
}

bool remote_settings_content_type_is_json(const char * value) {
    static const char json_type[] = "application/json";
    while (*value == ' ' || *value == '\t') value++;
    if (strncasecmp(value, json_type, sizeof(json_type) - 1) != 0) return false;
    value += sizeof(json_type) - 1;
    while (*value == ' ' || *value == '\t') value++;
    return *value == '\0' || *value == ';';
}

static void reply(char * status, size_t status_size, char ** body, const char * status_line, const char * json) {
    snprintf(status, status_size, "%s", status_line);
    *body = strdup(json ? json : "{}");
}

static void reply_error(char * status, size_t status_size, char ** body, const char * status_line,
                        const char * code) {
    char json[128];
    snprintf(json, sizeof(json), "{\"error\":\"settings_error\",\"code\":\"%s\"}", code);
    reply(status, status_size, body, status_line, json);
}

/* Who is asking. Loopback is the adb forward from Compas Tools; the USB cable
 * is already a developer trust boundary. Wi-Fi and Bluetooth peers never are. */
static void peer_info(int fd, bool * loopback, const char ** via) {
    struct sockaddr_storage addr;
    socklen_t length = sizeof(addr);
    *loopback = false;
    *via = "bluetooth";
    if (getpeername(fd, (struct sockaddr *) &addr, &length) != 0) return;
    if (addr.ss_family == AF_INET) {
        const struct sockaddr_in * in = (const struct sockaddr_in *) &addr;
        *loopback = (ntohl(in->sin_addr.s_addr) >> 24) == 127;
        *via = *loopback ? "usb" : "wifi";
    } else if (addr.ss_family == AF_INET6) {
        *via = "wifi";
    }
}

/* The whole body must arrive within BODY_DEADLINE_MS, not just each chunk. */
static char * read_body(int fd, size_t length) {
    char * body = malloc(length + 1);
    size_t got = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (body && got < length) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= BODY_DEADLINE_MS) { free(body); return NULL; }
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, (int) (BODY_DEADLINE_MS - elapsed));
        if (ready < 0 && errno == EINTR) continue;
        ssize_t n = ready > 0 ? read(fd, body + got, length - got) : -1;
        if (n <= 0) { free(body); return NULL; }
        got += (size_t) n;
    }
    if (body) body[length] = '\0';
    return body;
}

static bool whole_int(const cJSON * item, int * out) {
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || fabs(item->valuedouble) > 1e9) return false;
    *out = (int) item->valuedouble;
    return (double) *out == item->valuedouble;
}

/* Brackets outside strings may nest only BODY_NESTING_MAX deep: cJSON recurses
 * per level and the RFCOMM worker's stack is small. */
static bool nesting_ok(const char * text) {
    int depth = 0;
    bool in_string = false;
    for (const char * p = text; *p; p++) {
        if (in_string) {
            if (*p == '\\' && p[1]) p++;
            else if (*p == '"') in_string = false;
        } else if (*p == '"') in_string = true;
        else if ((*p == '{' || *p == '[') && ++depth > BODY_NESTING_MAX) return false;
        else if ((*p == '}' || *p == ']') && depth > 0) depth--;
    }
    return true;
}

static bool acknowledged(const cJSON * list, const char * key) {
    const cJSON * entry;
    cJSON_ArrayForEach(entry, list)
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, key) == 0) return true;
    return false;
}

static const char * error_status(setv_result_t code) {
    switch (code) {
        case SETV_UNKNOWN_KEY: case SETV_INVALID_TYPE: return "400 Bad Request";
        case SETV_READ_ONLY: return "403 Forbidden";
        case SETV_UNAVAILABLE: return "409 Conflict";
        default: return "422 Unprocessable Entity";
    }
}

static void reply_field_errors(char * status, size_t status_size, char ** body, const field_error_t * errors,
                               int count) {
    char json[ERRORS_MAX * 256 + 128];
    size_t n = 0;
#define APPEND(...) do { int w = snprintf(json + n, sizeof(json) - n, __VA_ARGS__); \
                         n = (w < 0 || (size_t) w >= sizeof(json) - n) ? sizeof(json) - 1 : n + (size_t) w; } while (0)
    APPEND("{\"error\":\"settings_error\",\"code\":\"validation_failed\",\"errors\":[");
    for (int i = 0; i < count; i++) {
        char key[160];
        remote_settings_json_escape(key, sizeof(key), errors[i].key);
        APPEND("%s{\"key\":\"%s\",\"code\":\"%s\"", i ? "," : "", key, settings_registry_error_code(errors[i].code));
        const setting_desc_t * d = errors[i].desc;
        if (d && errors[i].code == SETV_OUT_OF_RANGE) APPEND(",\"min\":%d,\"max\":%d", d->min, d->max);
        if (d && errors[i].code == SETV_CONFIRMATION_REQUIRED)
            APPEND(",\"confirmAbove\":%d", settings_registry_loud_threshold());
        APPEND("}");
    }
    APPEND("]}");
#undef APPEND
    reply(status, status_size, body, error_status(count ? errors[0].code : SETV_INVALID_TYPE), json);
}

/* Hands the job to the UI thread and waits for it. */
static void job_cond_init(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC); /* wall-clock jumps must not cut the waits short */
    pthread_cond_init(&job_cond, &attr);
    pthread_condattr_destroy(&attr);
}

static bool submit_job(const remote_settings_job_t * job, uint32_t * revision, bool * busy) {
    struct timespec deadline;
    *busy = false;
    pthread_once(&job_cond_once, job_cond_init);
    pthread_mutex_lock(&job_mutex);
    if (job_state != JOB_IDLE) {
        pthread_mutex_unlock(&job_mutex);
        *busy = true;
        return false;
    }
    job_data = *job;
    job_state = JOB_QUEUED;
    job_abandoned = false;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += QUEUE_WAIT_MS / 1000;
    deadline.tv_nsec += (QUEUE_WAIT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    while (job_state == JOB_QUEUED)
        if (pthread_cond_timedwait(&job_cond, &job_mutex, &deadline) == ETIMEDOUT) break;
    if (job_state == JOB_QUEUED) { /* the UI thread never picked it up: withdraw it */
        job_state = JOB_IDLE;
        pthread_mutex_unlock(&job_mutex);
        return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += APPLY_WAIT_MS / 1000;
    while (job_state == JOB_RUNNING)
        if (pthread_cond_timedwait(&job_cond, &job_mutex, &deadline) == ETIMEDOUT) break;
    bool done = job_state == JOB_DONE;
    if (done) { *revision = job_revision; job_state = JOB_IDLE; }
    else job_abandoned = true;
    pthread_mutex_unlock(&job_mutex);
    return done;
}

bool remote_settings_consume_job(remote_settings_job_t * job) {
    pthread_mutex_lock(&job_mutex);
    bool queued = job_state == JOB_QUEUED;
    if (queued) {
        *job = job_data;
        job_state = JOB_RUNNING;
        pthread_cond_broadcast(&job_cond);
    }
    pthread_mutex_unlock(&job_mutex);
    return queued;
}

void remote_settings_finish_job(uint32_t revision) {
    pthread_mutex_lock(&job_mutex);
    job_revision = revision;
    job_state = job_abandoned ? JOB_IDLE : JOB_DONE;
    pthread_cond_broadcast(&job_cond);
    pthread_mutex_unlock(&job_mutex);
}

void remote_settings_set_rescan_probe(bool (*busy)(void)) { rescan_busy_probe = busy; }

bool remote_settings_consume_rescan(void) {
    pthread_mutex_lock(&job_mutex);
    bool pending = rescan_pending;
    rescan_pending = false;
    pthread_mutex_unlock(&job_mutex);
    return pending;
}

static void reply_applied(char * status, size_t status_size, char ** body, const remote_settings_job_t * job,
                          int requested, uint32_t revision) {
    /* On the heap: the RFCOMM worker has only musl's default 128 KiB stack. */
    const size_t part = (size_t) REMOTE_SETTINGS_MAX_CHANGES * 72;
    char * applied = calloc(1, part), * pending = calloc(1, part), * also = calloc(1, part);
    char * json = malloc(3 * part + 128);
    if (!applied || !pending || !also || !json) {
        free(applied); free(pending); free(also); free(json);
        return reply_error(status, status_size, body, "500 Internal Server Error", "out_of_memory");
    }
    size_t a = 0, p = 0, o = 0;
    for (int i = 0; i < job->count; i++) {
        const setting_desc_t * d = job->changes[i].desc;
        if (i >= requested)
            o += (size_t) snprintf(also + o, part - o, "%s\"%s\"", o ? "," : "", d->key);
        if (d->apply == SET_APPLY_LIVE)
            a += (size_t) snprintf(applied + a, part - a, "%s\"%s\"", a ? "," : "", d->key);
        else
            p += (size_t) snprintf(pending + p, part - p, "%s{\"key\":\"%s\",\"reason\":\"%s\"}",
                                   p ? "," : "", d->key, remote_settings_apply_name(d->apply));
    }
    snprintf(json, 3 * part + 128, "{\"epoch\":%u,\"revision\":%u,\"applied\":[%s],\"pending\":[%s],\"alsoChanged\":[%s]}",
             settings_registry_epoch(), revision, applied, pending, also);
    reply(status, status_size, body, "200 OK", json);
    free(applied); free(pending); free(also); free(json);
}

/* Takes ownership of text. */
static void handle_post_settings_locked(char * text, const char * via, char * status, size_t status_size,
                                        char ** body) {
    cJSON * root = cJSON_Parse(text);
    free(text);
    const cJSON * changes = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "changes") : NULL;
    if (!cJSON_IsObject(changes) || !changes->child) {
        cJSON_Delete(root);
        return reply_error(status, status_size, body, "400 Bad Request", "invalid_body");
    }
    const cJSON * acks = cJSON_GetObjectItemCaseSensitive(root, "acknowledge");
    const cJSON * base = cJSON_GetObjectItemCaseSensitive(root, "baseRevision");
    int base_revision = -1;
    const cJSON * epoch = cJSON_GetObjectItemCaseSensitive(root, "epoch");
    int epoch_value = 0;
    bool epoch_given = epoch != NULL;
    if ((base && (!whole_int(base, &base_revision) || base_revision < 0)) ||
        (epoch_given && (!whole_int(epoch, &epoch_value) || epoch_value < 0))) {
        cJSON_Delete(root);
        return reply_error(status, status_size, body, "400 Bad Request", "invalid_base_revision");
    }

    remote_settings_job_t * job = calloc(1, sizeof(*job));
    field_error_t errors[ERRORS_MAX];
    int error_count = 0;
    if (!job) { cJSON_Delete(root); return reply_error(status, status_size, body, "500 Internal Server Error", "out_of_memory"); }
    snprintf(job->via, sizeof(job->via), "%s", via);
    for (const cJSON * item = changes->child; item; item = item->next) {
        const setting_desc_t * d = settings_registry_find(item->string);
        setv_result_t result = d ? SETV_OK : SETV_UNKNOWN_KEY;
        int value = 0;
        if (d && job->count >= (int) settings_registry_count()) result = SETV_INVALID_TYPE;
        else if (d && (d->flags & SET_F_READONLY)) result = SETV_READ_ONLY;
        else if (d) {
            for (int i = 0; i < job->count; i++) if (job->changes[i].desc == d) result = SETV_INVALID_TYPE; /* duplicate key */
            if (result == SETV_OK && d->type == SET_BOOL) {
                if (cJSON_IsBool(item)) value = cJSON_IsTrue(item) ? 1 : 0;
                else result = SETV_INVALID_TYPE;
            } else if (result == SETV_OK && !whole_int(item, &value)) {
                result = SETV_INVALID_TYPE;
            }
            if (result == SETV_OK) result = settings_registry_validate(d, value, acknowledged(acks, d->key));
        }
        if (result != SETV_OK) {
            if (error_count < ERRORS_MAX) {
                snprintf(errors[error_count].key, sizeof(errors[error_count].key), "%s", item->string ? item->string : "");
                errors[error_count].code = result;
                errors[error_count].desc = d;
                error_count++;
            }
        } else {
            job->changes[job->count].desc = d;
            job->changes[job->count].value = value;
            job->count++;
        }
    }
    cJSON_Delete(root);
    if (error_count) { reply_field_errors(status, status_size, body, errors, error_count); free(job); return; }

    int requested = job->count, bad = -1;
    int total = settings_registry_resolve(job->changes, job->count, &bad);
    if (total < 0) {
        snprintf(errors[0].key, sizeof(errors[0].key), "%s", job->changes[bad].desc->key);
        errors[0].code = SETV_DEPENDENCY_UNMET;
        errors[0].desc = job->changes[bad].desc;
        reply_field_errors(status, status_size, body, errors, 1);
        free(job);
        return;
    }
    job->count = total;
    /* The revision restarts at 0 on every boot, so a base or epoch from another boot
     * (or from the future) can never be trusted. */
    bool stale = (epoch_given && (uint32_t) epoch_value != settings_registry_epoch()) ||
                 (base_revision >= 0 && (uint32_t) base_revision > settings_registry_revision());
    for (int i = 0; i < requested && !stale && base_revision >= 0; i++)
        stale = settings_registry_changed_at(job->changes[i].desc) > (uint32_t) base_revision;
    if (stale) {
        char * values = remote_settings_build_values(0, true);
        const char * head = "{\"error\":\"settings_error\",\"code\":\"revision_mismatch\",\"current\":";
        size_t n = strlen(head) + (values ? strlen(values) : 2) + 2;
        char * full = malloc(n);
        if (full) snprintf(full, n, "%s%s}", head, values ? values : "{}");
        reply(status, status_size, body, "409 Conflict", full);
        free(full); free(values); free(job);
        return;
    }
    uint32_t revision = 0;
    bool busy = false;
    if (!submit_job(job, &revision, &busy))
        reply_error(status, status_size, body, busy ? "409 Conflict" : "503 Service Unavailable",
                    busy ? "settings_busy" : "player_busy");
    else
        reply_applied(status, status_size, body, job, requested, revision);
    free(job);
}

static void handle_post_settings(int fd, const remote_settings_request_t * request, const char * via,
                                 char * status, size_t status_size, char ** body) {
    if (request->content_length <= 0) return reply_error(status, status_size, body, "400 Bad Request", "body_required");
    if (request->content_length > REMOTE_SETTINGS_BODY_MAX)
        return reply_error(status, status_size, body, "413 Payload Too Large", "body_too_large");
    /* Read and shape-check the body before taking post_mutex so a slow sender cannot hold it. */
    char * text = read_body(fd, (size_t) request->content_length);
    if (!text) return reply_error(status, status_size, body, "400 Bad Request", "body_unreadable");
    if (!nesting_ok(text)) {
        free(text);
        return reply_error(status, status_size, body, "400 Bad Request", "invalid_body");
    }
    pthread_mutex_lock(&post_mutex);
    handle_post_settings_locked(text, via, status, status_size, body);
    pthread_mutex_unlock(&post_mutex);
}

static void handle_rescan(int fd, const remote_settings_request_t * request, char * status, size_t status_size,
                          char ** body) {
    if (request->content_length > REMOTE_SETTINGS_BODY_MAX)
        return reply_error(status, status_size, body, "413 Payload Too Large", "body_too_large");
    if (request->content_length > 0) free(read_body(fd, (size_t) request->content_length)); /* ignored */
    if (rescan_busy_probe && rescan_busy_probe())
        return reply_error(status, status_size, body, "409 Conflict", "rescan_running");
    pthread_mutex_lock(&job_mutex);
    rescan_pending = true;
    pthread_mutex_unlock(&job_mutex);
    reply(status, status_size, body, "202 Accepted", "{\"accepted\":true}");
}

bool remote_settings_handle(int fd, const char * method, const char * path_only, const char * path,
                            const remote_settings_request_t * request, char * status, size_t status_size,
                            char ** body) {
    bool schema = strcmp(path_only, "/api/settings/schema") == 0;
    bool values = strcmp(path_only, "/api/settings") == 0;
    bool rescan = strcmp(path_only, "/api/settings/actions/library.rescan") == 0;
    if (!schema && !values && !rescan) return false;
    bool loopback;
    const char * via;
    peer_info(fd, &loopback, &via);
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if ((rescan && !post) || (!rescan && !get && !(values && post))) {
        reply_error(status, status_size, body, "405 Method Not Allowed", "method_not_allowed");
        return true;
    }
    if (get) {
        char * json = NULL;
        if (schema) json = remote_settings_build_schema();
        else {
            bool has_since = false, has_epoch = false;
            uint32_t since = query_uint(path, "since", &has_since);
            uint32_t epoch = query_uint(path, "epoch", &has_epoch);
            /* A cursor from another boot, or from the future, cannot be trusted: send everything. */
            if (!has_since || since > settings_registry_revision() || (has_epoch && epoch != settings_registry_epoch()))
                since = 0;
            json = remote_settings_build_values(since, true);
        }
        if (json) { snprintf(status, status_size, "200 OK"); *body = json; }
        else reply_error(status, status_size, body, "500 Internal Server Error", "out_of_memory");
        return true;
    }
    /* Writes: header auth only (a cookie is ambient and forgeable from a web
     * page) and a JSON content type. */
    if (!request->header_auth)
        reply_error(status, status_size, body, "401 Unauthorized", "header_auth_required");
    else if (!request->json_content_type)
        reply_error(status, status_size, body, "415 Unsupported Media Type", "json_required");
    else if (rescan)
        handle_rescan(fd, request, status, status_size, body);
    else
        handle_post_settings(fd, request, via, status, status_size, body);
    return true;
}
