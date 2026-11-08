#include "bt_pairing_agent.h"
#include "bt_pairing_agent_internal.h"
#include "bt_rc_trust.h"
#include "bt_dbus_util.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/system/logging/debug_log.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef DBG_LOG
#define DBG_LOG(...) DB_LOG_DBG("BT", __VA_ARGS__)

#define BT_AGENT_PATH "/org/compas/agent"
#define BT_AGENT_CAPABILITY "DisplayYesNo"
#define BT_AGENT_PIN "0000"
#define BT_AGENT_POLL_TIMEOUT_MS 200
#define BT_AGENT_DISCOVERABLE_REASSERT_TENTHS 600
/* Short: a healthy bluetoothd answers in milliseconds, and callers sit on connect paths. */
#define BT_AGENT_CALL_TIMEOUT_MS 1000
#define BT_AGENT_CONNECT_ATTEMPTS 3
#define BT_AGENT_CONNECT_DELAY_US 200000
#define BT_AGENT_REREGISTER_TICKS (1000 / BT_AGENT_POLL_TIMEOUT_MS)
/* Upper bound a caller waits for the dispatch thread to finish a request. A healthy bluetoothd
 * answers in milliseconds; this only bounds a wedged bus so callers holding locks never hang. */
#define BT_AGENT_REQUEST_WAIT_MS 3000

/* A private connection (not bt_media_player's shared one) so this agent's blocking calls and
 * its own dispatch thread stay independent of that service's loop and lifetime. Once the
 * dispatch thread exists it is the only user of conn for I/O: a second thread blocking on a reply
 * would wait forever for the I/O path the dispatch loop keeps taking. Other threads post a
 * request to it (see agent_request) and wait for the result. */
static DBusConnection * conn;
static pthread_t dispatch_thread;
static pthread_once_t threads_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t agent_mutex = PTHREAD_MUTEX_INITIALIZER; /* serialises activate callers */
static atomic_bool thread_alive = false;
/* Request hand-off to the dispatch thread. Lock order: agent_mutex, then request_mutex; the
 * dispatch thread takes only request_mutex. */
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_done; /* CLOCK_MONOTONIC, initialised once in init_threads */
static bool request_cond_ready;
static uint64_t request_seq;      /* last request posted */
static uint64_t request_done_seq; /* last request the thread finished (or dropped on exit) */
/* Last request its caller stopped waiting for; its remaining calls are never sent. */
static uint64_t request_abandoned_seq;
static bool request_result;
/* Registered with bluetoothd (whether or not it is the default agent). Cleared by Release, by a
 * bluetoothd owner change, and by the connection dropping. */
static atomic_bool registered = false;
static atomic_bool reregister_pending = false;

static void init_threads(void) {
    (void) dbus_threads_init_default();
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) == 0) {
        request_cond_ready = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                             pthread_cond_init(&request_done, &attr) == 0;
        pthread_condattr_destroy(&attr);
    }
    if (!request_cond_ready) DBG_LOG("bt_pairing_agent: monotonic condition variable unavailable\n");
}

void bt_agent_send_empty_reply(DBusConnection * c, DBusMessage * msg) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

void bt_agent_send_error_reply(DBusConnection * c, DBusMessage * msg, const char * error_name, const char * error_msg) {
    DBusMessage * reply = dbus_message_new_error(msg, error_name, error_msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

void bt_agent_send_basic_reply(DBusConnection * c, DBusMessage * msg, int type, const void * value) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_message_append_args(reply, type, value, DBUS_TYPE_INVALID);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

/* Properties.Get on a Device1 property. The blocking call runs on the dispatch thread, which
 * is fine: libdbus queues other incoming messages meanwhile and bluetoothd answers it without
 * needing this agent. Returns the reply with *variant opened on the value, or NULL. */
static DBusMessage * device_property(DBusConnection * c, const char * device, const char * property,
                                     DBusMessageIter * variant) {
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", device, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return NULL;
    const char * interface = "org.bluez.Device1";
    DBusMessage * reply = NULL;
    if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
                                 DBUS_TYPE_INVALID)) {
        DBusError err;
        dbus_error_init(&err);
        reply = dbus_connection_send_with_reply_and_block(c, msg, BT_AGENT_CALL_TIMEOUT_MS, &err);
        dbus_error_free(&err);
    }
    dbus_message_unref(msg);
    DBusMessageIter iter;
    if (reply && dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, variant);
        return reply;
    }
    if (reply) dbus_message_unref(reply);
    return NULL;
}

