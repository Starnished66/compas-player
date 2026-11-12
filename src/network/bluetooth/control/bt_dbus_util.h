#ifndef BT_DBUS_UTIL_H
#define BT_DBUS_UTIL_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>

/* The unique bus name that owns well-known name (e.g. "org.bluez") on c; out is empty and false
 * when nobody owns it or the bus does not answer within a second. Blocks, so never call it on a
 * connection another thread is already reading. */
bool bt_dbus_get_name_owner(DBusConnection * c, const char * name, char * out, size_t size);

/* Wake fd: a nonblocking, close-on-exec eventfd one thread sleeps on (bt_dbus_wait_dispatch) and
 * any other thread signals when it hands that thread work. -1 on failure; every function below
 * accepts -1 and then does nothing (a wait with no wake fd re-checks every 200 ms). */
int bt_dbus_wake_create(void);
/* Async-signal-safe; an already pending signal is fine. */
void bt_dbus_wake_signal(int wake_fd);
/* Makes libdbus signal wake_fd when another thread leaves a message in c's queues: received
 * during a blocking call, or queued to send. Only needed on a connection other threads use. */
void bt_dbus_wake_attach(DBusConnection * c, int wake_fd);

/* Sleeps until c has traffic, wake_fd is signalled or timeout_ms passes (-1 = no timeout), then
 * does the pending I/O and dispatches every queued message. Messages libdbus already holds are
 * dispatched without sleeping. Returns whether c is still connected; on a lost connection it only
 * waits on wake_fd. Must be the only reader of c apart from blocking calls. */
bool bt_dbus_wait_dispatch(DBusConnection * c, int wake_fd, int timeout_ms);

#endif
