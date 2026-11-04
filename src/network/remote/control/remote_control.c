#include "remote_control.h"
#include "remote_control_internal.h"
#include "../discovery/remote_control_mdns.h"
#include "../discovery/remote_control_discovery.h"
#include "remote_control_webapp.h"
#include "../import/remote_import_files.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/persistence/settings_registry.h"
#include "remote_settings.h"
#include "remote_plugins.h"
#include "../../bluetooth/control/bt_rc_trust.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Port 8899 -- shared by the PIN-protected remote and native file API. */
#define REMOTE_CONTROL_PORT 8899
/* Same server on the default HTTP port, so http://r1.compas.local needs no
 * port number. Best-effort: 8899 keeps working if this cannot be bound. */
#define REMOTE_CONTROL_WEB_PORT 80

static atomic_bool running = false;
static int listen_fd = -1;
static int web_listen_fd = -1;
static pthread_t listener_thread;
static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Serializes start and stop end to end. listener_mutex cannot be held across
 * the listener join (the listener takes it), so a concurrent start would
 * otherwise reuse the listener state while stop is still joining. */
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
#define REMOTE_CLIENT_WORKERS 2
static int active_client_fds[REMOTE_CLIENT_WORKERS] = {-1, -1};
static pthread_t client_threads[REMOTE_CLIENT_WORKERS];
static bool client_thread_active[REMOTE_CLIENT_WORKERS];
static bool client_thread_done[REMOTE_CLIENT_WORKERS];

/* The PIN is shared by Wi-Fi and RFCOMM because both transports carry the
 * same HTTP request format. Keep reads and UI writes serialized. */
