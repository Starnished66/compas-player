#include "remote_control_discovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static const uint8_t discovery_request_prefix[] = "COMPAS_REMOTE_DISCOVER_V1";
static const uint8_t discovery_reply[] = "COMPAS_REMOTE_V1 8899\n";

static atomic_bool discovery_active;
static pthread_mutex_t discovery_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t discovery_worker;
static int discovery_socket = -1;
static int discovery_wake_read = -1;
static int discovery_wake_write = -1;

bool remote_control_discovery_request_is_valid(const uint8_t * packet, size_t packet_size) {
    if (!packet || packet_size != REMOTE_CONTROL_DISCOVERY_REQUEST_SIZE) return false;
    const size_t prefix_size = sizeof(discovery_request_prefix) - 1;
    if (memcmp(packet, discovery_request_prefix, prefix_size) != 0) return false;
    for (size_t i = prefix_size; i < packet_size; i++) {
        if (packet[i] != 0) return false;
    }
    return true;
}

size_t remote_control_discovery_make_reply(uint8_t * out, size_t out_size) {
    const size_t reply_size = sizeof(discovery_reply) - 1;
    if (!out || out_size < reply_size) return 0;
    memcpy(out, discovery_reply, reply_size);
    return reply_size;
}

static bool is_lan_ipv4(uint32_t network_address) {
    uint32_t address = ntohl(network_address);
    return (address >> 24) == 10 ||
           (address >> 20) == 0xac1 || /* 172.16.0.0/12 */
           (address >> 16) == 0xc0a8 || /* 192.168.0.0/16 */
           (address >> 16) == 0xa9fe; /* link-local */
}

static void * discovery_worker_main(void * unused) {
    (void) unused;
    int fd = discovery_socket;
    int wake_fd = discovery_wake_read;
    uint8_t packet[REMOTE_CONTROL_DISCOVERY_REQUEST_SIZE + 1];
    uint8_t reply[sizeof(discovery_reply) - 1];
    const size_t reply_size = remote_control_discovery_make_reply(reply, sizeof(reply));

    while (atomic_load_explicit(&discovery_active, memory_order_acquire)) {
        struct pollfd fds[2] = {
            { .fd = fd, .events = POLLIN },
            { .fd = wake_fd, .events = POLLIN },
        };
        int ready = poll(fds, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[0].revents & (POLLHUP | POLLNVAL)) break;
        if (fds[0].revents & POLLERR) {
            int socket_error = 0;
            socklen_t error_size = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) < 0 || socket_error == 0) break;
        }
        if (!(fds[0].revents & POLLIN)) continue;

        struct sockaddr_in source;
        socklen_t source_size = sizeof(source);
        ssize_t received = recvfrom(fd, packet, sizeof(packet), 0,
                                    (struct sockaddr *) &source, &source_size);
        if (received != REMOTE_CONTROL_DISCOVERY_REQUEST_SIZE ||
            source_size < sizeof(source) || source.sin_family != AF_INET ||
            !is_lan_ipv4(source.sin_addr.s_addr) ||
            !remote_control_discovery_request_is_valid(packet, (size_t) received)) {
            continue;
        }
        (void) sendto(fd, reply, reply_size, 0,
                      (const struct sockaddr *) &source, source_size);
    }
    return NULL;
}

void remote_control_discovery_start(void) {
    pthread_mutex_lock(&discovery_lifecycle_mutex);
    if (atomic_load_explicit(&discovery_active, memory_order_acquire)) {
        pthread_mutex_unlock(&discovery_lifecycle_mutex);
        return;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        pthread_mutex_unlock(&discovery_lifecycle_mutex);
        return;
    }
    (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(REMOTE_CONTROL_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (const struct sockaddr *) &address, sizeof(address)) < 0) {
        close(fd);
        pthread_mutex_unlock(&discovery_lifecycle_mutex);
        return;
    }

    int wake_pipe[2];
    if (pipe(wake_pipe) < 0) {
        close(fd);
        pthread_mutex_unlock(&discovery_lifecycle_mutex);
        return;
    }
    (void) fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
    (void) fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);

    discovery_socket = fd;
    discovery_wake_read = wake_pipe[0];
    discovery_wake_write = wake_pipe[1];
    atomic_store_explicit(&discovery_active, true, memory_order_release);
    if (pthread_create(&discovery_worker, NULL, discovery_worker_main, NULL) != 0) {
        atomic_store_explicit(&discovery_active, false, memory_order_release);
        close(discovery_socket);
        close(discovery_wake_read);
        close(discovery_wake_write);
        discovery_socket = discovery_wake_read = discovery_wake_write = -1;
    }
    pthread_mutex_unlock(&discovery_lifecycle_mutex);
}

void remote_control_discovery_stop(void) {
    pthread_mutex_lock(&discovery_lifecycle_mutex);
    if (!atomic_exchange_explicit(&discovery_active, false, memory_order_acq_rel)) {
        pthread_mutex_unlock(&discovery_lifecycle_mutex);
        return;
    }
    const uint8_t wake = 1;
    if (discovery_wake_write >= 0) (void) write(discovery_wake_write, &wake, sizeof(wake));
    pthread_join(discovery_worker, NULL);
    if (discovery_socket >= 0) close(discovery_socket);
    if (discovery_wake_read >= 0) close(discovery_wake_read);
    if (discovery_wake_write >= 0) close(discovery_wake_write);
    discovery_socket = discovery_wake_read = discovery_wake_write = -1;
    pthread_mutex_unlock(&discovery_lifecycle_mutex);
}
