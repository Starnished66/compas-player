/* Host regression tests against the real BlueALSA 5 implementation. Unused
 * device code is discarded by gc-sections. */
#define _GNU_SOURCE
#include <stdio.h>
#ifndef BT_CODEC_TEST_PATH
#define BT_CODEC_TEST_PATH "/tmp/ldacdec_fixture_XXXXXX"
#endif
/* Fixture for the LDAC decoder check; no library by default. */
static const char * test_ldac_decoder_path = "/nonexistent/libldacdec.so.1";
#define BT_LDAC_DECODER_PATH test_ldac_decoder_path
#include "bluetooth_control.c"
#include <assert.h>
#include <errno.h>

static const char * process_cmdline;
static size_t process_cmdline_size;
static bool process_present, check_lock;
static bool retain_process_after_kill;
static unsigned proc_index, kill_calls, spawn_calls, soft_volume_calls;
static unsigned soft_volume_checked_calls;
static unsigned seed_volume_calls, managed_objects_calls, transport_getall_calls;
static char seeded_path[256];
static int seeded_raw = -1;
static bool soft_volume_fail;
static bool soft_volume_readback_known;
static bool soft_volume_readback_value;
static bool source_pcm_present;
static bool source_volume_supported;
static bool source_volume_malformed;
static bool source_volume_no_volume;
static bool source_volume_wrong_device;
static bool nested_transport;
static bool source_running;
static bool seed_volume_fail;
static float stub_audio_volume = 0.40f;
static enum { BLUEZ_CLI_MODERN, BLUEZ_CLI_LEGACY, BLUEZ_CLI_EMPTY_HELP,
              BLUEZ_CLI_TRUNCATED_HELP } bluez_cli_mode;
static unsigned bluez_help_calls, bluez_help_failures, bluez_paired_calls;
static char soft_volume_state[8];
static char soft_volume_path[256];
static char discovered_source_path[256];
static char spawned[10][128];
static size_t spawned_argc;
static struct dirent proc_entry;
static unsigned codec_select_calls;
static char last_codec_applied_path[256];
static char last_codec_applied_name[64];
static const char * test_selected_codec;
static const char * test_available_codecs;
static const char * test_codec_select_fail_name;
static bool test_codec_select_fail_ldac;
static const char * test_codec_select_mismatch_name;
static const char * test_bluetooth_alias;
static unsigned bluetooth_info_checked_calls;
static bool bluetooth_info_fail_once;
static bool daemon_help_supports_ssc_auto_profile;
static unsigned daemon_help_calls;
static char test_source_pcm_path[256];
static unsigned test_transport_rate = 44100;
static unsigned rate_select_calls, reconnect_disconnect_calls, reconnect_connect_calls;

static void expect_locked(void) {
    if (check_lock) assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == EBUSY);
}

DIR * __wrap_opendir(const char * path) {
    assert(strcmp(path, "/proc") == 0);
    expect_locked();
    proc_index = 0;
    return (DIR *)&proc_entry;
}

struct dirent * __wrap_readdir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    if (proc_index++ || !process_present) return NULL;
    strcpy(proc_entry.d_name, "12345");
    return &proc_entry;
}

int __wrap_closedir(DIR * dir) {
    assert(dir == (DIR *)&proc_entry);
    return 0;
}

static void capture_argv(char * const argv[]) {
    expect_locked();
    spawn_calls++;
    spawned_argc = 0;
    while (argv[spawned_argc]) {
        assert(spawned_argc < 10);
        assert(strlen(argv[spawned_argc]) < sizeof(spawned[0]));
        strcpy(spawned[spawned_argc], argv[spawned_argc]);
        spawned_argc++;
    }
}

static bool spawned_has_arg(const char *arg) {
    for (size_t i = 0; i < spawned_argc; i++)
        if (strcmp(spawned[i], arg) == 0) return true;
    return false;
}

void subprocess_kill_all_matching(const char * needle) {
    expect_locked();
    kill_calls++;
    if (strcmp(needle, "bluealsad") == 0 && !retain_process_after_kill)
        process_present = false;
}

bool subprocess_spawn_daemon(char * const argv[]) {
    capture_argv(argv);
    return true;
}

bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path) {
    (void)log_path;
    capture_argv(argv);
    /* Stop apply_output_settings after constructing the actual daemon argv. */
    return false;
}

bool subprocess_run_checked(char * const argv[], char * out, size_t size,
                            int timeout_ms, int * out_exit_code) {
    expect_locked();
    assert(out_exit_code);
    if (strcmp(argv[0], "dbus-send") == 0) {
        if (strstr(argv[5], "GetManagedObjects")) {
            managed_objects_calls++;
            if (out && size) {
                const char * device = source_volume_wrong_device ? "dev_other" : "dev_source";
                const char * child = nested_transport ? "sep1/fd0" : "fd0";
                snprintf(out, size, "object path \"/org/bluez/hci0/%s/%s\"\n", device, child);
            }
        } else {
            transport_getall_calls++;
            if (out && size) {
                if (source_volume_malformed)
                    snprintf(out, size, "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\nstring \"Volume\"\n variant uint16 junk\n");
                else if (source_volume_no_volume)
                    snprintf(out, size, "string \"UUID\"\n variant string \"0000110a-0000-1000-8000-00805f9b34fb\"\n");
                else if (source_volume_supported)
                    snprintf(out, size, "string \"UUID\"\n variant string \"%s\"\nstring \"Volume\"\n variant uint16 88\n",
                             source_volume_wrong_device ? "0000110a-0000-1000-8000-00805f9b34fb" :
                             "0000110a-0000-1000-8000-00805f9b34fb");
                else snprintf(out, size, "string \"UUID\"\n variant string \"0000110b-0000-1000-8000-00805f9b34fb\"\nstring \"Volume\"\n variant uint16 88\n");
            }
        }
        *out_exit_code = 0;
        return true;
    }
    if (strcmp(argv[0], "bluetoothctl") == 0 && strcmp(argv[1], "info") == 0) {
        bluetooth_info_checked_calls++;
        assert(timeout_ms == 1000 && argv[2] && argv[3] == NULL);
        if (bluetooth_info_fail_once) {
            bluetooth_info_fail_once = false;
            return false;
        }
        snprintf(out, size, "Device %s\n\tAlias: %s\n", argv[2],
                 test_bluetooth_alias ? test_bluetooth_alias : "Headphones");
        *out_exit_code = 0;
        return true;
    }
    if (strcmp(argv[0], "/usr/bin/bluealsad") == 0 && strcmp(argv[1], "--help") == 0) {
        daemon_help_calls++;
        assert(timeout_ms == 1000 && argv[2] == NULL && out && size);
        snprintf(out, size, "BlueALSA options\n  %s\n",
                 daemon_help_supports_ssc_auto_profile ? "--ssc-auto-profile" : "--ssc-auto-profile-extra");
        *out_exit_code = 0;
        return true;
    }
    assert(strcmp(argv[0], "/usr/bin/bluealsactl") == 0);
    if (strcmp(argv[1], "soft-volume") == 0) {
        assert(out == NULL && size == 0 && timeout_ms == 5000);
        soft_volume_checked_calls++;
        snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
        snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
        *out_exit_code = soft_volume_fail ? 1 : 0;
        return true; /* Exercise callers that must inspect the exit status. */
    }
    if (strcmp(argv[1], "volume") == 0) {
        assert(strstr(argv[2], "/a2dpsrc/sink") != NULL);
        snprintf(seeded_path, sizeof(seeded_path), "%s", argv[2]);
        seeded_raw = atoi(argv[3]);
        seed_volume_calls++;
        *out_exit_code = seed_volume_fail ? 1 : 0;
        return true;
    }
    if (strcmp(argv[1], "codec") == 0) {
        if (strcmp(argv[2], "-r") == 0) {
            rate_select_calls++;
            test_transport_rate = (unsigned) strtoul(argv[3], NULL, 10);
            *out_exit_code = 0;
            return true;
        }
        codec_select_calls++;
        snprintf(last_codec_applied_path, sizeof(last_codec_applied_path), "%s", argv[2]);
        snprintf(last_codec_applied_name, sizeof(last_codec_applied_name), "%s", argv[3]);
        if ((test_codec_select_fail_name && strcmp(argv[3], test_codec_select_fail_name) == 0) ||
            (test_codec_select_fail_ldac && strcmp(argv[3], "LDAC") == 0)) {
            *out_exit_code = 1;
            return true;
        }
        test_selected_codec = test_codec_select_mismatch_name &&
                              strcmp(argv[3], test_codec_select_mismatch_name) == 0 ?
                              "AAC" : last_codec_applied_name;
        *out_exit_code = 0;
        return true;
    }
    /* Capability probing uses dbus-send via argv[0]; it is handled by a
     * separate branch below before the bluealsactl check above. */
    assert(!"unexpected checked command");
    return false;
}

float audio_get_volume(void) { return stub_audio_volume; }
void audio_set_volume(float volume) { stub_audio_volume = volume; }
void hiby_sys_server_report_volume(int percent) { (void)percent; }

/* Same fixture as subprocess_run(): the production code niced this call
 * down, which changes the child's scheduling priority, nothing the test
 * fixture models. */