static pthread_mutex_t pin_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int pin_failures = 0;
static char pin_failed_candidates[5][REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static time_t pin_failure_window = 0;
static time_t pin_blocked_until = 0;

#define REMOTE_CONTROL_PIN_MIN_LENGTH 4

static bool remote_control_pin_is_valid(const char * pin) {
    if (!pin) return false;
    size_t len = strlen(pin);
    if (len < REMOTE_CONTROL_PIN_MIN_LENGTH || len > REMOTE_CONTROL_PIN_MAX_LENGTH) return false;
    for (size_t i = 0; i < len; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

/* The fixed PIN earlier builds seeded as a placeholder for Android app
 * testing. Every device shipped that same value, so a saved copy is treated
 * as unset and replaced with a random PIN once. */
#define REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN "0000"
#define REMOTE_CONTROL_GENERATED_PIN_DIGITS 6

/* Fills out[] with a uniformly random REMOTE_CONTROL_GENERATED_PIN_DIGITS
 * PIN from the kernel CSPRNG. Rejection sampling keeps every PIN equally
 * likely (a plain modulo of a 32-bit value would favor low PINs). */
static bool remote_control_random_pin(char * out, size_t out_size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const uint32_t range = 1000000u;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    bool ok = false;
    for (int attempt = 0; attempt < 16 && !ok; attempt++) {
        uint32_t value = 0;
        size_t got = 0;
        while (got < sizeof(value)) {
            ssize_t n = read(fd, (uint8_t *) &value + got, sizeof(value) - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t) n;
        }
        if (got != sizeof(value)) break;
        if (value >= limit) continue;
        snprintf(out, out_size, "%0*u", REMOTE_CONTROL_GENERATED_PIN_DIGITS, (unsigned) (value % range));
        ok = true;
    }
    close(fd);
    return ok;
}

/* Caller holds pin_mutex. Forgets the wrong candidates counted so far. */
static void pin_reset_failures_locked(void) {
    pin_failures = 0;
    memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
}

/* Caller holds pin_mutex. A new PIN invalidates everything learned about the
 * old one, including an active lockout. */
static void remote_control_store_pin_locked(const char * pin) {
    settings_set_remote_control_pin(&current_settings, pin);
    /* Bluetooth trust earned with the old PIN goes too, in memory and in the snapshot saved below. */
    bt_rc_trust_clear();
    current_settings.bt_rc_trusted_count = 0;
    pin_reset_failures_locked();
    pin_failure_window = 0;
    pin_blocked_until = 0;
    settings_save_async(&current_settings);
}

/* Generates the PIN once on first use and reuses it after that. A saved
 * 4-12 digit PIN is kept, except the old shared placeholder. Returns false
 * only when no PIN exists and the CSPRNG cannot be read; callers then refuse
 * to serve rather than fall back to a guessable value. */
static bool remote_control_ensure_pin(void) {
    pthread_mutex_lock(&pin_mutex);
    const char * saved = current_settings.remote_control_pin;
    if (remote_control_pin_is_valid(saved) && strcmp(saved, REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN) != 0) {
        pthread_mutex_unlock(&pin_mutex);
        return true;
    }

    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

bool remote_control_generate_new_pin(void) {
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    pthread_mutex_lock(&pin_mutex);
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    /* Never "regenerate" to the same value, which would look like a no-op. */
    if (ok && strcmp(pin, current_settings.remote_control_pin) == 0)
        ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

void remote_control_get_pin(char * out_pin, size_t out_pin_size) {
    if (!out_pin || out_pin_size == 0) return;
    /* The UI shows the PIN before the server is ever enabled, so create it
     * here too rather than displaying an empty or placeholder value. */
    remote_control_ensure_pin();
    pthread_mutex_lock(&pin_mutex);
    snprintf(out_pin, out_pin_size, "%s", current_settings.remote_control_pin);
    pthread_mutex_unlock(&pin_mutex);
}

typedef enum {
    REMOTE_CONTROL_PIN_INVALID = 0,
    REMOTE_CONTROL_PIN_VALID = 1,
    REMOTE_CONTROL_PIN_RATE_LIMITED = 2
} remote_control_pin_result_t;

static remote_control_pin_result_t remote_control_pin_check(const char * candidate) {
    bool matches = false;
    time_t now = time(NULL);
    pthread_mutex_lock(&pin_mutex);
    if (now < pin_blocked_until) {
        pthread_mutex_unlock(&pin_mutex);
        return REMOTE_CONTROL_PIN_RATE_LIMITED;
    }

    const char * expected = current_settings.remote_control_pin;
    size_t candidate_len = candidate ? strlen(candidate) : 0;
    size_t expected_len = strlen(expected);
    unsigned int diff = (unsigned int) (candidate_len ^ expected_len);
    size_t compare_len = candidate_len > expected_len ? candidate_len : expected_len;
    for (size_t i = 0; i < compare_len; i++) {
        unsigned char a = i < candidate_len ? (unsigned char) candidate[i] : 0;
        unsigned char b = i < expected_len ? (unsigned char) expected[i] : 0;
        diff |= (unsigned int) (a ^ b);
    }
    matches = diff == 0 && remote_control_pin_is_valid(candidate) &&
              remote_control_pin_is_valid(expected);
    if (matches) {
        pin_reset_failures_locked();
        pin_failure_window = 0;
    } else {
        if (now - pin_failure_window >= 60 || pin_failure_window == 0) {
            pin_failure_window = now;
            pin_reset_failures_locked();
        }
        bool already_counted = false;
        for (unsigned int i = 0; i < pin_failures; i++) {
            if (strcmp(pin_failed_candidates[i], candidate) == 0) {
                already_counted = true;
                break;
            }
        }
        if (!already_counted && pin_failures < 5) {
            /* Kept truncated to the slot size, like the former snprintf. */
            char * slot = pin_failed_candidates[pin_failures];
            size_t keep = candidate_len < sizeof(pin_failed_candidates[0]) - 1 ? candidate_len
                                                                              : sizeof(pin_failed_candidates[0]) - 1;
            memcpy(slot, candidate, keep);
            slot[keep] = '\0';
            pin_failures++;
        }
        if (pin_failures >= 5) {
            pin_blocked_until = now + 60;
            pin_reset_failures_locked();
            pin_failure_window = 0;
        }
    }
    pthread_mutex_unlock(&pin_mutex);
    return matches ? REMOTE_CONTROL_PIN_VALID : REMOTE_CONTROL_PIN_INVALID;
}

static void send_pin_rate_limited_response(int fd) {
    static const char body[] = "{\"error\":\"rate_limited\",\"code\":\"pin_rate_limited\",\"retryAfterSeconds\":60}";
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                              "Content-Length: %zu\r\nRetry-After: 60\r\nConnection: close\r\n\r\n",
                              sizeof(body) - 1);
    send_all(fd, header, (size_t) header_len);
    send_all(fd, body, sizeof(body) - 1);
}

/* Browser session for the web app: after one request carrying the PIN header,
 * the PIN rides in an HttpOnly, SameSite=Strict cookie so refreshes and the
 * :80/:8899 ports of the same host never ask again. pin == NULL clears it. */
#define REMOTE_CONTROL_PIN_COOKIE "compas_pin"

static void send_session_cookie(int fd, const char * pin) {
    char header[384];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 204 No Content\r\nSet-Cookie: " REMOTE_CONTROL_PIN_COOKIE "=%s; Path=/; Max-Age=%d; "
                              "HttpOnly; SameSite=Strict\r\nCache-Control: no-store\r\nContent-Length: 0\r\n"
                              "Connection: close\r\n\r\n",
                              pin ? pin : "", pin ? 31536000 : 0);
    send_all(fd, header, (size_t) header_len);
}

/* Copies the compas_pin value from a "Cookie:" header value; false if absent
 * or not a plausible PIN (digits only, bounded). */
static bool cookie_pin_value(const char * cookies, char * out, size_t out_size) {
    static const char name[] = REMOTE_CONTROL_PIN_COOKIE "=";
    for (const char * p = cookies; p && *p;) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (strncmp(p, name, sizeof(name) - 1) == 0) {
            const char * value = p + sizeof(name) - 1;
            size_t len = 0;
            while (value[len] >= '0' && value[len] <= '9') len++;
            if (!len || len >= out_size || (value[len] && value[len] != ';' && value[len] != ' ')) return false;
            memcpy(out, value, len);
            out[len] = '\0';
            return true;
        }
        p = strchr(p, ';');
    }
    return false;
}

/* Bounds on the request line and header read; see read_request_line(). */
#define REQUEST_HEADERS_MAX 4096
#define REQUEST_HEADERS_DEADLINE_SECONDS 10
#define REQUEST_READ_IDLE_TIMEOUT_MS 2000

/* Bound the complete pre-authentication request line and header read while
 * retaining the per-read idle timeout for slow or fragmented stream links. */
static bool request_headers_deadline(struct timespec * deadline) {
    if (!deadline || clock_gettime(CLOCK_MONOTONIC, deadline) != 0) return false;
    deadline->tv_sec += REQUEST_HEADERS_DEADLINE_SECONDS;
    return true;
}

/* 1 means readable, 0 means the idle or total deadline expired, -1 means an
 * I/O or monotonic-clock error. Recompute after EINTR so signals cannot reset
 * the shared absolute deadline. */
static int request_wait_readable(int fd, const struct timespec * deadline) {
    for (;;) {
        struct timespec now;
        if (!deadline || clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        time_t seconds = deadline->tv_sec - now.tv_sec;
        long nanoseconds = deadline->tv_nsec - now.tv_nsec;
        if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
        if (seconds < 0 || (seconds == 0 && nanoseconds <= 0)) return 0;

        long long remaining_ms = (long long) seconds * 1000LL +
                                 (nanoseconds + 999999L) / 1000000L;
        int timeout_ms = remaining_ms < REQUEST_READ_IDLE_TIMEOUT_MS
                       ? (int) remaining_ms : REQUEST_READ_IDLE_TIMEOUT_MS;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready == 0) return 0;
        return 1;
    }
}

/* ---- Remote Control v1 extensions: folders, recently played, favorites,
 * queue move/play, and playlist management. Each family reports errors as
 * {"error":"<family>_error","code":"<reason>"}; docs/REMOTE_CONTROL_API.md
 * lists every code. ---- */

/* Routes added after the original API. Returns false for any other path. */
static bool handle_extension_route(int cfd, const char * method, const char * path_only, const char * path,
                                   long long content_length) {
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if (get && strcmp(path_only, "/api/folders/roots") == 0) handle_folder_roots_request(cfd);
    else if (get && strcmp(path_only, "/api/folders") == 0) handle_folder_list_request(cfd, path);
    else if (post && strcmp(path_only, "/api/folders/play") == 0) handle_folder_play_request(cfd, path);
    else if (get && strcmp(path_only, "/api/recent") == 0) handle_recent_request(cfd, path);
    else if (get && strcmp(path_only, "/api/favorites/state") == 0) handle_favorite_state_request(cfd, path);
    else if (post && strcmp(path_only, "/api/favorites/add") == 0) handle_favorite_edit_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/favorites/remove") == 0) handle_favorite_edit_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/queue/move") == 0) handle_queue_edit_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/queue/play") == 0) handle_queue_edit_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/playlists/create") == 0) handle_playlist_create_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/rename") == 0) handle_playlist_rename_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/delete") == 0) handle_playlist_delete_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/remove") == 0) handle_playlist_entry_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/playlists/move") == 0) handle_playlist_entry_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/playlists/import") == 0)
        handle_playlist_import_request(cfd, path, content_length);
    else if (strcmp(path_only, "/api/library/sorting") == 0) handle_library_sorting_route(cfd, method, path);
    else if (strcmp(path_only, "/api/library/albums") == 0 && query_param_present(path, "scope"))
        handle_scoped_albums_route(cfd, method, path);
    else if (strcmp(path_only, "/api/playback/collection") == 0) handle_collection_play_route(cfd, method, path);
    else return false;
    return true;
}