/* Mirrors bluez-tools' bt-agent: a service is authorized only for an already paired device. */
bool bt_agent_device_is_paired(DBusConnection * c, const char * device) {
    DBusMessageIter variant;
    DBusMessage * reply = device_property(c, device, "Paired", &variant);
    if (!reply) return false;
    dbus_bool_t value = FALSE;
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) dbus_message_iter_get_basic(&variant, &value);
    dbus_message_unref(reply);
    return value;
}

/* The device's Alias; its address (from the object path .../dev_AA_BB_...) when that is unavailable. */
void bt_agent_device_display_name(DBusConnection * c, const char * device, char * out, size_t size) {
    DBusMessageIter variant;
    DBusMessage * reply = device_property(c, device, "Alias", &variant);
    out[0] = '\0';
    if (reply) {
        const char * alias = NULL;
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&variant, &alias);
        if (alias) snprintf(out, size, "%s", alias);
        dbus_message_unref(reply);
    }
    if (out[0]) return;
    char mac[BT_RC_TRUST_MAC_SIZE];
    snprintf(out, size, "%s", bt_rc_trust_mac_from_path(device, mac) ? mac : device);
}

static DBusHandlerResult handle_introspect(DBusConnection * c, DBusMessage * msg) {
    static const char xml[] =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node>\n"
        " <interface name=\"org.bluez.Agent1\">\n"
        "  <method name=\"Release\"/>\n"
        "  <method name=\"RequestPinCode\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"out\"/></method>\n"
        "  <method name=\"DisplayPinCode\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>\n"
        "  <method name=\"RequestPasskey\"><arg type=\"o\" direction=\"in\"/><arg type=\"u\" direction=\"out\"/></method>\n"
        "  <method name=\"DisplayPasskey\"><arg type=\"o\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/><arg type=\"q\" direction=\"in\"/></method>\n"
        "  <method name=\"RequestConfirmation\"><arg type=\"o\" direction=\"in\"/><arg type=\"u\" direction=\"in\"/></method>\n"
        "  <method name=\"RequestAuthorization\"><arg type=\"o\" direction=\"in\"/></method>\n"
        "  <method name=\"AuthorizeService\"><arg type=\"o\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/></method>\n"
        "  <method name=\"Cancel\"/>\n"
        " </interface>\n"
        "</node>\n";
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_HANDLED;
    const char * p = xml;
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Unique name of the current org.bluez owner, empty when bluetoothd is not running. Seeded when the
 * connection comes up and kept by name_owner_filter, so it is only used on the dispatch thread
 * (and before that thread starts). */
static char bluez_owner[64];

/* Agent1 calls only come from bluetoothd; any other bus client is refused. */
static bool from_bluetoothd(DBusMessage * msg) {
    const char * sender = dbus_message_get_sender(msg);
    return sender && bluez_owner[0] && strcmp(sender, bluez_owner) == 0;
}

static void handle_release(DBusConnection * c, DBusMessage * msg) {
    DBG_LOG("bt_pairing_agent: Release\n");
    atomic_store(&registered, false);
    bt_agent_ui_state_clear();
    bt_agent_send_empty_reply(c, msg);
}

static void handle_cancel(DBusConnection * c, DBusMessage * msg) {
    DBG_LOG("bt_pairing_agent: Cancel\n");
    bt_agent_ui_state_clear();
    bt_agent_send_empty_reply(c, msg);
}

static void handle_request_pin_code(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    DBG_LOG("bt_pairing_agent: RequestPinCode %s\n", device);
    const char * pin = BT_AGENT_PIN;
    if (bt_agent_pairing_allowed(c, msg, device)) bt_agent_send_basic_reply(c, msg, DBUS_TYPE_STRING, &pin);
}

static void handle_display_pin_code(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    const char * text = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING, &text,
                               DBUS_TYPE_INVALID)) {
        bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Bad arguments");
    } else {
        DBG_LOG("bt_pairing_agent: DisplayPinCode %s %s\n", device, text);
        if (bt_agent_pairing_window_open()) bt_agent_display_begin(c, device, text);
        bt_agent_send_empty_reply(c, msg);
    }
    dbus_error_free(&err);
}

