#include "bt_pairing_agent.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/system/logging/debug_log.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#undef DBG_LOG
#define DBG_LOG(...) DB_LOG_DBG("BT", __VA_ARGS__)

#define BT_AGENT_PATH "/org/compas/agent"
#define BT_AGENT_CAPABILITY "DisplayYesNo"
#define BT_AGENT_PIN "0000"
#define BT_AGENT_POLL_TIMEOUT_MS 200
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
static pthread_mutex_t agent_mutex = PTHREAD_MUTEX_INITIALIZER; /* serialises activate and yield callers */
static atomic_bool thread_alive = false;
/* Request hand-off to the dispatch thread. Lock order: agent_mutex, then request_mutex; the
 * dispatch thread takes only request_mutex. */
typedef enum { AGENT_ACTION_ACTIVATE, AGENT_ACTION_YIELD } agent_action_t;
static pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t request_done; /* CLOCK_MONOTONIC, initialised once in init_threads */
static bool request_cond_ready;
static uint64_t request_seq;      /* last request posted */
static uint64_t request_done_seq; /* last request the thread finished (or dropped on exit) */
/* Last request its caller stopped waiting for. Its remaining calls are never sent, so a
 * timed-out yield cannot take the default role back after the caller moved on. */
static uint64_t request_abandoned_seq;
static agent_action_t request_action;
static bool request_result;
/* Registered with bluetoothd (whether or not it is the default agent). Cleared by Release, by a
 * bluetoothd owner change, and by the connection dropping. */
static atomic_bool registered = false;
/* Source mode wants this agent: set by activate, cleared by yield. With it set, the dispatch
 * thread registers again after bluetoothd restarts, without waiting for an activate() call. */
static atomic_bool wanted = false;
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

static void send_empty_reply(DBusConnection * c, DBusMessage * msg) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void send_error_reply(DBusConnection * c, DBusMessage * msg, const char * error_name, const char * error_msg) {
    DBusMessage * reply = dbus_message_new_error(msg, error_name, error_msg);
    if (!reply) return;
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

static void send_basic_reply(DBusConnection * c, DBusMessage * msg, int type, const void * value) {
    DBusMessage * reply = dbus_message_new_method_return(msg);
    if (!reply) return;
    dbus_message_append_args(reply, type, value, DBUS_TYPE_INVALID);
    dbus_connection_send(c, reply, NULL);
    dbus_message_unref(reply);
}

/* Mirrors bluez-tools' bt-agent: a service is authorized only for an already paired device. The
 * blocking call runs on the dispatch thread, which is fine: libdbus queues other incoming
 * messages meanwhile and bluetoothd answers Properties.Get without needing this agent. */
static bool device_is_paired(DBusConnection * c, const char * device) {
    DBusMessage * msg = dbus_message_new_method_call("org.bluez", device, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) return false;
    const char * interface = "org.bluez.Device1";
    const char * property = "Paired";
    bool paired = false;
    if (dbus_message_append_args(msg, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property,
                                 DBUS_TYPE_INVALID)) {
        DBusError err;
        dbus_error_init(&err);
        DBusMessage * reply = dbus_connection_send_with_reply_and_block(c, msg, BT_AGENT_CALL_TIMEOUT_MS, &err);
        if (reply) {
            DBusMessageIter iter, variant;
            if (dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&iter, &variant);
                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_BOOLEAN) {
                    dbus_bool_t value = FALSE;
                    dbus_message_iter_get_basic(&variant, &value);
                    paired = value;
                }
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&err);
    }
    dbus_message_unref(msg);
    return paired;
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

/* 32-bit atomics only (no 64-bit ones on the target). Tenths of a second on CLOCK_MONOTONIC,
 * compared by signed difference, so a stale deadline stays closed for years. */
static atomic_uint pairing_deadline;
static atomic_bool pairing_ever_opened;

static uint32_t monotonic_tenths(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 10 + (uint64_t) ts.tv_nsec / 100000000);
}

void bt_pairing_agent_allow_pairing_for(unsigned int ms) {
    uint32_t deadline = monotonic_tenths() + (ms + 99) / 100;
    uint32_t current = atomic_load(&pairing_deadline);
    /* Tenths since boot: an unsigned compare stays valid for over 13 years of uptime. */
    while (deadline > current &&
           !atomic_compare_exchange_weak(&pairing_deadline, &current, deadline)) {}
    atomic_store(&pairing_ever_opened, true);
}

