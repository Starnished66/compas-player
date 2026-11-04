#include "bt_pairing_agent.h"
#include "bt_pairing_agent_internal.h"
#include "bt_rc_trust.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/system/logging/debug_log.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#undef DBG_LOG
#define DBG_LOG(...) DB_LOG_DBG("BT", __VA_ARGS__)

/* 32-bit atomics only (no 64-bit ones on the target). Tenths of a second on CLOCK_MONOTONIC,
 * compared by signed difference, so a stale deadline stays closed for years. */
static atomic_uint pairing_deadline;
static atomic_bool pairing_ever_opened;
/* Set by every allow_pairing_for(); bluetoothd stops being discoverable after its
 * DiscoverableTimeout, which would hide the player from a phone pairing later. */
static atomic_bool discoverable_wanted;

uint32_t bt_agent_monotonic_tenths(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 10 + (uint64_t) ts.tv_nsec / 100000000);
}

static bool deadline_passed(uint32_t deadline) {
    return (int32_t) (bt_agent_monotonic_tenths() - deadline) >= 0;
}

void bt_pairing_agent_allow_pairing_for(unsigned int ms) {
    uint32_t deadline = bt_agent_monotonic_tenths() + (ms + 99) / 100;
    uint32_t current = atomic_load(&pairing_deadline);
    while ((int32_t) (deadline - current) > 0 &&
           !atomic_compare_exchange_weak(&pairing_deadline, &current, deadline)) {}
    atomic_store(&pairing_ever_opened, true);
    atomic_store(&discoverable_wanted, true); /* the dispatch thread sends it, never this caller */
}

bool bt_agent_take_discoverable_request(void) {
    return atomic_exchange(&discoverable_wanted, false);
}

bool bt_agent_pairing_window_open(void) {
    return atomic_load(&pairing_ever_opened) && !deadline_passed(atomic_load(&pairing_deadline));
}

/* Replies Rejected and returns false when no pairing window is open. */
bool bt_agent_pairing_allowed(DBusConnection * c, DBusMessage * msg, const char * device) {
    if (bt_agent_pairing_window_open()) return true;
    DBG_LOG("bt_pairing_agent: %s %s rejected (pairing not allowed now)\n",
            dbus_message_get_member(msg), device);
    bt_agent_send_error_reply(c, msg, "org.bluez.Error.Rejected", "Pairing not allowed now");
    return false;
}

/* Pairing prompts the UI shows. The numeric-comparison request keeps its D-Bus message so the
 * reply is sent later from the dispatch thread (bt_agent_ui_service), never from the UI thread: the UI
 * only records the answer. All fields are guarded by ui_mutex. Ids are never 0. */
#define BT_AGENT_CONFIRM_TIMEOUT_TENTHS 300
#define BT_AGENT_DISPLAY_TIMEOUT_TENTHS 600
#define BT_AGENT_PAIRED_CHECK_TENTHS 10
#define BT_AGENT_NAME_MAX 64
#define BT_AGENT_PATH_MAX 128
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ui_id_seq;
static struct {
    DBusMessage * msg; /* non-NULL while a request is pending */
    char name[BT_AGENT_NAME_MAX];
    char mac[BT_RC_TRUST_MAC_SIZE]; /* empty when the device path had none */
    uint32_t passkey, id, deadline;
    bool answered, accept;
} confirm;
static struct {
    bool active;
    char path[BT_AGENT_PATH_MAX];
    char name[BT_AGENT_NAME_MAX];
    char code[17];
    uint32_t id, deadline, next_check;
} display;

/* Ids are never 0. */
static uint32_t next_ui_id(void) {
    return ++ui_id_seq ? ui_id_seq : ++ui_id_seq;
}

/* Dispatch thread. Forgets both prompts without replying (Cancel, Release, bluetoothd gone). */
void bt_agent_ui_state_clear(void) {
    pthread_mutex_lock(&ui_mutex);
    if (confirm.msg) dbus_message_unref(confirm.msg);
    confirm.msg = NULL;
    display.active = false;
    pthread_mutex_unlock(&ui_mutex);
}

/* Dispatch thread. Takes the pending request (its message) and, when allowed, remembers it. */
bool bt_agent_confirm_begin(DBusConnection * c, DBusMessage * msg, const char * device, uint32_t passkey) {
    pthread_mutex_lock(&ui_mutex);
    bool busy = confirm.msg != NULL;
    pthread_mutex_unlock(&ui_mutex);
    if (busy) return false; /* only this thread sets msg, so it stays free until below */
    char name[BT_AGENT_NAME_MAX];
    bt_agent_device_display_name(c, device, name, sizeof(name));
    pthread_mutex_lock(&ui_mutex);
    confirm.msg = dbus_message_ref(msg);
    snprintf(confirm.name, sizeof(confirm.name), "%s", name);
    if (!bt_rc_trust_mac_from_path(device, confirm.mac)) confirm.mac[0] = '\0';
    confirm.passkey = passkey;
    confirm.id = next_ui_id();
    confirm.deadline = bt_agent_monotonic_tenths() + BT_AGENT_CONFIRM_TIMEOUT_TENTHS;
    confirm.answered = confirm.accept = false;
    pthread_mutex_unlock(&ui_mutex);
    return true;
}