bool subprocess_run_low_priority(char * const argv[], char * out, size_t size) {
    return subprocess_run(argv, out, size);
}

bool subprocess_run(char * const argv[], char * out, size_t size) {
    expect_locked();
    if (strcmp(argv[0], "bluetoothctl") == 0) {
        assert(out && size);
        if (strcmp(argv[1], "--help") == 0) {
            assert(argv[2] == NULL);
            bluez_help_calls++;
            if (bluez_help_failures) {
                bluez_help_failures--;
                return false;
            }
            if (bluez_cli_mode == BLUEZ_CLI_EMPTY_HELP) {
                out[0] = '\0';
            } else if (bluez_cli_mode == BLUEZ_CLI_TRUNCATED_HELP) {
                snprintf(out, size, "Commands:\n\tdevices\t\tList available");
            } else {
                snprintf(out, size, "%s", bluez_cli_mode == BLUEZ_CLI_MODERN ?
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tselect\t\tSelect default controller\n"
                         "\tdevices\t\tList available devices, with an optional property as the filter\n" :
                         "Commands:\n"
                         "\tlist\t\tList available controllers\n"
                         "\tshow\t\tController information\n"
                         "\tpaired-devices\tList paired devices\n");
                if (bluez_cli_mode == BLUEZ_CLI_MODERN) {
                    /* The real 5.87 --help is 9151 bytes; keep this test
                     * from accidentally validating an undersized capture. */
                    assert(size > 9151);
                    size_t len = strlen(out);
                    memset(out + len, ' ', 9150 - len);
                    out[9150] = '\n';
                    out[9151] = '\0';
                }
            }
            return true;
        }
        if (strcmp(argv[1], "devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_MODERN);
            assert(strcmp(argv[2], "Paired") == 0 && argv[3] == NULL);
        } else if (strcmp(argv[1], "paired-devices") == 0) {
            assert(bluez_cli_mode == BLUEZ_CLI_LEGACY);
            assert(argv[2] == NULL);
        } else if (strcmp(argv[1], "disconnect") == 0) {
            reconnect_disconnect_calls++;
            source_pcm_present = false;
            snprintf(out, size, "Successful disconnected\n");
            return true;
        } else if (strcmp(argv[1], "connect") == 0) {
            reconnect_connect_calls++;
            source_pcm_present = true;
            snprintf(out, size, "Connection successful\n");
            return true;
        } else if (strcmp(argv[1], "pair") == 0 || strcmp(argv[1], "trust") == 0) {
            out[0] = '\0';
            return true;
        } else {
            assert(strcmp(argv[1], "info") == 0);
            assert(argv[2] && argv[3] == NULL);
            snprintf(out, size, "Device %s\n\tName: Headphones\n%s%s%s\tPaired: yes\n\tConnected: %s\n",
                     argv[2], test_bluetooth_alias ? "\tAlias: " : "",
                     test_bluetooth_alias ? test_bluetooth_alias : "",
                     test_bluetooth_alias ? "\n" : "", strstr(argv[2], "02") ? "yes" : "no");
            return true;
        }
        bluez_paired_calls++;
        snprintf(out, size,
                 "Device AA:BB:CC:DD:EE:01 Headphones\n"
                 "Device AA:BB:CC:DD:EE:02 Backup Headphones\n");
        return true;
    }
    if (strcmp(argv[0], "/usr/bin/bluealsactl") == 0) {
        if (strcmp(argv[1], "list-pcms") == 0) {
            assert(out && size);
            const char *path = test_source_pcm_path[0] ? test_source_pcm_path :
                               "/org/bluealsa/hci0/dev_source/a2dpsrc/sink";
            if (source_pcm_present) snprintf(out, size, "%s\n", path);
            else out[0] = '\0';
            return true;
        }
        if (strcmp(argv[1], "info") == 0) {
            assert(out && size);
            snprintf(out, size, "Selected codec: %s\nAvailable codecs: %s\nRate: %u\nRunning: %s\n",
                     test_selected_codec ? test_selected_codec : "SBC",
                     test_available_codecs ? test_available_codecs : "SBC",
                     test_transport_rate,
                     source_running ? "true" : "false");
            return true;
        }
        if (strcmp(argv[1], "soft-volume") == 0 && argv[3] == NULL) {
            if (!soft_volume_readback_known) return false;
            assert(out && size);
            snprintf(out, size, "SoftVolume: %s\n", soft_volume_readback_value ? "true" : "false");
            return true;
        }
        assert(strcmp(argv[1], "soft-volume") == 0);
        /* Current-state query: unknown here, so the setting is written. */
        if (argv[3] == NULL) return false;
        assert(strcmp(argv[3], "on") == 0 || strcmp(argv[3], "off") == 0);
        soft_volume_calls++;
        snprintf(soft_volume_path, sizeof(soft_volume_path), "%s", argv[2]);
        snprintf(soft_volume_state, sizeof(soft_volume_state), "%s", argv[3]);
        return !soft_volume_fail;
    }
    assert(strcmp(argv[0], "ps") == 0);
    assert(out && size);
    if (process_present)
        snprintf(out, size, "12345 %s\n", process_cmdline ? process_cmdline :
                 "bluealsad");
    else out[0] = '\0';
    return true;
}

static void test_source_absolute_volume_capability(void) {
    char path[] = "/org/bluealsa/hci0/dev_source/a2dpsrc/sink";
    source_volume_supported = true;
    source_volume_malformed = false;
    source_volume_wrong_device = false;
    source_running = false;
    nested_transport = false;
    managed_objects_calls = transport_getall_calls = 0;
    assert(bluealsa_source_has_absolute_volume(path));
    assert(managed_objects_calls == 1 && transport_getall_calls == 1);
    nested_transport = true;
    assert(bluealsa_source_has_absolute_volume(path));
    assert(transport_getall_calls == 2);
    source_volume_supported = false; /* no sink transport: unsupported */
    assert(!bluealsa_source_has_absolute_volume(path));
    source_volume_supported = true;
    source_volume_no_volume = true;
    assert(!bluealsa_source_has_absolute_volume(path));
    source_volume_no_volume = false;
    source_volume_supported = true;
    source_volume_malformed = true;
    assert(!bluealsa_source_has_absolute_volume(path));
    source_volume_malformed = false;
    source_volume_wrong_device = true;
    transport_getall_calls = 0;
    assert(!bluealsa_source_has_absolute_volume(path));
    assert(transport_getall_calls == 0);
    source_volume_wrong_device = false;
}

static void test_source_software_seed_and_hardware_mode(void) {
    const char *path = "/org/bluealsa/hci0/dev_source/a2dpsrc/sink";
    source_pcm_present = true;
    source_volume_supported = false;
    source_volume_malformed = false;
    source_volume_wrong_device = false;
    source_running = true;
    stub_audio_volume = 0.40f;
    seed_volume_calls = soft_volume_checked_calls = 0;
    bt_soft_volume_applied_path[0] = '\0';
    atomic_store(&modern_soft_volume_requested, false);
    atomic_store(&bt_source_soft_volume, true);
    assert(find_source_pcm_path(discovered_source_path, sizeof(discovered_source_path)));
    assert(seed_volume_calls == 1 && soft_volume_checked_calls == 1);
    assert(atomic_load(&bt_source_soft_volume));
    /* The running PCM remains on its established software path until it is
     * recreated; probe support cannot switch gain owners mid-stream. */
    source_volume_supported = true;
    bluealsa_clear_soft_volume_path(NULL);
    source_running = true;
    bluealsa_apply_soft_volume(path);
    assert(atomic_load(&bt_source_soft_volume));
    source_running = false;
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume(path);
    assert(!atomic_load(&bt_source_soft_volume));
    assert(atomic_load(&modern_soft_volume_requested) == false);
    source_pcm_present = false;
}

static void test_source_volume_seed_readiness(void) {
    source_pcm_present = true;
    source_volume_supported = false;
    source_volume_malformed = false;
    source_volume_wrong_device = false;
    source_running = true;
    bt_soft_volume_applied_path[0] = '\0';
    seed_volume_fail = true;
    assert(!bt_control_prepare_source_volume());
    assert(bt_soft_volume_applied_path[0] == '\0');
    seed_volume_fail = false;
    assert(bt_control_prepare_source_volume());
    assert(strcmp(bt_soft_volume_applied_path,
                  "/org/bluealsa/hci0/dev_source/a2dpsrc/sink") == 0);
    source_pcm_present = false;
}

static void test_prepare_reprobes_avrcp_before_first_audio_open(void) {
    source_pcm_present = true;
    source_volume_supported = false;
    source_volume_malformed = false;
    source_volume_no_volume = false;
    source_volume_wrong_device = false;
    source_running = true;
    managed_objects_calls = transport_getall_calls = 0;
    seed_volume_calls = 0;
    seeded_raw = -1;
    soft_volume_readback_known = false;
    seed_volume_fail = false;
    stub_audio_volume = 0.63f;
    bt_source_volume_probe_ms = bt_control_monotonic_ms();
    atomic_store(&modern_soft_volume_requested, false);
    atomic_store(&bt_source_soft_volume, true);
    bt_soft_volume_applied_path[0] = '\0';

    /* Initial PCM discovery happens before AVRCP is ready, so it must choose
     * the safe software-gain fallback and seed the current app value. */
    assert(bt_control_prepare_source_volume());
    assert(atomic_load(&bt_source_soft_volume));
    assert(strcmp(soft_volume_state, "on") == 0);
    assert(seed_volume_calls == 1);
    assert(managed_objects_calls == 1);

    /* AVRCP arrives within the negative-probe cache interval. While the PCM
     * is still live, preparation must retain the existing software owner. */
    source_volume_supported = true;
    atomic_store(&modern_soft_volume_requested, true); /* explicit volume-sync off */
    assert(bt_control_prepare_source_volume());
    assert(atomic_load(&bt_source_soft_volume));
    assert(seed_volume_calls == 1);

    /* With sync enabled, a forced reprobe still respects the live PCM guard. */
    atomic_store(&modern_soft_volume_requested, false);
    assert(bt_control_prepare_source_volume());
    assert(atomic_load(&bt_source_soft_volume));
    assert(strcmp(soft_volume_state, "on") == 0);
    assert(seed_volume_calls == 2);

    /* Once stopped, the same real prepare-before-open path must recheck
     * immediately, transfer gain to hardware, and seed the latest app value. */
    source_running = false;
    assert(bt_control_prepare_source_volume());
    assert(!atomic_load(&bt_source_soft_volume));
    assert(strcmp(soft_volume_state, "off") == 0);
    assert(seed_volume_calls == 3);
    assert(strcmp(seeded_path, "/org/bluealsa/hci0/dev_source/a2dpsrc/sink") == 0);
    assert(seeded_raw == 80); /* latest 63% app setting, rounded on the 0-127 scale */
    assert(bt_soft_volume_applied_path[0] != '\0');
    unsigned objects_before_ready_check = managed_objects_calls;
    unsigned seeds_before_ready_check = seed_volume_calls;
    assert(bt_control_prepare_source_volume());
    assert(managed_objects_calls == objects_before_ready_check);
    assert(seed_volume_calls == seeds_before_ready_check);

    source_pcm_present = false;
    source_volume_supported = false;
    source_running = false;
    soft_volume_readback_known = false;
}

static void test_late_hardware_upgrade_seed_failure_clears_old_latch(void) {
    const char *path = "/org/bluealsa/hci0/dev_source/a2dpsrc/sink";
    source_pcm_present = true;
    source_volume_supported = false;
    source_volume_malformed = false;
    source_volume_no_volume = false;
    source_volume_wrong_device = false;
    source_running = true;
    soft_volume_readback_known = true;
    soft_volume_readback_value = true;
    seed_volume_fail = false;
    bt_soft_volume_applied_path[0] = '\0';
    atomic_store(&modern_soft_volume_requested, false);
    atomic_store(&bt_source_soft_volume, true);

    /* Start with a correctly seeded software-volume PCM and an applied-path
     * latch. A later positive capability probe must invalidate that latch
     * before trying to change modes and seed the new gain owner. */
    assert(bt_control_prepare_source_volume());
    assert(strcmp(bt_soft_volume_applied_path, path) == 0);
    assert(atomic_load(&bt_source_soft_volume));

    source_volume_supported = true;
    source_running = false;
    bt_source_volume_probe_ms = bt_control_monotonic_ms() - 5000;
    seed_volume_fail = true;
    assert(!bt_control_prepare_source_volume());
    assert(bt_soft_volume_applied_path[0] == '\0');
    assert(atomic_load(&bt_source_soft_volume));

    soft_volume_readback_known = false;
    seed_volume_fail = false;
    source_pcm_present = false;
}

bool subprocess_popen(char * const argv[], pid_t * pid, int * fd) {
    (void)argv; (void)pid; (void)fd;
    assert(!"unexpected monitor process");
    return false;
}

void subprocess_terminate(pid_t pid) {
    (void)pid;
    assert(!"unexpected process termination");
}

int __wrap_pthread_create(pthread_t * thread, const pthread_attr_t * attr,
                          void * (*start)(void *), void * arg) {
    (void)thread; (void)attr; (void)start; (void)arg;
    assert(!"unexpected monitor thread");
    return EAGAIN;
}

FILE * __real_fopen(const char * path, const char * mode);

FILE * __wrap_fopen(const char * path, const char * mode) {
    if (strncmp(path, "/proc/", 6) == 0) {
        assert(strcmp(path, "/proc/12345/cmdline") == 0);
        assert(strcmp(mode, "r") == 0);
        expect_locked();
        return fmemopen((void *)process_cmdline, process_cmdline_size, "r");
    }
    return __real_fopen(path, mode);
}

/* Automatic source: every codec, LDAC at high quality with adaptive bitrate. */
#define AUTO_ARGS "--all-codecs\0--ldac-abr\0--ldac-quality\0high\0"
static void assert_auto_codec_args(size_t * n) {
    assert(strcmp(spawned[(*n)++], "--all-codecs") == 0);
    assert(strcmp(spawned[(*n)++], "--ldac-abr") == 0);
    assert(strcmp(spawned[(*n)++], "--ldac-quality") == 0 && strcmp(spawned[(*n)++], "high") == 0);
}

/* DAC mode always offers aptX and aptX-HD on top of BlueALSA's defaults. */
static void assert_dac_stable_codecs(size_t * n) {
    assert(strcmp(spawned[(*n)++], "-c") == 0 && strcmp(spawned[(*n)++], "aptX") == 0);
    assert(strcmp(spawned[(*n)++], "-c") == 0 && strcmp(spawned[(*n)++], "aptX-HD") == 0);
}

static void test_daemon_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0" AUTO_ARGS;
    static const char sink[] = "bluealsad\0-p\0a2dp-sink\0";
    static const char other[] = "/usr/bin/bluealsa-aplay\0-p\0a2dp-source\0";
    const struct { const char * args; size_t size; bool present; } cases[] = {
        {source, sizeof(source), true},
        {sink, sizeof(sink), true},
        {other, sizeof(other), true},
        {source, sizeof(source), false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        process_cmdline = cases[i].args;
        process_cmdline_size = cases[i].size;
        process_present = cases[i].present;
        kill_calls = spawn_calls = 0;
        check_lock = true;
        ensure_bluealsa_running();
        check_lock = false;
        assert(kill_calls == 0);
        assert(spawn_calls == (unsigned)(!cases[i].present || cases[i].args == other));
        if (spawn_calls) {
            assert(spawned_argc == 7);
            assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
            assert(strcmp(spawned[1], "-p") == 0);
            assert(strcmp(spawned[2], "a2dp-source") == 0);
            size_t n = 3;
            assert_auto_codec_args(&n);
        }
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }

    for (int dac = 0; dac < 2; dac++) for (int volume = 0; volume < 2; volume++) {
        kill_calls = spawn_calls = 0;
        check_lock = true;
        assert(!bt_control_apply_output_settings(dac, volume));
        check_lock = false;
        assert(spawn_calls == 1 && kill_calls == 3);
        assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
        assert(strcmp(spawned[1], "-p") == 0);
        assert(strcmp(spawned[2], dac ? "a2dp-sink" : "a2dp-source") == 0);
        size_t n = 3;
        if (!dac) assert_auto_codec_args(&n);
        else assert_dac_stable_codecs(&n);
        assert(spawned_argc == n);
        assert(pthread_mutex_trylock(&bt_daemon_respawn_mutex) == 0);
        pthread_mutex_unlock(&bt_daemon_respawn_mutex);
    }
}

static void test_sbc_xq_lifecycle(void) {
    static const char modern_auto[] = "bluealsad\0-p\0a2dp-source\0";
    static const char modern_xq[] = "bluealsad\0-p\0a2dp-source\0--sbc-quality=xq\0";
    static const char modern_xq_split[] = "bluealsad\0--profile\0a2dp-source\0--sbc-quality\0xq\0";
    static const char modern_sink_profile[] = "bluealsad\0--profile=a2dp-sink\0";
    static const char modern_sink[] = "bluealsad\0-p\0a2dp-sink\0";

    bt_control_restore_codec_preference("sbc_xq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    /* Source reconciliation restarts stale quality settings and is stable
     * when the already-running source is configured correctly. */
    process_cmdline = modern_auto; process_cmdline_size = sizeof(modern_auto);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    retain_process_after_kill = false;
    process_cmdline = modern_xq; process_cmdline_size = sizeof(modern_xq);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_xq_split; process_cmdline_size = sizeof(modern_xq_split);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    process_cmdline = modern_sink_profile; process_cmdline_size = sizeof(modern_sink_profile);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Modern profile changes use the modern daemon and still keep XQ off the
     * receiver/sink profile. */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[3], "--sbc-quality=xq") == 0);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && kill_calls == 3 && spawned_argc == 7);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    size_t n = 3;
    assert_dac_stable_codecs(&n);
    /* No decoder is installed here, so LDAC is excluded from the sink. */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    /* With the rebuilt decoder installed, LDAC is offered too. The marker
     * straddles the reader's 4096-byte chunk to cover the carried tail. */
    char decoder_path[] = BT_CODEC_TEST_PATH;
    int decoder_fd = mkstemp(decoder_path);
    assert(decoder_fd >= 0);
    FILE * decoder = fdopen(decoder_fd, "wb");
    assert(decoder);
    for (int k = 0; k < 4090; k++) fputc(k & 1 ? '\0' : 'x', decoder);
    fputs("ldacDecode_type_len", decoder);
    fclose(decoder);
    test_ldac_decoder_path = decoder_path;
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 9);
    assert(strcmp(spawned[7], "-c") == 0 && strcmp(spawned[8], "LDAC") == 0);
    /* The stock decoder has no marker, so LDAC remains excluded. */
    decoder = fopen(decoder_path, "wb"); /* stock decoder: no marker */
    assert(decoder);
    fputs("ldacDecode_type", decoder);
    fclose(decoder);
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    unlink(decoder_path);
    test_ldac_decoder_path = "/nonexistent/libldacdec.so.1";
    process_cmdline = modern_sink; process_cmdline_size = sizeof(modern_sink);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    assert(bt_control_set_codec("sbc_xq"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    assert(!bt_control_set_codec("not-a-codec"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=SBC") == 0);
    bt_control_restore_codec_preference("auto");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    bt_control_restore_codec_preference("aptx");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=aptX") == 0);
    bt_control_restore_codec_preference("aac");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=AAC") == 0);
    bt_control_restore_codec_preference("ldac_hq");
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa:CODEC=LDAC") == 0);
    assert(bt_control_set_codec("ssc"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    bt_control_restore_codec_preference("auto");
}

static void test_modern_argv(void) {
    static const char source[] = "bluealsad\0-p\0a2dp-source\0";
    process_cmdline = source;
    process_cmdline_size = sizeof(source);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 1);
    assert(spawned_argc == 7);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    {
        size_t n = 3;
        assert_auto_codec_args(&n);
    }

    soft_volume_calls = 0;
    soft_volume_checked_calls = 0;
    atomic_store(&modern_soft_volume_requested, true);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    assert(soft_volume_calls == 0);
    assert(strcmp(soft_volume_state, "on") == 0);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    assert(soft_volume_checked_calls == 1);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2);
    bluealsa_clear_soft_volume_path("/org/bluealsa/hci0/dev_old/a2dpsnk/source");
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 2); /* stale PCMRemoved must not evict new PCM */
    atomic_store(&modern_soft_volume_requested, false);
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 3 && strcmp(soft_volume_state, "off") == 0);
    soft_volume_fail = true;
    bluealsa_clear_soft_volume_path(NULL);
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 4);
    soft_volume_fail = false;
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_new/a2dpsnk/source");
    assert(soft_volume_checked_calls == 5); /* nonzero exit must be retried */
    bluealsa_apply_soft_volume("/org/bluealsa/hci0/dev_receiver/a2dpsrc/sink");
    assert(soft_volume_checked_calls == 6 && strcmp(soft_volume_state, "on") == 0);
    source_pcm_present = true;
    bluealsa_clear_soft_volume_path(NULL);
    assert(find_source_pcm_path(discovered_source_path, sizeof(discovered_source_path)));
    assert(strcmp(discovered_source_path, "/org/bluealsa/hci0/dev_source/a2dpsrc/sink") == 0);
    assert(soft_volume_checked_calls == 7); /* initial discovery applies SoftVolume */

    assert(!bt_control_apply_output_settings(false, true));
    assert(!atomic_load(&modern_soft_volume_requested));
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(false, false));
    assert(spawn_calls == 1 && kill_calls == 3);
    assert(atomic_load(&modern_soft_volume_requested));
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(!bt_control_apply_output_settings(true, true));
    assert(atomic_load(&modern_soft_volume_requested)); /* DAC mode: phone volume via software */
}

