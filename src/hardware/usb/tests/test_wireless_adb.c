/* Exercise target reconciliation with a spawn held before process naming. */
#include <assert.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
static bool fail_thread;
static int create_worker(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*fn)(void *), void *arg) {
    if (fail_thread) return EAGAIN;
    return pthread_create(thread, attr, fn, arg);
}
static int fake_access(const char * path, int mode) { (void)path; (void)mode; return 0; }
static atomic_bool child_alive, child_owned, inherited_alive, claim_done;
static atomic_int reaped, waiters;
static atomic_bool fail_exec, fail_spawn;
static int fake_waitid(idtype_t type, id_t pid, siginfo_t *info, int options) {
    (void)info;
    assert(type == P_PID && pid == 123 && options == (WEXITED | WNOWAIT));
    while (atomic_load(&child_alive)) usleep(1000);
    assert(atomic_load(&child_owned));
    return 0;
}
static pid_t fake_waitpid(pid_t pid, int * status, int options) {
    assert(pid == 123 && options == 0);
    assert(atomic_fetch_add(&waiters, 1) == 0);
    assert(!atomic_load(&child_alive));
    assert(atomic_exchange(&child_owned, false));
    if (status) *status = 0;
    atomic_fetch_add(&reaped, 1);
    atomic_fetch_sub(&waiters, 1);
    return pid;
}
static atomic_int terminated;
static atomic_bool ignore_term;
static int fake_kill(pid_t pid, int signal) {
    assert(pid == 123 && atomic_load(&child_owned));
    if (signal == SIGTERM) atomic_fetch_add(&terminated, 1);
    if (signal == SIGKILL || !atomic_load(&ignore_term)) atomic_store(&child_alive, false);
    return 0;
}
#define waitid fake_waitid
#define kill fake_kill
#define pthread_create create_worker
#define access fake_access
#define waitpid fake_waitpid
#include "../wireless_adb.c"
#undef pthread_create
#undef access
#undef waitpid
#undef waitid
#undef kill

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool spawning, release_spawn;

bool subprocess_run(char * const argv[], char * out, size_t size) {
    (void)argv; (void)out; (void)size; assert(0); return false;
}
bool subprocess_is_running(const char * name) {
    assert(!strcmp(name, "wadbd"));
    return atomic_load(&inherited_alive);
}
void subprocess_kill_all_matching(const char * name) {
    assert(!strcmp(name, "wadbd"));
    atomic_store(&inherited_alive, false);
}
bool subprocess_start(char * const argv[], pid_t * pid) {
    assert(!strcmp(argv[0], "/tmp/wadbd") && !argv[1]);
    pthread_mutex_lock(&gate);
    spawning = true;
    pthread_cond_broadcast(&changed);
    while (!release_spawn) pthread_cond_wait(&changed, &gate);
    if (atomic_load(&fail_spawn) || atomic_load(&fail_exec)) {
        pthread_mutex_unlock(&gate);
        return false;
    }
    assert(!atomic_exchange(&child_owned, true));
    *pid = 123;
    atomic_store(&child_alive, !atomic_load(&fail_exec));
    pthread_mutex_unlock(&gate);
    return true;
}
static void wait_spawn(void) {
    pthread_mutex_lock(&gate);
    while (!spawning) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
static void allow_spawn(void) {
    pthread_mutex_lock(&gate);
    release_spawn = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}
static void settle(void) {
    for (int i = 0; i < 3000; ++i) {
        pthread_mutex_lock(&wadb_mutex);
        bool active = worker_active;
        pthread_mutex_unlock(&wadb_mutex);
        if (!active) return;
        usleep(1000);
    }
    assert(!"worker did not settle");
}
static void * claim(void * arg) {
    (void)arg;
    assert(wireless_adb_usb_adb_claim(true));
    assert(!atomic_load(&child_alive));
    atomic_store(&claim_done, true);
    return NULL;
}
int main(void) {
    fail_thread = true;
    wireless_adb_sync(true);
    assert(!worker_active && !spawning);
    fail_thread = false;
    wireless_adb_boot_sync(true, false);
    wait_spawn();
    /* This call must return while startup remains blocked in the worker. */
    wireless_adb_sync(false);
    allow_spawn();
    settle();
    assert(!atomic_load(&child_alive) && atomic_load(&terminated) == 1);
    spawning = release_spawn = false;
    wireless_adb_sync(true);
    wait_spawn();
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, claim, NULL));
    usleep(10000);
    assert(!atomic_load(&claim_done));
    allow_spawn();
    pthread_join(thread, NULL);
    settle();
    assert(atomic_load(&terminated) == 2);
    atomic_store(&inherited_alive, true);
    assert(wireless_adb_usb_adb_claim(true));
    assert(!atomic_load(&inherited_alive));
    settle();
    /* Direct exec failure does not create an owned child. */
    atomic_store(&fail_exec, true);
    pthread_mutex_lock(&reconcile_mutex);
    assert(!wadb_start() && wadb_pid == 0);
    pthread_mutex_unlock(&reconcile_mutex);
    assert(atomic_load(&reaped) == 2);
    atomic_store(&fail_exec, false);
    atomic_store(&fail_spawn, true);
    pthread_mutex_lock(&reconcile_mutex);
    assert(!wadb_start() && wadb_pid == 0);
    pthread_mutex_unlock(&reconcile_mutex);
    atomic_store(&fail_spawn, false);
    /* No later toggle is needed to reap a daemon that exits on its own. */
    wireless_adb_boot_sync(true, false);
    for (int i = 0; i < 3000 && !atomic_load(&child_alive); ++i) usleep(1000);
    assert(atomic_load(&child_alive));
    usleep(150000);
    atomic_store(&child_alive, false);
    pthread_mutex_lock(&child_mutex);
    assert(wadb_wait_stopped());
    pthread_mutex_unlock(&child_mutex);
    settle();
    assert(!atomic_load(&child_owned) && atomic_load(&reaped) == 3);
    /* Spontaneous exit and a simultaneous USB claim have one reaper. */
    wireless_adb_sync(true);
    for (int i = 0; i < 3000 && !atomic_load(&child_alive); ++i) usleep(1000);
    assert(atomic_load(&child_alive));
    usleep(150000);
    atomic_store(&child_alive, false);
    assert(wireless_adb_usb_adb_claim(true));
    pthread_mutex_lock(&child_mutex);
    assert(wadb_wait_stopped());
    pthread_mutex_unlock(&child_mutex);
    settle();
    assert(!atomic_load(&child_owned) && atomic_load(&reaped) == 4);
    /* Reaper creation failure must prevent spawning on USB release. */
    fail_thread = true;
    assert(!wireless_adb_usb_adb_claim(false));
    assert(!atomic_load(&child_owned) && atomic_load(&reaped) == 4);
    fail_thread = false;
    assert(wireless_adb_usb_adb_claim(false));
    assert(atomic_load(&child_alive));
    atomic_store(&child_alive, false);
    pthread_mutex_lock(&child_mutex);
    assert(wadb_wait_stopped());
    pthread_mutex_unlock(&child_mutex);
    settle();
    assert(!atomic_load(&child_owned));
    atomic_store(&ignore_term, true);
    assert(wireless_adb_usb_adb_claim(false));
    assert(wireless_adb_usb_adb_claim(true));
    assert(!atomic_load(&child_owned));
    puts("Wireless ADB nonblocking toggles, startup cancellation and USB claim passed");
}
