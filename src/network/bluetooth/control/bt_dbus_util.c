#include "bt_dbus_util.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

#define BT_DBUS_CALL_TIMEOUT_MS 1000
/* Re-check interval for a thread that has no wake fd or no pollable connection fd. */
#define BT_DBUS_FALLBACK_POLL_MS 200

/* The older libdbus shipped on the target lacks dbus_bus_get_name_owner(). Query the bus daemon
 * directly. */
bool bt_dbus_get_name_owner(DBusConnection * c, const char * name, char * out, size_t size) {
    if (size > 0) out[0] = '\0';
    DBusMessage * msg = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                     "org.freedesktop.DBus", "GetNameOwner");
    if (!msg) return false;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage * reply = dbus_connection_send_with_reply_and_block(c, msg, BT_DBUS_CALL_TIMEOUT_MS, &err);
    dbus_message_unref(msg);
    const char * value = NULL;
    bool ok = reply && dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &value, DBUS_TYPE_INVALID) && value;
    if (ok && size > 0) snprintf(out, size, "%s", value);
    if (reply) dbus_message_unref(reply);
    dbus_error_free(&err);
    return ok;
}

int bt_dbus_wake_create(void) {
    return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

void bt_dbus_wake_signal(int wake_fd) {
    if (wake_fd < 0) return;
    const uint64_t one = 1;
    ssize_t n;
    do { n = write(wake_fd, &one, sizeof(one)); } while (n < 0 && errno == EINTR);
    /* EAGAIN means the counter is saturated, so a wake is already pending. */
}

static void wake_drain(int wake_fd) {
    uint64_t count;
    ssize_t n;
    do { n = read(wake_fd, &count, sizeof(count)); } while (n < 0 && errno == EINTR);
}

static void dispatch_status_changed(DBusConnection * c, DBusDispatchStatus status, void * data) {
    (void) c;
    if (status == DBUS_DISPATCH_DATA_REMAINS) bt_dbus_wake_signal((int) (intptr_t) data);
}

static void wakeup_main(void * data) {
    bt_dbus_wake_signal((int) (intptr_t) data);
}

void bt_dbus_wake_attach(DBusConnection * c, int wake_fd) {
    if (wake_fd < 0) return;
    dbus_connection_set_dispatch_status_function(c, dispatch_status_changed, (void *) (intptr_t) wake_fd, NULL);
    dbus_connection_set_wakeup_main_function(c, wakeup_main, (void *) (intptr_t) wake_fd, NULL);
}

/* Dispatches every queued message, then writes out the replies the handlers queued. */
static void dispatch_all(DBusConnection * c) {
    while (dbus_connection_dispatch(c) == DBUS_DISPATCH_DATA_REMAINS) {}
    if (dbus_connection_has_messages_to_send(c)) dbus_connection_read_write(c, 0);
}

bool bt_dbus_wait_dispatch(DBusConnection * c, int wake_fd, int timeout_ms) {
    struct pollfd fds[2];
    nfds_t count = 0;
    int unix_fd;
    /* Queued messages first, connected or not: a lost connection still queues its Disconnected
     * signal, and the filters and exit-on-disconnect act on it. */
    if (dbus_connection_get_dispatch_status(c) == DBUS_DISPATCH_DATA_REMAINS) {
        dispatch_all(c);
        return dbus_connection_get_is_connected(c);
    }
    bool connected = dbus_connection_get_is_connected(c);
    if (wake_fd < 0 && (timeout_ms < 0 || timeout_ms > BT_DBUS_FALLBACK_POLL_MS))
        timeout_ms = BT_DBUS_FALLBACK_POLL_MS;
    if (connected && !dbus_connection_get_unix_fd(c, &unix_fd)) {
        dbus_connection_read_write_dispatch(c, timeout_ms < 0 ? BT_DBUS_FALLBACK_POLL_MS : timeout_ms);
        return dbus_connection_get_is_connected(c);
    }
    if (connected) {
        short events = POLLIN;
        if (dbus_connection_has_messages_to_send(c)) events |= POLLOUT;
        fds[count++] = (struct pollfd) { .fd = unix_fd, .events = events };
    }
    if (wake_fd >= 0) fds[count++] = (struct pollfd) { .fd = wake_fd, .events = POLLIN };
    if (!count) return false;
    int ready = poll(fds, count, timeout_ms);
    if (ready > 0 && wake_fd >= 0 && (fds[count - 1].revents & POLLIN)) wake_drain(wake_fd);
    if (!connected) {
        dispatch_all(c);
        return false;
    }
    /* Also after a timeout or EINTR: nothing to read is harmless, and a missed event is not. */
    dbus_connection_read_write(c, 0);
    dispatch_all(c);
    return dbus_connection_get_is_connected(c);
}
