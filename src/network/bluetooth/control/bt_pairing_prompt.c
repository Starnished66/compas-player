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
    char path[BT_AGENT_PATH_MAX];
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

/* A trust grant waits for the device to finish pairing: tapping Pair proves nothing if the
 * other side then cancels. Guarded by ui_mutex. Checked every dispatch tick: the peer connects
 * within milliseconds of bonding (see bt_pairing_agent_grant_pending). */
#define BT_AGENT_TRUST_WAIT_TENTHS 150
static struct {
    bool active;
    char path[BT_AGENT_PATH_MAX];
    char mac[BT_RC_TRUST_MAC_SIZE];
    unsigned int epoch; /* bt_rc_trust_epoch() when armed */
    uint32_t deadline;
} grant;

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
    grant.active = false;
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
    snprintf(confirm.path, sizeof(confirm.path), "%s", device);
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
    char accepted_path[BT_AGENT_PATH_MAX];
    char path[BT_AGENT_PATH_MAX];
    char grant_path[BT_AGENT_PATH_MAX];
    bool check_paired, check_grant;
};

/* The take_* helpers run under ui_mutex. */
static void take_confirm_due(struct ui_due * due) {
    if (confirm.msg && (confirm.answered || deadline_passed(confirm.deadline))) {
        due->msg = confirm.msg;
        due->accept = confirm.answered && confirm.accept;
        if (due->accept) {
            snprintf(due->accepted_mac, sizeof(due->accepted_mac), "%s", confirm.mac);
            snprintf(due->accepted_path, sizeof(due->accepted_path), "%s", confirm.path);
        }
        confirm.msg = NULL;
    }
}

static void take_grant_due(struct ui_due * due) {
    if (grant.active) {
        if (deadline_passed(grant.deadline)) {
            grant.active = false;
        } else {
            snprintf(due->grant_path, sizeof(due->grant_path), "%s", grant.path);
            due->check_grant = true;
        }
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
        /* The user compared the codes on the player's own screen: that is the trust decision,
         * applied once the device reports Paired (see grant). Already Paired now means this is
         * not a fresh bond (a re-pair, which then cancelled, would pass the wait), so it does
         * not arm: the PIN is needed. */
        if (due->accepted_mac[0] && !bt_agent_device_is_paired(c, due->accepted_path)) {
            pthread_mutex_lock(&ui_mutex);
            grant.epoch = bt_rc_trust_epoch();
            grant.active = true;
            snprintf(grant.path, sizeof(grant.path), "%s", due->accepted_path);
            snprintf(grant.mac, sizeof(grant.mac), "%s", due->accepted_mac);
            grant.deadline = bt_agent_monotonic_tenths() + BT_AGENT_TRUST_WAIT_TENTHS;
            pthread_mutex_unlock(&ui_mutex);
        }
        bt_agent_send_empty_reply(c, due->msg);
    } else {
        bt_agent_send_error_reply(c, due->msg, "org.bluez.Error.Rejected", "Pairing rejected");
    }
    dbus_message_unref(due->msg);
}

/* Trust is added before the grant is cleared, so a request waiting on bt_pairing_agent_grant_pending
 * sees the device trusted once the grant is gone. Only an authenticated (numeric comparison) key
 * counts: a Just Works bond made by a spoofer inside the window must not inherit the tap. */
static void grant_check(DBusConnection * c, const char * grant_path) {
    if (!bt_agent_device_is_paired(c, grant_path)) return;
    char mac[BT_RC_TRUST_MAC_SIZE] = "";
    unsigned int epoch = 0;
    pthread_mutex_lock(&ui_mutex);
    if (grant.active && strcmp(grant.path, grant_path) == 0) {
        snprintf(mac, sizeof(mac), "%s", grant.mac);
        epoch = grant.epoch;
    }
    pthread_mutex_unlock(&ui_mutex);
    if (!mac[0]) return;
    bt_rc_trust_add_at_epoch(mac, epoch, true); /* not after a PIN change */
    pthread_mutex_lock(&ui_mutex);
    if (grant.active && strcmp(grant.mac, mac) == 0) grant.active = false;
    pthread_mutex_unlock(&ui_mutex);
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
    take_grant_due(&due);
    take_display_due(&due);
    pthread_mutex_unlock(&ui_mutex);
    if (due.msg) confirm_reply(c, &due);
    if (due.check_grant) grant_check(c, due.grant_path);
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

bool bt_pairing_agent_grant_pending(const char * mac) {
    pthread_mutex_lock(&ui_mutex);
    bool pending = grant.active && strcmp(grant.mac, mac) == 0;
    pthread_mutex_unlock(&ui_mutex);
    return pending;
}

/* A pairing callback for a device is a new bond, so any earlier trust of that address (which a
 * spoofer could hold) and any grant still waiting for it are void. Dispatch thread. */
void bt_agent_revoke_trust_for(const char * device) {
    char mac[BT_RC_TRUST_MAC_SIZE];
    if (!bt_rc_trust_mac_from_path(device, mac)) return;
    bt_rc_trust_remove(mac);
    pthread_mutex_lock(&ui_mutex);
    if (grant.active && strcmp(grant.mac, mac) == 0) grant.active = false;
    pthread_mutex_unlock(&ui_mutex);
}
