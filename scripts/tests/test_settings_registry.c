/* Host test for the settings registry and the remote settings handlers.
 * Built by test_settings_registry.sh against the real sources; the UI-thread
 * side is a small stand-in that stores the values like the device does. */
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "src/core/persistence/settings.h"
#include "src/core/persistence/settings_registry.h"
#include "src/network/remote/control/remote_settings.h"
#include "src/network/remote/control/remote_settings_gate.h"

player_settings_t current_settings;
const int SCREEN_TIMEOUT_STEPS[SCREEN_TIMEOUT_STEP_COUNT] = { 15, 30, 60, 120, 300, 600, 1800 };
const int SCREEN_DIM_DELAY_STEPS[SCREEN_DIM_DELAY_STEP_COUNT] = { 5, 10, 15, 30, 60, 120, 300 };
const int IDLE_SHUTDOWN_STEPS[IDLE_SHUTDOWN_STEP_COUNT] = { 5, 10, 15, 30, 60 };
const int SLEEP_TIMER_STEPS[SLEEP_TIMER_STEP_COUNT] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180 };
const char * i18n_tr(const char * s) { return s; }
const char * app_version_label(void) { return "test"; }

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void store(const setting_desc_t * d, int v) {
    unsigned char * base = (unsigned char *) &current_settings;
    if (d->size == 1) *(bool *) (base + d->offset) = v != 0;
    else memcpy(base + d->offset, &v, sizeof(int));
}

/* Stand-in for gui_remote_settings_poll(). */
static volatile int ui_run = 1;
static void * ui_thread(void * arg) {
    (void) arg;
    static remote_settings_job_t job;
    while (ui_run) {
        if (remote_settings_consume_job(&job)) {
            for (int i = 0; i < job.count; i++) store(job.changes[i].desc, job.changes[i].value);
            settings_registry_note_save(&current_settings);
            remote_settings_finish_job(settings_registry_revision());
        }
        usleep(1000);
    }
    return NULL;
}

static bool feature(unsigned flag) { (void) flag; return true; }
static bool runtime(const setting_desc_t * d, int * out) { (void) d; *out = 1; return true; }

static void test_table(void) {
    size_t n = settings_registry_count();
    CHECK(n > 20 && n <= REMOTE_SETTINGS_MAX_CHANGES);
    for (size_t i = 0; i < n; i++) {
        const setting_desc_t * d = settings_registry_at(i);
        CHECK(d->key && d->key[0] && d->group && d->label);
        CHECK(d->offset + d->size <= sizeof(player_settings_t));
        CHECK(d->size == 1 || d->size == sizeof(int) || d->type == SET_STRING);
        for (size_t j = i + 1; j < n; j++) CHECK(strcmp(d->key, settings_registry_at(j)->key) != 0);
        if (d->type == SET_STRING) CHECK(d->flags & SET_F_READONLY);
        if (d->depends) CHECK(settings_registry_find(d->depends) && settings_registry_find(d->depends)->type == SET_BOOL);
        /* Fuzz: out-of-domain values are never accepted. */
        int probes[] = { INT_MIN, -1, 2, 3, 101, 1000, INT_MAX, d->min - 1, d->max + 1 };
        for (size_t k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
            setv_result_t r = settings_registry_validate(d, probes[k], true);
            if (d->flags & SET_F_READONLY) { CHECK(r == SETV_READ_ONLY); continue; }
            bool in_domain = false;
            if (d->type == SET_BOOL) in_domain = probes[k] == 0 || probes[k] == 1;
            else if (d->type == SET_INT) in_domain = probes[k] >= d->min && probes[k] <= d->max;
            for (int p = 0; p < d->preset_count && d->type == SET_PRESET; p++) in_domain |= d->presets[p] == probes[k];
            for (int p = 0; p < d->option_count && d->type == SET_ENUM; p++) in_domain |= d->options[p].value == probes[k];
            CHECK((r == SETV_OK) == in_domain);
        }
        /* Every legal value is accepted (acknowledged). */
        for (int p = 0; p < d->preset_count; p++) CHECK(settings_registry_validate(d, d->presets[p], true) == SETV_OK);
        for (int p = 0; p < d->option_count; p++) CHECK(settings_registry_validate(d, d->options[p].value, true) == SETV_OK);
    }
    CHECK(settings_registry_validate(settings_registry_find("brightness_percent"), 4, false) == SETV_OUT_OF_RANGE);
    CHECK(settings_registry_validate(settings_registry_find("brightness_percent"), 5, false) == SETV_OK);
    CHECK(settings_registry_validate(settings_registry_find("screen_timeout_seconds"), 7, false) == SETV_NOT_AN_OPTION);
    CHECK(settings_registry_validate(settings_registry_find("idle_shutdown_minutes"), 1, false) == SETV_NOT_AN_OPTION);
}

