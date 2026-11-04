#ifndef BT_DBUS_UTIL_H
#define BT_DBUS_UTIL_H

#include <dbus/dbus.h>
#include <stdbool.h>
#include <stddef.h>

/* The unique bus name that owns well-known name (e.g. "org.bluez") on c; out is empty and false
 * when nobody owns it or the bus does not answer within a second. Blocks, so never call it on a
 * connection another thread is already reading. */
bool bt_dbus_get_name_owner(DBusConnection * c, const char * name, char * out, size_t size);

#endif
