/* Focused lifecycle regression test for the two Bluetooth monitor workers.
 * Include the production implementation so the test exercises its actual
 * pthread/poll/pipe lifecycle.  The subprocess and audio edges below are
 * deliberately small stubs; each monitor still gets a real forked child and
 * a real pipe, rather than a fake thread callback. */
#define _GNU_SOURCE

#include "bluetooth_control.c"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/wait.h>

enum stub_monitor_mode {
    STUB_MONITOR_EOF,
    STUB_MONITOR_HOLD,
    STUB_MONITOR_DISCONNECT,
    STUB_MONITOR_VOLUME,
    STUB_MONITOR_SAME_BATCH,
    STUB_MONITOR_SPLIT_MATCH,
    STUB_MONITOR_FRAGMENTED_MATCH,
    STUB_MONITOR_PERSISTENT_REMOVE,
    STUB_MONITOR_UNRELATED_ADD,
    STUB_MONITOR_LATE_MATCH,
    STUB_MONITOR_STOP_PENDING,
    STUB_MONITOR_EOF_PENDING
};

#define TEST_PCM_PATH "/org/bluealsa/hci0/dev_test/a2dpsrc/sink"
#define TEST_UNRELATED_PCM "/org/bluealsa/hci0/dev_other/a2dpsrc/sink"

static enum stub_monitor_mode stub_mode;
static atomic_int stub_spawn_count;
static atomic_int stub_terminate_count;
static atomic_int stub_reaped_count;
static atomic_int stub_live_child_count;
static atomic_int stub_list_pcms_calls;
static atomic_int stub_volume_calls;
static atomic_int stub_last_volume_raw;
static atomic_int stub_seed_volume_calls;
static atomic_int stub_monitor_volume_raw = 80;
static atomic_int stub_live_volume_packed = (80 << 8) | 80;
static atomic_bool stub_live_volume_as_bytes;
static atomic_bool stub_volume_readback_fail;
static atomic_bool stub_soft_volume_apply_fail;
static atomic_bool stub_source_pcm_present = true;
static const char *stub_managed_objects = "";
static const char *stub_transport_props = "";
static float stub_audio_volume = 0.50f;
static atomic_bool stub_gate_popen;
static atomic_bool stub_popen_entered;
static atomic_bool stub_release_popen;
static atomic_bool stub_stop_call_started;
static pthread_mutex_t stub_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stub_gate_cond = PTHREAD_COND_INITIALIZER;

static void wait_for_true(bool (*predicate)(void)) {
    struct timespec pause_time = { .tv_sec = 0, .tv_nsec = 10000000L };
    for (int i = 0; i < 300; i++) {
        if (predicate()) return;
        nanosleep(&pause_time, NULL);
    }
    assert(!"timed out waiting for monitor state");
}

static bool spawned_once(void) {
    return atomic_load(&stub_spawn_count) > 0;
}

static bool disconnect_worker_inactive(void) {
    return !atomic_load(&bt_output_disconnect_active);
}

static bool volume_worker_inactive(void) {
    return !atomic_load(&bt_source_vol_sync_active);
}

static bool popen_entered(void) {
    return atomic_load(&stub_popen_entered);
}

static bool stop_call_started(void) {
    return atomic_load(&stub_stop_call_started);
}

static void write_child_text(int fd, const char * text) {
    size_t left = strlen(text);
    const char * p = text;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        assert(n > 0);
        p += n;
        left -= (size_t) n;
    }
}

static void hold_child(void) {
    for (;;) pause();
}

int access(const char * path, int mode) {
    (void) mode;
    assert(strcmp(path, "/usr/bin/bluealsad") == 0 ||
           strcmp(path, "/usr/bin/bluealsactl") == 0);
    return -1; /* exercise the legacy bluealsa-cli monitor argv */
}

/* Same fixture as subprocess_run(): the production code niced this call
 * down, which changes the child's scheduling priority, nothing the test
 * fixture models. */
bool subprocess_run_low_priority(char * const argv[], char * out, size_t size) {
    return subprocess_run(argv, out, size);
}