static void test_bluez_paired_devices(void) {
    bt_device_t devices[2] = {0};

    /* A failed, empty, or truncated help probe must not poison the cache or
     * fall back blindly to a command the current bluetoothctl may not implement. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_EMPTY_HELP;
    bluez_help_calls = bluez_help_failures = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_TRUNCATED_HELP;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 2 && bluez_paired_calls == 0);
    bluez_cli_mode = BLUEZ_CLI_LEGACY;
    bluez_help_failures = 1;
    assert(bt_control_list_paired_states(devices, 2) == -1);
    assert(bluez_help_calls == 3 && bluez_paired_calls == 0);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 4 && bluez_paired_calls == 1);
    assert(strcmp(devices[0].mac, "AA:BB:CC:DD:EE:01") == 0);
    assert(strcmp(devices[0].name, "Headphones") == 0);
    assert(devices[0].paired && !devices[0].connected);
    assert(bt_control_is_connected());
    assert(bluez_help_calls == 4 && bluez_paired_calls == 2);

    /* 5.87's help marker selects `devices Paired`, and the successful
     * capability remains cached across both paired-list callers. */
    bt_bluetoothctl_devices_capability = BT_BLUETOOTHCTL_DEVICES_UNKNOWN;
    bluez_cli_mode = BLUEZ_CLI_MODERN;
    bluez_help_calls = bluez_paired_calls = 0;
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 1);
    assert(strcmp(devices[1].name, "Backup Headphones") == 0);
    assert(devices[1].paired && devices[1].connected);
    assert(bt_control_list_paired_states(devices, 2) == 2);
    assert(bluez_help_calls == 1 && bluez_paired_calls == 2);
}