/* Returns the text after the header name (given with its colon) when the
 * line starts with it, ignoring case; otherwise NULL. Whitespace after the
 * colon is left for the caller, because the parsers differ on it. */
static const char * header_after(const char * line, size_t len, const char * name) {
    size_t name_len = strlen(name);
    return len >= name_len && strncasecmp(line, name, name_len) == 0 ? line + name_len : NULL;
}

/* The request headers this server acts on. */
typedef struct {
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    bool pin_seen;
    bool pin_invalid;
    char cookie_pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    bool cookie_pin_seen;
    long long content_length; /* -1 until a valid header is read */
    bool content_length_invalid;
    bool transfer_encoding_seen;
    bool json_content_type; /* JSON settings and plugin/layout writes */
} request_headers_t;

/* Records one complete header line (no line ending) in h. */
static void parse_header_line(const char * line, size_t len, request_headers_t * h) {
    const char * value;
    if ((value = header_after(line, len, "X-Compas-PIN:")) != NULL) {
        while (*value == ' ' || *value == '\t') value++;
        size_t value_len = strlen(value);
        while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
        if (h->pin_seen || value_len == 0 || value_len >= sizeof(h->pin)) {
            /* The header was sent, so the cookie must not stand in for it. */
            h->pin_invalid = true;
            h->pin_seen = true;
        } else {
            memcpy(h->pin, value, value_len);
            h->pin[value_len] = '\0';
            h->pin_seen = true;
        }
    }
    if (!h->cookie_pin_seen && (value = header_after(line, len, "Cookie:")) != NULL)
        h->cookie_pin_seen = cookie_pin_value(value, h->cookie_pin, sizeof(h->cookie_pin));
    if ((value = header_after(line, len, "Content-Length:")) != NULL) {
        while (*value == ' ' || *value == '\t') value++;
        char * end = NULL;
        errno = 0;
        long long parsed = (*value >= '0' && *value <= '9') ? strtoll(value, &end, 10) : -1;
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (parsed < 0 || errno == ERANGE || !end || *end != '\0' || h->content_length >= 0)
            h->content_length_invalid = true;
        else
            h->content_length = parsed;
    }
    if ((value = header_after(line, len, "Content-Type:")) != NULL)
        h->json_content_type = remote_settings_content_type_is_json(value);
    if (header_after(line, len, "Transfer-Encoding:") != NULL)
        h->transfer_encoding_seen = true;
}