void bt_agent_display_begin(DBusConnection * c, const char * device, const char * code) {
    pthread_mutex_lock(&ui_mutex);
    bool same = display.active && strcmp(display.path, device) == 0 && strcmp(display.code, code) == 0;
    pthread_mutex_unlock(&ui_mutex);
    char name[BT_AGENT_NAME_MAX] = "";
    if (!same) bt_agent_device_display_name(c, device, name, sizeof(name));
    pthread_mutex_lock(&ui_mutex);
    if (!same) {
        display.active = true;
        snprintf(display.path, sizeof(display.path), "%s", device);
        snprintf(display.name, sizeof(display.name), "%s", name);
        snprintf(display.code, sizeof(display.code), "%s", code);
        display.id = next_ui_id();
        display.deadline = bt_agent_monotonic_tenths() + BT_AGENT_DISPLAY_TIMEOUT_TENTHS;
        display.next_check = bt_agent_monotonic_tenths() + BT_AGENT_PAIRED_CHECK_TENTHS;
    }
    pthread_mutex_unlock(&ui_mutex);
}

/* What ui_service found due while holding ui_mutex; acted on after releasing it. */
struct ui_due {
    DBusMessage * msg;
    bool accept;
    char accepted_mac[BT_RC_TRUST_MAC_SIZE];
    char path[BT_AGENT_PATH_MAX];
    bool check_paired;
};

/* The take_* helpers run under ui_mutex. */
static void take_confirm_due(struct ui_due * due) {
    if (confirm.msg && (confirm.answered || deadline_passed(confirm.deadline))) {
        due->msg = confirm.msg;
        due->accept = confirm.answered && confirm.accept;
        if (due->accept) {
            snprintf(due->accepted_mac, sizeof(due->accepted_mac), "%s", confirm.mac);
        }
        confirm.msg = NULL;
    }
}

static void take_display_due(struct ui_due * due) {
    if (display.active) {
        if (deadline_passed(display.deadline)) {
            display.active = false;
        } else if (deadline_passed(display.next_check)) {
            display.next_check = bt_agent_monotonic_tenths() + BT_AGENT_PAIRED_CHECK_TENTHS;
            snprintf(due->path, sizeof(due->path), "%s", display.path);
            due->check_paired = true;
        }
    }
}

static void confirm_reply(DBusConnection * c, const struct ui_due * due) {
    DBG_LOG("bt_pairing_agent: confirmation %s\n", due->accept ? "accepted" : "rejected");
    if (due->accept) {
        /* The user compared the codes on the player's own screen: that is the trust decision. It
         * is applied by the link key of this very pairing (see bt_rc_trust_arm_grant), also when
         * the device was bonded before (the phone forgot the player and pairs again). */
        if (due->accepted_mac[0]) bt_rc_trust_arm_grant(due->accepted_mac);
        bt_agent_send_empty_reply(c, due->msg);
    } else {
        bt_agent_send_error_reply(c, due->msg, "org.bluez.Error.Rejected", "Pairing rejected");
    }
    dbus_message_unref(due->msg);
}

static void display_check(DBusConnection * c, const char * path) {
    if (!bt_agent_device_is_paired(c, path)) return;
    pthread_mutex_lock(&ui_mutex);
    if (display.active && strcmp(display.path, path) == 0) display.active = false;
    pthread_mutex_unlock(&ui_mutex);
}

/* Dispatch thread, every loop turn. Sends the reply to a request the UI answered or that timed
 * out, and drops a displayed code that expired or whose device finished pairing. */
void bt_agent_ui_service(DBusConnection * c) {
    struct ui_due due;
    memset(&due, 0, sizeof(due));
    pthread_mutex_lock(&ui_mutex);
    take_confirm_due(&due);
    take_display_due(&due);
    pthread_mutex_unlock(&ui_mutex);
    if (due.msg) confirm_reply(c, &due);
    if (due.check_paired) display_check(c, due.path);
}

bool bt_pairing_agent_pending_confirmation(char * name, size_t name_size, uint32_t * passkey, uint32_t * request_id) {
    pthread_mutex_lock(&ui_mutex);
    bool pending = confirm.msg && !confirm.answered;
    if (pending) {
        if (name && name_size) snprintf(name, name_size, "%s", confirm.name);
        if (passkey) *passkey = confirm.passkey;
        if (request_id) *request_id = confirm.id;
    }
    pthread_mutex_unlock(&ui_mutex);
    return pending;
}

void bt_pairing_agent_answer_confirmation(uint32_t request_id, bool accept) {
    pthread_mutex_lock(&ui_mutex);
    if (confirm.msg && !confirm.answered && confirm.id == request_id) {
        confirm.answered = true;
        confirm.accept = accept;
    }
    pthread_mutex_unlock(&ui_mutex);
}

bool bt_pairing_agent_display_code(char * name, size_t name_size, char * code, size_t code_size, uint32_t * request_id) {
    pthread_mutex_lock(&ui_mutex);
    bool active = display.active;
    if (active) {
        if (name && name_size) snprintf(name, name_size, "%s", display.name);
        if (code && code_size) snprintf(code, code_size, "%s", display.code);
        if (request_id) *request_id = display.id;
    }
    pthread_mutex_unlock(&ui_mutex);
    return active;
}

void bt_pairing_agent_cancel_prompts(void) {
    pthread_mutex_lock(&ui_mutex);
    if (confirm.msg && !confirm.answered) {
        confirm.answered = true;
        confirm.accept = false; /* the dispatch thread replies Rejected within a tick */
    }
    display.active = false;
    pthread_mutex_unlock(&ui_mutex);
    atomic_store(&pairing_ever_opened, false); /* later requests are rejected at once, not left to time out */
}

/* A pairing callback for a device is a new bond, so any earlier trust of that address (which a
 * spoofer could hold) and any grant still waiting for it are void (the removal drops both). Dispatch thread. */
void bt_agent_revoke_trust_for(const char * device) {
    char mac[BT_RC_TRUST_MAC_SIZE];
    if (!bt_rc_trust_mac_from_path(device, mac)) return;
    bt_rc_trust_remove(mac);
}