/* 44.1 kHz is the one transport rate the daemon can be told to negotiate, and
 * it is the default, so the argument has to reach the source daemon and stay
 * off the sink. */
static void test_force_audio_cd_argv(void) {
    bt_control_restore_codec_preference("auto");
    bt_control_set_sample_rate(44100);

    process_cmdline = NULL; process_cmdline_size = 0;
    process_present = false; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawn_calls == 1 && spawned_argc == 8);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(strcmp(spawned[3], "--a2dp-force-audio-cd") == 0);
    {
        size_t n = 4;
        assert_auto_codec_args(&n);
    }

    /* A daemon already carrying the argument is left alone. */
    static const char forced[] = "/usr/bin/bluealsad\0-p\0a2dp-source\0--a2dp-force-audio-cd\0" AUTO_ARGS;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* Automatic resolves to 44.1 kHz, so it keeps the argument. */
    bt_control_set_sample_rate(0);
    source_pcm_present = false;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* An explicit non-44.1 rate drops it, which is a real argv mismatch.
     * Nothing is connected, so correcting it may restart the daemon. */
    bt_control_set_sample_rate(48000);
    source_pcm_present = false;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; retain_process_after_kill = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_argc == 7);
    {
        size_t n = 3;
        assert_auto_codec_args(&n);
    }
    retain_process_after_kill = false;

    /* Same mismatch with an accessory connected must NOT kill the daemon:
     * that would drop a live A2DP link just to correct an argument. */
    source_pcm_present = true;
    process_cmdline = forced; process_cmdline_size = sizeof(forced);
    process_present = true; kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    source_pcm_present = false;

    /* DAC mode never forces the rate: the phone picks it. */
    bt_control_set_sample_rate(0); /* automatic still means 44.1 for the source */
    kill_calls = spawn_calls = 0;
    assert(!bt_control_apply_output_settings(true, false));
    assert(spawn_calls == 1 && spawned_argc == 7);
    assert(strcmp(spawned[2], "a2dp-sink") == 0);
    bt_control_set_sample_rate(48000);
}