static void handle_request_passkey(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    DBG_LOG("bt_pairing_agent: RequestPasskey %s\n", device);
    dbus_uint32_t zero = 0;
    if (bt_agent_pairing_allowed(c, msg, device)) bt_agent_send_basic_reply(c, msg, DBUS_TYPE_UINT32, &zero);
}

static void handle_display_passkey(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    dbus_uint32_t passkey = 0;
    dbus_uint16_t entered = 0;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
                               DBUS_TYPE_UINT16, &entered, DBUS_TYPE_INVALID)) {
        bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Bad arguments");
    } else {
        DBG_LOG("bt_pairing_agent: DisplayPasskey %s %06u\n", device, (unsigned) passkey);
        char code[17];
        snprintf(code, sizeof(code), "%06u", (unsigned) passkey);
        if (bt_agent_pairing_window_open()) bt_agent_display_begin(c, device, code);
        bt_agent_send_empty_reply(c, msg);
    }
    dbus_error_free(&err);
}

static void handle_request_confirmation(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    dbus_uint32_t passkey = 0;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
                               DBUS_TYPE_INVALID)) {
        bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Bad arguments");
    } else {
        DBG_LOG("bt_pairing_agent: RequestConfirmation %s %06u\n", device, (unsigned) passkey);
        /* No reply here: the UI shows the code and ui_service() replies once it is answered. */
        if (bt_agent_pairing_allowed(c, msg, device) && !bt_agent_confirm_begin(c, msg, device, passkey))
            bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Another pairing is in progress");
    }
    dbus_error_free(&err);
}

static void handle_request_authorization(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    DBG_LOG("bt_pairing_agent: RequestAuthorization %s\n", device);
    if (bt_agent_pairing_allowed(c, msg, device)) bt_agent_send_empty_reply(c, msg);
}

static void handle_authorize_service(DBusConnection * c, DBusMessage * msg) {
    const char * device = "?";
    const char * text = NULL;
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING, &text,
                               DBUS_TYPE_INVALID)) {
        bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Bad arguments");
    } else {
        bool paired = bt_agent_device_is_paired(c, device);
        DBG_LOG("bt_pairing_agent: AuthorizeService %s %s, %s\n", device, text,
                paired ? "accepted" : "rejected (not paired)");
        if (paired) bt_agent_send_empty_reply(c, msg);
        else bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Device is not paired");
    }
    dbus_error_free(&err);
}

/* A pairing callback's device path is the first argument; it voids earlier trust (see revoke). */
static const struct {
    const char * member;
    void (*handler)(DBusConnection *, DBusMessage *);
    bool pairing_callback;
} agent_methods[] = {
    { "Release", handle_release, false },
    { "Cancel", handle_cancel, false },
    { "RequestPinCode", handle_request_pin_code, true },
    { "DisplayPinCode", handle_display_pin_code, true },
    { "RequestPasskey", handle_request_passkey, true },
    { "DisplayPasskey", handle_display_passkey, true },
    { "RequestConfirmation", handle_request_confirmation, true },
    { "RequestAuthorization", handle_request_authorization, true },
    { "AuthorizeService", handle_authorize_service, false },
};

