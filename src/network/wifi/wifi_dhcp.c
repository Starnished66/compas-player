#include "wifi_dhcp.h"
#include "wifi_status.h"
#include "../../core/device/hostname_apply.h"
#include "../../core/process/subprocess.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/prctl.h>

#define WIFI_INTERFACE "wlan0"
#define DHCP_PIDFILE "/var/run/udhcpc." WIFI_INTERFACE ".pid"
#define LOOP_MS 500
#define PING_TICKS 4   /* liveness check after 2 s of silence */
#define RETRY_TICKS 10 /* 5 s before relaunching a client that died */

static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static atomic_bool running;
/* Worker-thread state; wifi_dhcp_stop() touches it only after the join. */
static pid_t client_pid;
static int retry_ticks;

/* Stops our client and any other udhcpc: the vendor script's one-shot
 * instance, or one left by an earlier player run. */
static void client_stop(void) {
    if (client_pid > 0) subprocess_terminate(client_pid);
    client_pid = 0;
    subprocess_kill_all_matching("udhcpc");
    unlink(DHCP_PIDFILE);
}

/* Long-lived foreground client: renews, re-discovers after a NAK, asks for
 * broadcast replies (some mesh and extender access points need them) and
 * declines an offered address another device already answers for (-a). */
static void client_start(void) {
    client_stop();
    char name[64], hostname_opt[80];
    hostname_device_name(name, sizeof(name));
    snprintf(hostname_opt, sizeof(hostname_opt), "hostname:%s", name);
    char * argv[] = { (char *) "udhcpc", (char *) "-f", (char *) "-i", (char *) WIFI_INTERFACE, (char *) "-B",
                      (char *) "-a", (char *) "-x", hostname_opt, (char *) "-p", (char *) DHCP_PIDFILE, NULL };
    if (subprocess_start(argv, &client_pid)) return;
    client_pid = 0;
    retry_ticks = RETRY_TICKS;
}

/* Reaps a client that exited on its own and schedules its relaunch. */
static void client_reap(void) {
    if (client_pid <= 0 || waitpid(client_pid, NULL, WNOHANG) != client_pid) return;
    client_pid = 0;
    retry_ticks = RETRY_TICKS;
}

/* ifconfig wlan0 0.0.0.0: the kernel keeps the address after the link drops. */
static void clear_address(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct ifreq req;
    memset(&req, 0, sizeof(req));
    snprintf(req.ifr_name, sizeof(req.ifr_name), WIFI_INTERFACE);
    ((struct sockaddr_in *) &req.ifr_addr)->sin_family = AF_INET;
    (void) ioctl(fd, SIOCSIFADDR, &req);
    close(fd);
}

/* Control socket receiving wpa_supplicant's events, or -1. */
static int attach(void) {
    int fd = wifi_wpa_ctrl_open();
    if (fd < 0) return -1;
    char reply[16];
    if (wifi_wpa_ctrl_request(fd, "ATTACH", reply, sizeof(reply), 1500) && strncmp(reply, "OK", 2) == 0) return fd;
    wifi_wpa_ctrl_close(fd);
    return -1;
}

static void handle_event(const char * msg, bool * associated) {
    static int last_id = -1; /* wpa_supplicant network id of the current association */
    if (strstr(msg, "CTRL-EVENT-CONNECTED")) {
        const char * id = strstr(msg, "[id=");
        int net = id ? atoi(id + 4) : -1;
        /* A roam within the same network keeps its lease: renew instead of starting over. */
        if (*associated && client_pid > 0 && net >= 0 && net == last_id) kill(client_pid, SIGUSR1);
        else client_start();
        last_id = net;
        *associated = true;
    } else if (strstr(msg, "CTRL-EVENT-DISCONNECTED")) {
        *associated = false;
        last_id = -1;
        client_stop();
        clear_address();
    }
}

static void * worker_func(void * arg) {
    (void) prctl(PR_SET_NAME, "dhcp");
    (void) arg;
    int fd = -1, ticks = 0;
    bool associated = false;
    client_stop(); /* the vendor script's client, started before association */
    while (atomic_load(&running)) {
        if (fd < 0) {
            fd = attach();
            if (fd < 0) {
                poll(NULL, 0, LOOP_MS);
                continue;
            }
            /* Already associated, or a new wpa_supplicant replaced the one we knew. */
            associated = wifi_get_connected();
            if (associated) client_start();
            else client_stop();
        }
        client_reap();
        if (retry_ticks > 0) retry_ticks--;
        if (associated && client_pid <= 0 && retry_ticks == 0) client_start();

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        char msg[1024];
        if (poll(&pfd, 1, LOOP_MS) > 0) {
            ssize_t n = recv(fd, msg, sizeof(msg) - 1, MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
            } else {
                msg[n] = '\0';
                handle_event(msg, &associated);
                if (!strstr(msg, "CTRL-EVENT-TERMINATING")) continue;
            }
        } else if (++ticks % PING_TICKS != 0 || send(fd, "PING", 4, MSG_DONTWAIT) >= 0) {
            continue; /* a PONG arrives as an ordinary message above */
        }
        /* wpa_supplicant is gone (Wi-Fi off, radio script restart): reattach when it is back. */
        wifi_wpa_ctrl_close(fd);
        fd = -1;
    }
    if (fd >= 0) wifi_wpa_ctrl_close(fd);
    return NULL;
}

void wifi_dhcp_start(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    if (!atomic_load(&running)) {
        atomic_store(&running, true);
        if (pthread_create(&worker, NULL, worker_func, NULL) != 0) atomic_store(&running, false);
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}

void wifi_dhcp_stop(void) {
    pthread_mutex_lock(&lifecycle_mutex);
    if (atomic_exchange(&running, false)) {
        pthread_join(worker, NULL);
        client_stop();
        clear_address();
    }
    pthread_mutex_unlock(&lifecycle_mutex);
}
