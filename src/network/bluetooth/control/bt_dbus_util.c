#include "bt_dbus_util.h"

#include <stdio.h>

#define BT_DBUS_CALL_TIMEOUT_MS 1000

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