bool subprocess_run(char * const argv[], char * out, size_t size) {
    assert(argv && argv[0] && argv[1]);
    /* Soft-volume state query and the absolute-volume probe: unknown. */
    if (strcmp(argv[0], "dbus-send") == 0 || strcmp(argv[1], "soft-volume") == 0) return false;
    /* Automatic codec selection reads the PCM's codecs; SBC only, already
     * selected, so it settles without a switch. */
    if (strcmp(argv[1], "info") == 0) {
        if (out && size) snprintf(out, size, "Selected codec: SBC\nAvailable codecs: SBC\nRunning: false\n");
        return true;
    }
    /* The volume worker's startup discovery is intentionally empty; this
     * keeps the test focused on the monitor pipe and avoids a fake PCM. */
    assert(strcmp(argv[1], "list-pcms") == 0);
    atomic_fetch_add(&stub_list_pcms_calls, 1);
    if (out && size) snprintf(out, size, "%s", atomic_load(&stub_source_pcm_present) ?
                              TEST_PCM_PATH "\n" : "");
    return true;
}

bool subprocess_run_checked(char * const argv[], char * out, size_t size,
                            int timeout_ms, int * out_exit_code) {
    (void) timeout_ms;
    if (argv && strcmp(argv[0], "dbus-send") == 0) {
        if (strstr(argv[5], "Properties.Get") != NULL) {
            if (atomic_load(&stub_volume_readback_fail)) return false;
            if (out && size) {
                if (atomic_load(&stub_live_volume_as_bytes))
                    snprintf(out, size, "method return sender=:1.2 -> dest=:1.4 reply_serial=2\n   variant       array of bytes [\n         1c 1c\n      ]\n");
                else
                    snprintf(out, size,
                        "method return sender=:1.2 -> dest=:1.4 reply_serial=2\n   variant       uint16 %d\n",
                        atomic_load(&stub_live_volume_packed));
            }
            if (out_exit_code) *out_exit_code = 0;
            return true;
        }
        if (out && size) {
            const char * fixture = strstr(argv[5], "GetManagedObjects") ?
                                   stub_managed_objects : stub_transport_props;
            snprintf(out, size, "%s", fixture);
        }
        if (out_exit_code) *out_exit_code = 0;
        return true;
    }
    if (argv && argv[1] && strcmp(argv[1], "volume") == 0) {
        assert(argv[2] && argv[3]);
        atomic_fetch_add(&stub_volume_calls, 1);
        atomic_fetch_add(&stub_seed_volume_calls, 1);
        atomic_store(&stub_last_volume_raw, atoi(argv[3]));
    } else if (argv && argv[1] && strcmp(argv[1], "soft-volume") == 0) {
        /* Record application writes; readback remains unknown in subprocess_run. */
        assert(argv[2] && argv[3]);
        if (atomic_load(&stub_soft_volume_apply_fail) && out_exit_code) *out_exit_code = 1;
    }
    if (out_exit_code) *out_exit_code = 0;
    return true;
}