static void revoke_for_pairing_callback(DBusMessage * msg) {
    /* The device path is the first argument of all of these. */
    DBusMessageIter it;
    const char * path = NULL;
    if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH)
        dbus_message_iter_get_basic(&it, &path);
    if (path) bt_agent_revoke_trust_for(path);
}

static DBusHandlerResult agent_message_handler(DBusConnection * c, DBusMessage * msg, void * user_data) {
    (void) user_data;
    const char * interface = dbus_message_get_interface(msg);
    const char * member = dbus_message_get_member(msg);
    if (!interface || !member || dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(interface, "org.freedesktop.DBus.Introspectable") == 0 && strcmp(member, "Introspect") == 0)
        return handle_introspect(c, msg);
    if (strcmp(interface, "org.bluez.Agent1") != 0) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!from_bluetoothd(msg)) {
        bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Agent requests must come from bluetoothd");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    for (size_t i = 0; i < sizeof(agent_methods) / sizeof(agent_methods[0]); i++) {
        if (strcmp(member, agent_methods[i].member) != 0) continue;
        if (agent_methods[i].pairing_callback) revoke_for_pairing_callback(msg);
        agent_methods[i].handler(c, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable agent_vtable = {
    NULL, agent_message_handler, NULL, NULL, NULL, NULL
};

/* bluetoothd restarting drops our registration with it; the next activate() registers again. */
static DBusHandlerResult name_owner_filter(DBusConnection * c, DBusMessage * msg, void * data) {
    (void) c;
    (void) data;
    if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
        const char * name = NULL, * old_owner = NULL, * new_owner = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
                                  DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
            strcmp(name, "org.bluez") == 0) {
            DBG_LOG("bt_pairing_agent: bluetoothd %s\n", new_owner[0] ? "started" : "stopped");
            snprintf(bluez_owner, sizeof(bluez_owner), "%s", new_owner);
            atomic_store(&registered, false);
            bt_agent_ui_state_clear();
            /* Only a running bluetoothd can take a registration; Bluetooth off stops it. */
            atomic_store(&reregister_pending, new_owner[0] != '\0');
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool register_as_default(uint64_t seq);
static bool agent_manager_call(uint64_t seq, const char * method, bool with_capability);

/* Runs activation on the dispatch thread and publishes its result. */
static void service_request(void) {
    pthread_mutex_lock(&request_mutex);
    if (request_done_seq == request_seq) {
        pthread_mutex_unlock(&request_mutex);
        return;
    }
    uint64_t seq = request_seq;
    pthread_mutex_unlock(&request_mutex);

    bool ok = register_as_default(seq);
    if (ok) atomic_store(&reregister_pending, false);

    pthread_mutex_lock(&request_mutex);
    request_result = ok;
    request_done_seq = seq;
    pthread_cond_broadcast(&request_done);
    pthread_mutex_unlock(&request_mutex);
}

/* Makes the adapter discoverable again while pairing is being allowed, at most once a minute.
 * Sent without waiting for the reply; a failure (adapter off) is ignored. Never turned off here:
 * it relies on bluetoothd's DiscoverableTimeout (180 s in /etc/bluetooth/main.conf). */
static void discoverable_service(void) {
    static uint32_t last_sent;
    static bool sent_once;
    if (!bt_agent_take_discoverable_request()) return;
    uint32_t now = bt_agent_monotonic_tenths();
    if (sent_once && now - last_sent < BT_AGENT_DISCOVERABLE_REASSERT_TENTHS) return;
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/org/bluez/hci0",
                                                     "org.freedesktop.DBus.Properties", "Set");
    if (!msg) return;
    const char * interface = "org.bluez.Adapter1";
    const char * property = "Discoverable";
    dbus_bool_t on = TRUE;
    DBusMessageIter iter, variant;
    dbus_message_iter_init_append(msg, &iter);
    if (dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface) &&
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &property) &&
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant) &&
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &on) &&
        dbus_message_iter_close_container(&iter, &variant)) {
        dbus_message_set_no_reply(msg, TRUE);
        if (dbus_connection_send(conn, msg, NULL)) dbus_connection_flush(conn);
        last_sent = now;
        sent_once = true;
    }
    dbus_message_unref(msg);
}

