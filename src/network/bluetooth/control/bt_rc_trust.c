#include "bt_rc_trust.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bt_link_key_watch.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#ifndef BT_RC_TRUST_STORAGE_DIR
#define BT_RC_TRUST_STORAGE_DIR "/usr/data/bluetooth"
#endif

#define LINK_KEY_HEX 32
#define GRANT_WAIT_S 15

static pthread_mutex_t trust_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_rc_trust_entry_t trusted[BT_RC_TRUST_MAX]; /* oldest first */
static int trusted_count;
static bool trusted_dirty;
static unsigned int trusted_epoch;
/* The Pair tap the user made, waiting for the key of that very pairing (see arm_grant). */
static struct {
    bool active;
    char mac[BT_RC_TRUST_MAC_SIZE];
    unsigned int epoch;
    struct timespec deadline;
} grant;

bool bt_rc_trust_normalize(const char * mac, char out[BT_RC_TRUST_MAC_SIZE]) {
    if (!mac || strlen(mac) != 17) return false;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? mac[i] != ':' : !isxdigit((unsigned char) mac[i])) return false;
        out[i] = (char) toupper((unsigned char) mac[i]);
    }
    out[17] = '\0';
    return true;
}

bool bt_rc_trust_mac_from_path(const char * path, char out[BT_RC_TRUST_MAC_SIZE]) {
    const char * dev = path ? strrchr(path, '/') : NULL;
    if (!dev || strncmp(dev, "/dev_", 5) != 0 || strlen(dev + 5) != 17) return false;
    char mac[BT_RC_TRUST_MAC_SIZE];
    for (int i = 0; i < 17; i++) mac[i] = i % 3 == 2 ? (dev[5 + i] == '_' ? ':' : '?') : dev[5 + i];
    mac[17] = '\0';
    return bt_rc_trust_normalize(mac, out);
}

/* Fingerprint of the 32 hex digits at hex (up to the line end); false when they are not a key. */
static bool key_fingerprint(const char * hex, char out[BT_RC_TRUST_FP_SIZE]) {
    size_t n = strcspn(hex, "\r\n");
    bool ok = n == LINK_KEY_HEX;
    for (size_t i = 0; ok && i < n; i++) ok = isxdigit((unsigned char) hex[i]) != 0;
    unsigned char digest[32];
    if (ok) ok = mbedtls_sha256((const unsigned char *) hex, n, digest, 0) == 0;
    if (ok)
        for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    return ok;
}

/* Reads the "Key=" of the [LinkKey] section of one bluetoothd info file; the key is hashed. */
static bool read_bond_file(const char * path, char out[BT_RC_TRUST_FP_SIZE]) {
    FILE * f = fopen(path, "r");
    if (!f) return false;
    char iobuf[512], line[128];
    setvbuf(f, iobuf, _IOFBF, sizeof(iobuf)); /* a buffer of ours, so the key can be wiped */
    bool in_link_key = false, have_key = false, continued = false;
    while (fgets(line, sizeof(line), f)) {
        bool line_start = !continued;
        continued = strchr(line, '\n') == NULL;
        if (!line_start) continue; /* the rest of a long line (Name= is the peer's) is not a line of its own */
        if (line[0] == '[') {
            if (have_key) break;
            in_link_key = strncmp(line, "[LinkKey]", 9) == 0;
        } else if (in_link_key && !have_key && strncmp(line, "Key=", 4) == 0) {
            have_key = key_fingerprint(line + 4, out);
            if (!have_key) break;
        }
    }
    fclose(f);
    mbedtls_platform_zeroize(line, sizeof(line));
    mbedtls_platform_zeroize(iobuf, sizeof(iobuf));
    return have_key;
}

static bool bond_fingerprint(const char * mac, char out[BT_RC_TRUST_FP_SIZE]) {
    char key[BT_RC_TRUST_MAC_SIZE];
    const char * dir = BT_RC_TRUST_STORAGE_DIR;
    if (!bt_rc_trust_normalize(mac, key)) return false;
    DIR * d = opendir(dir);
    if (!d) return false;
    bool found = false;
    int seen = 0;
    struct dirent * e;
    while ((e = readdir(d))) {
        char path[512];
        if (e->d_name[0] == '.') continue;
        if (snprintf(path, sizeof(path), "%s/%s/%s/info", dir, e->d_name, key) >= (int) sizeof(path)) continue;
        if (access(path, F_OK) != 0) continue;
        seen++;
        found = read_bond_file(path, out);
    }
    closedir(d);
    return found && seen == 1; /* more than one adapter holds a bond: which is live is unknown */
}

