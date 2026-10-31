#ifndef REMOTE_PLUGINS_H
#define REMOTE_PLUGINS_H

#include <stdbool.h>
#include <stddef.h>

#define REMOTE_PLUGINS_BODY_MAX 4096
#define REMOTE_PLUGINS_RESPONSE_MAX (1024 * 1024)
#define REMOTE_PLUGINS_ID_MAX 256
#define REMOTE_PLUGINS_PAGE_MAX 200

typedef enum {
    REMOTE_PLUGINS_STORE_STATE,
    REMOTE_PLUGINS_STORE_REFRESH,
    REMOTE_PLUGINS_STORE_INSTALL,
    REMOTE_PLUGINS_STORE_UPDATE,
    REMOTE_PLUGINS_STORE_UPDATE_ALL,
    REMOTE_PLUGINS_STORE_UNINSTALL,
    REMOTE_PLUGINS_LIST,
    REMOTE_PLUGINS_SET_ENABLED,
    REMOTE_PLUGINS_REFRESH,
    REMOTE_PLUGINS_LAYOUTS,
    REMOTE_PLUGINS_SELECT_LAYOUT,
    REMOTE_PLUGINS_STORE_PREVIEW,
    REMOTE_PLUGINS_LAYOUT_PREVIEW,
    REMOTE_PLUGINS_OPEN_SETTINGS,
} remote_plugins_operation_t;

typedef struct {
    remote_plugins_operation_t operation;
    char id[REMOTE_PLUGINS_ID_MAX];
    int offset;
    int limit;
    bool enabled;
    bool force;
} remote_plugins_job_t;

typedef struct {
    int status; /* HTTP status code */
    char * body; /* malloc-owned JSON, transferred to finish_job */
} remote_plugins_result_t;

typedef struct {
    long long content_length;
    bool header_auth;
    bool json_content_type;
} remote_plugins_http_request_t;

/* Call only after the shared PIN gate. Writes additionally require the PIN
 * header and application/json. No Lua uploads or caller-supplied URLs/paths. */
bool remote_plugins_handle(int fd, const char * method, const char * path_only,
                            const char * path, const remote_plugins_http_request_t * request,
                            char * status, size_t status_size, char ** body);

/* UI-thread handoff. GET snapshots and all mutations execute on the UI
 * thread; a timed-out queued request is cancelled before it can take effect. */
bool remote_plugins_consume_job(remote_plugins_job_t * out);
void remote_plugins_finish_job(remote_plugins_result_t * result);
void remote_plugins_error(remote_plugins_result_t * result, int status, const char * code);

#endif
