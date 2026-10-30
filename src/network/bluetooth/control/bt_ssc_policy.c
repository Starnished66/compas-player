#include "bt_ssc_policy.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bt_ssc_signal_t bt_ssc_policy_signal(bool endpoint, bool modalias, bool remembered, bool name) {
    if (endpoint) return BT_SSC_SIGNAL_ENDPOINT;
    if (modalias) return BT_SSC_SIGNAL_MODALIAS;
    if (remembered) return BT_SSC_SIGNAL_REMEMBERED;
    return name ? BT_SSC_SIGNAL_NAME : BT_SSC_SIGNAL_NONE;
}

const char * bt_ssc_policy_signal_name(bt_ssc_signal_t signal) {
    switch (signal) {
    case BT_SSC_SIGNAL_ENDPOINT: return "SSC endpoint";
    case BT_SSC_SIGNAL_MODALIAS: return "Samsung Modalias";
    case BT_SSC_SIGNAL_REMEMBERED: return "remembered";
    case BT_SSC_SIGNAL_NAME: return "name";
    default: return "none";
    }
}

static bool hex_pair_is(const char * tok, unsigned int vendor, unsigned int codec) {
    const char * colon = strchr(tok, ':');
    if (!colon) return false;
    char * end = NULL;
    unsigned long v = strtoul(tok, &end, 16);
    if (end != colon || colon == tok) return false;
    unsigned long c = strtoul(colon + 1, &end, 16);
    return *end == '\0' && colon[1] && v == vendor && c == codec;
}

bool bt_ssc_policy_endpoint_offered(const char * info) {
    const char * line = info ? strstr(info, "Available codecs:") : NULL;
    if (!line) return false;
    line += strlen("Available codecs:");
    const char * eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    char list[512];
    if (len >= sizeof(list)) len = sizeof(list) - 1;
    memcpy(list, line, len);
    list[len] = '\0';
    char * save = NULL;
    for (char * tok = strtok_r(list, " \t\r", &save); tok; tok = strtok_r(NULL, " \t\r", &save)) {
        if (strcasecmp(tok, "samsung-SC") == 0 || strcasecmp(tok, "SSC") == 0) return true;
        if (hex_pair_is(tok, BT_SSC_VENDOR_ID, BT_SSC_CODEC_ID)) return true;
    }
    return false;
}

bool bt_ssc_policy_modalias_is_samsung(const char * info) {
    const char * p = info ? strstr(info, "Modalias:") : NULL;
    if (!p) return false;
    p += strlen("Modalias:");
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "bluetooth:v", 11) != 0) return false;
    p += 11;
    char * end = NULL;
    unsigned long vendor = strtoul(p, &end, 16);
    return end && end - p == 4 && vendor == BT_SSC_VENDOR_ID;
}

bool bt_ssc_policy_modalias_excludes_name(const char * info) {
    const char * p = info ? strstr(info, "Modalias:") : NULL;
    if (!p) return false;
    p += strlen("Modalias:");
    while (*p == ' ' || *p == '\t') p++;
    bool bluetooth = strncasecmp(p, "bluetooth:v", 11) == 0;
    if (!bluetooth && strncasecmp(p, "usb:v", 5) != 0) return false;
    p += bluetooth ? 11 : 5;
    char * end = NULL;
    unsigned long vendor = strtoul(p, &end, 16);
    if (!end || end - p != 4) return false;
    return !(bluetooth ? vendor == BT_SSC_VENDOR_ID : vendor == 0x04E8ul /* Samsung's USB ID */);
}

void bt_ssc_policy_offer_selected(char * info, size_t size) {
    char * sel = info ? strstr(info, "Selected codec:") : NULL;
    char * avail = info ? strstr(info, "Available codecs:") : NULL;
    if (!sel || !avail) return; /* codecs not known yet: nothing to add to */
    sel += strlen("Selected codec:");
    while (*sel == ' ') sel++;
    char codec[48];
    size_t n = strcspn(sel, " \r\n:");
    if (n == 0 || n >= sizeof(codec)) return;
    memcpy(codec, sel, n);
    codec[n] = '\0';
    char * line = avail + strlen("Available codecs:");
    char * eol = strchr(line, '\n');
    size_t len = eol ? (size_t)(eol - line) : strlen(line);
    char list[512];
    if (len >= sizeof(list)) return;
    memcpy(list, line, len);
    list[len] = '\0';
    char * save = NULL;
    for (char * tok = strtok_r(list, " \t\r", &save); tok; tok = strtok_r(NULL, " \t\r", &save))
        if (strcasecmp(tok, codec) == 0) return;
    size_t used = strlen(info), add = n + 1;
    if (used + add + 1 > size) return;
    memmove(line + add, line, strlen(line) + 1);
    line[0] = ' ';
    memcpy(line + 1, codec, n);
}

