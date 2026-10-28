#ifndef BT_ADAPTER_QUERY_H
#define BT_ADAPTER_QUERY_H

enum {
    BT_ADAPTER_UNKNOWN = -1, /* no definite answer: ask bluetoothctl */
    BT_ADAPTER_OFF = 0,
    BT_ADAPTER_POWERED = 1,
    BT_ADAPTER_NONE = 2      /* bluetoothd answered but lists no adapter ("No default controller available") */
};

#ifdef HOST_BUILD
static inline int bt_adapter_dbus_powered(void) { return BT_ADAPTER_UNKNOWN; }
#else
/* The state `bluetoothctl show` would report, obtained like it does (ObjectManager
 * GetManagedObjects on org.bluez, Powered of the first Adapter1) but without a process, on a
 * private connection that stays open between polls. BT_ADAPTER_UNKNOWN on any bus error or
 * timeout, including a bluetoothd that does not answer the managed-objects request. Caller
 * serialises calls. */
int bt_adapter_dbus_powered(void);
#endif

#endif
