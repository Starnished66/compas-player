#include "bt_link_key_watch.h"
#include "bt_rc_trust.h"
#include "../../../core/system/logging/debug_log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/platform_util.h"

/* The system Bluetooth headers are not in the cross toolchain, so the few kernel constants are
 * spelled out here (include/net/bluetooth/hci_sock.h and mgmt.h). */
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef PF_BLUETOOTH
#define PF_BLUETOOTH AF_BLUETOOTH
#endif
#define BTPROTO_HCI 1
#define HCI_DEV_NONE 0xffff
#define HCI_CHANNEL_CONTROL 3

struct sockaddr_hci_mgmt {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

#define BT_MGMT_EV_NEW_LINK_KEY 0x0009
#define BT_MGMT_HDR_SIZE 6
#define BT_MGMT_NEW_LINK_KEY_TYPE_OFFSET 8 /* in the payload: store_hint, addr 6, addr_type, then key_type and val */
#define BT_MGMT_NEW_LINK_KEY_KEY_OFFSET 9
#define BT_MGMT_NEW_LINK_KEY_SIZE 26 /* store_hint, then mgmt_link_key_info: addr 6, addr_type, key_type, val 16, pin_len */
#define BT_MGMT_OP_GET_CONNECTIONS 0x0015
#define BT_MGMT_EV_CMD_COMPLETE 0x0001
#define BT_MGMT_EV_CMD_STATUS 0x0002
#define BT_MGMT_STATUS_NOT_POWERED 0x0f
#define BT_MGMT_STATUS_INVALID_INDEX 0x11
#define BT_MGMT_ADDR_INFO_SIZE 7 /* bdaddr[6] (reversed), addr_type */

typedef enum {
    BT_MGMT_CONNS_IGNORE, /* not the answer to Get Connections for hci0 */
    BT_MGMT_CONNS_LIST,   /* success: *count addresses, see connection_mac */
    BT_MGMT_CONNS_NONE,   /* controller off or gone: no connections */
    BT_MGMT_CONNS_BAD     /* failed or malformed: the connections are unknown */
} bt_mgmt_conns_t;

/* The upper-case MAC of a wire address (6 bytes, reversed). */
static void wire_address_mac(const uint8_t * a, char mac[BT_RC_TRUST_MAC_SIZE]) {
    snprintf(mac, BT_RC_TRUST_MAC_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* One mgmt event (little endian header { opcode, index, len } then payload; see the kernel's
 * Documentation/bluetooth/mgmt-api.txt). True for a well-formed New Link Key event, with the
 * peer's upper-case MAC (the wire address is reversed) and its store_hint. The key is left in buf. */
static bool parse_new_link_key(const uint8_t * buf, size_t len, char mac[BT_RC_TRUST_MAC_SIZE],
                               uint8_t * store_hint) {
    if (!buf || len < BT_MGMT_HDR_SIZE + BT_MGMT_NEW_LINK_KEY_SIZE) return false;
    unsigned opcode = buf[0] | (unsigned) buf[1] << 8;
    unsigned plen = buf[4] | (unsigned) buf[5] << 8;
    if (opcode != BT_MGMT_EV_NEW_LINK_KEY || plen != BT_MGMT_NEW_LINK_KEY_SIZE ||
        len < BT_MGMT_HDR_SIZE + plen)
        return false;
    const uint8_t * p = buf + BT_MGMT_HDR_SIZE;
    *store_hint = p[0];
    wire_address_mac(p + 1, mac);
    return true;
}

/* One mgmt event judged as the answer to Get Connections on index 0 (the only controller the
 * player uses, hci0). Command Complete carries { u16 opcode, u8 status, u16 count, count x
 * addr_info }, Command Status { u16 opcode, u8 status }. Lengths are checked strictly. */
static bt_mgmt_conns_t parse_connections(const uint8_t * buf, size_t len, int * count) {
    if (!buf || len < BT_MGMT_HDR_SIZE) return BT_MGMT_CONNS_IGNORE;
    unsigned ev = buf[0] | (unsigned) buf[1] << 8, index = buf[2] | (unsigned) buf[3] << 8;
    unsigned plen = buf[4] | (unsigned) buf[5] << 8;
    if ((ev != BT_MGMT_EV_CMD_COMPLETE && ev != BT_MGMT_EV_CMD_STATUS) || index != 0 || plen < 3)
        return BT_MGMT_CONNS_IGNORE;
    const uint8_t * p = buf + BT_MGMT_HDR_SIZE;
    if ((p[0] | (unsigned) p[1] << 8) != BT_MGMT_OP_GET_CONNECTIONS) return BT_MGMT_CONNS_IGNORE;
    if (len < BT_MGMT_HDR_SIZE + plen) return BT_MGMT_CONNS_BAD;
    unsigned status = p[2];
    if (status == BT_MGMT_STATUS_NOT_POWERED || status == BT_MGMT_STATUS_INVALID_INDEX) return BT_MGMT_CONNS_NONE;
    if (status != 0 || ev != BT_MGMT_EV_CMD_COMPLETE || plen < 5) return BT_MGMT_CONNS_BAD;
    unsigned n = p[3] | (unsigned) p[4] << 8;
    if (plen < 5 + (size_t) BT_MGMT_ADDR_INFO_SIZE * n) return BT_MGMT_CONNS_BAD;
    *count = (int) n;
    return BT_MGMT_CONNS_LIST;
}

/* The upper-case MAC of connection i of a response BT_MGMT_CONNS_LIST accepted. */
static void connection_mac(const uint8_t * buf, int i, char mac[BT_RC_TRUST_MAC_SIZE]) {
    wire_address_mac(buf + BT_MGMT_HDR_SIZE + 5 + (size_t) BT_MGMT_ADDR_INFO_SIZE * (size_t) i, mac);
}

#define REOPEN_DELAY_S 5
#define POLL_MS 1000
#define CONNECTIONS_WAIT_S 2
/* What one queued event can cost the receive buffer (skb overhead included), taken high. */
#define EVENT_BUFFER_COST 1024

static pthread_mutex_t watch_mutex = PTHREAD_MUTEX_INITIALIZER;
static int watch_fd = -1;       /* guarded by watch_mutex; only the thread opens and closes it */
static bool watch_broken;       /* a read failed; the thread closes the socket */
static int watch_rcvbuf = 212992; /* the effective SO_RCVBUF of watch_fd (guarded by watch_mutex) */
static atomic_bool watch_ok = false;
static pthread_once_t start_once = PTHREAD_ONCE_INIT;

static void set_ok(bool ok) {
    if (atomic_exchange(&watch_ok, ok) != ok)
        DBG_LOG("bt_link_key_watch: %s\n", ok ? "watching" : "not watching, Remote Control trust is off");
}

static int open_mgmt(void) {
    int fd = socket(PF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, BTPROTO_HCI);
    if (fd < 0) return -1;
    struct sockaddr_hci_mgmt addr = {.hci_family = AF_BLUETOOTH, .hci_dev = HCI_DEV_NONE,
                                     .hci_channel = HCI_CHANNEL_CONTROL};
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    /* The kernel drops events when this socket's queue is full, with no count; a big queue
     * leaves only a reader starved long enough to overflow it as a risk (see drain). */
    int rcvbuf = 1 << 20;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf)) < 0)
        (void) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    socklen_t len = sizeof(rcvbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0 && rcvbuf > 0) watch_rcvbuf = rcvbuf;
    /* Without CAP_NET_ADMIN the bind succeeds but no events arrive: that must not look healthy. */
    if (geteuid() != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool bt_link_key_watch_healthy(void) {
    return atomic_load(&watch_ok);
}

/* One queued event: all of it is decided by bt_rc_trust_link_key_event. */
static void handle_event(const uint8_t * buf, size_t n) {
    char mac[BT_RC_TRUST_MAC_SIZE];
    uint8_t store_hint;
    if (!parse_new_link_key(buf, n, mac, &store_hint)) return;
    const uint8_t * payload = buf + BT_MGMT_HDR_SIZE;
    bt_rc_trust_link_key_event(mac, store_hint != 0, payload[BT_MGMT_NEW_LINK_KEY_TYPE_OFFSET],
                               payload + BT_MGMT_NEW_LINK_KEY_KEY_OFFSET);
}

/* Reads one queued packet into buf. 1: got one, 0: queue empty, -1: the socket failed. */
static int read_event(int fd, uint8_t * buf, size_t size, size_t * n) {
    for (;;) {
        ssize_t r = recv(fd, buf, size, MSG_DONTWAIT);
        if (r > 0) {
            *n = (size_t) r;
            return 1;
        }
        if (r < 0 && errno == EINTR) continue;
        return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
}

static void revoke_connected(int fd);

/* Reads every queued event. FIONREAD cannot size the queue (it is not served on this channel and
 * would report one packet), so the events of one pass are counted at EVENT_BUFFER_COST each: a
 * pass that held half the receive buffer or more may have overflowed it, and the kernel drops
 * events then without a count. That fails closed: all trust is revoked and the connected devices
 * are swept again. Residual risk: events lost to a failed allocation inside the kernel, or to a
 * queue that overflowed and was drained between two passes, look like nothing. */
void bt_link_key_watch_drain(void) {
    pthread_mutex_lock(&watch_mutex);
    unsigned long queued = 0;
    while (watch_fd >= 0 && !watch_broken) {
        uint8_t buf[512];
        size_t n = 0;
        int r = read_event(watch_fd, buf, sizeof(buf), &n);
        if (r > 0) {
            handle_event(buf, n);
            queued++;
        }
        mbedtls_platform_zeroize(buf, sizeof(buf));
        if (r > 0) continue;
        if (r < 0) {
            watch_broken = true;
            set_ok(false);
        } else if (queued * EVENT_BUFFER_COST >= (unsigned long) watch_rcvbuf / 2) {
            DBG_LOG("bt_link_key_watch: receive queue may have overflowed, revoking all trust\n");
            bt_rc_trust_clear();
            revoke_connected(watch_fd);
        }
        break;
    }
    pthread_mutex_unlock(&watch_mutex);
}

static long ms_until(const struct timespec * deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (deadline->tv_sec - now.tv_sec) * 1000L + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

/* One queued event during revoke_connected: the answer to Get Connections revokes each listed
 * device. Returns true once the request is finished; *answered when its list was usable. */
static bool handle_connections_event(const uint8_t * buf, size_t n, bool * answered) {
    int count = 0;
    switch (parse_connections(buf, n, &count)) {
        case BT_MGMT_CONNS_LIST:
            for (int i = 0; i < count; i++) {
                char mac[BT_RC_TRUST_MAC_SIZE];
                connection_mac(buf, i, mac);
                bt_rc_trust_remove(mac);
            }
            *answered = true;
            return true;
        case BT_MGMT_CONNS_NONE: *answered = true; return true;
        case BT_MGMT_CONNS_BAD: return true; /* not answered: everything is revoked by the caller */
        case BT_MGMT_CONNS_IGNORE: handle_event(buf, n); break;
    }
    return false;
}

/* Reads what is queued on fd until the request is finished. Returns false when the socket failed. */
static bool read_connections(int fd, bool * answered, bool * finished) {
    uint8_t buf[512];
    size_t n = 0;
    int r = 1;
    while (!*finished && (r = read_event(fd, buf, sizeof(buf), &n)) > 0)
        *finished = handle_connections_event(buf, n, answered);
    mbedtls_platform_zeroize(buf, sizeof(buf));
    return r >= 0;
}

/* A link already up when the watch starts may have been re-paired unseen (the process was not
 * running), and the kernel drops a temporary key when the link goes down. So every connected
 * device loses its trust; if the list cannot be had, everything does. Caller holds watch_mutex;
 * the lock order is watch_mutex then trust_mutex, as in drain. Sets watch_broken on a socket error. */
static void revoke_connected(int fd) {
    static const uint8_t cmd[BT_MGMT_HDR_SIZE] = {BT_MGMT_OP_GET_CONNECTIONS & 0xff, BT_MGMT_OP_GET_CONNECTIONS >> 8};
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += CONNECTIONS_WAIT_S;
    /* A command the kernel rejects before its handler (no hci0 yet at boot: INVALID_INDEX)
     * returns 0, not the length; the status still arrives below. */
    bool answered = false, finished = false, sent = write(fd, cmd, sizeof(cmd)) >= 0;
    while (sent && !finished && !watch_broken) {
        long wait = ms_until(&deadline);
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (wait <= 0) break;
        if (poll(&pfd, 1, (int) wait) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (!read_connections(fd, &answered, &finished)) watch_broken = true;
    }
    if (!answered) bt_rc_trust_clear();
}

static void * watch_thread(void * arg) {
    (void) arg;
    bool failed_before = false;
    for (;;) {
        int fd = open_mgmt();
        if (fd < 0) {
            failed_before = true;
            sleep(REOPEN_DELAY_S);
            continue;
        }
        /* Keys made while the watch was down went unseen: nothing stays trusted across the gap. */
        if (failed_before) bt_rc_trust_clear();
        pthread_mutex_lock(&watch_mutex);
        watch_fd = fd;
        watch_broken = false;
        revoke_connected(fd); /* healthy only once the links already up have been revoked */
        bool usable = !watch_broken;
        pthread_mutex_unlock(&watch_mutex);
        if (usable) set_ok(true);
        while (usable) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            (void) poll(&pfd, 1, POLL_MS);
            bt_link_key_watch_drain();
            pthread_mutex_lock(&watch_mutex);
            bool broken = watch_broken;
            pthread_mutex_unlock(&watch_mutex);
            if (broken) break;
        }
        pthread_mutex_lock(&watch_mutex);
        watch_fd = -1;
        pthread_mutex_unlock(&watch_mutex);
        close(fd);
        failed_before = true;
        sleep(REOPEN_DELAY_S);
    }
    return NULL;
}

static void start_thread(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, watch_thread, NULL) == 0) pthread_detach(t);
}

void bt_link_key_watch_start(void) {
    pthread_once(&start_once, start_thread);
}