/* The value of `string "key"` followed by `variant <type> <value>`. */
static bool dbus_prop(const char * out, const char * key, const char * type, char * value, size_t size) {
    char needle[48];
    snprintf(needle, sizeof(needle), "string \"%s\"", key);
    const char * p = strstr(out, needle);
    p = p ? strchr(p, '\n') : NULL;
    const char * v = p ? strstr(p, "variant") : NULL;
    if (!v) return false;
    v += strlen("variant");
    while (*v == ' ') v++;
    if (strncmp(v, type, strlen(type)) != 0) return false;
    v += strlen(type);
    while (*v == ' ') v++;
    bool quoted = *v == '"';
    if (quoted) v++;
    size_t i = 0;
    while (*v && *v != '\n' && *v != (quoted ? '"' : ' ') && i + 1 < size) value[i++] = *v++;
    value[i] = '\0';
    return true;
}

void bt_ssc_policy_dbus_to_info(const char * dbus_out, char * out, size_t size) {
    char v[160];
    static const char * const keys[] = { "Name", "Alias", "Modalias" };
    size_t used = 0;
    out[0] = '\0';
    if (!dbus_out || size == 0) return;
    for (size_t k = 0; k < 3; k++)
        if (dbus_prop(dbus_out, keys[k], "string", v, sizeof(v))) {
            int w = snprintf(out + used, size - used, " %s: %s\n", keys[k], v);
            used = w > 0 && used + (size_t) w < size ? used + (size_t) w : size - 1;
        }
    if (dbus_prop(dbus_out, "Connected", "boolean", v, sizeof(v)))
        snprintf(out + used, size - used, " Connected: %s\n", strcmp(v, "true") == 0 ? "yes" : "no");
}

bt_ssc_recovery_verdict_t bt_ssc_policy_recovery_verdict(const char * bt_codec, const char * cached_mac,
                                                         const char * pending_mac) {
    if (!bt_codec || (strcmp(bt_codec, "ssc") != 0 && strcmp(bt_codec, "auto") != 0))
        return BT_SSC_RECOVERY_CANCEL;
    if (cached_mac && cached_mac[0] && pending_mac && strcmp(cached_mac, pending_mac) != 0)
        return BT_SSC_RECOVERY_CANCEL;
    return BT_SSC_RECOVERY_START;
}

bt_ssc_link_t bt_ssc_policy_link_state(const char * info) {
    const char * p = info ? strstr(info, "Connected:") : NULL;
    if (!p) return BT_SSC_LINK_UNKNOWN;
    p += strlen("Connected:");
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "yes", 3) == 0) return BT_SSC_LINK_CONNECTED;
    if (strncmp(p, "no", 2) == 0) return BT_SSC_LINK_DISCONNECTED;
    return BT_SSC_LINK_UNKNOWN;
}

static uint32_t nonzero(uint32_t t) { return t ? t : 1; }

void bt_ssc_dev_switch_begin(bt_ssc_dev_t * d, uint32_t now) {
    d->switch_begin = nonzero(now);
    d->pcm_back = 0;
    d->confirmed = 0;
    d->confirmed_ssc = false;
}

void bt_ssc_dev_switch_abort(bt_ssc_dev_t * d) {
    d->switch_begin = 0;
}

void bt_ssc_dev_pcm_added(bt_ssc_dev_t * d, uint32_t now) {
    d->pcm_back = nonzero(now);
    d->pcm_added_at = nonzero(now);
}

uint32_t bt_ssc_dev_stream_open_at(const bt_ssc_dev_t * d) {
    return d->pcm_added_at ? d->pcm_added_at + BT_SSC_STREAM_OPEN_MS : 0;
}

void bt_ssc_dev_confirmed(bt_ssc_dev_t * d, uint32_t now, bool ssc) {
    if (d->confirmed) return; /* the first match starts the flowing clock */
    d->confirmed = nonzero(now);
    d->confirmed_ssc = ssc;
}

uint32_t bt_ssc_dev_reconfigure_deadline(const bt_ssc_dev_t * d, uint32_t removed_at) {
    if (!d->switch_begin || d->pcm_back) return 0;
    if ((int32_t)(removed_at - d->switch_begin) < 0) return 0; /* a removal before our switch */
    if ((int32_t)(removed_at - d->switch_begin) >= (int32_t) BT_SSC_SWITCH_CAP_MS) return 0;
    return d->switch_begin + BT_SSC_SWITCH_CAP_MS;
}

bool bt_ssc_dev_flowing(const bt_ssc_dev_t * d, uint32_t at) {
    if (!d->confirmed) return false;
    uint32_t since = d->pcm_back && (int32_t)(d->pcm_back - d->confirmed) > 0 ? d->pcm_back : d->confirmed;
    return (int32_t)(at - since) >= (int32_t) BT_SSC_FLOWING_MS;
}

bool bt_ssc_dev_blames_codec(const bt_ssc_dev_t * d, uint32_t dropped_at, bt_ssc_link_t link) {
    return link == BT_SSC_LINK_DISCONNECTED && !bt_ssc_dev_flowing(d, dropped_at);
}