bool subprocess_popen(char * const argv[], pid_t * out_pid, int * out_read_fd) {
    assert(argv && argv[0] && strcmp(argv[1], "monitor") == 0);
    assert(out_pid && out_read_fd);

    if (atomic_load(&stub_gate_popen)) {
        pthread_mutex_lock(&stub_gate_mutex);
        atomic_store(&stub_popen_entered, true);
        pthread_cond_broadcast(&stub_gate_cond);
        while (!atomic_load(&stub_release_popen))
            pthread_cond_wait(&stub_gate_cond, &stub_gate_mutex);
        pthread_mutex_unlock(&stub_gate_mutex);
    }

    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(pipe_fds[0]);
        if (stub_mode == STUB_MONITOR_DISCONNECT) {
            write_child_text(pipe_fds[1], "PCMRemoved " TEST_PCM_PATH "\n");
        } else if (stub_mode == STUB_MONITOR_VOLUME) {
            usleep(150000);
            char line[160];
            int len = snprintf(line, sizeof(line), "PropertyChanged %s Volume %d\n",
                               TEST_PCM_PATH, atomic_load(&stub_monitor_volume_raw));
            assert(write(pipe_fds[1], line, (size_t) len) == len);
        } else if (stub_mode == STUB_MONITOR_SAME_BATCH) {
            write_child_text(pipe_fds[1], "PCMRemoved " TEST_PCM_PATH "\nPCMAdded " TEST_PCM_PATH "\n");
        } else if (stub_mode == STUB_MONITOR_SPLIT_MATCH) {
            write_child_text(pipe_fds[1], "PCMRemoved " TEST_PCM_PATH "\n");
            usleep(90000);
            write_child_text(pipe_fds[1], "PCMAdded " TEST_PCM_PATH "\n");
        } else if (stub_mode == STUB_MONITOR_FRAGMENTED_MATCH) {
            write_child_text(pipe_fds[1], "PCMRemoved " TEST_PCM_PATH "\nPCMAdded "
                                          "/org/bluealsa/hci0/dev_test/a2dpsrc/s");
            usleep(20000);
            write_child_text(pipe_fds[1], "ink\n");
        } else if (stub_mode == STUB_MONITOR_PERSISTENT_REMOVE ||
                   stub_mode == STUB_MONITOR_STOP_PENDING ||
                   stub_mode == STUB_MONITOR_UNRELATED_ADD ||
                   stub_mode == STUB_MONITOR_LATE_MATCH ||
                   stub_mode == STUB_MONITOR_EOF_PENDING) {
            write_child_text(pipe_fds[1], "PCMRemoved " TEST_PCM_PATH "\n");
            if (stub_mode == STUB_MONITOR_UNRELATED_ADD)
                write_child_text(pipe_fds[1], "PCMAdded " TEST_UNRELATED_PCM "\n");
            if (stub_mode == STUB_MONITOR_LATE_MATCH) {
                usleep(BT_OUTPUT_RECONFIGURE_GRACE_MS * 1000 + 100000);
                write_child_text(pipe_fds[1], "PCMAdded " TEST_PCM_PATH "\n");
            }
        }
        if (stub_mode == STUB_MONITOR_HOLD ||
            stub_mode == STUB_MONITOR_DISCONNECT ||
            stub_mode == STUB_MONITOR_VOLUME ||
            stub_mode == STUB_MONITOR_SAME_BATCH ||
            stub_mode == STUB_MONITOR_SPLIT_MATCH ||
            stub_mode == STUB_MONITOR_FRAGMENTED_MATCH ||
            stub_mode == STUB_MONITOR_PERSISTENT_REMOVE ||
            stub_mode == STUB_MONITOR_UNRELATED_ADD ||
            stub_mode == STUB_MONITOR_LATE_MATCH ||
            stub_mode == STUB_MONITOR_STOP_PENDING) {
            hold_child();
        }
        close(pipe_fds[1]);
        _exit(0);
    }

    close(pipe_fds[1]);
    *out_pid = pid;
    *out_read_fd = pipe_fds[0];
    atomic_fetch_add(&stub_spawn_count, 1);
    atomic_fetch_add(&stub_live_child_count, 1);
    return true;
}

void subprocess_terminate(pid_t pid) {
    assert(pid > 0);
    atomic_fetch_add(&stub_terminate_count, 1);
    (void) kill(pid, SIGTERM);
    int status;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    assert(result == pid);
    assert(WIFEXITED(status) || WIFSIGNALED(status));
    atomic_fetch_sub(&stub_live_child_count, 1);
    atomic_fetch_add(&stub_reaped_count, 1);
}

float audio_get_volume(void) {
    return stub_audio_volume;
}

void audio_set_volume(float volume) {
    stub_audio_volume = volume;
}

void hiby_sys_server_report_volume(int percent) {
    (void) percent;
}

static void reset_stub_counters(void) {
    atomic_store(&stub_spawn_count, 0);
    atomic_store(&stub_terminate_count, 0);
    atomic_store(&stub_reaped_count, 0);
    atomic_store(&stub_live_child_count, 0);
    atomic_store(&stub_list_pcms_calls, 0);
    atomic_store(&stub_volume_calls, 0);
    atomic_store(&stub_seed_volume_calls, 0);
    atomic_store(&stub_last_volume_raw, -1);
    atomic_store(&stub_gate_popen, false);
    atomic_store(&stub_popen_entered, false);
    atomic_store(&stub_release_popen, false);
    atomic_store(&stub_stop_call_started, false);
}

static void * stop_disconnect_watch(void * unused) {
    (void) unused;
    atomic_store(&stub_stop_call_started, true);
    bt_control_output_disconnect_watch_stop();
    return NULL;
}