bool bt_rc_trust_fingerprint_valid(const char * fp) {
    if (!fp || strlen(fp) != BT_RC_TRUST_FP_SIZE - 1) return false;
    for (int i = 0; fp[i]; i++)
        if (!isxdigit((unsigned char) fp[i])) return false;
    return true;
}

/* Caller holds trust_mutex. */
static int find_locked(const char * mac) {
    for (int i = 0; i < trusted_count; i++)
        if (strcmp(trusted[i].mac, mac) == 0) return i;
    return -1;
}

/* Caller holds trust_mutex. */
static void erase_locked(int i) {
    memmove(&trusted[i], &trusted[i + 1], sizeof(trusted[0]) * (size_t) (trusted_count - i - 1));
    trusted_count--;
    trusted_dirty = true;
}

/* Caller holds trust_mutex. Every revocation advances the epoch, so a grant decided before it
 * (it may have read the bond that is being revoked) cannot trust the device afterwards. */
static void revoke_locked(int i) {
    erase_locked(i);
    trusted_epoch++;
}

bool bt_rc_trust_is_trusted(const char * mac) {
    char key[BT_RC_TRUST_MAC_SIZE], stored[BT_RC_TRUST_FP_SIZE], now[BT_RC_TRUST_FP_SIZE];
    if (!bt_rc_trust_normalize(mac, key)) return false;
    bt_link_key_watch_drain(); /* before taking trust_mutex: it removes revoked devices */
    if (!bt_link_key_watch_healthy()) return false;
    pthread_mutex_lock(&trust_mutex);
    int i = find_locked(key);
    if (i >= 0) memcpy(stored, trusted[i].fingerprint, sizeof(stored));
    pthread_mutex_unlock(&trust_mutex);
    if (i < 0) return false;
    if (bond_fingerprint(key, now) && strcmp(stored, now) == 0) return true;
    /* The bond is gone or was remade (maybe by someone else): trust does not carry over. */
    pthread_mutex_lock(&trust_mutex);
    i = find_locked(key);
    if (i >= 0 && strcmp(trusted[i].fingerprint, stored) == 0) revoke_locked(i);
    pthread_mutex_unlock(&trust_mutex);
    return false;
}

/* Caller holds trust_mutex. */
static void upsert_locked(const char * key, const char * fp) {
    int i = find_locked(key);
    if (i >= 0) {
        if (strcmp(trusted[i].fingerprint, fp) != 0) {
            memcpy(trusted[i].fingerprint, fp, BT_RC_TRUST_FP_SIZE);
            trusted_dirty = true;
        }
        return;
    }
    if (trusted_count == BT_RC_TRUST_MAX) erase_locked(0);
    memcpy(trusted[trusted_count].mac, key, BT_RC_TRUST_MAC_SIZE);
    memcpy(trusted[trusted_count++].fingerprint, fp, BT_RC_TRUST_FP_SIZE);
    trusted_dirty = true;
}

void bt_rc_trust_add_at_epoch(const char * mac, unsigned int epoch) {
    char key[BT_RC_TRUST_MAC_SIZE], fp[BT_RC_TRUST_FP_SIZE];
    if (!bt_rc_trust_normalize(mac, key)) return;
    bt_link_key_watch_drain(); /* a key reported meanwhile revokes first and so changes the epoch */
    if (!bond_fingerprint(key, fp)) return; /* no bond, no trust */
    pthread_mutex_lock(&trust_mutex);
    if (epoch == trusted_epoch) upsert_locked(key, fp);
    pthread_mutex_unlock(&trust_mutex);
}

unsigned int bt_rc_trust_epoch(void) {
    pthread_mutex_lock(&trust_mutex);
    unsigned int epoch = trusted_epoch;
    pthread_mutex_unlock(&trust_mutex);
    return epoch;
}

void bt_rc_trust_remove(const char * mac) {
    char key[BT_RC_TRUST_MAC_SIZE];
    if (!bt_rc_trust_normalize(mac, key)) return;
    pthread_mutex_lock(&trust_mutex);
    int i = find_locked(key);
    if (i >= 0) revoke_locked(i);
    else trusted_epoch++; /* a grant for this device may be in flight */
    pthread_mutex_unlock(&trust_mutex);
}

static bool deadline_passed(const struct timespec * deadline) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