static void test_ssc_auto_profile_daemon_flag(void) {
    bt_control_restore_codec_preference("auto");
    bt_daemon_ssc_auto_profile_feature = -1;
    daemon_help_calls = 0;
    daemon_help_supports_ssc_auto_profile = true;
    bt_control_set_sample_rate(48000);
    assert(!bt_ssc_auto_profile_enabled());
    assert(daemon_help_calls == 0); /* Explicit rate never probes the feature. */

    bt_control_set_sample_rate(0);
    assert(bt_ssc_auto_profile_enabled());
    assert(bt_ssc_auto_profile_enabled());
    assert(daemon_help_calls == 1); /* Cached for this player process. */

    process_cmdline = NULL;
    process_cmdline_size = 0;
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawn_calls == 1 && spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    assert(spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));

    static const char profile_auto[] =
        "/usr/bin/bluealsad\0-p\0a2dp-source\0--a2dp-force-audio-cd\0--ssc-auto-profile\0--all-codecs\0"
        "--ldac-abr\0--ldac-quality\0high\0";
    process_cmdline = profile_auto;
    process_cmdline_size = sizeof(profile_auto);
    process_present = true;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* A daemon without the flag is reconciled while idle, then respawned
     * with it; an active PCM still prevents a disruptive restart. */
    static const char old_auto[] =
        "/usr/bin/bluealsad\0-p\0a2dp-source\0--a2dp-force-audio-cd\0--all-codecs\0"
        "--ldac-abr\0--ldac-quality\0high\0";
    process_cmdline = old_auto;
    process_cmdline_size = sizeof(old_auto);
    process_present = true;
    source_pcm_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1 && spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    source_pcm_present = true;
    process_cmdline = old_auto;
    process_cmdline_size = sizeof(old_auto);
    process_present = true;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);
    source_pcm_present = false;

    /* Each explicit rate keeps its existing daemon arguments. */
    bt_control_set_sample_rate(44100);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(!spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    bt_control_set_sample_rate(48000);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(!spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(!spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    bt_control_set_sample_rate(96000);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(!spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(!spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));

    /* A player-only upgrade must still launch an older daemon safely. */
    bt_daemon_ssc_auto_profile_feature = -1;
    daemon_help_supports_ssc_auto_profile = false;
    bt_control_set_sample_rate(0);
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(spawn_calls == 1 && spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(!spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    assert(daemon_help_calls == 2);

    /* A correction keeps its captured explicit rate even if a status refresh
     * publishes Automatic before daemon argv construction. */
    atomic_store(&bt_daemon_ssc_auto_profile_feature, 1);
    bt_control_set_sample_rate(0);
    process_present = false;
    kill_calls = spawn_calls = 0;
    assert(ensure_bluealsa_running_rate_ex(44100, NULL));
    assert(spawn_calls == 1 && spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(!spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));

    /* A settled PCM gets a refresh when its saved per-device rate changes,
     * even without a new PCMAdded notification. */
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.running = true;
    atomic_store(&bt_pref.stop, false);
    snprintf(bt_pref.path, sizeof(bt_pref.path), "/settled/path");
    bt_pref.refresh_due = false;
    bt_pref.scan_requested = false;
    pthread_mutex_unlock(&bt_pref.mutex);
    bt_control_set_sample_rate(44100);
    pthread_mutex_lock(&bt_pref.mutex);
    assert(bt_pref.refresh_due && !bt_pref.scan_requested);
    bt_pref.running = false;
    bt_pref.path[0] = '\0';
    bt_pref.refresh_due = false;
    pthread_mutex_unlock(&bt_pref.mutex);

    bt_control_set_sample_rate(48000);
}

static void test_auto_rate_reconnect_preserves_request(void) {
    const char *path = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
    snprintf(test_source_pcm_path, sizeof(test_source_pcm_path), "%s", path);
    bt_control_restore_codec_preference("auto");
    atomic_store(&bt_daemon_ssc_auto_profile_feature, 1);
    source_pcm_present = true;
    process_present = false;
    test_transport_rate = 96000;
    bt_control_set_sample_rate(0);
    rate_select_calls = reconnect_disconnect_calls = reconnect_connect_calls = 0;
    kill_calls = spawn_calls = 0;

    assert(bt_control_reconnect_for_rate_change(0));
    assert(atomic_load(&bt_sample_rate) == 0);
    assert(rate_select_calls == 0); /* Automatic SSC uses the daemon profile. */
    assert(reconnect_disconnect_calls == 1 && reconnect_connect_calls == 1);
    assert(spawn_calls == 1 && spawned_has_arg(BT_SSC_AUTO_PROFILE_ARG));
    assert(spawned_has_arg(BT_A2DP_FORCE_44K1_ARG));
    assert(test_transport_rate == 96000); /* Any nonzero negotiated rate is valid for Auto. */

    /* Automatic leaves an SSC HiFi/UHQ rate alone; explicit 44.1 kHz checks
     * the live transport and requests it when the daemon profile chose more. */
    test_transport_rate = 48000;
    bt_control_set_sample_rate(0);
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 0 && test_transport_rate == 48000);
    bt_control_set_sample_rate(44100);
    assert(bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 1 && test_transport_rate == 44100);

    source_pcm_present = false;
    test_source_pcm_path[0] = '\0';
    bt_control_set_sample_rate(48000);
}

/* Capability decoding, against dbus-send output captured verbatim from a
 * real headset (Galaxy Buds2 Pro, AAC endpoint). Both byte-array spellings
 * are covered: requiring either one alone silently produced an empty rate
 * list on hardware that used the other. */
static void test_a2dp_capability_rates(void) {
    static const char bare[] =
        "   array [\n"
        "      dict entry(\n"
        "         string \"UUID\"\n"
        "         variant             string \"0000110b-0000-1000-8000-00805f9b34fb\"\n"
        "      )\n"
        "      dict entry(\n"
        "         string \"Codec\"\n"
        "         variant             byte 2\n"
        "      )\n"
        "      dict entry(\n"
        "         string \"Capabilities\"\n"
        "         variant             array of bytes [\n"
        "               80 01 8c 83 e8 00\n"
        "            ]\n"
        "      )\n"
        "   ]\n";
    static const char prefixed[] =
        "         string \"Capabilities\"\n"
        "         variant             array of bytes [\n"
        "               byte 0x80, byte 0x01, byte 0x8c, byte 0x83, byte 0xe8, byte 0x00\n"
        "            ]\n";

    uint8_t caps[32];
    size_t caps_len = 0;
    assert(dbus_prop_byte_array(bare, "Capabilities", caps, sizeof(caps), &caps_len));
    assert(caps_len == 6 && caps[0] == 0x80 && caps[1] == 0x01 && caps[2] == 0x8c && caps[5] == 0x00);

    uint8_t codec_id = 0;
    assert(dbus_prop_byte(bare, "Codec", &codec_id) && codec_id == 0x02);
    assert(a2dp_caps_match_codec(caps, caps_len, codec_id, "AAC"));
    assert(!a2dp_caps_match_codec(caps, caps_len, codec_id, "SBC"));

    /* 0x018: bit 4 is 44.1 kHz and bit 3 is 48 kHz, listed low to high. */
    unsigned int rates[12];
    int n = a2dp_caps_rates(caps, caps_len, "AAC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);

    /* Real BlueZ vendor endpoint output uses decimal 255 for vendor codecs;
     * parsing it as hex previously prevented the SSC rate list from loading. */
    static const char decimal_ssc[] =
        "string \"Codec\"\n"
        "variant byte 255\n"
        "string \"Capabilities\"\n"
        "variant array of bytes [\n"
        "  75 00 00 00 03 01 3c\n"
        "]\n";
    assert(dbus_prop_byte(decimal_ssc, "Codec", &codec_id) && codec_id == 255);
    uint8_t ssc_caps_from_dbus[32];
    size_t ssc_caps_from_dbus_len = 0;
    assert(dbus_prop_byte_array(decimal_ssc, "Capabilities", ssc_caps_from_dbus,
                                sizeof(ssc_caps_from_dbus), &ssc_caps_from_dbus_len));
    assert(a2dp_caps_match_codec(ssc_caps_from_dbus, ssc_caps_from_dbus_len,
                                 codec_id, "samsung-SC"));
    n = a2dp_caps_rates(ssc_caps_from_dbus, ssc_caps_from_dbus_len, "samsung-SC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);

    static const char hex_scalar[] = "string \"Codec\" variant byte 0xff\n";
    assert(dbus_prop_byte(hex_scalar, "Codec", &codec_id) && codec_id == 255);
    static const char uppercase_hex_scalar[] = "string \"Codec\" variant byte 0XFF\n";
    assert(dbus_prop_byte(uppercase_hex_scalar, "Codec", &codec_id) && codec_id == 255);
    assert(!dbus_prop_byte("string \"Codec\" variant byte 256\n", "Codec", &codec_id));
    assert(!dbus_prop_byte("string \"Codec\" variant byte -1\n", "Codec", &codec_id));
    assert(!dbus_prop_byte("string \"Codec\" variant byte 255junk\n", "Codec", &codec_id));
    assert(!dbus_prop_byte("string \"Codec\" variant byte 0x\n", "Codec", &codec_id));

    size_t prefixed_len = 0;
    uint8_t prefixed_caps[32];
    assert(dbus_prop_byte_array(prefixed, "Capabilities", prefixed_caps, sizeof(prefixed_caps), &prefixed_len));
    assert(prefixed_len == 6 && memcmp(prefixed_caps, caps, 6) == 0);

    /* Wire layouts for the other codecs this player offers. */
    static const uint8_t sbc[] = { 0x3f, 0xff, 2, 53 };
    n = a2dp_caps_rates(sbc, sizeof(sbc), "SBC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);

    static const uint8_t ldac[] = { 0x2d, 0x01, 0x00, 0x00, 0xaa, 0x00, 0x3c, 0x07 };
    n = a2dp_caps_rates(ldac, sizeof(ldac), "LDAC", rates, 12);
    assert(n == 4 && rates[0] == 44100 && rates[1] == 48000 && rates[2] == 88200 && rates[3] == 96000);
    assert(a2dp_caps_match_codec(ldac, sizeof(ldac), 0xff, "LDAC"));
    assert(!a2dp_caps_match_codec(ldac, sizeof(ldac), 0xff, "aptX"));

    /* A truncated or unparsable array must yield nothing rather than junk.
     * Checked against a fresh buffer: reusing the one above would still hold
     * the decoded AAC blob and pass even if a parse failure wrote garbage. */
    uint8_t empty_caps[32];
    size_t empty_len = 1;
    memset(empty_caps, 0xAA, sizeof(empty_caps));
    assert(!dbus_prop_byte_array("string \"Capabilities\" variant array of bytes [ ]",
                                 "Capabilities", empty_caps, sizeof(empty_caps), &empty_len));
    assert(!dbus_prop_byte_array("string \"Capabilities\" variant array of bytes [ 80 01",
                                 "Capabilities", empty_caps, sizeof(empty_caps), &empty_len));
    assert(a2dp_caps_rates(empty_caps, 2, "AAC", rates, 12) == 0); /* AAC needs 3 bytes */

    /* aptX shares SBC's nibble layout but sits after the 6-byte vendor header. */
    static const uint8_t aptx[] = { 0x4f, 0x00, 0x00, 0x00, 0x01, 0x00, 0x32 };
    n = a2dp_caps_rates(aptx, sizeof(aptx), "aptX", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);
    assert(a2dp_caps_match_codec(aptx, sizeof(aptx), 0xff, "aptX"));
    assert(!a2dp_caps_match_codec(aptx, sizeof(aptx), 0xff, "aptX-HD"));

    /* Samsung Scalable Codec (SSC): vendor 0x0075, codec 0x0103 */
    static const uint8_t ssc_caps[] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0xb8 };
    n = a2dp_caps_rates(ssc_caps, sizeof(ssc_caps), "samsung-SC", rates, 12);
    assert(n == 3 && rates[0] == 44100 && rates[1] == 48000 && rates[2] == 96000);
    n = a2dp_caps_rates(ssc_caps, sizeof(ssc_caps), "SSC", rates, 12);
    assert(n == 3 && rates[0] == 44100 && rates[1] == 48000 && rates[2] == 96000);
    assert(a2dp_caps_match_codec(ssc_caps, sizeof(ssc_caps), 0xff, "samsung-SC"));
    assert(a2dp_caps_match_codec(ssc_caps, sizeof(ssc_caps), 0xff, "SSC"));
    assert(!a2dp_caps_match_codec(ssc_caps, sizeof(ssc_caps), 0xff, "LDAC"));

    static const uint8_t buds2_caps[] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x2c };
    n = a2dp_caps_rates(buds2_caps, sizeof(buds2_caps), "samsung-SC", rates, 12);
    assert(n == 2 && rates[0] == 44100 && rates[1] == 48000);
    static const uint8_t hifi24_only_caps[] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x04 };
    n = a2dp_caps_rates(hifi24_only_caps, sizeof(hifi24_only_caps), "samsung-SC", rates, 12);
    assert(n == 1 && rates[0] == 48000);
    static const uint8_t hifi96_only_caps[] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x80 };
    n = a2dp_caps_rates(hifi96_only_caps, sizeof(hifi96_only_caps), "samsung-SC", rates, 12);
    assert(n == 1 && rates[0] == 96000);
    static const uint8_t uhq2_caps[] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x0a };
    n = a2dp_caps_rates(uhq2_caps, sizeof(uhq2_caps), "SSC", rates, 12);
    assert(n == 1 && rates[0] == 96000);
}

static void test_auto_codec_ranking(void) {
    char best[32];
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", 0, best, sizeof(best)) &&
           strcmp(best, "LDAC") == 0);
    assert(bluealsa_best_available_codec("Selected codec: SBC\nAvailable codecs: SBC AAC\nRate: 44100\n", 0,
                                         best, sizeof(best)) && strcmp(best, "AAC") == 0);
    assert(bluealsa_best_available_codec("Available codecs: AAC aptX aptX-HD SBC\n", 0, best, sizeof(best)) &&
           strcmp(best, "aptX-HD") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC aptX AAC\n", 0, best, sizeof(best)) &&
           strcmp(best, "aptX") == 0);
    /* Codecs outside the ranking are ignored; SBC is always the floor. */
    assert(bluealsa_best_available_codec("Available codecs: FastStream LC3plus SBC\n", 0, best, sizeof(best)) &&
           strcmp(best, "SBC") == 0);
    /* Not known yet: the accessory's endpoints have not been published. */
    assert(!bluealsa_best_available_codec("Available codecs: [ Unknown ]\n", 0, best, sizeof(best)));
    assert(!bluealsa_best_available_codec("Selected codec: AAC\n", 0, best, sizeof(best)));
    /* Codecs Automatic found unusable on a device are skipped. */
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", 1u << 0, best, sizeof(best)) &&
           strcmp(best, "aptX") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX LDAC\n", (1u << 0) | (1u << 3),
                                         best, sizeof(best)) && strcmp(best, "AAC") == 0);

    /* Test samsung-SC in ranking and rejection */
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC samsung-SC aptX LDAC\n", 0, best, sizeof(best)) &&
           strcmp(best, "LDAC") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC samsung-SC aptX LDAC\n", 1u << 0, best, sizeof(best)) &&
           strcmp(best, "samsung-SC") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX-HD samsung-SC\n", 0, best, sizeof(best)) &&
           strcmp(best, "samsung-SC") == 0);
    assert(bluealsa_best_available_codec("Available codecs: SBC AAC aptX-HD samsung-SC\n", 1u << 1, best, sizeof(best)) &&
           strcmp(best, "aptX-HD") == 0);

    /* Per-device memory: a failed codec, and one followed by a quick
     * disconnect, are skipped next time; SBC is never rejected. */
    const char * dev_a = "/org/bluealsa/hci0/dev_AA_AA_AA_AA_AA_AA/a2dpsrc/sink";
    const char * dev_b = "/org/bluealsa/hci0/dev_BB_BB_BB_BB_BB_BB/a2dpsrc/sink";
    const char * dev_c = "/org/bluealsa/hci0/dev_CC_CC_CC_CC_CC_CC/a2dpsrc/sink";
    assert(bt_auto_rejected(dev_a) == 0);
    bt_auto_reject(dev_a, "LDAC");
    assert(bt_auto_rejected(dev_a) == (1u << 0) && bt_auto_rejected(dev_b) == 0);
    bt_auto_note_switch(dev_b, "aptX");
    bt_auto_note_disconnect(dev_b);
    assert(bt_auto_rejected(dev_b) == (1u << 3));
    bt_auto_reject(dev_b, "SBC");
    assert(bt_auto_rejected(dev_b) == (1u << 3));

    assert(bt_auto_rejected(dev_c) == 0);
    bt_auto_reject(dev_c, "samsung-SC");
    assert(bt_auto_rejected(dev_c) == (1u << 1));
}

