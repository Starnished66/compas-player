#include "bluetooth_control_internal.h"

#ifndef HOST_BUILD
#define BT_SNOOP_DIR "/data/mnt/sd_0/.logs"
#define BT_SNOOP_PATH BT_SNOOP_DIR "/bluetooth.snoop"
#define BT_SNOOP_MAX_BYTES (32L * 1024 * 1024)
#define BT_SNOOP_POLL_SEC 10
static pthread_mutex_t bt_snoop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bt_snoop_cond = PTHREAD_COND_INITIALIZER;
/* Guarded by bt_snoop_mutex, which is never held across I/O or a wait for a
 * child, so the toggle, the hotplug poll and shutdown never block on it. */
static bool bt_snoop_wanted, bt_snoop_held, bt_snoop_final, bt_snoop_thread_started;
static pid_t bt_snoop_pid; /* btmon child, published from spawn until reaped (both under the mutex) */

/* SIGTERM, then SIGKILL after a second. Reaped only under the mutex, so a
 * release never signals a reaped (possibly reused) pid. */
static void bt_snoop_stop(pid_t pid) {
    kill(pid, SIGTERM);
    for (int waited_ms = 0;; waited_ms += 50) {
        pthread_mutex_lock(&bt_snoop_mutex);
        bool gone = waitpid(pid, NULL, WNOHANG) == pid;
        if (gone) bt_snoop_pid = 0;
        else if (waited_ms == 1000) kill(pid, SIGKILL);
        pthread_mutex_unlock(&bt_snoop_mutex);
        if (gone) return;
        usleep(50000);
    }
}

/* Owns the btmon child: starts and stops it to match the requested state,
 * and every poll stops it on card removal or once the capture passes the cap
 * (the next poll rotates and restarts). Retrying each poll also recovers a
 * failed start or a card that appears or becomes writable later. */
static void * bt_snoop_worker(void * arg) {
    (void) arg;
    subprocess_kill_all_matching("btmon -w " BT_SNOOP_PATH); /* left by a crashed previous run */
    bool applied = false;
    for (;;) {
        pthread_mutex_lock(&bt_snoop_mutex);
        /* A request made while this was busy (or before it first waited) is not lost. */
        if (bt_snoop_wanted == applied) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += BT_SNOOP_POLL_SEC;
            pthread_cond_timedwait(&bt_snoop_cond, &bt_snoop_mutex, &until);
        }
        bool want = applied = bt_snoop_wanted;
        want = want && !bt_snoop_held;
        pid_t pid = bt_snoop_pid;
        if (pid > 0 && waitpid(pid, NULL, WNOHANG) == pid) pid = bt_snoop_pid = 0;
        pthread_mutex_unlock(&bt_snoop_mutex);

        bool record = want && sd_card_root_is_mounted();
        struct stat st;
        if (pid > 0 && (!record || (stat(BT_SNOOP_PATH, &st) == 0 && st.st_size > BT_SNOOP_MAX_BYTES))) {
            bt_snoop_stop(pid);
            pid = 0;
        }
        if (pid == 0 && record) {
            /* Remove first: rename over an existing file is unreliable on vfat.
             * Only with a capture to keep, so a failed start never costs the backup. */
            if (access(BT_SNOOP_PATH, F_OK) == 0) {
                remove(BT_SNOOP_PATH ".1");
                rename(BT_SNOOP_PATH, BT_SNOOP_PATH ".1"); /* best-effort */
            }
            (void) mkdir(BT_SNOOP_DIR, 0755);
            char * argv[] = { (char *) "/usr/bin/btmon", (char *) "-w", (char *) BT_SNOOP_PATH, NULL };
            int fd;
            /* Spawned and published in one step, so a release can never miss it. */
            pthread_mutex_lock(&bt_snoop_mutex);
            if (bt_snoop_wanted && !bt_snoop_held && subprocess_popen_stdin(argv, &pid, &fd)) {
                close(fd);
                bt_snoop_pid = pid;
            }
            pthread_mutex_unlock(&bt_snoop_mutex);
        }
    }
    return NULL;
}
#endif

/* Settings > Developer Options toggle. Only records the request; a worker
 * thread runs btmon (HCI capture, A2DP audio included) and enforces the size
 * cap, so nothing blocks the caller and it works with the screen off. */
void bt_control_set_debug_log_enabled(bool enabled) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bt_snoop_mutex);
    bt_snoop_wanted = enabled;
    /* Started on the first call (startup) even when off, so a capture left
     * running by a crashed previous run is always stopped. */
    if (!bt_snoop_thread_started && access("/usr/bin/btmon", X_OK) == 0) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, bt_snoop_worker, NULL) == 0) {
            pthread_detach(tid);
            bt_snoop_thread_started = true;
        }
    }
    pthread_cond_signal(&bt_snoop_cond);
    pthread_mutex_unlock(&bt_snoop_mutex);
#else
    (void) enabled;
#endif
}

void bt_control_debug_capture_release(bool for_good) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bt_snoop_mutex);
    bt_snoop_held = true;
    bt_snoop_final = bt_snoop_final || for_good;
    pid_t pid = bt_snoop_pid;
    if (pid > 0) kill(pid, SIGKILL); /* the worker reaps it */
    pthread_mutex_unlock(&bt_snoop_mutex);
    /* Up to a second for it to close its file (exited, or a zombie), so an
     * unmount right after this finds the card free. */
    char path[32], state = 0;
    snprintf(path, sizeof(path), "/proc/%d/stat", (int) pid);
    for (int waited_ms = 0; pid > 0 && waited_ms < 1000; waited_ms += 50) {
        FILE * f = fopen(path, "r");
        bool closed = !f || fscanf(f, "%*d %*s %c", &state) != 1 || state == 'Z';
        if (f) fclose(f);
        if (closed) break;
        usleep(50000);
    }
#else
    (void) for_good;
#endif
}

void bt_control_debug_capture_resume(bool after_failed_shutdown) {
#ifndef HOST_BUILD
    pthread_mutex_lock(&bt_snoop_mutex);
    if (after_failed_shutdown) bt_snoop_final = false;
    if (bt_snoop_held && !bt_snoop_final) {
        bt_snoop_held = false;
        pthread_cond_signal(&bt_snoop_cond);
    }
    pthread_mutex_unlock(&bt_snoop_mutex);
#else
    (void) after_failed_shutdown;
#endif
}