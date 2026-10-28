#include "remote_settings_gate.h"

#include <pthread.h>
#include <time.h>

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool gate_on;
static bool gate_expiry_pending;
static uint64_t gate_until_ms;

/* CLOCK_BOOTTIME keeps counting while the player is suspended, so the 30 minutes
 * cannot outlast an overnight suspend. */
uint64_t remote_settings_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t) ts.tv_sec * 1000u + (uint64_t) ts.tv_nsec / 1000000u;
}

void remote_settings_gate_set(bool on, uint64_t now_ms) {
    pthread_mutex_lock(&gate_mutex);
    gate_on = on;
    gate_expiry_pending = false;
    gate_until_ms = now_ms + REMOTE_SETTINGS_ALLOW_MS;
    pthread_mutex_unlock(&gate_mutex);
}

/* Caller holds gate_mutex. */
static void expire_locked(uint64_t now_ms) {
    if (gate_on && now_ms >= gate_until_ms) {
        gate_on = false;
        gate_expiry_pending = true;
    }
}

bool remote_settings_gate_open(uint64_t now_ms) {
    pthread_mutex_lock(&gate_mutex);
    expire_locked(now_ms);
    bool open = gate_on;
    pthread_mutex_unlock(&gate_mutex);
    return open;
}

bool remote_settings_gate_consume_expired(uint64_t now_ms) {
    pthread_mutex_lock(&gate_mutex);
    expire_locked(now_ms);
    bool expired = gate_expiry_pending;
    gate_expiry_pending = false;
    pthread_mutex_unlock(&gate_mutex);
    return expired;
}