static bool contains_nocase(const char * hay, const char * needle) { return hay && strcasestr(hay, needle); }

static void test_no_secrets(void) {
    static const char * const banned[] = { "pin", "password", "subsonic", "jellyfin", "tidal", "qobuz", "token",
                                           "secret", "credential", "username", "last_track", "path" };
    snprintf(current_settings.remote_control_pin, sizeof(current_settings.remote_control_pin), "%s", "424242");
    snprintf(current_settings.subsonic_password, sizeof(current_settings.subsonic_password), "%s", "hunter2secret");
    snprintf(current_settings.subsonic_url, sizeof(current_settings.subsonic_url), "%s", "https://leak.example");
    snprintf(current_settings.hostname, sizeof(current_settings.hostname), "%s", "r1");
    settings_registry_note_save(&current_settings);
    char * schema = remote_settings_build_schema();
    char * values = remote_settings_build_values(0, false);
    CHECK(schema && values);
    for (size_t b = 0; b < sizeof(banned) / sizeof(banned[0]); b++) {
        CHECK(!contains_nocase(schema, banned[b]));
        CHECK(!contains_nocase(values, banned[b]));
        for (size_t i = 0; i < settings_registry_count(); i++) CHECK(!contains_nocase(settings_registry_at(i)->key, banned[b]));
    }
    CHECK(!strstr(values, "424242") && !strstr(values, "hunter2secret") && !strstr(values, "leak.example"));
    CHECK(strstr(values, "\"hostname\":\"r1\"")); /* the allowed read-only string is present */
    CHECK(!settings_registry_find("remote_control_pin") && !settings_registry_find("subsonic_password"));
    free(schema); free(values);
}

static void test_dependencies_and_revision(void) {
    settings_registry_note_save(&current_settings);
    uint32_t rev = settings_registry_revision();
    settings_registry_note_save(&current_settings);
    CHECK(settings_registry_revision() == rev); /* unchanged values: no bump */
    current_settings.volume = 0.3f; current_settings.last_position = 12.0;
    settings_registry_note_save(&current_settings);
    CHECK(settings_registry_revision() == rev); /* fields outside the registry: no bump */
    current_settings.lyrics_enabled = !current_settings.lyrics_enabled;
    settings_registry_note_save(&current_settings);
    CHECK(settings_registry_revision() == rev + 1);
    CHECK(settings_registry_changed_at(settings_registry_find("lyrics_enabled")) == rev + 1);
    CHECK(settings_registry_changed_at(settings_registry_find("gapless_enabled")) <= rev);

    current_settings.gapless_enabled = false; current_settings.crossfade_enabled = false;
    settings_registry_note_save(&current_settings);
    setting_change_t c[REMOTE_SETTINGS_MAX_CHANGES];
    int bad = -1;
    c[0] = (setting_change_t){ settings_registry_find("crossfade_enabled"), 1 };
    CHECK(settings_registry_resolve(c, 1, &bad) == -1 && bad == 0); /* gapless is off */
    c[1] = (setting_change_t){ settings_registry_find("gapless_enabled"), 1 };
    CHECK(settings_registry_resolve(c, 2, &bad) == 2); /* turned on in the same request */
    current_settings.gapless_enabled = true; current_settings.crossfade_enabled = true;
    settings_registry_note_save(&current_settings);
    c[0] = (setting_change_t){ settings_registry_find("gapless_enabled"), 0 };
    CHECK(settings_registry_resolve(c, 1, &bad) == 2 && c[1].desc == settings_registry_find("crossfade_enabled") && c[1].value == 0);
}