static void test_galaxy_buds_matching(void) {
    assert(bt_is_galaxy_buds_name("Galaxy Buds"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds+ de Jose"));
    assert(bt_is_galaxy_buds_name("Buds2 Pro de Jose"));
    assert(bt_is_galaxy_buds_name("Buds2Pro de Jose"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds3FE"));
    assert(!bt_is_galaxy_buds_name("Galaxy Buds2Professional"));
    assert(bt_is_galaxy_buds_name("Buds2 de Jose"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds3 FE"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds4"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds4 Pro"));
    assert(bt_is_galaxy_buds_name("Galaxy Buds Live"));
    assert(bt_is_galaxy_buds_name("Jose's Galaxy Buds2 Pro"));
    assert(bt_is_galaxy_buds_name("Buds Pro de Jose"));
    assert(!bt_is_galaxy_buds_name("Buds"));
    assert(!bt_is_galaxy_buds_name("Buds de Jose"));
    assert(!bt_is_galaxy_buds_name("My Buds headphones"));
    assert(!bt_is_galaxy_buds_name("Galaxy Buds unknown"));
}

static void test_buds_ssc_selection(void) {
    const char *path = "/org/bluealsa/hci0/dev_10_20_30_40_50_60/a2dpsrc/sink";
    test_bluetooth_alias = "Buds2 Pro de Jose";
    assert(bt_control_set_codec("auto"));
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    test_selected_codec = "SBC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(bluealsa_apply_codec_preference(path));
    assert(codec_select_calls == 1 && strcmp(last_codec_applied_name, "samsung-SC") == 0);

    const char *late = "/org/bluealsa/hci0/dev_11_20_30_40_50_60/a2dpsrc/sink";
    test_available_codecs = "SBC AAC";
    test_selected_codec = "SBC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    uint32_t defer_until = 0;
    assert(!bluealsa_apply_codec_preference_deferred(late, NULL, &defer_until));
    assert(defer_until != 0 && codec_select_calls == 0);
    pthread_mutex_lock(&bt_pref.mutex);
    snprintf(bt_pref.path, sizeof(bt_pref.path), "%s", late);
    bt_pref.attempts = 0;
    bt_pref.discovery_pending = false;
    bt_pref_record_codec_result_locked(late, false, defer_until);
    assert(bt_pref.attempts == 0 && bt_pref.discovery_pending &&
           bt_pref.retry_at == defer_until && bt_pref_retry_scheduled_locked());
    bt_pref_begin_work_locked(false); /* refresh-only PCMAdded retains the deadline */
    assert(bt_pref.discovery_pending && bt_pref_retry_scheduled_locked());
    bt_pref_begin_work_locked(true);
    bt_pref_record_codec_result_locked(late, false, 0);
    assert(bt_pref.attempts == 1 && !bt_pref.discovery_pending);
    bt_pref.attempts = 0;
    bt_pref.path[0] = '\0';
    pthread_mutex_unlock(&bt_pref.mutex);
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    assert(bluealsa_apply_codec_preference(late));
    assert(codec_select_calls == 1 && strcmp(last_codec_applied_name, "samsung-SC") == 0);

    const char *expired = "/org/bluealsa/hci0/dev_12_20_30_40_50_60/a2dpsrc/sink";
    test_available_codecs = "SBC AAC LDAC";
    test_selected_codec = "SBC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(!bluealsa_apply_codec_preference(expired));
    pthread_mutex_lock(&bt_buds_state_mutex);
    bt_buds_wait_started = bt_control_monotonic_ms() - BT_BUDS_SSC_DISCOVERY_GRACE_MS - 1;
    pthread_mutex_unlock(&bt_buds_state_mutex);
    assert(bluealsa_apply_codec_preference(expired));
    assert(strcmp(last_codec_applied_name, "LDAC") == 0);

    const char *explicit_late = "/org/bluealsa/hci0/dev_18_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("ssc"));
    test_available_codecs = "SBC AAC";
    test_selected_codec = "SBC";
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(!bluealsa_apply_codec_preference_deferred(explicit_late, NULL, &defer_until));
    assert(defer_until != 0);
    pthread_mutex_lock(&bt_buds_state_mutex);
    bt_buds_wait_started = bt_control_monotonic_ms() - BT_BUDS_SSC_DISCOVERY_GRACE_MS - 1;
    pthread_mutex_unlock(&bt_buds_state_mutex);
    assert(bluealsa_apply_codec_preference_deferred(explicit_late, NULL, &defer_until));
    assert(defer_until == 0 && strcmp(last_codec_applied_name, "AAC") == 0);

    const char *rejected = "/org/bluealsa/hci0/dev_13_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("ssc"));
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    test_selected_codec = "SBC";
    test_codec_select_fail_name = "samsung-SC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(bluealsa_apply_codec_preference(rejected));
    assert(codec_select_calls == 2 && strcmp(last_codec_applied_name, "LDAC") == 0);
    test_codec_select_fail_name = NULL;

    const char *auto_rejected = "/org/bluealsa/hci0/dev_17_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("auto"));
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    test_selected_codec = "SBC";
    test_codec_select_fail_name = "samsung-SC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(!bluealsa_apply_codec_preference(auto_rejected));
    test_codec_select_fail_name = NULL;
    assert(bluealsa_apply_codec_preference(auto_rejected));
    assert(codec_select_calls == 2 && strcmp(last_codec_applied_name, "LDAC") == 0);

    const char *explicit_aac = "/org/bluealsa/hci0/dev_14_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("aac"));
    unsigned info_before = bluetooth_info_checked_calls;
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    test_selected_codec = "SBC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(bluealsa_apply_codec_preference(explicit_aac));
    assert(bluetooth_info_checked_calls == info_before);
    assert(codec_select_calls == 1 && strcmp(last_codec_applied_name, "AAC") == 0);

    const char *retry = "/org/bluealsa/hci0/dev_15_20_30_40_50_60/a2dpsrc/sink";
    bt_buds_reset(NULL);
    bluetooth_info_fail_once = true;
    bool is_buds = true, metadata_known = true;
    uint32_t wait_started = 0;
    bt_buds_state(retry, &is_buds, &metadata_known, &wait_started, NULL);
    assert(!is_buds && !metadata_known);
    bt_buds_state(retry, &is_buds, &metadata_known, &wait_started, NULL);
    assert(is_buds && metadata_known && bluetooth_info_checked_calls >= 2);

    const char *metadata_retry = "/org/bluealsa/hci0/dev_16_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("auto"));
    test_bluetooth_alias = "Galaxy Buds de Jose";
    bluetooth_info_fail_once = true;
    test_available_codecs = "SBC AAC LDAC samsung-SC";
    test_selected_codec = "SBC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(!bluealsa_apply_codec_preference_deferred(metadata_retry, NULL, &defer_until));
    assert(defer_until != 0 && codec_select_calls == 0 && !bluealsa_codec_verified(metadata_retry));
    assert(bluealsa_apply_codec_preference(metadata_retry));
    assert(codec_select_calls == 1 && strcmp(last_codec_applied_name, "samsung-SC") == 0);

    const char *already_rejected = "/org/bluealsa/hci0/dev_1A_20_30_40_50_60/a2dpsrc/sink";
    test_bluetooth_alias = "Buds2 Pro de Jose";
    test_available_codecs = "SBC AAC";
    test_selected_codec = "SBC";
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    bt_auto_reject(already_rejected, "samsung-SC");
    assert(bluealsa_apply_codec_preference_deferred(already_rejected, NULL, &defer_until));
    assert(defer_until == 0 && strcmp(last_codec_applied_name, "AAC") == 0);

    const char *nonbuds = "/org/bluealsa/hci0/dev_19_20_30_40_50_60/a2dpsrc/sink";
    test_bluetooth_alias = "Generic Headphones";
    test_available_codecs = "SBC AAC";
    test_selected_codec = "SBC";
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    assert(bluealsa_apply_codec_preference_deferred(nonbuds, NULL, &defer_until));
    assert(defer_until == 0 && strcmp(last_codec_applied_name, "AAC") == 0);

    pthread_mutex_lock(&bt_pref.mutex);
    snprintf(bt_pref.path, sizeof(bt_pref.path), "%s", late);
    bt_pref.discovery_pending = true;
    bt_pref.attempts = 0;
    pthread_mutex_unlock(&bt_pref.mutex);
    bt_pref_stop();
    assert(!bt_pref.discovery_pending && bt_pref.attempts == 0 && !bt_pref.path[0]);
    /* SSC is already decisive: failed name lookup must not delay it. */
    const char *advertised = "/org/bluealsa/hci0/dev_18_20_30_40_50_60/a2dpsrc/sink";
    assert(bt_control_set_codec("auto"));
    bluealsa_clear_codec_path(NULL);
    test_available_codecs = "SBC AAC samsung-SC";
    test_selected_codec = "SBC";
    bluetooth_info_fail_once = true;
    unsigned lookup_before = bluetooth_info_checked_calls;
    codec_select_calls = 0;
    assert(bluealsa_apply_codec_preference(advertised));
    assert(codec_select_calls == 1 && strcmp(last_codec_applied_name, "samsung-SC") == 0);
    assert(bluetooth_info_checked_calls == lookup_before);
    bluetooth_info_fail_once = false;
    assert(bt_control_set_codec("auto"));
    test_bluetooth_alias = NULL;
}

static void test_codec_display_name(void) {
    char name[32];
    bluealsa_codec_display_name("SBC:24150223", name, sizeof(name));   /* 44.1k dual 16/8/loudness */
    assert(strcmp(name, "SBC-XQ") == 0);
    bluealsa_codec_display_name("SBC:21150235", name, sizeof(name));   /* joint stereo */
    assert(strcmp(name, "SBC") == 0);
    bluealsa_codec_display_name("SBC:14150235", name, sizeof(name));   /* 48k dual */
    assert(strcmp(name, "SBC") == 0);
    bluealsa_codec_display_name("SBC", name, sizeof(name));
    assert(strcmp(name, "SBC") == 0);
    bluealsa_codec_display_name("AAC:80010401f400", name, sizeof(name));
    assert(strcmp(name, "AAC") == 0);
    bluealsa_codec_display_name("LDAC", name, sizeof(name));
    assert(strcmp(name, "LDAC") == 0);
    bluealsa_codec_display_name("samsung-SC", name, sizeof(name));
    assert(strcmp(name, "SSC") == 0);
    bluealsa_codec_display_name("samsung-SC:2c", name, sizeof(name));
    assert(strcmp(name, "SSC") == 0);
}

static void test_rate_refusal_memory(void) {
    /* A refused rate survives routine re-application (e.g. on reconnect);
     * only a rate the user chooses clears it. */
    snprintf(bt_rate_refused_path, sizeof(bt_rate_refused_path), "%s", "/org/bluealsa/hci0/dev_X/a2dpsrc/sink");
    snprintf(bt_rate_refused_codec, sizeof(bt_rate_refused_codec), "%s", "AAC");
    bt_rate_refused_rate = 96000;
    bt_control_set_sample_rate(96000);
    bt_control_set_sample_rate(48000);
    assert(bt_rate_refused_rate == 96000 && bt_rate_refused_path[0]);
    bt_control_choose_sample_rate(48000);
    assert(bt_rate_refused_rate == 0 && !bt_rate_refused_path[0] && !bt_rate_refused_codec[0]);
}

static void test_ssc_explicit_rate_queues_targeted_correction(void) {
    const char *path = "/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsrc/sink";
    char mac[18] = {0};
    unsigned int queued_rate = 0;
    snprintf(test_source_pcm_path, sizeof(test_source_pcm_path), "%s", path);
    test_selected_codec = "samsung-SC";
    test_transport_rate = 96000;
    source_pcm_present = true;
    bt_control_choose_sample_rate(44100);
    rate_select_calls = 0;
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 0 && test_transport_rate == 96000);
    assert(bt_control_ssc_rate_correction_peek(mac, sizeof(mac), &queued_rate));
    assert(strcmp(mac, "AA:BB:CC:DD:EE:FF") == 0 && queued_rate == 44100);
    /* A repeated pass for the same PCM coalesces instead of issuing -r. */
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 0);
    bt_control_choose_sample_rate(44100);
    test_transport_rate = 0; /* Unknown current rate must not trigger -r or recovery. */
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 0);
    assert(!bt_control_ssc_rate_correction_peek(mac, sizeof(mac), &queued_rate));
    test_transport_rate = 96000;
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(bt_control_ssc_rate_correction_peek(mac, sizeof(mac), &queued_rate));
    assert(bt_control_ssc_rate_correction_take(mac, queued_rate));
    assert(!bt_control_ssc_rate_correction_peek(mac, sizeof(mac), &queued_rate));

    /* Automatic and explicit non-SSC choices retain their old behavior. */
    bt_control_choose_sample_rate(0);
    test_transport_rate = 96000;
    assert(!bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 0);
    bt_control_choose_sample_rate(44100);
    test_selected_codec = "AAC";
    assert(bluealsa_apply_rate_preference_cancellable(path, NULL));
    assert(rate_select_calls == 1 && test_transport_rate == 44100);
    bt_control_choose_sample_rate(48000);
    test_selected_codec = "SBC";
    source_pcm_present = false;
    test_source_pcm_path[0] = '\0';
}

static void test_ssc_codec_preference(void) {
    /* 1. Preference selection and PCM / daemon name mappings */
    assert(bt_control_set_codec("ssc"));
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    assert(strcmp(bt_codec_daemon_name(), "samsung-SC") == 0);
    assert(strcmp(bt_codec_select_name(), "samsung-SC") == 0);
    bt_control_set_sample_rate(0);
    assert(bt_rate_wants_audio_cd());
    assert(bt_playback_codec_override() == NULL);
    /* Other explicit preferences still create the same asoundrc pin. */
    atomic_store(&bt_codec_preference, BT_CODEC_APTX);
    assert(strcmp(bt_playback_codec_override(), "aptX") == 0);
    atomic_store(&bt_codec_preference, BT_CODEC_SSC);

    /* Automatic retains the player's existing 44.1 kHz transport behavior. */
    assert(bt_rate_wants_audio_cd());
    bt_control_set_sample_rate(44100);
    assert(bt_rate_wants_audio_cd());
    bt_control_set_sample_rate(48000);
    assert(!bt_rate_wants_audio_cd());

    /* 2. Enable advertised fallbacks; the per-PCM selector applies SSC. */
    process_cmdline = NULL;
    process_cmdline_size = 0;
    process_present = false;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 1);
    assert(spawned_argc == 4);
    assert(strcmp(spawned[0], "/usr/bin/bluealsad") == 0);
    assert(strcmp(spawned[1], "-p") == 0);
    assert(strcmp(spawned[2], "a2dp-source") == 0);
    assert(strcmp(spawned[3], "--all-codecs") == 0);

    /* 3. Stale daemon reconciliation: --all-codecs matches the SSC preference. */
    static const char ssc_running[] = "bluealsad\0-p\0a2dp-source\0--all-codecs\0";
    process_cmdline = ssc_running;
    process_cmdline_size = sizeof(ssc_running);
    process_present = true;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 0 && spawn_calls == 0);

    /* When running daemon has different codec (e.g. aptX), ensure_bluealsa_running restarts it */
    static const char aptx_running[] = "bluealsad\0-p\0a2dp-source\0-c\0aptX\0";
    process_cmdline = aptx_running;
    process_cmdline_size = sizeof(aptx_running);
    process_present = true;
    kill_calls = spawn_calls = 0;
    ensure_bluealsa_running();
    assert(kill_calls == 1 && spawn_calls == 1);
    assert(spawned_argc == 4);
    assert(strcmp(spawned[3], "--all-codecs") == 0);

    /* 4. Selecting samsung-SC on a live PCM via bluealsactl codec */
    const char * pcm_path = "/org/bluealsa/hci0/dev_source/a2dpsrc/sink";
    codec_select_calls = 0;
    last_codec_applied_path[0] = '\0';
    last_codec_applied_name[0] = '\0';
    test_selected_codec = NULL;
    test_available_codecs = "SBC AAC samsung-SC aptX";
    bluealsa_clear_codec_path(NULL);
    assert(bluealsa_apply_codec_preference(pcm_path));
    assert(codec_select_calls == 1);
    assert(strcmp(last_codec_applied_path, pcm_path) == 0);
    assert(strcmp(last_codec_applied_name, "samsung-SC") == 0);
    bt_auto_note_disconnect(pcm_path);
    assert(bt_auto_rejected(pcm_path) & (1u << bt_auto_rank_index("samsung-SC")));

    /* A known non-SSC capability list falls back to the best supported codec,
     * while retaining the saved SSC preference for future connections. */
    test_available_codecs = "SBC AAC aptX";
    test_selected_codec = NULL;
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(bluealsa_apply_codec_preference(pcm_path));
    assert(codec_select_calls == 1);
    assert(strcmp(last_codec_applied_name, "aptX") == 0);
    assert(strcmp(bt_control_get_playback_pcm(), "bluealsa") == 0);
    assert(bluealsa_apply_codec_preference(pcm_path));
    assert(codec_select_calls == 1); /* verified fallback is latched; no retry loop */

    /* An advertised SSC configuration can still be rejected. Remember SSC
     * for this device and immediately select the best remaining enabled codec. */
    const char * rejected_pcm = "/org/bluealsa/hci0/dev_DD_DD_DD_DD_DD_DD/a2dpsrc/sink";
    test_available_codecs = "SBC AAC samsung-SC aptX LDAC";
    test_selected_codec = NULL;
    test_codec_select_fail_name = "samsung-SC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(bluealsa_apply_codec_preference(rejected_pcm));
    assert(codec_select_calls == 2);
    assert(strcmp(last_codec_applied_name, "LDAC") == 0);
    assert(bt_auto_rejected(rejected_pcm) & (1u << 1));
    test_codec_select_fail_name = NULL;

    /* Reject a failed fallback too, then move down the ranking on the next
     * retry instead of repeatedly selecting LDAC. */
    const char * fallback_rejected_pcm = "/org/bluealsa/hci0/dev_EE_EE_EE_EE_EE_EE/a2dpsrc/sink";
    test_available_codecs = "SBC AAC samsung-SC aptX LDAC";
    test_selected_codec = NULL;
    test_codec_select_fail_name = "samsung-SC";
    test_codec_select_fail_ldac = true;
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(!bluealsa_apply_codec_preference(fallback_rejected_pcm));
    assert(codec_select_calls == 2);
    assert((bt_auto_rejected(fallback_rejected_pcm) & 3u) == 3u);
    assert(bluealsa_apply_codec_preference(fallback_rejected_pcm));
    assert(codec_select_calls == 3);
    assert(strcmp(last_codec_applied_name, "aptX") == 0);
    test_codec_select_fail_name = NULL;
    test_codec_select_fail_ldac = false;

    /* A successful SelectCodec command can still report a different live
     * codec. Reject that SSC result, then select the next ranked codec. */
    const char * mismatch_pcm = "/org/bluealsa/hci0/dev_FF_FF_FF_FF_FF_FF/a2dpsrc/sink";
    test_available_codecs = "SBC AAC samsung-SC aptX LDAC";
    test_selected_codec = NULL;
    test_codec_select_mismatch_name = "samsung-SC";
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(bluealsa_apply_codec_preference(mismatch_pcm));
    assert(codec_select_calls == 2);
    assert(bt_auto_rejected(mismatch_pcm) & (1u << 1));
    assert(strcmp(last_codec_applied_name, "LDAC") == 0);
    test_codec_select_mismatch_name = NULL;

    test_available_codecs = "[ Unknown ]";
    test_selected_codec = NULL;
    codec_select_calls = 0;
    bluealsa_clear_codec_path(NULL);
    assert(!bluealsa_apply_codec_preference(pcm_path));
    assert(codec_select_calls == 0);
    test_available_codecs = NULL;

    /* Clean up back to auto */
    assert(bt_control_set_codec("auto"));
    test_selected_codec = NULL;
}

int main(void) {
    /* A locking regression must fail promptly rather than hang the target. */
    alarm(10);
    test_auto_codec_ranking();
    test_galaxy_buds_matching();
    test_buds_ssc_selection();
    test_rate_refusal_memory();
    test_ssc_explicit_rate_queues_targeted_correction();
    test_codec_display_name();
    test_ssc_codec_preference();

    int monitor_volume = -1;
    assert(parse_monitor_volume("0x7f7f", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127", &monitor_volume) && monitor_volume == 127);
    assert(parse_monitor_volume("127[M]", &monitor_volume) && monitor_volume == 127);
    assert(!parse_monitor_volume("127junk", &monitor_volume));
    assert(!parse_monitor_volume("128", &monitor_volume));
    assert(!parse_monitor_volume("0x7f7fjunk", &monitor_volume));

    assert(bt_control_set_codec("auto"));
    /* The existing argv assertions below cover codec arguments, so the rate
     * is parked on one that adds no argument of its own to keep them
     * positional. Automatic is NOT that: it resolves to 44.1 kHz and so
     * carries --a2dp-force-audio-cd. A dedicated case for it follows. */
    bt_control_set_sample_rate(48000);
    test_daemon_argv();
    test_sbc_xq_lifecycle();
    test_modern_argv();
    test_source_absolute_volume_capability();
    test_source_software_seed_and_hardware_mode();
    test_source_volume_seed_readiness();
    test_prepare_reprobes_avrcp_before_first_audio_open();
    test_late_hardware_upgrade_seed_failure_clears_old_latch();
    test_force_audio_cd_argv();
    test_ssc_auto_profile_daemon_flag();
    test_auto_rate_reconnect_preserves_request();
    test_a2dp_capability_rates();
    test_bluez_paired_devices();
    alarm(0);
    puts("bluetooth-codec-selftest: PASS (BlueALSA 5 daemon/codec behavior, soft volume, BlueZ compatibility)");
    return 0;
}
