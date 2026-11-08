#include "bluetooth_control_internal.h"

static pthread_mutex_t bt_dac_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_dac_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_dac_stream_info_t bt_dac_info;
static pthread_t bt_dac_info_thread;
static bool bt_dac_info_active;
static pid_t bt_dac_info_monitor_pid = -1;
static char named_path[256];
static char device_name[64];
static struct { char path[256]; bool running; } pcm_edges[32];
static uint32_t stream_generation;
static atomic_bool output_reserved;

void bt_control_dac_set_reserved(bool reserved) { atomic_store(&output_reserved, reserved); }

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bt_dac_info_mutex);
    *out = bt_dac_info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static void bt_dac_info_store(const bt_dac_stream_info_t * info) {
    pthread_mutex_lock(&bt_dac_info_mutex);
    bt_dac_info = *info;
    bt_dac_info.generation = stream_generation;
    if (!info->available) named_path[0] = device_name[0] = '\0';
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static unsigned int pcm_format_bit_depth(const char * format) {
    if (!format) return 0;
    if (strstr(format, "S16") || strstr(format, "U16")) return 16;
    if (strstr(format, "S24") || strstr(format, "U24")) return 24;
    if (strstr(format, "S32") || strstr(format, "U32") || strstr(format, "FLOAT")) return 32;
    if (strstr(format, "S8") || strstr(format, "U8")) return 8;
    return 0;
}

static void bt_dac_info_refresh_event(const char * path, bool removed, int running_event) {
    int slot = -1;
    for (int i = 0; i < 32; i++) {
        if (!strcmp(pcm_edges[i].path, path)) { slot = i; break; }
        if (!pcm_edges[i].path[0] && slot < 0) slot = i;
    }
    if (slot < 0) return;
    bool was_running = pcm_edges[slot].running;
    snprintf(pcm_edges[slot].path, sizeof(pcm_edges[slot].path), "%s", path);
    bt_dac_stream_info_t info = {0};
    bt_dac_stream_info_t previous;
    bt_control_get_dac_stream_info(&previous);
    if (removed) {
        bluealsa_clear_soft_volume_path(path);
        pcm_edges[slot].path[0] = '\0';
        pcm_edges[slot].running = false;
        if (!strcmp(previous.pcm_path, path)) {
            stream_generation++;
            bt_dac_info_store(&info);
        }
        return;
    }
    const char * ctl = BLUEALSA_CTL;
    snprintf(info.pcm_path, sizeof(info.pcm_path), "%s", path);
    bt_pcm_path_mac(path, info.mac);
    bluealsa_apply_soft_volume(path);
    info.available = true;
    if (strcmp(named_path, path) != 0) {
        named_path[0] = '\0';
        device_name[0] = '\0';
        char mac[18], device_info[2048];
        if (bt_pcm_path_mac(path, mac) && bt_device_info_text(mac, device_info, sizeof(device_info))) {
            copy_info_field(device_name, sizeof(device_name), device_info, "Alias:");
            if (!device_name[0]) copy_info_field(device_name, sizeof(device_name), device_info, "Name:");
        }
        if (device_name[0]) snprintf(named_path, sizeof(named_path), "%s", path);
    }
    snprintf(info.device_name, sizeof(info.device_name), "%s", device_name);
    char info_out[2048];
    char * info_argv[] = { (char *) ctl, (char *) "info", (char *) path, NULL };
    if (subprocess_run(info_argv, info_out, sizeof(info_out))) {
        copy_info_field(info.codec, sizeof(info.codec), info_out, "Selected codec:");
        copy_info_field(info.pcm_format, sizeof(info.pcm_format), info_out, "Format:");
        const char * sampling = strstr(info_out, "Sampling:");
        if (!sampling) sampling = strstr(info_out, "Rate:");
        const char * channels = strstr(info_out, "Channels:");
        const char * running = strstr(info_out, "Running:");
        if (sampling) {
            const char * value = strchr(sampling, ':');
            if (value) (void) sscanf(value + 1, "%u", &info.sample_rate);
        }
        if (channels) (void) sscanf(channels + strlen("Channels:"), "%u", &info.channels);
        if (running) {
            running += strlen("Running:");
            while (*running == ' ' || *running == '\t') running++;
            info.running = !strncmp(running, "true", 4) || !strncmp(running, "yes", 3) || *running == '1';
        }
        info.bit_depth = pcm_format_bit_depth(info.pcm_format);
    }
    if (running_event >= 0) info.running = running_event != 0;
    pcm_edges[slot].running = info.running;
    bool owner = previous.running;
    bool same = !strcmp(previous.pcm_path, path);
    bool start = info.running && !was_running;
    if (same || (!owner && start && !atomic_load(&output_reserved)) || (!previous.available && !info.running)) {
        if (start || (same && previous.running != info.running)) stream_generation++;
        bt_dac_info_store(&info);
    }
}

static void bt_dac_info_reset(void) {
    memset(pcm_edges, 0, sizeof(pcm_edges));
    stream_generation++;
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
}

static void bt_dac_info_refresh(void) {
    char output[4096];
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "list-pcms", NULL };
    if (!subprocess_run(argv, output, sizeof(output))) { bt_dac_info_reset(); return; }
    char * paths[32], * save = NULL;
    int count = 0;
    for (char * line = strtok_r(output, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
        if (strstr(line, "/a2dpsnk/source") && count < 32) paths[count++] = line;
    /* Snapshot order has no arrival history: sort by the MAC-bearing path. */
    for (int i = 0; i < count; i++) for (int j = i + 1; j < count; j++)
        if (strcmp(paths[i], paths[j]) > 0) { char * p = paths[i]; paths[i] = paths[j]; paths[j] = p; }
    for (int i = 0; i < 32; i++) {
        if (!pcm_edges[i].path[0]) continue;
        bool found = false;
        for (int j = 0; j < count; j++) if (!strcmp(pcm_edges[i].path, paths[j])) found = true;
        if (!found) { char old[256]; memcpy(old, pcm_edges[i].path, sizeof(old)); bt_dac_info_refresh_event(old, true, -1); }
    }
    for (int i = 0; i < count; i++) bt_dac_info_refresh_event(paths[i], false, -1);
}

/* One `bluealsactl monitor` session: returns once that child has exited and
 * been reaped, or right away if it could not be started or a stop landed. */
static void bt_dac_info_monitor_run_once(void) {
    /* Codec and Running are property changes, which the monitor omits unless
     * asked for: a PCM appears before playback starts, so without these the
     * idle-to-running transition never arrives. The set is narrowed because
     * Delay/ClientDelay change continuously during playback and each event
     * costs two subprocesses in bt_dac_info_refresh(). */
    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "monitor",
                      (char *) "--properties=Codec,Running", NULL };
    pid_t pid;
    int fd;
    if (!subprocess_popen(argv, &pid, &fd)) return;

    /* Publish the child only while holding the same lock stop() uses. If a
     * stop landed in the small fork-to-publish window, terminate our own
     * child instead of entering fgets() after stop() already looked for it. */
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }
    bt_dac_info_monitor_pid = pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);

    FILE * f = fdopen(fd, "r");
    if (!f) {
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        subprocess_terminate(pid);
        close(fd);
        return;
    }

    /* Subscribe first so changes occurring during this initial blocking
     * query remain queued in the monitor pipe and cannot be missed. */
    bt_dac_info_refresh();
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ServiceStopped")) bt_dac_info_reset();
        else {
            char * path = strstr(line, "/org/bluealsa/");
            if (!path) continue;
            bool removed = strstr(line, "PCMRemoved") != NULL;
            int running = -1;
            const char * property = strstr(path, "Running");
            if (property) {
                if (strstr(property, "true")) running = 1;
                else if (strstr(property, "false")) running = 0;
            }
            path[strcspn(path, " \t\r\n\"'")] = '\0';
            if (strstr(path, "/a2dpsnk/source")) bt_dac_info_refresh_event(path, removed, running);
        }
    }
    fclose(f);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    /* This thread owns reaping its monitor child. stop() only signals and
     * joins, avoiding two threads racing waitpid() on the same PID. */
    (void) waitpid(pid, NULL, 0);
    bt_dac_info_reset();
}

