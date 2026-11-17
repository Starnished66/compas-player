/* Host-only executable: terminal system calls and storage are all fakes. */
#include "../idle_shutdown.c"
#include <assert.h>
#include <stdarg.h>

player_settings_t current_settings;
static atomic_bool hold_numeric, entered_numeric;
static bool persistence_ok = true;
static int poweroffs, notices, deleted, recovery_cancellations;
static pthread_t ui_thread;
static bool queue_ready = true;
static atomic_bool history_ready = true;
static int queue_cancels;
bool track_metadata_history_ready(void) { return atomic_load(&history_ready); }
void gui_player_queue_flush_cancel(void) { queue_cancels++; }
const char *i18n_tr(const char *text) { return text; }
void show_error_toast(const char *text) { (void)text; notices++; }
bool rating_writer_flush(void) { return true; }
bool tagcache_flush_numeric(void) {
    atomic_store(&entered_numeric, true);
    while (atomic_load(&hold_numeric)) usleep(1000);
    return persistence_ok;
}
void firmware_update_recovery_cancel(void) { recovery_cancellations++; }
bool firmware_update_busy(void) { return true; }
bool audio_is_playing(void) { return true; }
bool audio_is_paused(void) { return false; }
double audio_get_resume_position_seconds(void) { return 42; }
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *data) {
    (void)cb; (void)period; (void)data;
    static int timer;
    return (lv_timer_t *)&timer;
}
bool settings_shutdown_wait(void) { return persistence_ok; }
bool settings_shutdown_start(void) { assert(pthread_equal(ui_thread, pthread_self())); return true; }
bool settings_shutdown_ready(void) { return true; }
void settings_shutdown_cancel(void) {}
void settings_factory_reset_commit(void) {}
void settings_save_async(const player_settings_t *s) { (void)s; }
bool gui_player_queue_flush(void) { assert(pthread_equal(ui_thread, pthread_self())); return queue_ready; }
void lv_timer_delete(lv_timer_t *timer) { (void)timer; deleted++; }
void bt_control_debug_capture_release(bool value) { (void)value; }
void bt_control_debug_capture_resume(bool value) { (void)value; }
bool selector_request_write(char *error, size_t size) { (void)error; (void)size; return true; }
bool selector_request_cancel(void) { return true; }
reboot_handoff_result_t reboot_request_orderly_shutdown(void (*hook)(void)) {
    (void)hook; return REBOOT_HANDOFF_UNAVAILABLE;
}
void __wrap_sync(void) {}
int __wrap_execl(const char *path, const char *arg, ...) { (void)path; (void)arg; poweroffs++; return -1; }
int __wrap_reboot(int command) { (void)command; return -1; }
ssize_t __wrap_readlink(const char *path, char *buf, size_t size) { (void)path; (void)buf; (void)size; return -1; }
void __wrap__exit(int status) { (void)status; assert(false); abort(); }

static void start_persistence(bool blocked) {
    shutdown_work.action = 0;
    shutdown_work.running = true;
    shutdown_work.notified = false;
    shutdown_work.started = shutdown_milliseconds();
    atomic_store(&shutdown_work.done, false);
    atomic_store(&shutdown_work.committed, false);
    atomic_store(&shutdown_work.cancelled, false);
    atomic_store(&entered_numeric, false);
    atomic_store(&hold_numeric, blocked);
    assert(!pthread_create(&shutdown_work.thread, NULL, shutdown_persist, NULL));
    while (!atomic_load(&entered_numeric)) usleep(1000);
}

static void finish_persistence(void) {
    atomic_store(&hold_numeric, false);
    while (!atomic_load(&shutdown_work.done)) usleep(1000);
    shutdown_poll(NULL);
}

static void test_recovery_request(void) {
    queue_ready = false;
    idle_shutdown_recovery_reboot();
    assert(!shutdown_timer);
    idle_shutdown_poll_recovery();
    assert(shutdown_timer && shutdown_work.action == 4);
    assert(current_settings.last_position == 42);
    shutdown_poll(shutdown_timer);
    assert(!shutdown_work.running && !shutdown_work.settings_started);
    queue_ready = true;
    atomic_store(&hold_numeric, true);
    shutdown_poll(shutdown_timer);
    assert(shutdown_work.running && shutdown_work.settings_started);
    finish_persistence();
    assert(!shutdown_timer && recovery_cancellations == 1);
}

static void test_history_barrier(void) {
    shutdown_work.action = 0;
    shutdown_work.running = true;
    shutdown_work.notified = false;
    shutdown_work.started = shutdown_milliseconds();
    atomic_store(&shutdown_work.done, false);
    atomic_store(&shutdown_work.committed, false);
    atomic_store(&shutdown_work.cancelled, false);
    atomic_store(&entered_numeric, false);
    atomic_store(&history_ready, false);
    assert(!pthread_create(&shutdown_work.thread, NULL, shutdown_persist, NULL));
    usleep(20000);
    assert(!atomic_load(&entered_numeric) && !atomic_load(&shutdown_work.done));
    atomic_store(&history_ready, true);
    finish_persistence();
    assert(atomic_load(&entered_numeric));

    atomic_store(&history_ready, false);
    atomic_store(&entered_numeric, false);
    atomic_store(&shutdown_work.done, false);
    atomic_store(&shutdown_work.committed, false);
    shutdown_work.running = true;
    shutdown_work.notified = false;
    shutdown_work.started = shutdown_milliseconds() - 6000;
    assert(!pthread_create(&shutdown_work.thread, NULL, shutdown_persist, NULL));
    finish_persistence();
    assert(!atomic_load(&entered_numeric));
    atomic_store(&history_ready, true);
}

int main(void) {
    ui_thread = pthread_self();
    start_persistence(true);
    shutdown_work.started -= 6000;
    uint64_t before = shutdown_milliseconds();
    shutdown_poll(NULL);
    assert(shutdown_milliseconds() - before < 100);
    assert(notices == 1 && !poweroffs && !deleted);
    finish_persistence();
    assert(!poweroffs && deleted == 1 && queue_cancels == 1);
    persistence_ok = false;
    start_persistence(false);
    finish_persistence();
    assert(!poweroffs && deleted == 2);
    persistence_ok = true;
    start_persistence(false);
    finish_persistence();
    assert(poweroffs == 1 && deleted == 3);
    test_recovery_request();
    test_history_barrier();
    puts("Shutdown persistence timeout, failure, success and recovery UI preparation passed");
    return 0;
}