/* Reads the request line into buf. Stream transports (including RFCOMM) may
 * deliver it in several reads, so read through LF, bounded by the buffer
 * size, an idle timeout, and one deadline shared with the header read.
 * Returns false when the connection should be dropped (any reply is sent). */
static bool read_request_line(int cfd, const struct timespec * deadline, char * buf, size_t size) {
    size_t n = 0;
    bool line_complete = false;
    while (n + 1 < size) {
        int ready = request_wait_readable(cfd, deadline);
        if (ready <= 0) {
            if (ready == 0) send_response(cfd, "408 Request Timeout", "text/plain", "Request Timeout");
            return false;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return false; /* caller owns fd cleanup */
        buf[n++] = c;
        if (c == '\n') {
            line_complete = true;
            break;
        }
    }
    if (!line_complete) {
        send_response(cfd, "414 URI Too Long", "text/plain", "Request line too long");
        return false;
    }
    buf[n] = '\0';
    return true;
}

/* Consumes bounded headers before dispatch. Body-owning routes read their
 * payload afterward. Accepts CRLF or LF line endings. Returns false when the
 * connection should be dropped (any reply is sent). */
static bool read_request_headers(int cfd, const struct timespec * deadline, request_headers_t * h) {
    size_t header_bytes = 0;
    bool headers_complete = false;
    char header_line[512];
    size_t header_line_len = 0;
    memset(h, 0, sizeof(*h));
    h->content_length = -1; /* absent until a valid header is read */
    while (header_bytes < REQUEST_HEADERS_MAX) {
        int ready = request_wait_readable(cfd, deadline);
        if (ready <= 0) {
            send_response(cfd, ready == 0 ? "408 Request Timeout" : "400 Bad Request",
                          "text/plain", ready == 0 ? "Request Timeout" : "Bad Request");
            return false;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return false;
        header_bytes++;
        if (c == '\n') {
            if (header_line_len && header_line[header_line_len - 1] == '\r') header_line_len--;
            header_line[header_line_len] = '\0';
            if (header_line_len == 0) {
                headers_complete = true;
                break;
            }
            parse_header_line(header_line, header_line_len, h);
            header_line_len = 0;
        } else if (header_line_len + 1 < sizeof(header_line)) {
            header_line[header_line_len++] = c;
        } else {
            send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
            return false;
        }
    }
    if (!headers_complete) {
        send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
        return false;
    }
    return true;
}

/* GET /api/v1/capabilities: the public handshake. */
static void handle_capabilities_request(int cfd, bool trusted) {
    char capabilities[1536] = {0};
    char library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE] = {0};
    char escaped_library_id[METADATA_DB_CATALOG_LIBRARY_ID_SIZE * 2] = {0};
    char library_id_field[METADATA_DB_CATALOG_LIBRARY_ID_SIZE * 2 + 24] = {0};
    if (metadata_db_try_get_catalog_library_id(library_id, sizeof(library_id))) {
        json_escape_append(escaped_library_id, sizeof(escaped_library_id), library_id);
        snprintf(library_id_field, sizeof(library_id_field), "\"libraryId\":\"%s\",",
                 escaped_library_id);
    }
    snprintf(capabilities, sizeof(capabilities),
        "{%s\"version\":1,\"authRequired\":%s,\"authHeader\":\"X-Compas-PIN\","
        "\"transports\":[\"wifi\",\"bluetooth_classic_rfcomm\"],\"features\":["
        "\"status\",\"playback_controls\",\"queue\",\"library_browse\","
        "\"playlists\",\"album_art\",\"screenshots\",\"catalog_sync_v1\","
        "\"folder_browse\",\"recently_played\",\"favorites_edit\",\"queue_edit\","
        "\"playlist_edit\",\"playlist_import\",\"player_accent\",\"device_info\","
        "\"collection_playback\",\"shuffle_modes\",\"library_sorting\","
        "\"wifi_import\",\"wifi_import_files\",\"settings_v1\",\"plugin_store\",\"plugin_management\",\"player_layouts\"],"
        "\"settingsSchemaVersion\":%d}", library_id_field, trusted ? "false" : "true", SETTINGS_REGISTRY_SCHEMA_VERSION);
    send_response(cfd, "200 OK", "application/json", capabilities);
}

#define BT_GRANT_WAIT_MS 3000
#define BT_GRANT_POLL_MS 100

/* Whether a Bluetooth peer is trusted. A device whose Pair tap is still waiting for the pairing to
 * finish connects within milliseconds of bonding, before the grant lands, so it is waited for
 * (bounded, no lock held). Wi-Fi (bt_mac NULL) is never trusted and never waits. */
static bool bt_peer_trusted(const char * bt_mac) {
    if (!bt_mac) return false;
    for (int waited = 0;; waited += BT_GRANT_POLL_MS) {
        /* Pending is read first: a grant leaves pending only after its trust was added. */
        bool pending = bt_rc_trust_grant_pending(bt_mac);
        if (bt_rc_trust_is_trusted(bt_mac)) return true;
        if (!pending || waited >= BT_GRANT_WAIT_MS) return false;
        usleep(BT_GRANT_POLL_MS * 1000);
    }
}

/* Session-cookie and PIN gate. Only the shell is public. All library,
 * status, artwork, playback, screenshot, playlist, queue, and asset requests
 * require the PIN, except on a trusted Bluetooth link (bt_rc_trust.h). A valid
 * PIN from a Bluetooth peer makes it trusted from then on. Returns true when
 * the request may be routed; otherwise the reply (or the session cookie) has
 * been sent. */
static bool request_authorized(int cfd, const char * method, const char * path_only,
                               const request_headers_t * h, bool trusted, const char * bt_mac) {
    bool public_request = strcmp(method, "GET") == 0 && strcmp(path_only, "/") == 0;
    if (strcmp(method, "POST") == 0 && strcmp(path_only, "/api/session/clear") == 0) {
        send_session_cookie(cfd, NULL);
        return false;
    }
    if (public_request) return true;
    if (trusted) {
        /* There is no PIN to remember: the link itself is the credential. */
        if (strcmp(method, "POST") == 0 && strcmp(path_only, "/api/session") == 0) {
            send_session_cookie(cfd, NULL);
            return false;
        }
        return true;
    }
    /* Read before the check: a PIN replaced meanwhile clears the trust list, which drops this add. */
    unsigned int epoch = bt_mac ? bt_rc_trust_epoch() : 0;
    /* An explicit header wins; the cookie only stands in when it is absent. */
    const char * candidate = h->pin_seen ? (h->pin_invalid ? NULL : h->pin)
                                         : (h->cookie_pin_seen ? h->cookie_pin : NULL);
    remote_control_pin_result_t pin_result = candidate ? remote_control_pin_check(candidate)
                                                       : REMOTE_CONTROL_PIN_INVALID;
    if (pin_result == REMOTE_CONTROL_PIN_RATE_LIMITED) {
        send_pin_rate_limited_response(cfd);
        return false;
    }
    if (pin_result != REMOTE_CONTROL_PIN_VALID) {
        send_api_error(cfd, "401 Unauthorized", "unauthorized", "pin_required");
        return false;
    }
    if (bt_mac) bt_rc_trust_add_at_epoch(bt_mac, epoch);
    if (strcmp(method, "POST") == 0 && strcmp(path_only, "/api/session") == 0) {
        send_session_cookie(cfd, candidate);
        return false;
    }
    return true;
}

/* /api/import/files*: the Wi-Fi import file API. Returns true when the
 * request was answered; false hands it back to the normal routes. */
static bool handle_import_files_route(int cfd, const char * method, const char * path, const char * path_only,
                                      long long content_length) {
    if (strncmp(path_only, "/api/import/files", sizeof("/api/import/files") - 1) != 0) return false;
    char normalized_path[REQUEST_LINE_MAX];
    const char *query = strchr(path, '?');
    int written = snprintf(normalized_path, sizeof(normalized_path), "%s%s", path_only, query ? query : "");
    if (written < 0 || (size_t)written >= sizeof(normalized_path)) {
        send_reason_error(cfd, "414 URI Too Long", "uri_too_long");
        return true;
    }
    remote_import_state_t import_state;
    remote_control_get_import_state(&import_state, NULL, 0);
    if (import_state != REMOTE_IMPORT_READY || !remote_import_files_is_ready()) {
        send_reason_error(cfd, "409 Conflict", "import_not_ready");
        return true;
    }
    struct sockaddr_storage local_address;
    socklen_t local_length = sizeof(local_address);
    bool wifi_transport = getsockname(cfd, (struct sockaddr *)&local_address, &local_length) == 0 &&
                          local_address.ss_family == AF_INET;
    return remote_import_files_handle(cfd, method, normalized_path, content_length, wifi_transport);
}

/* Settings registry routes. Returns false when the request is not theirs. */
static bool handle_settings_route(int cfd, const char * method, const char * path_only, const char * path,
                                  const request_headers_t * h, bool trusted) {
    remote_settings_request_t settings_request = { h->content_length, trusted || (h->pin_seen && !h->pin_invalid),
                                                   h->json_content_type };
    char settings_status[40];
    char * settings_body = NULL;
    if (!remote_settings_handle(cfd, method, path_only, path, &settings_request, settings_status,
                                sizeof(settings_status), &settings_body)) return false;
    send_response(cfd, settings_status, "application/json", settings_body ? settings_body : "{}");
    free(settings_body);
    return true;
}

/* Plugin and layout routes. Returns false when the request is not theirs. */
static bool handle_plugins_route(int cfd, const char * method, const char * path_only, const char * path,
                                 const request_headers_t * h, bool trusted) {
    remote_plugins_http_request_t plugins_request = { h->content_length, trusted || (h->pin_seen && !h->pin_invalid),
                                                     h->json_content_type };
    char plugins_status[40];
    char * plugins_body = NULL;
    if (!remote_plugins_handle(cfd, method, path_only, path, &plugins_request, plugins_status,
                               sizeof(plugins_status), &plugins_body)) return false;
    send_response(cfd, plugins_status, "application/json", plugins_body ? plugins_body :
                  "{\"error\":\"plugin_api_error\",\"code\":\"out_of_memory\"}");
    free(plugins_body);
    return true;
}

/* The original GET routes (fixed paths), then 404. */
static void handle_legacy_get(int cfd, const char * path_only, const char * path) {
    if (strcmp(path_only, "/api/status") == 0) {
        handle_status_get(cfd);
    } else if (strcmp(path_only, "/api/import") == 0) {
        handle_import_get(cfd);
    } else if (strcmp(path_only, "/api/catalog") == 0) {
        handle_catalog_page_request(cfd, path, false);
    } else if (strcmp(path_only, "/api/catalog/covers") == 0) {
        handle_catalog_page_request(cfd, path, true);
    } else if (strcmp(path_only, "/api/catalog/art") == 0) {
        handle_catalog_art_request(cfd, path);
    } else if (strcmp(path_only, "/api/library") == 0) {
        handle_library_get(cfd, path);
    } else if (handle_group_list_request(cfd, path_only)) {
        /* artists, album_artists or genres */
    } else if (strcmp(path_only, "/api/library/albums") == 0) {
        handle_albums_get(cfd, path);
    } else if (strcmp(path_only, "/api/art") == 0) {
        handle_art_request(cfd, path);
    } else if (strcmp(path_only, "/api/playlists") == 0) {
        handle_playlists_get(cfd);
    } else if (strcmp(path_only, "/api/playlists/songs") == 0) {
        handle_playlist_songs_request(cfd, path);
    } else if (strcmp(path_only, "/api/queue") == 0) {
        handle_queue_get(cfd, path);
    } else if (strcmp(path_only, "/") == 0) {
        send_response(cfd, "200 OK", "text/html", REMOTE_CONTROL_APP_HTML);
    } else {
        send_response(cfd, "404 Not Found", "text/plain", "Not Found");
    }
}

static void handle_legacy_route(int cfd, const char * method, const char * path_only, const char * path) {
    if (strcmp(method, "GET") == 0) {
        handle_legacy_get(cfd, path_only, path);
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(path_only, "/api/playlists") == 0 || strcmp(path_only, "/api/playlists/add") == 0)
            handle_legacy_playlist_post(cfd, path_only, path);
        else
            handle_legacy_playback_post(cfd, path_only, path);
    } else {
        send_response(cfd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
    }
}

/* The v1 namespace is an alias layer over the original routes: rewrites path_only
 * to the unversioned route, or answers /api/v1/capabilities itself (returns true). */
static bool route_v1_alias(int cfd, const char * method, char * path_only, bool trusted) {
    /* Require the slash after the version so that similarly named paths
     * (for example /api/v10) never match. */
    bool api_v1 = strncmp(path_only, "/api/v1/", sizeof("/api/v1/") - 1) == 0;
    if (api_v1) {
        if (strcmp(path_only, "/api/v1/capabilities") == 0 && strcmp(method, "GET") == 0) {
            handle_capabilities_request(cfd, trusted);
            return true;
        }
        /* Remove only "/v1" so "/api/v1/status" routes as "/api/status". */
        memmove(path_only + sizeof("/api") - 1, path_only + sizeof("/api/v1") - 1,
                strlen(path_only + sizeof("/api/v1") - 1) + 1);
    }
    return false;
}

/* Routes a request that passed authorization. */
static void route_authorized(int cfd, const char * method, const char * path_only, const char * path,
                             const request_headers_t * headers, bool trusted) {
    if (headers->content_length_invalid) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Content-Length");
        return;
    }
    if (headers->transfer_encoding_seen) {
        send_reason_error(cfd, "400 Bad Request", "transfer_encoding_unsupported");
        return;
    }
    if (handle_import_files_route(cfd, method, path, path_only, headers->content_length)) return;
    /* Import lifecycle routes come before the generic POST command
     * dispatcher below, which otherwise owns all non-GET methods. */
    if (handle_import_lifecycle_route(cfd, method, path_only)) return;
    if (handle_settings_route(cfd, method, path_only, path, headers, trusted)) return;
    if (handle_plugins_route(cfd, method, path_only, path, headers, trusted)) return;
    if (handle_extension_route(cfd, method, path_only, path, headers->content_length)) return;

    handle_legacy_route(cfd, method, path_only, path);
}

