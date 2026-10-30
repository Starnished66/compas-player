#include "bt_adapter_query.h"
#include "../../../core/system/logging/debug_log.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>

/* Short: a healthy bluetoothd answers in milliseconds, and a slow one is handed to bluetoothctl. */
#define BT_ADAPTER_QUERY_TIMEOUT_MS 1000

static DBusConnection * connection;
static pthread_once_t threads_once = PTHREAD_ONCE_INIT;

static void init_threads(void) {
    (void) dbus_threads_init_default();
}

static void drop_connection(void) {
    if (!connection) return;
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    connection = NULL;
}

/* Walks a{oa{sa{sv}}}: Powered of the first org.bluez.Adapter1, the controller bluetoothctl
 * picks as default. Returns BT_ADAPTER_NONE when there is none, UNKNOWN if the reply is malformed. */
static int adapter_state_from_reply(DBusMessage * reply) {
    DBusMessageIter top, objects;
    if (!dbus_message_has_signature(reply, "a{oa{sa{sv}}}")) return BT_ADAPTER_UNKNOWN;
    if (!dbus_message_iter_init(reply, &top) || dbus_message_iter_get_arg_type(&top) != DBUS_TYPE_ARRAY)
        return BT_ADAPTER_UNKNOWN;
    dbus_message_iter_recurse(&top, &objects);
    for (; dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&objects)) {
        DBusMessageIter object, interfaces;
        dbus_message_iter_recurse(&objects, &object);
        if (!dbus_message_iter_next(&object) || dbus_message_iter_get_arg_type(&object) != DBUS_TYPE_ARRAY)
            return BT_ADAPTER_UNKNOWN;
        dbus_message_iter_recurse(&object, &interfaces);
        for (; dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&interfaces)) {
            DBusMessageIter interface, props;
            const char * name = NULL;
            dbus_message_iter_recurse(&interfaces, &interface);
            dbus_message_iter_get_basic(&interface, &name);
            if (!name || strcmp(name, "org.bluez.Adapter1") != 0) continue;
            if (!dbus_message_iter_next(&interface) || dbus_message_iter_get_arg_type(&interface) != DBUS_TYPE_ARRAY)
                return BT_ADAPTER_UNKNOWN;
            dbus_message_iter_recurse(&interface, &props);
            for (; dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&props)) {
                DBusMessageIter prop, variant;
                const char * key = NULL;
                dbus_message_iter_recurse(&props, &prop);
                dbus_message_iter_get_basic(&prop, &key);
                if (!key || strcmp(key, "Powered") != 0) continue;
                if (!dbus_message_iter_next(&prop) || dbus_message_iter_get_arg_type(&prop) != DBUS_TYPE_VARIANT)
                    return BT_ADAPTER_UNKNOWN;
                dbus_message_iter_recurse(&prop, &variant);
                if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_BOOLEAN) return BT_ADAPTER_UNKNOWN;
                dbus_bool_t powered = FALSE;
                dbus_message_iter_get_basic(&variant, &powered);
                return powered ? BT_ADAPTER_POWERED : BT_ADAPTER_OFF;
            }
            return BT_ADAPTER_OFF; /* an adapter without the property: bluetoothctl prints no "Powered: yes" */
        }
    }
    return BT_ADAPTER_NONE;
}

int bt_adapter_dbus_powered(void) {
    pthread_once(&threads_once, init_threads);
    DBusError err;
    dbus_error_init(&err);
    if (!connection) {
        connection = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!connection) {
            dbus_error_free(&err);
            return BT_ADAPTER_UNKNOWN;
        }
        dbus_connection_set_exit_on_disconnect(connection, FALSE);
    }

    int result = BT_ADAPTER_UNKNOWN;
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
                                                     "GetManagedObjects");
    if (msg) {
        DBusMessage * reply = dbus_connection_send_with_reply_and_block(connection, msg,
                                                                        BT_ADAPTER_QUERY_TIMEOUT_MS, &err);
        dbus_message_unref(msg);
        if (reply) {
            result = adapter_state_from_reply(reply);
            dbus_message_unref(reply);
        }
    }
    if (result == BT_ADAPTER_UNKNOWN) {
        DBG_LOG("bt_adapter_query: D-Bus managed-objects read unavailable (%s), using bluetoothctl\n",
                dbus_error_is_set(&err) ? err.name : "unexpected reply");
        if (!dbus_connection_get_is_connected(connection)) drop_connection();
    }
    dbus_error_free(&err);
    return result;
}