static void * dispatch_thread_func(void * arg) {
    (void) arg;
    int retry_ticks = 0;
    while (dbus_connection_get_is_connected(conn)) {
        dbus_connection_read_write_dispatch(conn, BT_AGENT_POLL_TIMEOUT_MS);
        service_request();
        discoverable_service();
        bt_agent_ui_service(conn);
        if (!atomic_load(&reregister_pending)) continue;
        /* A restarted bluetoothd may take its name before AgentManager1 is up: retry about
         * once a second. */
        if (retry_ticks-- > 0) continue;
        retry_ticks = BT_AGENT_REREGISTER_TICKS;
        if (register_as_default(0)) atomic_store(&reregister_pending, false);
    }
    DBG_LOG("bt_pairing_agent: bus connection lost\n");
    atomic_store(&registered, false);
    bt_agent_ui_state_clear();
    /* Wake waiters with failure and drop any request still pending. */
    pthread_mutex_lock(&request_mutex);
    request_result = false;
    request_done_seq = request_seq;
    atomic_store(&thread_alive, false); /* last touch of conn: activate() may now drop it */
    pthread_cond_broadcast(&request_done);
    pthread_mutex_unlock(&request_mutex);
    return NULL;
}

static void drop_connection(void) {
    if (!conn) return;
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    conn = NULL;
}

static bool connect_bus(void) {
    DBusError err;
    dbus_error_init(&err);
    for (int attempt = 0; attempt < BT_AGENT_CONNECT_ATTEMPTS && !conn; attempt++) {
        conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!conn) {
            DBG_LOG("bt_pairing_agent: dbus_bus_get_private failed: %s\n", err.message ? err.message : "(no message)");
            dbus_error_free(&err);
            if (attempt + 1 < BT_AGENT_CONNECT_ATTEMPTS) usleep(BT_AGENT_CONNECT_DELAY_US);
        }
    }
    if (!conn) return false;
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return true;
}

/* Caller holds agent_mutex. Brings up the connection, object path, match and dispatch thread. */
static bool ensure_connection(void) {
    if (conn && atomic_load(&thread_alive)) return true;
    drop_connection(); /* a dead dispatch thread no longer uses it */
    pthread_once(&threads_once, init_threads);
    if (!request_cond_ready) return false; /* the dispatch thread signals request_done */

    if (!connect_bus()) return false;

    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                             "member='NameOwnerChanged',arg0='org.bluez'", &err);
    if (dbus_error_is_set(&err)) {
        DBG_LOG("bt_pairing_agent: failed to watch bluetoothd owner: %s\n", err.message);
        dbus_error_free(&err);
    }
    /* After the match: a change in between is queued for the filter. */
    (void) bt_dbus_get_name_owner(conn, "org.bluez", bluez_owner, sizeof(bluez_owner));
    if (!dbus_connection_add_filter(conn, name_owner_filter, NULL, NULL) ||
        !dbus_connection_register_object_path(conn, BT_AGENT_PATH, &agent_vtable, NULL)) {
        DBG_LOG("bt_pairing_agent: failed to register object path\n");
        drop_connection();
        return false;
    }
    atomic_store(&thread_alive, true);
    if (pthread_create(&dispatch_thread, NULL, dispatch_thread_func, NULL) != 0) {
        atomic_store(&thread_alive, false);
        drop_connection();
        return false;
    }
    pthread_detach(dispatch_thread);
    return true;
}

/* Request seq is still wanted: the newest one and not given up by its caller. Seq 0 is the
 * re-registration path after bluetoothd restarts. Caller holds request_mutex. */
static bool request_still_wanted(uint64_t seq) {
    return seq == 0 || (seq == request_seq && seq > request_abandoned_seq);
}