/* Supervises the monitor for as long as DAC mode is on: if the child exits on
 * its own, a PCM seen by the next session would otherwise keep a stale
 * "already applied" soft-volume entry, so the cache is cleared and a new
 * monitor started (after a short pause, so a child that cannot start does
 * not spin). stop() clears bt_dac_info_active and ends the loop. */
static void * bt_dac_info_thread_func(void * arg) {
    (void) arg;
    for (;;) {
        bt_dac_info_monitor_run_once();
        while (atomic_load(&output_reserved)) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            bool active = bt_dac_info_active;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (!active) break;
            usleep(20000);
        }
        bluealsa_clear_soft_volume_path(NULL);
        bool active = true;
        for (int waited_ms = 0; active && waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            active = bt_dac_info_active;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (active) usleep(50000);
        }
        if (!active) break;
    }
    return NULL;
}


void bt_dac_info_monitor_stop(void) {
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (!bt_dac_info_active) {
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        bt_dac_info_reset(); return;
    }
    bt_dac_info_active = false;
    pid_t pid = bt_dac_info_monitor_pid;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    if (pid > 0) {
        kill(pid, SIGTERM);
        /* fgets() normally unblocks as soon as SIGTERM closes the pipe.
         * Keep the same one-second grace period as subprocess_terminate(),
         * but let the monitor thread itself reap the child. */
        for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
            pthread_mutex_lock(&bt_dac_monitor_mutex);
            bool still_same_child = bt_dac_info_monitor_pid == pid;
            pthread_mutex_unlock(&bt_dac_monitor_mutex);
            if (!still_same_child) break;
            usleep(50000);
        }
        pthread_mutex_lock(&bt_dac_monitor_mutex);
        bool still_same_child = bt_dac_info_monitor_pid == pid;
        pthread_mutex_unlock(&bt_dac_monitor_mutex);
        if (still_same_child) kill(pid, SIGKILL);
    }
    pthread_join(bt_dac_info_thread, NULL);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    bt_dac_info_reset();
}

void bt_dac_info_monitor_start(void) {
    bt_dac_info_monitor_stop();
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    bt_dac_info_active = true;
    if (pthread_create(&bt_dac_info_thread, NULL, bt_dac_info_thread_func, NULL) != 0) {
        bt_dac_info_active = false;
    }
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
}