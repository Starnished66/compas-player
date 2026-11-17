#include "idle_shutdown.h"
#include "../../updates/firmware_update.h"
#include "../../persistence/settings.h"
#include "../../../library/playlists/rating_writer.h"
#include "../../../library/workers/track_metadata_worker.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../audio/engine/audio.h"
#include "reboot_handoff.h"
#include "../../../network/bluetooth/control/bluetooth_control.h"
#include <stdio.h>
#ifndef HOST_BUILD
#include "../../../bootloader/selector/ui/controller/selector_request.h"
#endif

static void selector_error(char * error, size_t error_size, const char * message) {
    if (!error || error_size == 0) return;
    snprintf(error, error_size, "%s", message);
}

#ifndef HOST_BUILD
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/reboot.h>

static bool parent_is_bootloader(void) {
    char path[32];
    char target[256];
    snprintf(path, sizeof(path), "/proc/%d/exe", (int) getppid());
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) return false;
    target[length] = '\0';
    const char * base = strrchr(target, '/');
    base = base ? base + 1 : target;
    return strncmp(base, "compas_loader", strlen("compas_loader")) == 0 ||
           strncmp(base, "compas_bootloader", strlen("compas_bootloader")) == 0 ||
           strncmp(base, "compas_rockbox_bootloader", strlen("compas_rockbox_bootloader")) == 0;
}


#endif

bool idle_shutdown_reboot_handoff(void) {
#ifndef HOST_BUILD
    bt_control_debug_capture_release(true); /* the supervisor unmounts the card next */
    /* Let the supervisor release the SD card before restarting it. The
     * supervisor recognizes this status as a requested reboot. */
    if (parent_is_bootloader()) _exit(IDLE_SHUTDOWN_REBOOT_EXIT_CODE);
    sync();
    /* Standalone player launches have no supervisor to reap them. Ask init
     * to run its orderly shutdown hooks before the bounded raw fallback. */
    (void)reboot_request_orderly_shutdown(NULL);
    if (reboot(RB_AUTOBOOT) == 0) return true;
    bt_control_debug_capture_resume(true);
    /* Terminal callers keep saves latched. A caller returning to the live UI
     * must explicitly cancel shutdown after recording the syscall errno. */
    return false;
#else
    return false;
#endif
}

#ifndef HOST_BUILD
#include "lvgl/lvgl.h"
#include "../../../ui/player/screen/gui_player.h"
#include "../../../ui/text/i18n/i18n.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

extern void show_error_toast(const char *message);
static struct {
    pthread_t thread;
    atomic_bool done, cancelled, committed;
    bool running, notified, settings_started;
    int action;
    uint64_t started;
    char error[128];
} shutdown_work;
static lv_timer_t *shutdown_timer;
static atomic_bool recovery_reboot_pending;

static uint64_t shutdown_milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool shutdown_expired(void) {
    return shutdown_milliseconds() - shutdown_work.started >= 5000;
}

static void shutdown_terminal(int action) {
    if (action == 3) { settings_factory_reset_commit(); return; }
    if (action == 2 && !selector_request_write(shutdown_work.error, sizeof(shutdown_work.error))) return;
    if (action != 0) {
        if (!idle_shutdown_reboot_handoff() && action == 2) (void)selector_request_cancel();
        return;
    }
    bt_control_debug_capture_release(true);
    sync();
    execl("/sbin/poweroff", "poweroff", (char *)NULL);
    (void)reboot(RB_POWER_OFF);
    bt_control_debug_capture_resume(true);
}

static bool shutdown_drain_history(void) {
    const struct timespec delay = { .tv_nsec = 1000000 };
    while (!track_metadata_history_ready()) {
        if (atomic_load(&shutdown_work.cancelled) || shutdown_expired()) return false;
        nanosleep(&delay, NULL);
    }
    return true;
}