/* Dispatch thread only. Calls an org.bluez.AgentManager1 method taking the agent path (plus the
 * capability for RegisterAgent). The send is queued under request_mutex after checking that the
 * request is still wanted, so it is either sent before its caller gives up or not at all; the
 * reply is awaited outside the lock. True on success or, for RegisterAgent,
 * org.bluez.Error.AlreadyExists. */
static bool agent_manager_call(uint64_t seq, const char * method, bool with_capability) {
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", "/org/bluez", "org.bluez.AgentManager1", method);
    if (!msg) return false;
    const char * path = BT_AGENT_PATH;
    const char * capability = BT_AGENT_CAPABILITY;
    bool ok = with_capability
        ? dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &capability, DBUS_TYPE_INVALID)
        : dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
    DBusPendingCall * pending = NULL;
    if (ok) {
        pthread_mutex_lock(&request_mutex);
        ok = request_still_wanted(seq) &&
             dbus_connection_send_with_reply(conn, msg, &pending, BT_AGENT_CALL_TIMEOUT_MS) && pending;
        pthread_mutex_unlock(&request_mutex);
    }
    dbus_message_unref(msg);
    if (!ok) return false;
    dbus_pending_call_block(pending);
    DBusMessage * reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);
    if (!reply) return false;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char * name = dbus_message_get_error_name(reply);
        ok = with_capability && name && strcmp(name, "org.bluez.Error.AlreadyExists") == 0;
        if (!ok) DBG_LOG("bt_pairing_agent: %s failed: %s\n", method, name ? name : "(no reply)");
    }
    dbus_message_unref(reply);
    return ok;
}

/* Dispatch thread only. Registers if needed, then takes the default role. A
 * RequestDefaultAgent failure may mean bluetoothd forgot us without the owner change being
 * seen, so register (AlreadyExists counts as success) and try once more. */
static bool register_as_default(uint64_t seq) {
    if (!atomic_load(&registered)) atomic_store(&registered, agent_manager_call(seq, "RegisterAgent", true));
    if (!atomic_load(&registered)) return false;
    if (agent_manager_call(seq, "RequestDefaultAgent", false)) return true;
    return agent_manager_call(seq, "RegisterAgent", true) && agent_manager_call(seq, "RequestDefaultAgent", false);
}

/* Caller holds agent_mutex. Has the dispatch thread run register_as_default() and waits for the
 * result, bounded. The wait only ends early if the thread exits (bus lost), reported as failure. */
static bool agent_request(void) {
    if (!ensure_connection()) return false;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += BT_AGENT_REQUEST_WAIT_MS / 1000;
    deadline.tv_nsec += (long) (BT_AGENT_REQUEST_WAIT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    if (!request_cond_ready) return false;
    pthread_mutex_lock(&request_mutex);
    if (!atomic_load(&thread_alive)) {
        pthread_mutex_unlock(&request_mutex);
        return false;
    }
    uint64_t seq = ++request_seq;
    bool timed_out = false;
    while (request_done_seq != seq && atomic_load(&thread_alive) && !timed_out)
        timed_out = pthread_cond_timedwait(&request_done, &request_mutex, &deadline) == ETIMEDOUT;
    bool ok = request_done_seq == seq && request_result;
    if (request_done_seq != seq) request_abandoned_seq = seq; /* its unsent calls are dropped */
    pthread_mutex_unlock(&request_mutex);
    if (timed_out && !ok) DBG_LOG("bt_pairing_agent: request timed out\n");
    return ok;
}

bool bt_pairing_agent_activate(void) {
    pthread_once(&threads_once, init_threads);
    pthread_mutex_lock(&agent_mutex);
    /* With a live connection this is one round trip: a bt-agent started since (bt_resume spawns
     * one when none is running) may hold the default role, and bluetoothd only hands it back if
     * we are next in line when that agent is killed. */
    bool ok = agent_request();
    if (!ok) DBG_LOG("bt_pairing_agent: activate failed\n");
    pthread_mutex_unlock(&agent_mutex);
    return ok;
}
