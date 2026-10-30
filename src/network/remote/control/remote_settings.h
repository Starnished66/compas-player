#ifndef REMOTE_SETTINGS_H
#define REMOTE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../core/persistence/settings_registry.h"

/* /api/v1/settings: schema, values, changes and actions, built on the
 * settings registry. Reads need only the PIN. Writes also need the PIN as a
 * header (never the browser cookie) and a JSON content type. The UI thread applies
 * the changes (gui_remote_settings.c); the HTTP thread waits for the result. */

#define REMOTE_SETTINGS_BODY_MAX 4096
#define REMOTE_SETTINGS_MAX_CHANGES 96 /* >= settings_registry_count() */

typedef struct {
    long long content_length;
    bool header_auth;        /* the PIN arrived in X-Compas-PIN, not the cookie */
    bool json_content_type;
} remote_settings_request_t;

/* True for "application/json" with optional parameters. */
bool remote_settings_content_type_is_json(const char * value);

/* Handles the settings routes (path_only already has /api/v1 rewritten to
 * /api). Returns false when the path is not a settings route. Otherwise
 * fills status ("200 OK") and a malloc'd JSON body the caller sends and frees. */
bool remote_settings_handle(int fd, const char * method, const char * path_only, const char * path,
                            const remote_settings_request_t * request, char * status, size_t status_size,
                            char ** body);

/* JSON builders (remote_settings_json.c); each returns a malloc'd string or NULL. */
char * remote_settings_build_schema(void);
char * remote_settings_build_values(uint32_t since, bool writable);
const char * remote_settings_apply_name(set_apply_t apply);
/* Appends a JSON-escaped copy of text to out (size bytes). */
void remote_settings_json_escape(char * out, size_t size, const char * text);

/* One queued change set, handed to the UI thread. */
typedef struct {
    setting_change_t changes[REMOTE_SETTINGS_MAX_CHANGES];
    int count;
    char via[12]; /* "wifi", "bluetooth" or "usb" */
} remote_settings_job_t;

/* Optional: true while a library scan is running (set by the UI at boot, read
 * on the HTTP thread to answer the rescan action). */
void remote_settings_set_rescan_probe(bool (*busy)(void));

/* UI thread only. Edge-triggered: returns true once per queued job. */
bool remote_settings_consume_job(remote_settings_job_t * job);
/* UI thread: tell the waiting HTTP thread the job is done. */
void remote_settings_finish_job(uint32_t revision);
/* UI thread: a rescan was requested. */
bool remote_settings_consume_rescan(void);

#endif /* REMOTE_SETTINGS_H */