/* Reads one request from cfd and routes it. The caller owns closing cfd.
 * bt_mac is the normalized peer MAC of a Bluetooth connection, NULL for Wi-Fi. */
static void handle_connection(int cfd, const char * bt_mac) {
    struct timespec preauth_deadline;
    if (!request_headers_deadline(&preauth_deadline)) return;
    if (!remote_control_ensure_pin()) {
        send_response(cfd, "503 Service Unavailable", "text/plain", "PIN generation unavailable");
        return;
    }
    char buf[REQUEST_LINE_MAX];
    if (!read_request_line(cfd, &preauth_deadline, buf, sizeof(buf))) return;
    request_headers_t headers;
    if (!read_request_headers(cfd, &preauth_deadline, &headers)) return;
    /* Judged once the request is in, so a revoke while it was being read is honoured. */
    bool trusted = bt_peer_trusted(bt_mac);

    char method[8] = {0};
    char path[REQUEST_LINE_MAX] = {0};
    if (sscanf(buf, "%7s %4095s", method, path) != 2) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    /* Path only, no query string, for the exact-match routes below; the
     * full path keeps the query for the handlers that read parameters. */
    char path_only[REQUEST_LINE_MAX];
    snprintf(path_only, sizeof(path_only), "%s", path);
    char * q = strchr(path_only, '?');
    if (q) *q = '\0';

    if (route_v1_alias(cfd, method, path_only, trusted)) return;
    if (!request_authorized(cfd, method, path_only, &headers, trusted, bt_mac)) return;
    route_authorized(cfd, method, path_only, path, &headers, trusted);
}