/* Caller holds trust_mutex. True while a grant for mac can still complete; an expired one, or one
 * that a revocation made stale, is dropped. */
static bool grant_pending_locked(const char * mac) {
    if (grant.active && (grant.epoch != trusted_epoch || deadline_passed(&grant.deadline))) grant.active = false;
    return grant.active && strcmp(grant.mac, mac) == 0;
}

void bt_rc_trust_arm_grant(const char * mac) {
    char key[BT_RC_TRUST_MAC_SIZE];
    if (!bt_rc_trust_normalize(mac, key)) return;
    pthread_mutex_lock(&trust_mutex);
    grant.active = true;
    memcpy(grant.mac, key, sizeof(key));
    grant.epoch = trusted_epoch;
    clock_gettime(CLOCK_MONOTONIC, &grant.deadline);
    grant.deadline.tv_sec += GRANT_WAIT_S;
    pthread_mutex_unlock(&trust_mutex);
}

bool bt_rc_trust_grant_pending(const char * mac) {
    char key[BT_RC_TRUST_MAC_SIZE];
    if (!bt_rc_trust_normalize(mac, key)) return false;
    pthread_mutex_lock(&trust_mutex);
    bool pending = grant_pending_locked(key);
    pthread_mutex_unlock(&trust_mutex);
    return pending;
}

void bt_rc_trust_link_key_event(const char * mac, bool stored, int key_type,
                                const uint8_t key[BT_RC_TRUST_LINK_KEY_SIZE]) {
    char norm[BT_RC_TRUST_MAC_SIZE], hex[2 * BT_RC_TRUST_LINK_KEY_SIZE + 1], fp[BT_RC_TRUST_FP_SIZE];
    if (!bt_rc_trust_normalize(mac, norm)) return;
    /* The text bluetoothd writes as Key=: upper-case hex of the key bytes in order. */
    for (int i = 0; i < BT_RC_TRUST_LINK_KEY_SIZE; i++) snprintf(hex + 2 * i, 3, "%02X", key[i]);
    bool ok = key_fingerprint(hex, fp);
    mbedtls_platform_zeroize(hex, sizeof(hex));
    pthread_mutex_lock(&trust_mutex);
    if (grant_pending_locked(norm)) {
        grant.active = false; /* it completes here or not at all */
        /* Only a MITM-protected key (numeric comparison, kernel types 0x05 and 0x08) that was stored
         * is the pairing the user confirmed; a Just Works key by someone else is not. */
        if (ok && stored && (key_type == 0x05 || key_type == 0x08)) {
            upsert_locked(norm, fp);
            pthread_mutex_unlock(&trust_mutex);
            return;
        }
    }
    int i = find_locked(norm);
    if (i >= 0 && (!stored || !ok || strcmp(trusted[i].fingerprint, fp) != 0)) revoke_locked(i);
    else if (!stored) trusted_epoch++; /* a grant for this device may be in flight */
    pthread_mutex_unlock(&trust_mutex);
}

void bt_rc_trust_clear(void) {
    pthread_mutex_lock(&trust_mutex);
    if (trusted_count > 0) trusted_dirty = true;
    trusted_count = 0;
    trusted_epoch++;
    pthread_mutex_unlock(&trust_mutex);
}

void bt_rc_trust_import(const bt_rc_trust_entry_t * entries, int count) {
    pthread_mutex_lock(&trust_mutex);
    trusted_count = 0;
    for (int i = 0; entries && i < count && trusted_count < BT_RC_TRUST_MAX; i++) {
        char key[BT_RC_TRUST_MAC_SIZE];
        if (!bt_rc_trust_normalize(entries[i].mac, key) || !bt_rc_trust_fingerprint_valid(entries[i].fingerprint) ||
            find_locked(key) >= 0)
            continue;
        memcpy(trusted[trusted_count].mac, key, sizeof(key));
        memcpy(trusted[trusted_count++].fingerprint, entries[i].fingerprint, BT_RC_TRUST_FP_SIZE);
    }
    trusted_dirty = false;
    pthread_mutex_unlock(&trust_mutex);
}

bool bt_rc_trust_export_if_dirty(bt_rc_trust_entry_t * entries, int * count) {
    pthread_mutex_lock(&trust_mutex);
    bool dirty = trusted_dirty;
    if (dirty) {
        memcpy(entries, trusted, sizeof(trusted[0]) * (size_t) trusted_count);
        *count = trusted_count;
        trusted_dirty = false;
    }
    pthread_mutex_unlock(&trust_mutex);
    return dirty;
}
