#include "wireless_adb.h"
#include "../../core/process/subprocess.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

/* The copy's file name is also the process name, which is how it is found. */
#define WADBD_NAME "wadbd"
#define WADBD_PATH "/tmp/" WADBD_NAME

static pthread_mutex_t wadb_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reconcile_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool worker_active;
static unsigned generation;
static bool wadb_wanted;
static bool wadb_usb_claimed;

#ifdef HOST_BUILD
/* No adbd to manage on the host. */
static bool wadb_apply(bool run) { (void) run; return true; }
#else
static pid_t wadb_pid;
static pthread_mutex_t child_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t child_changed;
static pthread_once_t child_changed_once = PTHREAD_ONCE_INIT;

/* Monotonic, so a clock step while stopping cannot stretch the bound. */
static void init_child_changed(void) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&child_changed, &attr);
    pthread_condattr_destroy(&attr);
}

static void * wadb_reap_thread(void * arg) {
    pthread_once(&child_changed_once, init_child_changed);
    (void)arg;
    pthread_mutex_lock(&child_mutex);
    pid_t pid = wadb_pid;
    pthread_mutex_unlock(&child_mutex);
    if (pid <= 0) return NULL;

    /* Leave the zombie pinned until signaling and reaping share the lock. */
    siginfo_t info;
    int rc;
    do { rc = waitid(P_PID, pid, &info, WEXITED | WNOWAIT); } while (rc < 0 && errno == EINTR);
    pthread_mutex_lock(&child_mutex);
    int status = 0;
    pid_t result;
    do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    if (result == pid || (result < 0 && errno == ECHILD)) {
        wadb_pid = 0;
        pthread_cond_broadcast(&child_changed);
    }
    pthread_mutex_unlock(&child_mutex);
    return NULL;
}

/* child_mutex is held; the reaper alone clears the owned PID. */
static bool wadb_wait_stopped(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec++;
    while (wadb_pid > 0) {
        if (pthread_cond_timedwait(&child_changed, &child_mutex, &deadline) != 0) break;
    }
    return wadb_pid == 0;
}

static bool wadb_start(void) {
    pthread_once(&child_changed_once, init_child_changed);
    pthread_mutex_lock(&child_mutex);
    bool running = wadb_pid > 0;
    pthread_mutex_unlock(&child_mutex);
    if (running || subprocess_is_running(WADBD_NAME)) return true;
    if (access(WADBD_PATH, X_OK) != 0) {
        char * cp[] = { (char *) "cp", (char *) "/usr/bin/adbd", (char *) WADBD_PATH, NULL };
        if (!subprocess_run(cp, NULL, 0) || access(WADBD_PATH, X_OK) != 0) return false;
    }
    pthread_mutex_lock(&child_mutex);
    pthread_t reaper;
    /* Reserve the reaper before creating a child we would have to clean up. */
    if (pthread_create(&reaper, NULL, wadb_reap_thread, NULL) != 0) {
        pthread_mutex_unlock(&child_mutex);
        return false;
    }
    /* Direct spawn confirms exec, unlike starting a shell first. */
    char * run[] = { (char *) WADBD_PATH, NULL };
    bool started = subprocess_start(run, &wadb_pid);
    if (!started) wadb_pid = 0;
    pthread_mutex_unlock(&child_mutex);
    if (started) pthread_detach(reaper);
    else pthread_join(reaper, NULL);
    return started;
}

static bool wadb_stop(void) {
    pthread_once(&child_changed_once, init_child_changed);
    pthread_mutex_lock(&child_mutex);
    if (wadb_pid > 0) {
        kill(wadb_pid, SIGTERM);
        if (!wadb_wait_stopped()) {
            kill(wadb_pid, SIGKILL);
            (void)wadb_wait_stopped();
        }
    }
    bool stopped = wadb_pid == 0;
    pthread_mutex_unlock(&child_mutex);
    if (!stopped) return false;
    /* Include a daemon inherited from a previous player instance. */
    subprocess_kill_all_matching(WADBD_NAME);
    for (int i = 0; i < 20 && subprocess_is_running(WADBD_NAME); i++) usleep(50000);
    return !subprocess_is_running(WADBD_NAME);
}

static bool wadb_apply(bool run) {
    return run ? wadb_start() : wadb_stop();
}
#endif

static bool start_worker_locked(void);
/* Protected by reconcile_mutex. */
static unsigned applied_generation;
static bool last_applied;

/* Slow operations serialize separately from the UI's desired state. */
static bool wadb_reconcile(void) {
    bool applied;
    pthread_mutex_lock(&reconcile_mutex);
    for (;;) {
        pthread_mutex_lock(&wadb_mutex);
        unsigned observed = generation;
        bool run = wadb_wanted && !wadb_usb_claimed;
        pthread_mutex_unlock(&wadb_mutex);
        if (observed != applied_generation) {
            last_applied = wadb_apply(run);
            applied_generation = observed;
            if (!last_applied) fprintf(stderr, "wireless_adb: reconciliation failed\n");
        }
        applied = last_applied;
        pthread_mutex_lock(&wadb_mutex);
        bool settled = observed == generation;
        pthread_mutex_unlock(&wadb_mutex);
        if (settled) break;
    }
    pthread_mutex_unlock(&reconcile_mutex);
    return applied;
}

static void * wadb_reconcile_thread(void * arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&wadb_mutex);
        unsigned observed = generation;
        pthread_mutex_unlock(&wadb_mutex);
        wadb_reconcile();
        pthread_mutex_lock(&wadb_mutex);
        bool changed = observed != generation;
        if (!changed) worker_active = false;
        pthread_mutex_unlock(&wadb_mutex);
        if (!changed) return NULL;
    }
}

static bool start_worker_locked(void) {
    if (worker_active) return true;
    pthread_t thread;
    worker_active = true;
    if (pthread_create(&thread, NULL, wadb_reconcile_thread, NULL) == 0) pthread_detach(thread);
    else {
        worker_active = false;
        fprintf(stderr, "wireless_adb: could not start reconciliation worker\n");
    }
    return worker_active;
}

void wireless_adb_sync(bool wanted) {
    pthread_mutex_lock(&wadb_mutex);
    wadb_wanted = wanted;
    ++generation;
    (void)start_worker_locked();
    pthread_mutex_unlock(&wadb_mutex);
}

void wireless_adb_boot_sync(bool wanted, bool claimed) {
    pthread_mutex_lock(&wadb_mutex);
    wadb_wanted = wanted;
    wadb_usb_claimed = claimed;
    ++generation;
    (void)start_worker_locked();
    pthread_mutex_unlock(&wadb_mutex);
}

bool wireless_adb_usb_adb_claim(bool claimed) {
    pthread_mutex_lock(&wadb_mutex);
    wadb_usb_claimed = claimed;
    ++generation;
    pthread_mutex_unlock(&wadb_mutex);
    /* USB mode workers must finish reconciliation before starting USB adbd. */
    return wadb_reconcile();
}
