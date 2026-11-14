#include "wireless_adb.h"
#include "../../core/process/subprocess.h"

#include <pthread.h>
#include <unistd.h>

/* The copy's file name is also the process name, which is how it is found. */
#define WADBD_NAME "wadbd"
#define WADBD_PATH "/tmp/" WADBD_NAME

static pthread_mutex_t wadb_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool wadb_wanted;
static bool wadb_usb_claimed;

#ifdef HOST_BUILD
/* No adbd to manage on the host. */
static void wadb_apply(bool run) { (void) run; }
#else
static void wadb_start(void) {
    if (subprocess_is_running(WADBD_NAME)) return;
    if (access(WADBD_PATH, X_OK) != 0) {
        char * cp[] = { (char *) "cp", (char *) "/usr/bin/adbd", (char *) WADBD_PATH, NULL };
        subprocess_run(cp, NULL, 0);
    }
    /* Detached, in its own session, cwd /, so it outlives the player. */
    char * run[] = { (char *) "sh", (char *) "-c", (char *) "cd / && exec " WADBD_PATH, NULL };
    subprocess_spawn_daemon(run);
}

static void wadb_stop(void) {
    if (!subprocess_is_running(WADBD_NAME)) return;
    subprocess_kill_all_matching(WADBD_NAME);
    for (int i = 0; i < 20 && subprocess_is_running(WADBD_NAME); i++) usleep(50000);
}

static void wadb_apply(bool run) {
    if (run) wadb_start();
    else wadb_stop();
}
#endif

static void wadb_reconcile_locked(void) {
    wadb_apply(wadb_wanted && !wadb_usb_claimed);
}

/* Reads the flags when it runs, so racing toggles still end on the last one. */
static void * wadb_reconcile_thread(void * arg) {
    (void) arg;
    pthread_mutex_lock(&wadb_mutex);
    wadb_reconcile_locked();
    pthread_mutex_unlock(&wadb_mutex);
    return NULL;
}

void wireless_adb_sync(bool wanted) {
    pthread_mutex_lock(&wadb_mutex);
    wadb_wanted = wanted;
    pthread_mutex_unlock(&wadb_mutex);
    /* Off the UI thread: starting copies and forks, stopping waits for the exit. */
    pthread_t thread;
    if (pthread_create(&thread, NULL, wadb_reconcile_thread, NULL) == 0) pthread_detach(thread);
    else wadb_reconcile_thread(NULL);
}

void wireless_adb_usb_adb_claim(bool claimed) {
    pthread_mutex_lock(&wadb_mutex);
    wadb_usb_claimed = claimed;
    wadb_reconcile_locked();
    pthread_mutex_unlock(&wadb_mutex);
}