/* Handle one HTTP request on an already connected stream socket. The caller
 * retains ownership of fd and is responsible for closing it. */
void remote_control_handle_stream(int fd, const char * bt_mac) {
    char mac[BT_RC_TRUST_MAC_SIZE];
    if (fd >= 0) handle_connection(fd, bt_mac && bt_rc_trust_normalize(bt_mac, mac) ? mac : NULL);
}

/* Timeout for poll() on listen_fd so listener_thread_func periodically checks
 * running and terminates cleanly on stop. */
#define ACCEPT_POLL_TIMEOUT_MS 200

typedef struct { int slot; int fd; } remote_client_start_t;

static void * client_worker_func(void *arg) {
    remote_client_start_t *start = arg;
    int slot = start->slot, fd = start->fd;
    free(start);
    handle_connection(fd, NULL);
    pthread_mutex_lock(&listener_mutex);
    if (slot >= 0 && slot < REMOTE_CLIENT_WORKERS && active_client_fds[slot] == fd) {
        active_client_fds[slot] = -1;
        close(fd);
        client_thread_done[slot] = true;
    }
    pthread_mutex_unlock(&listener_mutex);
    return NULL;
}

static void * listener_thread_func(void * arg) {
    (void) arg;
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_lock(&listener_mutex);
        struct pollfd pfds[2] = {
            { .fd = listen_fd, .events = POLLIN, .revents = 0 },
            { .fd = web_listen_fd, .events = POLLIN, .revents = 0 },
        };
        pthread_mutex_unlock(&listener_mutex);
        if (pfds[0].fd < 0) break;
        nfds_t count = pfds[1].fd >= 0 ? 2 : 1;
        int pr = poll(pfds, count, ACCEPT_POLL_TIMEOUT_MS);
        if (pr <= 0) continue; /* Timeout or interrupted -- re-check running */
        if (!atomic_load_explicit(&running, memory_order_acquire)) break;

        /* Accept only from a listener that is readable (an error or hangup
         * on one must not block or spin on accept). */
        int fd = (pfds[0].revents & POLLIN) ? pfds[0].fd
               : (count == 2 && (pfds[1].revents & POLLIN)) ? pfds[1].fd : -1;
        if (fd < 0) {
            usleep(50 * 1000); /* a persistent error state must not spin */
            continue;
        }
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) continue;
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        struct timeval send_timeout = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        pthread_mutex_lock(&listener_mutex);
        if (!atomic_load_explicit(&running, memory_order_acquire)) {
            pthread_mutex_unlock(&listener_mutex);
            close(cfd);
            break;
        }
        int slot = -1;
        for (int i = 0; i < REMOTE_CLIENT_WORKERS; i++) {
            if (client_thread_active[i] && client_thread_done[i]) {
                pthread_t old = client_threads[i];
                client_thread_active[i] = false;
                client_thread_done[i] = false;
                pthread_mutex_unlock(&listener_mutex);
                pthread_join(old, NULL);
                pthread_mutex_lock(&listener_mutex);
                i = -1;
                continue;
            }
            if (!client_thread_active[i]) { slot = i; break; }
        }
        if (slot < 0) {
            pthread_mutex_unlock(&listener_mutex);
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)send(cfd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
            close(cfd);
            continue;
        }
        active_client_fds[slot] = cfd;
        client_thread_active[slot] = true;
        client_thread_done[slot] = false;
        remote_client_start_t *start = malloc(sizeof(*start));
        if (!start) {
            active_client_fds[slot] = -1;
            client_thread_active[slot] = false;
            pthread_mutex_unlock(&listener_mutex);
            close(cfd);
            continue;
        }
        *start = (remote_client_start_t){slot, cfd};
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        (void)pthread_attr_setstacksize(&attr, REMOTE_CONTROL_CLIENT_STACK_SIZE);
        int create_result = pthread_create(&client_threads[slot], &attr, client_worker_func, start);
        pthread_attr_destroy(&attr);
        if (create_result != 0) {
            free(start);
            active_client_fds[slot] = -1;
            client_thread_active[slot] = false;
            pthread_mutex_unlock(&listener_mutex);
            close(cfd);
            continue;
        }
        pthread_mutex_unlock(&listener_mutex);
    }
    pthread_mutex_lock(&listener_mutex);
    for (int i = 0; i < REMOTE_CLIENT_WORKERS; i++)
        if (active_client_fds[i] >= 0) shutdown(active_client_fds[i], SHUT_RDWR);
    pthread_mutex_unlock(&listener_mutex);
    for (int i = 0; i < REMOTE_CLIENT_WORKERS; i++) {
        pthread_mutex_lock(&listener_mutex);
        bool active = client_thread_active[i]; pthread_t thread = client_threads[i];
        pthread_mutex_unlock(&listener_mutex);
        if (active) pthread_join(thread, NULL);
    }
    return NULL;
}