static void test_disconnect_eof_and_pending_event(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_EOF;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    wait_for_true(disconnect_worker_inactive);
    bt_control_output_disconnect_watch_stop(); /* drains the EOF/HUP worker */
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
    assert(!bt_control_output_disconnect_consume());

    reset_stub_counters();
    stub_mode = STUB_MONITOR_DISCONNECT;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    bool pending = false;
    for (int i = 0; i < 300 && !pending; i++) {
        pending = bt_control_output_disconnect_consume();
        usleep(10000);
    }
    assert(pending);
    assert(!bt_control_output_disconnect_consume());
    bt_control_output_disconnect_watch_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void test_disconnect_stop_restart_cleanup(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_HOLD;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    bt_control_output_disconnect_watch_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);

    reset_stub_counters();
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    bt_control_output_disconnect_watch_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void test_disconnect_stop_during_spawn(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_HOLD;
    atomic_store(&stub_gate_popen, true);
    bt_control_output_disconnect_watch_start();
    wait_for_true(popen_entered);

    pthread_t stopper;
    assert(pthread_create(&stopper, NULL, stop_disconnect_watch, NULL) == 0);
    wait_for_true(stop_call_started);

    /* stop() has already cleared the active flag while the worker is still
     * blocked inside subprocess_popen(). Let the stub complete the spawn and
     * verify that the worker performs its own child cleanup before joining. */
    pthread_mutex_lock(&stub_gate_mutex);
    atomic_store(&stub_release_popen, true);
    pthread_cond_broadcast(&stub_gate_cond);
    pthread_mutex_unlock(&stub_gate_mutex);
    assert(pthread_join(stopper, NULL) == 0);
    assert(atomic_load(&stub_spawn_count) == 1);
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void assert_no_disconnect_for_ms(int duration_ms) {
    for (int elapsed = 0; elapsed < duration_ms; elapsed += 10) {
        assert(!bt_control_output_disconnect_consume());
        usleep(10000);
    }
}

static void stop_disconnect_clean(void) {
    bt_control_output_disconnect_watch_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void test_disconnect_reconfigure_grace_cases(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_SAME_BATCH;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    assert_no_disconnect_for_ms(900);
    stop_disconnect_clean();

    reset_stub_counters();
    stub_mode = STUB_MONITOR_SPLIT_MATCH;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    assert_no_disconnect_for_ms(900);
    stop_disconnect_clean();

    reset_stub_counters();
    stub_mode = STUB_MONITOR_FRAGMENTED_MATCH;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    assert_no_disconnect_for_ms(900);
    stop_disconnect_clean();

    reset_stub_counters();
    stub_mode = STUB_MONITOR_PERSISTENT_REMOVE;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    assert_no_disconnect_for_ms(500);
    usleep((BT_OUTPUT_RECONFIGURE_GRACE_MS - 500 + 150) * 1000);
    assert(bt_control_output_disconnect_consume());
    assert(!bt_control_output_disconnect_consume());
    stop_disconnect_clean();

    reset_stub_counters();
    stub_mode = STUB_MONITOR_UNRELATED_ADD;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    assert_no_disconnect_for_ms(500);
    usleep((BT_OUTPUT_RECONFIGURE_GRACE_MS - 500 + 150) * 1000);
    assert(bt_control_output_disconnect_consume());
    assert(!bt_control_output_disconnect_consume());
    stop_disconnect_clean();

    /* The matching addition arrives after confirmation but before the test
     * consumes it. It must retract the unconsumed confirmation. */
    reset_stub_counters();
    stub_mode = STUB_MONITOR_LATE_MATCH;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    usleep((BT_OUTPUT_RECONFIGURE_GRACE_MS + 250) * 1000);
    assert(!bt_control_output_disconnect_consume());
    stop_disconnect_clean();

    reset_stub_counters();
    stub_mode = STUB_MONITOR_STOP_PENDING;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    usleep(100000);
    stop_disconnect_clean();
    assert(!bt_control_output_disconnect_consume());

    /* EOF while a removal is pending is not itself a confirmed disconnect;
     * the normal polling fallback handles a missing PCM. */
    reset_stub_counters();
    stub_mode = STUB_MONITOR_EOF_PENDING;
    bt_control_output_disconnect_watch_start();
    wait_for_true(spawned_once);
    wait_for_true(disconnect_worker_inactive);
    bt_control_output_disconnect_watch_stop(); /* drains the EOF exit */
    assert(!bt_control_output_disconnect_consume());
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void test_volume_monitor_eof_stop_restart_and_event(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_EOF;
    bt_control_source_volume_sync_start();
    wait_for_true(spawned_once);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", TEST_PCM_PATH);
    atomic_store(&bt_source_soft_volume, false);
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    wait_for_true(volume_worker_inactive);
    /* Explicitly drain the joinable EOF worker before checking counters or
     * resetting the stub state for the next lifecycle phase. */
    bt_control_source_volume_sync_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);

    reset_stub_counters();
    stub_mode = STUB_MONITOR_HOLD;
    bt_control_source_volume_sync_start();
    wait_for_true(spawned_once);
    bt_control_source_volume_sync_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);

    reset_stub_counters();
    stub_mode = STUB_MONITOR_VOLUME;
    atomic_store(&stub_monitor_volume_raw, 80);
    atomic_store(&stub_live_volume_packed, (80 << 8) | 80);
    atomic_store(&stub_volume_readback_fail, false);
    atomic_store(&stub_soft_volume_apply_fail, false);
    stub_managed_objects = "object path \"/org/bluez/hci0/dev_test/sep1/fd0\"";
    stub_transport_props = "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\nstring \"Volume\"\n variant uint16 64\n";
    atomic_store(&bt_source_soft_volume, false);
    atomic_store(&modern_soft_volume_requested, false);
    bt_soft_volume_applied_path[0] = '\0';
    bt_control_source_volume_sync_start();
    wait_for_true(spawned_once);
    pthread_mutex_lock(&bt_soft_volume_mutex);
    snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", TEST_PCM_PATH);
    atomic_store(&bt_source_soft_volume, false);
    pthread_mutex_unlock(&bt_soft_volume_mutex);
    int percent = -1;
    for (int i = 0; i < 300 && percent < 0; i++) {
        (void) bt_control_source_volume_sync_consume_percent(&percent);
        usleep(10000);
    }
    assert(percent == 63); /* 80 / 127, rounded to a player percentage */
    assert(stub_audio_volume > 0.62f && stub_audio_volume < 0.64f);
    stub_managed_objects = "";
    stub_transport_props = "";
    bt_control_source_volume_sync_stop();
    assert(atomic_load(&stub_terminate_count) == 1);
    assert(atomic_load(&stub_reaped_count) == 1);
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void run_volume_adoption_case(bool fail_apply, bool fail_readback,
                                     int event_raw, int live_raw, float expected) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_VOLUME;
    atomic_store(&stub_monitor_volume_raw, event_raw);
    atomic_store(&stub_live_volume_packed, (live_raw << 8) | live_raw);
    atomic_store(&stub_volume_readback_fail, fail_readback);
    atomic_store(&stub_soft_volume_apply_fail, fail_apply);
    stub_managed_objects = "object path \"/org/bluez/hci0/dev_test/sep1/fd0\"";
    stub_transport_props = "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\nstring \"Volume\"\n variant uint16 64\n";
    atomic_store(&bt_source_soft_volume, true);
    atomic_store(&modern_soft_volume_requested, false);
    atomic_store(&bt_source_vol_pending_percent, -1);
    bt_soft_volume_applied_path[0] = '\0';
    stub_audio_volume = 0.50f;
    bt_control_source_volume_sync_start();
    wait_for_true(spawned_once);
    if (!fail_apply) {
        pthread_mutex_lock(&bt_soft_volume_mutex);
        snprintf(bt_soft_volume_applied_path, sizeof(bt_soft_volume_applied_path), "%s", TEST_PCM_PATH);
        atomic_store(&bt_source_soft_volume, false);
        pthread_mutex_unlock(&bt_soft_volume_mutex);
    }
    usleep(250000);
    bt_control_source_volume_sync_stop();
    assert(stub_audio_volume == expected);
    if (expected == 0.50f) {
        int percent = -1;
        assert(!bt_control_source_volume_sync_consume_percent(&percent));
    }
    atomic_store(&stub_volume_readback_fail, false);
    atomic_store(&stub_soft_volume_apply_fail, false);
}

static void test_volume_adoption_requires_ready_and_live_match(void) {
    /* A clear/unready PCM must not accept an event during reseeding. */
    run_volume_adoption_case(true, false, 80, 80, 0.50f);
    /* A queued old 127 event cannot override the new PCM's seeded 32. */
    run_volume_adoption_case(false, false, 127, 32, 0.50f);
    /* A current hardware event is adopted when the PCM reports that value. */
    run_volume_adoption_case(false, false, 80, 80, 80.0f / 127.0f);
    /* R1 BlueALSA 5.0 formats Volume as a two-byte stereo array. */
    atomic_store(&stub_live_volume_as_bytes, true);
    run_volume_adoption_case(false, false, 28, 80, 28.0f / 127.0f);
    atomic_store(&stub_live_volume_as_bytes, false);
    /* Unknown live state is never treated as an accessory update. */
    run_volume_adoption_case(false, true, 80, 80, 0.50f);
}

static void test_source_volume_reply_parser(void) {
    int volume = -1;
    assert(bluealsa_parse_source_volume_reply(
        "method return sender=:1.2 -> dest=:1.4 reply_serial=2\n   variant       array of bytes [\n         1c 1c\n      ]\n", &volume));
    assert(volume == 28);
    assert(bluealsa_parse_source_volume_reply("variant uint16 2056\n", &volume));
    assert(volume == 8);
    assert(!bluealsa_parse_source_volume_reply("variant array of bytes [ 1c ]\n", &volume));
    assert(!bluealsa_parse_source_volume_reply("variant array of bytes [ 1c 1c 1c ]\n", &volume));
    assert(!bluealsa_parse_source_volume_reply("variant array of bytes [ 1c 9c ]\n", &volume));
    assert(!bluealsa_parse_source_volume_reply("variant array of bytes [ xx 1c ]\n", &volume));
    assert(!bluealsa_parse_source_volume_reply("variant array of bytes [ 1c 1c ] junk\n", &volume));
}

static void test_software_gain_ignores_inbound_volume_but_pushes_app_value(void) {
    reset_stub_counters();
    stub_mode = STUB_MONITOR_VOLUME;
    atomic_store(&bt_source_soft_volume, true);
    atomic_store(&modern_soft_volume_requested, true);
    stub_audio_volume = 0.25f;
    bt_control_source_volume_sync_start();
    wait_for_true(spawned_once);
    /* The app's selected level is still sent to the source PCM even though
     * the accessory's inbound AVRCP report must not replace it. */
    for (int i = 0; i < 300 && atomic_load(&stub_volume_calls) == 0; i++) usleep(10000);
    assert(atomic_load(&stub_volume_calls) >= 1);
    assert(atomic_load(&stub_last_volume_raw) == 32);
    usleep(100000);
    assert(stub_audio_volume == 0.25f);
    int percent = -1;
    assert(!bt_control_source_volume_sync_consume_percent(&percent));
    bt_control_source_volume_sync_stop();
    assert(atomic_load(&stub_live_child_count) == 0);
}

static void test_unchanged_app_volume_still_discovers_recreated_pcm(void) {
    reset_stub_counters();
    atomic_store(&stub_source_pcm_present, false);
    stub_audio_volume = 0.50f;
    float last_synced_app_percent = 0.50f;
    bt_source_vol_last_synced_raw = 64;
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);
    assert(atomic_load(&stub_list_pcms_calls) == 1);
    assert(atomic_load(&stub_volume_calls) == 0);
    atomic_store(&stub_source_pcm_present, true);
}

static void test_unchanged_volume_reseeds_recreated_same_path_pcm(void) {
    reset_stub_counters();
    atomic_store(&stub_source_pcm_present, true);
    atomic_store(&bt_source_soft_volume, true);
    atomic_store(&modern_soft_volume_requested, true);
    snprintf(bt_source_vol_pcm_path, sizeof(bt_source_vol_pcm_path), "%s", TEST_PCM_PATH);
    bt_source_vol_last_synced_raw = 64;
    stub_audio_volume = 0.50f;
    float last_synced_app_percent = 0.50f;

    /* Discovery applies and seeds the currently selected software mode even
     * though the PCM path and app percentage match the prior values. */
    bt_soft_volume_applied_path[0] = '\0';
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);
    assert(atomic_load(&stub_volume_calls) == 1);
    assert(atomic_load(&stub_last_volume_raw) == 64);

    /* A same-path remove/recreate clears the applied latch. The unchanged
     * app value must seed the new PCM before output resumes. */
    bluealsa_clear_soft_volume_path(TEST_PCM_PATH);
    bt_source_push_app_volume_if_changed(&last_synced_app_percent);
    assert(atomic_load(&stub_volume_calls) == 2);
    assert(atomic_load(&stub_last_volume_raw) == 64);
    assert(stub_audio_volume == 0.50f);
}

int main(void) {
    test_source_volume_reply_parser();
    test_disconnect_eof_and_pending_event();
    test_disconnect_stop_restart_cleanup();
    test_disconnect_stop_during_spawn();
    test_disconnect_reconfigure_grace_cases();
    test_volume_monitor_eof_stop_restart_and_event();
    test_volume_adoption_requires_ready_and_live_match();
    test_software_gain_ignores_inbound_volume_but_pushes_app_value();
    test_unchanged_app_volume_still_discovers_recreated_pcm();
    test_unchanged_volume_reseeds_recreated_same_path_pcm();
    puts("bluetooth monitor lifecycle tests passed");
    return 0;
}
