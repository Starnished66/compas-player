#include "bt_media_player.h"
#include "../control/bluetooth_control.h"
#include <dbus/dbus.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#define PHONE_COMMAND_CAPACITY 32
static pthread_mutex_t phone_mutex = PTHREAD_MUTEX_INITIALIZER;
static int phone_commands[PHONE_COMMAND_CAPACITY];
static unsigned phone_head, phone_count;
static bool phone_enabled;
static unsigned phone_generation;
static char phone_target[18];

void bt_media_player_set_phone_target(const char *mac) {
    if (!mac || strlen(mac) != 17) mac = "";
    pthread_mutex_lock(&phone_mutex);
    if (strcasecmp(phone_target, mac)) {
        snprintf(phone_target, sizeof(phone_target), "%s", mac);
        phone_count = 0;
        phone_generation++;
    }
    pthread_mutex_unlock(&phone_mutex);
}

void bt_media_player_set_phone_control_enabled(bool enabled) {
    pthread_mutex_lock(&phone_mutex);
    phone_enabled = enabled;
    phone_count = 0;
    phone_generation++;
    pthread_mutex_unlock(&phone_mutex);
}

static bool phone_command_current(unsigned generation) {
    pthread_mutex_lock(&phone_mutex);
    bool current = phone_enabled && generation == phone_generation;
    pthread_mutex_unlock(&phone_mutex);
    return current;
}

void bt_media_player_control_phone(int direction) {
    pthread_mutex_lock(&phone_mutex);
    if (phone_enabled && phone_target[0] && phone_count < PHONE_COMMAND_CAPACITY) {
        phone_commands[(phone_head + phone_count) % PHONE_COMMAND_CAPACITY] = direction;
        phone_count++;
    }
    pthread_mutex_unlock(&phone_mutex);
}

static DBusMessage *phone_call(DBusConnection *bus, const char *path, const char *interface,
                               const char *method) {
    DBusMessage *call = dbus_message_new_method_call("org.bluez", path, interface, method);
    if (!call) return NULL;
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, call, 1000, NULL);
    dbus_message_unref(call);
    return reply;
}

/* Find the connected phone's selected player, rather than choosing an arbitrary player object. */
static void phone_player(DBusMessage *reply, const char *mac, char *path, size_t size) {
    char device[32] = "dev_";
    for (size_t i = 0; i < 17; i++)
        device[i + 4] = mac[i] == ':' ? '_' : (char)toupper((unsigned char)mac[i]);
    device[21] = '\0';
    DBusMessageIter root, objects;
    if (!reply || !dbus_message_iter_init(reply, &root) ||
        dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) return;
    dbus_message_iter_recurse(&root, &objects);
    while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter object, interfaces;
        dbus_message_iter_recurse(&objects, &object);
        const char *object_path = NULL;
        dbus_message_iter_get_basic(&object, &object_path);
        const char *owner = object_path ? strrchr(object_path, '/') : NULL;
        if (!owner || strcasecmp(owner + 1, device)) {
            dbus_message_iter_next(&objects);
            continue;
        }
        if (!dbus_message_iter_next(&object)) return;
        dbus_message_iter_recurse(&object, &interfaces);
        while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry, properties;
            const char *name = NULL;
            dbus_message_iter_recurse(&interfaces, &entry);
            dbus_message_iter_get_basic(&entry, &name);
            dbus_message_iter_next(&entry);
            if (name && !strcmp(name, "org.bluez.MediaControl1")) {
                bool connected = false;
                const char *player = NULL;
                dbus_message_iter_recurse(&entry, &properties);
                while (dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter property, value;
                    dbus_message_iter_recurse(&properties, &property);
                    dbus_message_iter_get_basic(&property, &name);
                    dbus_message_iter_next(&property);
                    dbus_message_iter_recurse(&property, &value);
                    int type = dbus_message_iter_get_arg_type(&value);
                    if (!strcmp(name, "Connected") && type == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t yes;
                        dbus_message_iter_get_basic(&value, &yes);
                        connected = yes;
                    }
                    if (!strcmp(name, "Player") && type == DBUS_TYPE_OBJECT_PATH)
                        dbus_message_iter_get_basic(&value, &player);
                    dbus_message_iter_next(&properties);
                }
                if (connected && player && strcmp(player, "/")) {
                    snprintf(path, size, "%s", player);
                    return;
                }
            }
            dbus_message_iter_next(&interfaces);
        }
        dbus_message_iter_next(&objects);
    }
}

static bool phone_status(DBusConnection *bus, const char *path, bool *playing) {
    const char *interface = "org.bluez.MediaPlayer1", *property = "Status";
    DBusMessage *call = dbus_message_new_method_call("org.bluez", path,
                                                   "org.freedesktop.DBus.Properties", "Get");
    if (!call) return false;
    dbus_message_append_args(call, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, call, 1000, NULL);
    dbus_message_unref(call);
    DBusMessageIter iter, value;
    const char *status = NULL;
    if (reply && dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &value);
        if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&value, &status);
    }
    bool known = status && (!strcmp(status, "playing") || !strcmp(status, "paused") || !strcmp(status, "stopped"));
    *playing = status && !strcmp(status, "playing");
    if (reply) dbus_message_unref(reply);
    return known;
}

/* One command per dispatch iteration keeps the FIFO bounded and preserves press order. */
void bt_media_player_dispatch_phone_command(void) {
    pthread_mutex_lock(&phone_mutex);
    if (!phone_count) { pthread_mutex_unlock(&phone_mutex); return; }
    unsigned generation = phone_generation;
    char target[18];
    snprintf(target, sizeof(target), "%s", phone_target);
    int direction = phone_commands[phone_head];
    phone_head = (phone_head + 1) % PHONE_COMMAND_CAPACITY;
    phone_count--;
    pthread_mutex_unlock(&phone_mutex);
    DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, NULL);
    if (!bus) return;
    dbus_connection_set_exit_on_disconnect(bus, false);
    DBusMessage *reply = phone_call(bus, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
    char path[256] = "";
    phone_player(reply, target, path, sizeof(path));
    if (reply) dbus_message_unref(reply);
    bool playing = false;
    if (path[0] && !direction && !phone_status(bus, path, &playing)) {
        bt_dac_stream_info_t info;
        bt_control_get_dac_stream_info(&info);
        playing = info.running && !strcasecmp(info.mac, target);
    }
    if (path[0] && phone_command_current(generation)) {
        const char *method = direction > 0 ? "Next" : direction < 0 ? "Previous" :
                             playing ? "Pause" : "Play";
        reply = phone_call(bus, path, "org.bluez.MediaPlayer1", method);
        if (reply) dbus_message_unref(reply);
    }
    dbus_connection_close(bus);
    dbus_connection_unref(bus);
}