static void *shutdown_persist(void *unused) {
    (void)unused;
    bool ok = shutdown_drain_history();
    if (ok) ok = rating_writer_flush();
    if (ok) ok = tagcache_flush_numeric();
    ok = settings_shutdown_wait() && ok;
    if (ok && !atomic_load(&shutdown_work.cancelled) && !shutdown_expired()) {
        atomic_store(&shutdown_work.committed, true);
        shutdown_terminal(shutdown_work.action);
    }
    atomic_store(&shutdown_work.done, true);
    return NULL;
}

static void shutdown_poll(lv_timer_t *timer) {
    if (shutdown_expired() && !shutdown_work.notified && !atomic_load(&shutdown_work.committed)) {
        atomic_store(&shutdown_work.cancelled, true);
        shutdown_work.notified = true;
        show_error_toast(TR("Could not save changes"));
    }
    if (!shutdown_work.running && !shutdown_work.notified && gui_player_queue_flush()) {
        shutdown_work.settings_started = settings_shutdown_start();
        if (!shutdown_work.settings_started) {
            atomic_store(&shutdown_work.done, true);
        } else if (!pthread_create(&shutdown_work.thread, NULL, shutdown_persist, NULL))
            shutdown_work.running = true;
        else atomic_store(&shutdown_work.done, true);
    }
    if (shutdown_work.running && !atomic_load(&shutdown_work.done)) return;
    if (!shutdown_work.notified && !atomic_load(&shutdown_work.done)) return;
    if (!shutdown_work.running && !settings_shutdown_ready()) return;
    if (shutdown_work.running) pthread_join(shutdown_work.thread, NULL);
    gui_player_queue_flush_cancel();
    settings_shutdown_cancel();
    if (shutdown_work.action == 4) firmware_update_recovery_cancel();
    if (shutdown_work.settings_started) settings_save_async(&current_settings);
    if (!shutdown_work.notified)
        show_error_toast(shutdown_work.error[0] ? shutdown_work.error : TR("Could not save changes"));
    lv_timer_delete(timer);
    shutdown_timer = NULL;
}

static bool shutdown_request(int action) {
    if (action != 4 && firmware_update_busy()) return false;
    if (shutdown_timer) return true;
    if (audio_is_playing() || audio_is_paused())
        current_settings.last_position = audio_get_resume_position_seconds();
    shutdown_work.running = shutdown_work.notified = shutdown_work.settings_started = false;
    shutdown_work.action = action;
    shutdown_work.error[0] = 0;
    shutdown_work.started = shutdown_milliseconds();
    atomic_store(&shutdown_work.done, false);
    atomic_store(&shutdown_work.committed, false);
    atomic_store(&shutdown_work.cancelled, false);
    shutdown_timer = lv_timer_create(shutdown_poll, 25, NULL);
    return shutdown_timer != NULL;
}
#endif

void idle_shutdown_now(void) {
#ifndef HOST_BUILD
    (void)shutdown_request(0);
#endif
}

bool idle_shutdown_pending(void) {
#ifndef HOST_BUILD
    return shutdown_timer != NULL;
#else
    return false;
#endif
}

void idle_shutdown_reboot_now(void) {
#ifndef HOST_BUILD
    (void)shutdown_request(1);
#endif
}

bool idle_shutdown_reboot_to_bootloader(char *error, size_t error_size) {
#ifdef HOST_BUILD
    selector_error(error, error_size, "Boot selector restart is unavailable on this host");
    return false;
#else
    if (shutdown_request(2)) return true;
    selector_error(error, error_size, "Could not start restart preparation");
    return false;
#endif
}

void idle_shutdown_factory_reset(void) {
#ifndef HOST_BUILD
    (void)shutdown_request(3);
#endif
}

/* The recovery worker only posts a request. Snapshotting remains on the UI. */
void idle_shutdown_recovery_reboot(void) {
#ifndef HOST_BUILD
    atomic_store(&recovery_reboot_pending, true);
#endif
}

void idle_shutdown_poll_recovery(void) {
#ifndef HOST_BUILD
    if (atomic_load(&recovery_reboot_pending) && !shutdown_timer && shutdown_request(4))
        atomic_store(&recovery_reboot_pending, false);
#endif
}
