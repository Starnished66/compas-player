#include "hiby_sys_server.h"
#include "bluetooth_control.h"
#include "debug_log.h"
#include "utf8_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <stdint.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_SERVER_SOCKET_PATH
#define SYS_SERVER_SOCKET_PATH "/var/run/sys_server"
#endif

/* Reports are copied into bounded latest-value slots. Socket and Bluetooth
 * queries run on this process-lifetime worker, never on the caller/UI. */
#define SYS_SERVER_TIMEOUT_MS 300
enum { REPORT_STATUS, REPORT_METADATA, REPORT_POSITION, REPORT_VOLUME, REPORT_COUNT };
typedef struct {
    char command[600];
    uint64_t sequence;
    bool pending;
    int volume;
} report_slot_t;
static report_slot_t reports[REPORT_COUNT];
static pthread_mutex_t report_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t report_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t report_once = PTHREAD_ONCE_INIT;
static bool report_worker_ready;
static uint64_t report_sequence;

static int wait_socket(int fd, short events) {
    struct pollfd p = { .fd = fd, .events = events };
    int result;
    do { result = poll(&p, 1, SYS_SERVER_TIMEOUT_MS); } while (result < 0 && errno == EINTR);
    return result > 0 && (p.revents & events);
}

static void send_command(const char * cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SYS_SERVER_SOCKET_PATH);
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS || !wait_socket(fd, POLLOUT)) goto done;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) goto done;
    }
    size_t sent = 0, length = strlen(cmd);
    while (sent < length) {
        ssize_t count = send(fd, cmd + sent, length - sent, MSG_NOSIGNAL);
        if (count > 0) sent += (size_t) count;
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && wait_socket(fd, POLLOUT)) continue;
        else goto done;
    }
    /* Drain one reply so the daemon's send cannot block. */
    if (wait_socket(fd, POLLIN)) {
        char reply[64];
        ssize_t got = recv(fd, reply, sizeof(reply) - 1, 0);
        if (got > 0) {
            reply[got] = '\0';
            DBG_LOG("hiby_sys_server: \"%s\" -> \"%s\"\n", cmd, reply);
        }
    }
done:
    close(fd);
}

static void * report_worker(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&report_mutex);
        int selected;
        for (;;) {
            selected = -1;
            for (int i = 0; i < REPORT_COUNT; ++i)
                if (reports[i].pending && (selected < 0 || reports[i].sequence < reports[selected].sequence)) selected = i;
            if (selected >= 0) break;
            pthread_cond_wait(&report_cond, &report_mutex);
        }
        report_slot_t report = reports[selected];
        reports[selected].pending = false;
        pthread_mutex_unlock(&report_mutex);
        if (selected == REPORT_VOLUME) {
            char mac[18];
            if (!bt_control_get_connected_device_mac(mac, sizeof(mac))) continue;
            int raw = (report.volume * 127 + 50) / 100;
            snprintf(report.command, sizeof(report.command), "BT:ABSVOL:%s %d", mac, raw);
        }
        send_command(report.command);
    }
    return NULL;
}

static void start_report_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, report_worker, NULL) == 0) {
        pthread_detach(thread);
        report_worker_ready = true;
    }
}

static void enqueue_report(int kind, const char * command, int volume) {
    pthread_once(&report_once, start_report_worker);
    if (!report_worker_ready) return;
    pthread_mutex_lock(&report_mutex);
    /* A new track supersedes any unsent position from the previous track.
     * The subsequent position call is sequenced after this metadata. */
    if (kind == REPORT_METADATA) reports[REPORT_POSITION].pending = false;
    report_slot_t * slot = &reports[kind];
    snprintf(slot->command, sizeof(slot->command), "%s", command ? command : "");
    slot->volume = volume;
    slot->sequence = ++report_sequence;
    slot->pending = true;
    pthread_cond_signal(&report_cond);
    pthread_mutex_unlock(&report_mutex);
}

void hiby_sys_server_report_playback_status(bool playing) {
    enqueue_report(REPORT_STATUS, playing ? "BT:PLAYER_STATUS:playing" : "BT:PLAYER_STATUS:paused", 0);
}

void hiby_sys_server_report_metadata(const char * title, const char * artist,
                                      const char * album, const char * genre, long length_ms) {
    /* The daemon stores title/artist/album in 128 bytes and genre in 64.
     * Bound each field before framing, so long snapshots cannot truncate
     * later fields or
     * split a UTF-8 character. Tabs are protocol separators, not tag text. */
    const char * source[] = { title, artist, album, genre };
    char fields[4][128];
    for (size_t i = 0; i < 4; i++) {
        const char * text = source[i] ? source[i] : "";
        utf8_truncate_safe_bounded(fields[i], i == 3 ? 64 : sizeof(fields[i]), text, strlen(text));
        for (char * p = fields[i]; *p; p++)
            if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    }
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "BT:METADATA:%s\t%s\t%s\t%s\t%ld",
             fields[0], fields[1], fields[2], fields[3], length_ms);
    enqueue_report(REPORT_METADATA, cmd, 0);
}

void hiby_sys_server_report_position(long position_ms) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "BT:POSITION:%ld", position_ms);
    enqueue_report(REPORT_POSITION, cmd, 0);
}

void hiby_sys_server_report_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    enqueue_report(REPORT_VOLUME, NULL, percent);
}