static void test_acknowledgement(void) {
    const setting_desc_t * d = settings_registry_find("max_volume_percent");
    current_settings.max_volume_percent = 60;
    settings_registry_note_save(&current_settings);
    CHECK(settings_registry_loud_threshold() == SETTINGS_REGISTRY_LOUD_DEFAULT);
    CHECK(settings_registry_validate(d, 85, false) == SETV_OK);
    CHECK(settings_registry_validate(d, 86, false) == SETV_CONFIRMATION_REQUIRED);
    CHECK(settings_registry_validate(d, 100, true) == SETV_OK);
    settings_registry_hooks_t hooks = { .feature_present = feature, .runtime_value = runtime, .loud_volume_percent = 70 };
    settings_registry_set_hooks(&hooks);
    CHECK(settings_registry_validate(d, 75, false) == SETV_CONFIRMATION_REQUIRED);
    CHECK(settings_registry_validate(d, 70, false) == SETV_OK);
    current_settings.max_volume_percent = 100;
    settings_registry_note_save(&current_settings);
    CHECK(settings_registry_validate(d, 90, false) == SETV_OK); /* lowering an already high cap */
    current_settings.max_volume_percent = 60;
    settings_registry_note_save(&current_settings);
}

static void test_gate_clock(void) {
    CHECK(!remote_settings_gate_open(0));
    remote_settings_gate_set(true, 1000);
    CHECK(remote_settings_gate_open(1000 + REMOTE_SETTINGS_ALLOW_MS - 1));
    CHECK(!remote_settings_gate_open(1000 + REMOTE_SETTINGS_ALLOW_MS));
    CHECK(remote_settings_gate_consume_expired(1000 + REMOTE_SETTINGS_ALLOW_MS));
    CHECK(!remote_settings_gate_consume_expired(1000 + REMOTE_SETTINGS_ALLOW_MS + 1));
    remote_settings_gate_set(true, 5);
    remote_settings_gate_set(false, 6);
    CHECK(!remote_settings_gate_open(7) && !remote_settings_gate_consume_expired(7));
}

static int tcp_listener = -1;
static bool use_tcp;

/* A fresh connection per request, like the real server (it closes after each). */
static void open_pair(int * server_fd, int * client_fd) {
    if (!use_tcp) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        *server_fd = sv[1]; *client_fd = sv[0];
        return;
    }
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(addr);
    if (tcp_listener < 0) {
        tcp_listener = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(bind(tcp_listener, (struct sockaddr *) &addr, sizeof(addr)) == 0 && listen(tcp_listener, 4) == 0);
    }
    getsockname(tcp_listener, (struct sockaddr *) &addr, &len);
    *client_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(*client_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    *server_fd = accept(tcp_listener, NULL, NULL);
}

/* Runs one request through the handler on a connected socket pair. */
static int call(const char * method, const char * path, bool header_auth,
                bool json, const char * body, char ** reply) {
    int server_fd, client_fd;
    open_pair(&server_fd, &client_fd);
    char status[40];
    char * out = NULL;
    size_t length = body ? strlen(body) : 0;
    if (length) CHECK(write(client_fd, body, length) == (ssize_t) length);
    remote_settings_request_t req = { body ? (long long) length : -1, header_auth, json };
    char path_only[128];
    snprintf(path_only, sizeof(path_only), "%s", path);
    if (strchr(path_only, '?')) *strchr(path_only, '?') = '\0';
    bool routed = remote_settings_handle(server_fd, method, path_only, path, &req, status, sizeof(status), &out);
    CHECK(routed);
    close(server_fd); close(client_fd);
    *reply = out; if (getenv("DBG")) printf("%s %s -> %s %s\n", method, path, status, out ? out : "");
    return atoi(status);
}

static bool rescan_busy;
static bool probe_busy(void) { return rescan_busy; }