static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void remote_control_start_locked(void) {
    if (!remote_control_ensure_pin()) return;
    pthread_mutex_lock(&listener_mutex);
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    listen_fd = open_listener(REMOTE_CONTROL_PORT);
    if (listen_fd < 0) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    web_listen_fd = open_listener(REMOTE_CONTROL_WEB_PORT);

    atomic_store_explicit(&running, true, memory_order_release);
    if (pthread_create(&listener_thread, NULL, listener_thread_func, NULL) != 0) {
        atomic_store_explicit(&running, false, memory_order_release);
        close(listen_fd);
        listen_fd = -1;
        if (web_listen_fd >= 0) close(web_listen_fd);
        web_listen_fd = -1;
    } else {
        /* DNS-SD runs on its own best-effort worker. A missing Wi-Fi link or
         * occupied multicast socket never affects the HTTP listener. */
        remote_control_mdns_start();
        remote_control_discovery_start();
    }
    pthread_mutex_unlock(&listener_mutex);
}

void remote_control_start(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    remote_control_start_locked();
    pthread_mutex_unlock(&lifecycle_mutex);
}

bool remote_control_is_running(void) {
    return atomic_load_explicit(&running, memory_order_acquire);
}

bool remote_control_serves_default_port(void) {
    pthread_mutex_lock(&listener_mutex);
    bool serving = atomic_load_explicit(&running, memory_order_acquire) && web_listen_fd >= 0;
    pthread_mutex_unlock(&listener_mutex);
    return serving;
}