/* Replies Rejected and returns false when no pairing window is open. */
static bool pairing_allowed(DBusConnection * c, DBusMessage * msg, const char * device) {
    if (atomic_load(&pairing_ever_opened) && atomic_load(&pairing_deadline) > monotonic_tenths())
        return true;
    DBG_LOG("bt_pairing_agent: %s %s rejected (pairing not allowed now)\n",
            dbus_message_get_member(msg), device);
    send_error_reply(c, msg, "org.bluez.Error.Rejected", "Pairing not allowed now");
    return false;
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

    const char * device = "?";
    const char * text = NULL;
    dbus_uint32_t passkey = 0;
    dbus_uint16_t entered = 0;
    DBusError err;
    dbus_error_init(&err);

    if (strcmp(member, "Release") == 0) {
        DBG_LOG("bt_pairing_agent: Release\n");
        atomic_store(&registered, false);
        send_empty_reply(c, msg);
    } else if (strcmp(member, "Cancel") == 0) {
        DBG_LOG("bt_pairing_agent: Cancel\n");
        send_empty_reply(c, msg);
    } else if (strcmp(member, "RequestPinCode") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: RequestPinCode %s\n", device);
        const char * pin = BT_AGENT_PIN;
        if (pairing_allowed(c, msg, device)) send_basic_reply(c, msg, DBUS_TYPE_STRING, &pin);
    } else if (strcmp(member, "DisplayPinCode") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: DisplayPinCode %s %s\n", device, text ? text : "");
        send_empty_reply(c, msg);
    } else if (strcmp(member, "RequestPasskey") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: RequestPasskey %s\n", device);
        dbus_uint32_t zero = 0;
        if (pairing_allowed(c, msg, device)) send_basic_reply(c, msg, DBUS_TYPE_UINT32, &zero);
    } else if (strcmp(member, "DisplayPasskey") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
                              DBUS_TYPE_UINT16, &entered, DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: DisplayPasskey %s %06u\n", device, (unsigned) passkey);
        send_empty_reply(c, msg);
    } else if (strcmp(member, "RequestConfirmation") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_UINT32, &passkey,
                              DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: RequestConfirmation %s %06u\n", device, (unsigned) passkey);
        if (pairing_allowed(c, msg, device)) send_empty_reply(c, msg);
    } else if (strcmp(member, "RequestAuthorization") == 0) {
        dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_INVALID);
        DBG_LOG("bt_pairing_agent: RequestAuthorization %s\n", device);
        if (pairing_allowed(c, msg, device)) send_empty_reply(c, msg);
    } else if (strcmp(member, "AuthorizeService") == 0) {
        if (!dbus_message_get_args(msg, &err, DBUS_TYPE_OBJECT_PATH, &device, DBUS_TYPE_STRING, &text,
                                   DBUS_TYPE_INVALID)) {
            send_error_reply(c, msg, "org.bluez.Error.Rejected", "Bad arguments");
        } else {
            bool paired = device_is_paired(c, device);
            DBG_LOG("bt_pairing_agent: AuthorizeService %s %s, %s\n", device, text,
                    paired ? "accepted" : "rejected (not paired)");
            if (paired) send_empty_reply(c, msg);
            else send_error_reply(c, msg, "org.bluez.Error.Rejected", "Device is not paired");
        }
    } else {
        dbus_error_free(&err);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    dbus_error_free(&err);
    return DBUS_HANDLER_RESULT_HANDLED;
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
            atomic_store(&registered, false);
            /* Only a running bluetoothd can take a registration; Bluetooth off stops it. */
            atomic_store(&reregister_pending, new_owner[0] != '\0');
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool register_as_default(uint64_t seq);
static bool agent_manager_call(uint64_t seq, const char * method, bool with_capability);
#define AGENT_CALL_ALWAYS UINT64_MAX /* seq for a call that is sent whatever the request state */

/* Runs a posted activate or yield on the dispatch thread and publishes the result. The callers
 * set (or cleared) wanted before posting, and re-registration runs on this same thread, so a
 * yield can never be followed by a re-claim of the default role. */
static void service_request(void) {
    pthread_mutex_lock(&request_mutex);
    if (request_done_seq == request_seq) {
        pthread_mutex_unlock(&request_mutex);
        return;
    }
    uint64_t seq = request_seq;
    agent_action_t action = request_action;
    pthread_mutex_unlock(&request_mutex);

    bool ok = register_as_default(seq);
    if (ok && action == AGENT_ACTION_ACTIVATE) atomic_store(&reregister_pending, false);

    pthread_mutex_lock(&request_mutex);
    request_result = ok;
    request_done_seq = seq;
    /* A yield that failed, or whose caller gave up, may have left RequestDefaultAgent queued
     * for delivery after that caller started the DAC agent. Unregistering on this connection
     * lands after it, so the DAC agent keeps the default role; ours only stops being its
     * fallback. */
    bool unregister = !atomic_load(&wanted) && (!ok || request_abandoned_seq >= seq);
    pthread_cond_broadcast(&request_done);
    pthread_mutex_unlock(&request_mutex);
    if (unregister && atomic_load(&registered)) {
        DBG_LOG("bt_pairing_agent: yield gave up, unregistering\n");
        (void) agent_manager_call(AGENT_CALL_ALWAYS, "UnregisterAgent", false);
        atomic_store(&registered, false);
    }
}

static void * dispatch_thread_func(void * arg) {
    (void) arg;
    int retry_ticks = 0;
    while (dbus_connection_get_is_connected(conn)) {
        dbus_connection_read_write_dispatch(conn, BT_AGENT_POLL_TIMEOUT_MS);
        service_request();
        if (!atomic_load(&reregister_pending)) continue;
        /* Yielded, a re-claim could take the default role from the DAC agent. */
        if (!atomic_load(&wanted)) { atomic_store(&reregister_pending, false); continue; }
        /* A restarted bluetoothd may take its name before AgentManager1 is up: retry about
         * once a second. */
        if (retry_ticks-- > 0) continue;
        retry_ticks = BT_AGENT_REREGISTER_TICKS;
        if (register_as_default(0)) atomic_store(&reregister_pending, false);
    }
    DBG_LOG("bt_pairing_agent: bus connection lost\n");
    atomic_store(&registered, false);
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

/* Caller holds agent_mutex. Brings up the connection, object path, match and dispatch thread. */
static bool ensure_connection(void) {
    if (conn && atomic_load(&thread_alive)) return true;
    drop_connection(); /* a dead dispatch thread no longer uses it */
    pthread_once(&threads_once, init_threads);
    if (!request_cond_ready) return false; /* the dispatch thread signals request_done */

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

    dbus_error_init(&err);
    dbus_bus_add_match(conn, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                             "member='NameOwnerChanged',arg0='org.bluez'", &err);
    if (dbus_error_is_set(&err)) {
        DBG_LOG("bt_pairing_agent: failed to watch bluetoothd owner: %s\n", err.message);
        dbus_error_free(&err);
    }
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
 * re-registration path, wanted while source mode wants this agent. Caller holds request_mutex. */
static bool request_still_wanted(uint64_t seq) {
    if (seq == AGENT_CALL_ALWAYS) return true;
    return seq ? seq == request_seq && seq > request_abandoned_seq : atomic_load(&wanted);
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
static bool agent_request(agent_action_t action) {
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
    request_action = action;
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
    atomic_store(&wanted, true);
    /* With a live connection this is one round trip: a bt-agent started since (bt_resume spawns
     * one when none is running) may hold the default role, and bluetoothd only hands it back if
     * we are next in line when that agent is killed. */
    bool ok = agent_request(AGENT_ACTION_ACTIVATE);
    if (!ok) DBG_LOG("bt_pairing_agent: activate failed\n");
    pthread_mutex_unlock(&agent_mutex);
    return ok;
}

bool bt_pairing_agent_yield(void) {
    pthread_once(&threads_once, init_threads);
    /* wanted is cleared before the request is posted, and re-registration runs on the same
     * thread that services it: one already under way finishes first, none starts after, so it
     * cannot take the default role back from the agent that follows. */
    pthread_mutex_lock(&agent_mutex);
    atomic_store(&wanted, false);
    atomic_store(&reregister_pending, false);
    /* BlueZ only falls back to agents that asked for the default role (a queue, newest first),
     * so ours asks once more now; the DAC agent spawned next goes ahead of it. This also
     * re-registers ours if a bluetoothd restart dropped it. */
    bool ok = agent_request(AGENT_ACTION_YIELD);
    pthread_mutex_unlock(&agent_mutex);
    return ok;
}