static void test_http(void) {
    char * r;
    pthread_t ui;
    pthread_create(&ui, NULL, ui_thread, NULL);

    remote_settings_gate_set(false, remote_settings_now_ms());
    const char * body = "{\"changes\":{\"lyrics_enabled\":false}}";
    CHECK(call("POST", "/api/settings", false, true, body, &r) == 401); /* cookie only */
    CHECK(strstr(r, "header_auth_required")); free(r);
    CHECK(call("POST", "/api/settings", true, false, body, &r) == 415); free(r);
    CHECK(call("POST", "/api/settings", true, true, body, &r) == 403); /* switch is off */
    CHECK(strstr(r, "remote_changes_disabled")); free(r);
    CHECK(call("POST", "/api/settings/actions/library.rescan", true, true, NULL, &r) == 403); free(r);
    CHECK(!remote_settings_consume_rescan());
    CHECK(call("GET", "/api/settings", false, false, NULL, &r) == 200); /* reads need no switch */
    CHECK(strstr(r, "\"writable\":false")); free(r);
    CHECK(call("GET", "/api/settings/schema", false, false, NULL, &r) == 200);
    CHECK(strstr(r, "\"settings_v1\"") == NULL && strstr(r, "library.rescan")); free(r);

    remote_settings_gate_set(true, remote_settings_now_ms());
    current_settings.lyrics_enabled = true;
    settings_registry_note_save(&current_settings);
    uint32_t rev = settings_registry_revision();
    CHECK(call("POST", "/api/settings", true, true, body, &r) == 200);
    CHECK(!current_settings.lyrics_enabled && settings_registry_revision() == rev + 1);
    CHECK(strstr(r, "\"applied\":[\"lyrics_enabled\"]")); free(r);
    CHECK(call("POST", "/api/settings", true, true,
               "{\"baseRevision\":0,\"changes\":{\"lyrics_enabled\":true}}", &r) == 409);
    CHECK(strstr(r, "revision_mismatch")); free(r);

    /* A cursor from another boot, or from the future, is never trusted. */
    uint32_t epoch = settings_registry_epoch(), now_rev = settings_registry_revision();
    char path[96];
    CHECK(epoch != 0 && epoch < 1000000000u);
    CHECK(call("GET", "/api/settings", false, false, NULL, &r) == 200);
    CHECK(strstr(r, "\"full\":true")); free(r);
    snprintf(path, sizeof(path), "/api/settings?since=%u&epoch=%u", now_rev, epoch);
    CHECK(call("GET", path, false, false, NULL, &r) == 200);
    CHECK(strstr(r, "\"full\":false") && strstr(r, "\"values\":{}")); free(r);
    snprintf(path, sizeof(path), "/api/settings?since=%u&epoch=%u", now_rev, epoch + 1);
    CHECK(call("GET", path, false, false, NULL, &r) == 200 && strstr(r, "\"full\":true")); free(r);
    snprintf(path, sizeof(path), "/api/settings?since=%u&epoch=%u", now_rev + 40, epoch);
    CHECK(call("GET", path, false, false, NULL, &r) == 200 && strstr(r, "\"full\":true")); free(r);
    snprintf(path, sizeof(path), "{\"baseRevision\":%u,\"changes\":{\"lyrics_enabled\":true}}", now_rev + 40);
    CHECK(call("POST", "/api/settings", true, true, path, &r) == 409 && strstr(r, "revision_mismatch")); free(r);
    snprintf(path, sizeof(path), "{\"epoch\":%u,\"changes\":{\"lyrics_enabled\":true}}", epoch + 1);
    CHECK(call("POST", "/api/settings", true, true, path, &r) == 409 && strstr(r, "revision_mismatch")); free(r);
    snprintf(path, sizeof(path), "{\"epoch\":%u,\"baseRevision\":%u,\"changes\":{\"lyrics_enabled\":true}}",
             epoch, settings_registry_revision());
    CHECK(call("POST", "/api/settings", true, true, path, &r) == 200 && strstr(r, "\"epoch\"")); free(r);
    CHECK(call("POST", "/api/settings", true, true,
               "{\"changes\":{\"lyrics_enabled\":true},\"x\":[[[[[[1]]]]]]}", &r) == 400);
    CHECK(strstr(r, "invalid_body")); free(r);
    CHECK(call("POST", "/api/settings", true, true,
               "{\"changes\":{\"lyrics_enabled\":true},\"note\":\"[[[[[[[[[[\"}", &r) == 200); free(r); /* brackets in strings do not count */

    const char * bad[] = {
        "{\"changes\":{\"remote_control_enabled\":true}}", "{\"changes\":{\"wifi_enabled\":false}}",
        "{\"changes\":{\"hostname\":\"x\"}}", "{\"changes\":{\"usb_mode\":1}}", "{\"changes\":{\"bt_codec\":\"sbc\"}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(call("POST", "/api/settings", true, true, bad[i], &r) == 403);
        CHECK(strstr(r, "read_only")); free(r);
    }
    CHECK(call("POST", "/api/settings", true, true, "{\"changes\":{\"remote_control_pin\":\"1\"}}", &r) == 400);
    CHECK(strstr(r, "unknown_key")); free(r);
    CHECK(call("POST", "/api/settings", true, true, "{\"changes\":{\"lyrics_enabled\":1}}", &r) == 400); free(r);
    CHECK(call("POST", "/api/settings", true, true, "{\"changes\":{\"brightness_percent\":2}}", &r) == 422);
    CHECK(strstr(r, "\"min\":5")); free(r);
    CHECK(call("POST", "/api/settings", true, true, "{\"changes\":{\"max_volume_percent\":100}}", &r) == 422);
    CHECK(strstr(r, "confirmation_required")); free(r);
    CHECK(current_settings.max_volume_percent == 60); /* nothing was applied */
    CHECK(call("POST", "/api/settings", true, true,
               "{\"changes\":{\"max_volume_percent\":100},\"acknowledge\":[\"max_volume_percent\"]}", &r) == 200);
    CHECK(current_settings.max_volume_percent == 100); free(r);
    CHECK(call("POST", "/api/settings", true, true,
               "{\"changes\":{\"brightness_percent\":50,\"hostname\":\"x\"}}", &r) == 403); /* all or nothing */
    CHECK(current_settings.brightness_percent != 50); free(r);
    CHECK(call("POST", "/api/settings", true, true,
               "{\"changes\":{\"gapless_enabled\":false}}", &r) == 200);
    CHECK(strstr(r, "alsoChanged\":[\"crossfade_enabled\"]") || !current_settings.crossfade_enabled); free(r);
    CHECK(call("POST", "/api/settings/actions/library.rescan", true, true, NULL, &r) == 202); free(r);
    CHECK(remote_settings_consume_rescan() && !remote_settings_consume_rescan());
    rescan_busy = true;
    remote_settings_set_rescan_probe(probe_busy);
    CHECK(call("POST", "/api/settings/actions/library.rescan", true, true, NULL, &r) == 409);
    CHECK(strstr(r, "rescan_running") && !remote_settings_consume_rescan()); free(r);
    rescan_busy = false;

    /* Loopback (the adb forward) is exempt from the switch, not from header auth or JSON. */
    remote_settings_gate_set(false, remote_settings_now_ms());
    use_tcp = true;
    CHECK(call("POST", "/api/settings", true, true, body, &r) == 200); free(r);
    CHECK(call("POST", "/api/settings", false, true, body, &r) == 401); free(r);
    CHECK(call("GET", "/api/settings", false, false, NULL, &r) == 200);
    CHECK(strstr(r, "\"writable\":true")); free(r);

    ui_run = 0;
    pthread_join(ui, NULL);
    close(tcp_listener);
}

int main(void) {
    settings_registry_hooks_t boot_hooks = { .feature_present = feature, .runtime_value = runtime };
    settings_registry_set_hooks(&boot_hooks);
    test_table();
    test_no_secrets();
    test_dependencies_and_revision();
    test_acknowledgement();
    test_gate_clock();
    settings_registry_hooks_t hooks = { .feature_present = feature, .runtime_value = runtime };
    settings_registry_set_hooks(&hooks);
    test_http();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
