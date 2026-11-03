#include "bluetooth_control_internal.h"

static pthread_mutex_t bt_dac_info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t bt_dac_monitor_mutex = PTHREAD_MUTEX_INITIALIZER;
static bt_dac_stream_info_t bt_dac_info;
static pthread_t bt_dac_info_thread;
static bool bt_dac_info_active;
static pid_t bt_dac_info_monitor_pid = -1;

void bt_control_get_dac_stream_info(bt_dac_stream_info_t * out) {
    if (!out) return;
    pthread_mutex_lock(&bt_dac_info_mutex);
    *out = bt_dac_info;
    pthread_mutex_unlock(&bt_dac_info_mutex);
}

static void bt_dac_info_store(const bt_dac_stream_info_t * info) {
    pthread_mutex_lock(&bt_dac_info_mutex);
    bt_dac_info = *info;
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

static void bt_dac_info_refresh(void) {
    bt_dac_stream_info_t info = {0};
    const char * ctl = BLUEALSA_CTL;
    char list_out[4096];
    char * list_argv[] = { (char *) ctl, (char *) "list-pcms", NULL };
    if (!subprocess_run(list_argv, list_out, sizeof(list_out))) {
        bt_dac_info_store(&info);
        return;
    }
    char path[256] = {0};
    char * save = NULL;
    for (char * line = strtok_r(list_out, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        if (strstr(line, "/a2dpsnk/source")) {
            snprintf(path, sizeof(path), "%s", line);
            break;
        }
    }
    if (!path[0]) {
        bluealsa_clear_soft_volume_path(NULL);
        bt_dac_info_store(&info); /* PCM disappeared: clear stale data now. */
        return;
    }
    bluealsa_apply_soft_volume(path);
    info.available = true;
    char info_out[2048];
    char * info_argv[] = { (char *) ctl, (char *) "info", path, NULL };
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
    bt_dac_info_store(&info);
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
        if ((strstr(line, "PCMRemoved") && strstr(line, "/a2dpsnk/source")) ||
                strstr(line, "ServiceStopped")) {
            /* A PCM that comes back under the same path (reconnect, daemon
             * restart) starts with its own SoftVolume, so apply it again. */
            bluealsa_clear_soft_volume_path(NULL);
            bt_dac_stream_info_t empty = {0};
            bt_dac_info_store(&empty);
        } else if (strstr(line, "PCM") || strstr(line, "/a2dpsnk/source") || strstr(line, "Codec") || strstr(line, "Running")) {
            bt_dac_info_refresh();
        }
    }
    fclose(f);
    pthread_mutex_lock(&bt_dac_monitor_mutex);
    if (bt_dac_info_monitor_pid == pid) bt_dac_info_monitor_pid = -1;
    pthread_mutex_unlock(&bt_dac_monitor_mutex);
    /* This thread owns reaping its monitor child. stop() only signals and
     * joins, avoiding two threads racing waitpid() on the same PID. */
    (void) waitpid(pid, NULL, 0);
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
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
        bt_dac_stream_info_t empty = {0}; bt_dac_info_store(&empty); return;
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
    bt_dac_stream_info_t empty = {0};
    bt_dac_info_store(&empty);
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