static void remote_control_stop_locked(void) {
    remote_control_mdns_stop();
    remote_control_discovery_stop();
    pthread_mutex_lock(&listener_mutex);
    if (!atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    atomic_store_explicit(&running, false, memory_order_release);
    /* Interrupt poll(), accept(), and a client read before joining. The
     * client handler owns close(); shutdown only wakes a concurrent read. */
    int stopped_listen_fd = listen_fd;
    int stopped_web_listen_fd = web_listen_fd;
    if (stopped_listen_fd >= 0) shutdown(stopped_listen_fd, SHUT_RDWR);
    if (stopped_web_listen_fd >= 0) shutdown(stopped_web_listen_fd, SHUT_RDWR);
    for (int i = 0; i < REMOTE_CLIENT_WORKERS; i++)
        if (active_client_fds[i] >= 0) shutdown(active_client_fds[i], SHUT_RDWR);
    pthread_mutex_unlock(&listener_mutex);

    pthread_join(listener_thread, NULL);
    pthread_mutex_lock(&listener_mutex);
    if (listen_fd == stopped_listen_fd && listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    if (web_listen_fd == stopped_web_listen_fd && web_listen_fd >= 0) {
        close(web_listen_fd);
        web_listen_fd = -1;
    }
    for (int i = 0; i < REMOTE_CLIENT_WORKERS; i++) {
        active_client_fds[i] = -1;
        client_thread_active[i] = false;
        client_thread_done[i] = false;
    }
    pthread_mutex_unlock(&listener_mutex);
}

void remote_control_stop(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    remote_control_stop_locked();
    pthread_mutex_unlock(&lifecycle_mutex);
}