void bt_ssc_dev_connection_end(bt_ssc_dev_t * d, bool same_connection) {
    bt_ssc_dev_t kept = {0};
    if (same_connection) {
        kept.conn_started = d->conn_started;
        kept.skip_ssc = d->skip_ssc;
        kept.endpoint_failure_counted = d->endpoint_failure_counted;
    }
    *d = kept;
}

static pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_ssc_memory_t memory[BT_SSC_MEMORY_MAX];
static uint32_t memory_used[BT_SSC_MEMORY_MAX]; /* last use, for eviction */
static uint32_t memory_tick;
static int memory_count;
static bool memory_dirty;

/* Caller holds memory_mutex. A full table evicts the least recently used
 * entry, preferring one SSC never worked on. */
static bt_ssc_memory_t * memory_find(const char * mac, bool create) {
    if (!mac || !mac[0]) return NULL;
    for (int i = 0; i < memory_count; i++)
        if (strcasecmp(memory[i].mac, mac) == 0) {
            memory_used[i] = ++memory_tick;
            return &memory[i];
        }
    if (!create) return NULL;
    if (memory_count == BT_SSC_MEMORY_MAX) {
        int victim = 0;
        for (int i = 1; i < memory_count; i++) {
            bool better = memory[victim].worked ? !memory[i].worked || memory_used[i] < memory_used[victim]
                                                : !memory[i].worked && memory_used[i] < memory_used[victim];
            if (better) victim = i;
        }
        memmove(&memory[victim], &memory[victim + 1], sizeof(memory[0]) * (size_t)(memory_count - victim - 1));
        memmove(&memory_used[victim], &memory_used[victim + 1],
                sizeof(memory_used[0]) * (size_t)(memory_count - victim - 1));
        memory_count--;
        memory_dirty = true;
    }
    bt_ssc_memory_t * m = &memory[memory_count];
    memory_used[memory_count++] = ++memory_tick;
    memset(m, 0, sizeof(*m));
    snprintf(m->mac, sizeof(m->mac), "%s", mac);
    return m;
}

void bt_ssc_memory_import(const bt_ssc_memory_t * entries, int count) {
    pthread_mutex_lock(&memory_mutex);
    memory_count = 0;
    for (int i = 0; entries && i < count && i < BT_SSC_MEMORY_MAX; i++) {
        memory_used[memory_count] = ++memory_tick;
        memory[memory_count++] = entries[i];
    }
    memory_dirty = false;
    pthread_mutex_unlock(&memory_mutex);
}

bool bt_ssc_memory_export_if_dirty(bt_ssc_memory_t * out, int * count) {
    pthread_mutex_lock(&memory_mutex);
    bool dirty = memory_dirty;
    if (dirty) {
        memcpy(out, memory, sizeof(memory[0]) * (size_t) memory_count);
        *count = memory_count;
        memory_dirty = false;
    }
    pthread_mutex_unlock(&memory_mutex);
    return dirty;
}

bool bt_ssc_memory_worked(const char * mac) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, false);
    bool worked = m && m->worked;
    pthread_mutex_unlock(&memory_mutex);
    return worked;
}

bool bt_ssc_memory_get(const char * mac, bt_ssc_memory_t * out) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, false);
    if (m) *out = *m;
    pthread_mutex_unlock(&memory_mutex);
    return m != NULL;
}

bool bt_ssc_memory_connection_begin(const char * mac) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, false);
    bool skip = m && m->skip > 0;
    if (skip) {
        m->skip--;
        memory_dirty = true;
    }
    pthread_mutex_unlock(&memory_mutex);
    return skip;
}

void bt_ssc_memory_note_worked(const char * mac) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, true);
    if (m && !m->worked) {
        m->worked = true;
        memory_dirty = true;
    }
    pthread_mutex_unlock(&memory_mutex);
}

void bt_ssc_memory_note_failure(const char * mac) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, true);
    if (m) {
        if (m->failures < 255) m->failures++;
        /* Repeated failures skip SSC on the next connections: 1, 2, 4, ...
         * A device SSC never worked on is backed off from the first one. */
        unsigned int after = m->worked ? BT_SSC_BACKOFF_AFTER : 1u;
        if (m->failures >= after) {
            unsigned int over = m->failures - after;
            unsigned int run = 1u << (over < 3 ? over : 3);
            m->skip = (uint8_t)(run < BT_SSC_BACKOFF_MAX_SKIP ? run : BT_SSC_BACKOFF_MAX_SKIP);
        }
        memory_dirty = true;
    }
    pthread_mutex_unlock(&memory_mutex);
}

void bt_ssc_memory_note_session_ok(const char * mac) {
    pthread_mutex_lock(&memory_mutex);
    bt_ssc_memory_t * m = memory_find(mac, true);
    if (m && (m->failures || m->skip || !m->worked)) {
        m->failures = 0;
        m->skip = 0;
        m->worked = true;
        memory_dirty = true;
    }
    pthread_mutex_unlock(&memory_mutex);
}

void bt_ssc_memory_reset(void) {
    pthread_mutex_lock(&memory_mutex);
    memory_count = 0;
    memory_dirty = false;
    pthread_mutex_unlock(&memory_mutex);
}
