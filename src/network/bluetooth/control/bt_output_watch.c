#include "bluetooth_control_internal.h"

/* See bt_control_output_disconnect_watch_start()'s own doc comment
 * (bluetooth_control.h) for what this is and why. Plain `monitor`, no `-p`
 * needed -- confirmed via `strings` on the real bluealsactl binary that
 * PCMAdded/PCMRemoved come from its base InterfacesAdded/InterfacesRemoved
 * subscription (path_namespace='/org/bluealsa'), which is always active;
 * `-p` only adds the separate PropertyChanged stream (Volume/Codec/Running/
 * SoftVolume) the volume-sync threads (see bt_source_volume.c) already use -- this doesn't
 * need any of that. "/a2dpsrc/sink" matches find_source_pcm_path()'s own
 * naming exactly: the PCM this app itself writes local playback into
 * (audio_output.c's `aplay -D bluealsa`), not any other PCM bluealsa might
 * have (e.g. one from DAC mode). */
static pthread_t bt_output_disconnect_thread;
static atomic_bool bt_output_disconnect_active = false;
static bool bt_output_disconnect_joinable;
static atomic_bool bt_output_disconnect_flag = false;
static pthread_mutex_t bt_output_disconnect_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char bt_output_disconnect_confirmed_path[256];

#define BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS 16
/* Codec re-selection after a failed attempt: 2 s, 4 s, 6 s, 8 s apart. */
#define BT_CODEC_RETRY_BASE_MS 2000u
#define BT_CODEC_RETRY_MAX_ATTEMPTS 4

/* Preference worker, owned by the output watch thread. Codec, rate and
 * soft-volume selection run bluealsactl subprocesses bounded by their own
 * timeouts; doing that here keeps the watch thread reading PCMRemoved/
 * PCMAdded, so a disconnect is noticed on time. The watch thread and
 * reconciliation only post requests (short lock, no I/O).
 *
 * One PCM is tracked at a time. A failed codec selection is retried
 * BT_CODEC_RETRY_MAX_ATTEMPTS times, spaced out, and never while a
 * disconnect is pending. The budget belongs to the PCM path: a PCMAdded for
 * the same path (a replacement, which a codec selection itself can cause)
 * asks only for rate and soft volume to be re-applied, so a device that
 * keeps falling back is not switched back and forth without end; only a
 * different path or a confirmed disconnect starts over. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool running;        /* read/written under mutex */
    atomic_bool stop;
    bool paused;         /* a removal is pending: no new selection */
    bool in_flight;      /* a selection pass is running */
    bool scan_requested; /* look for a PCM that predates the subscription */
    bool codec_due;      /* select the codec for path now */
    bool refresh_due;    /* re-apply rate and soft volume only */
    char path[256];
    int attempts;        /* failed codec selections for path */
    uint32_t retry_at;
    bool discovery_pending; /* waiting for SEP/name discovery; no attempt charged */
} bt_pref = { .mutex = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

void bt_pref_rate_changed(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (bt_pref.running && !atomic_load(&bt_pref.stop)) {
        if (bt_pref.path[0]) bt_pref.refresh_due = true;
        else bt_pref.scan_requested = true;
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

static bool bt_pref_retry_scheduled_locked(void) {
    return bt_pref.path[0] && (bt_pref.discovery_pending ||
           (bt_pref.attempts > 0 && bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS));
}

/* Called with bt_pref.mutex held. Discovery deferrals do not consume the
 * finite selection-failure budget. */
static void bt_pref_record_codec_result_locked(const char *path, bool applied, uint32_t defer_until) {
    if (strcmp(bt_pref.path, path) != 0) return;
    if (defer_until) {
        bt_pref.discovery_pending = true;
        bt_pref.retry_at = defer_until;
    } else {
        bt_pref.discovery_pending = false;
        if (applied) {
            bt_pref.attempts = 0;
        } else if (++bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS) {
            bt_pref.retry_at = bt_control_monotonic_ms() + BT_CODEC_RETRY_BASE_MS * (uint32_t) bt_pref.attempts;
        } else {
            DBG_LOG("bt_control: codec selection for %s gave up after %d attempts\n",
                    path, bt_pref.attempts);
        }
    }
}

static void bt_pref_begin_work_locked(bool codec_work) {
    bt_pref.codec_due = false;
    if (codec_work) bt_pref.discovery_pending = false;
    bt_pref.refresh_due = false;
}

static void * bt_pref_worker_func(void * arg) {
    (void) arg;
    pthread_mutex_lock(&bt_pref.mutex);
    while (!atomic_load(&bt_pref.stop)) {
        uint32_t now = bt_control_monotonic_ms();
        bool retry_due = !bt_pref.paused && bt_pref_retry_scheduled_locked() &&
                         (int32_t) (now - bt_pref.retry_at) >= 0;
        bool codec_work = !bt_pref.paused && bt_pref.path[0] && (bt_pref.codec_due || retry_due);
        bool refresh_work = !bt_pref.paused && bt_pref.path[0] && bt_pref.refresh_due;
        if (bt_pref.scan_requested) {
            bt_pref.scan_requested = false;
            pthread_mutex_unlock(&bt_pref.mutex);
            char found[256];
            bool present = !atomic_load(&bt_pref.stop) &&
                           find_source_pcm_path_ex(found, sizeof(found), false);
            pthread_mutex_lock(&bt_pref.mutex);
            if (present && !bt_pref.path[0]) {
                snprintf(bt_pref.path, sizeof(bt_pref.path), "%s", found);
                bt_pref.attempts = 0;
                bt_pref.discovery_pending = false;
                bt_pref.codec_due = true;
            }
            continue;
        }
        if (!codec_work && !refresh_work) {
            uint32_t wait_ms = 1000;
            if (!bt_pref.paused && bt_pref_retry_scheduled_locked()) {
                int32_t left = (int32_t) (bt_pref.retry_at - now);
                wait_ms = left <= 0 ? 0 : (uint32_t) left < wait_ms ? (uint32_t) left : wait_ms;
            }
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_ms / 1000;
            ts.tv_nsec += (long) (wait_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&bt_pref.cond, &bt_pref.mutex, &ts);
            continue;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s", bt_pref.path);
        bt_pref_begin_work_locked(codec_work);
        bt_pref.in_flight = true;
        pthread_mutex_unlock(&bt_pref.mutex);

        /* Codec first: selecting one can cycle the PCM, which would
         * otherwise drop a soft-volume setting applied just before. Stop is
         * checked between steps, so it waits for at most one of them. */
        uint32_t defer_until = 0;
        bool applied = codec_work ? bluealsa_apply_codec_preference_deferred(path, &bt_pref.stop,
                                                                               &defer_until) : true;
        /* Refresh-only passes too: the rate step waits for a verified codec,
         * so it never reconfigures a fallback codec or pre-empts a retry. */
        bool codec_verified = bluealsa_codec_verified(path);
        /* The rate belongs to the codec just selected: skipped while that
         * selection has not succeeded, so a fallback codec's refusal is not
         * recorded (and later applied) in its place. */
        bool rate_reconfigured = applied && codec_verified && !atomic_load(&bt_pref.stop) &&
                                 bluealsa_apply_rate_preference_cancellable(path, &bt_pref.stop);
        if (rate_reconfigured) {
            /* The link was re-established for the rate: the codec it ended
             * up on must be checked again (cheap when unchanged). */
            bluealsa_clear_codec_path(path);
        }
        bluealsa_apply_soft_volume_cancellable_ex(path, &bt_pref.stop, false);

        pthread_mutex_lock(&bt_pref.mutex);
        bt_pref.in_flight = false;
        if (rate_reconfigured && applied && strcmp(bt_pref.path, path) == 0 &&
            bt_pref.attempts < BT_CODEC_RETRY_MAX_ATTEMPTS)
            bt_pref.codec_due = true;
        if (codec_work && strcmp(bt_pref.path, path) == 0) {
            bt_pref_record_codec_result_locked(path, applied, defer_until);
        }
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return NULL;
}

static void bt_pref_start(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    atomic_store(&bt_pref.stop, false);
    bt_pref.paused = false;
    bt_pref.in_flight = false;
    bt_pref.scan_requested = false;
    bt_pref.codec_due = false;
    bt_pref.refresh_due = false;
    bt_pref.path[0] = '\0';
    bt_pref.attempts = 0;
    bt_pref.discovery_pending = false;
    bt_pref.running = pthread_create(&bt_pref.thread, NULL, bt_pref_worker_func, NULL) == 0;
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* Joins the worker; it finishes at most the step it is running. */
static void bt_pref_stop(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running;
    atomic_store(&bt_pref.stop, true);
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
    if (running) pthread_join(bt_pref.thread, NULL);
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.running = false;
    bt_pref.discovery_pending = false;
    bt_pref.attempts = 0;
    bt_pref.path[0] = '\0';
    pthread_mutex_unlock(&bt_pref.mutex);
}

static void bt_pref_request_scan(void) {
    pthread_mutex_lock(&bt_pref.mutex);
    bt_pref.scan_requested = true;
    pthread_cond_signal(&bt_pref.cond);
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A PCMAdded (or reconciliation). Returns false when no worker is running,
 * leaving the caller to apply inline. */
bool bt_pref_request(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    bool running = bt_pref.running && !atomic_load(&bt_pref.stop);
    if (running) {
        if (strcmp(bt_pref.path, path) != 0) {
            snprintf(bt_pref.path, sizeof(bt_pref.path), "%.*s", (int) sizeof(bt_pref.path) - 1, path);
            bt_pref.attempts = 0;
            bt_pref.discovery_pending = false;
            bt_pref.codec_due = true;
            bt_pref.refresh_due = false;
        } else {
            /* Same PCM again. Its soft volume must be re-applied; the codec
             * only when no selection is in flight or scheduled (a settled
             * success is then a cheap latch check). */
            bt_pref.refresh_due = true;
            if (!bt_pref.in_flight && bt_pref.attempts == 0 && !bt_pref.discovery_pending)
                bt_pref.codec_due = true;
        }
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
    return running;
}

/* Mirrors the watch's pending-removal state: no new selection meanwhile. */
static void bt_pref_set_paused(bool paused) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (bt_pref.paused != paused) {
        bt_pref.paused = paused;
        pthread_cond_signal(&bt_pref.cond);
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

/* A confirmed disconnect: the next connection starts with a fresh budget. */
static void bt_pref_forget(const char * path) {
    pthread_mutex_lock(&bt_pref.mutex);
    if (!path || strcmp(bt_pref.path, path) == 0) {
        bt_pref.path[0] = '\0';
        bt_pref.attempts = 0;
        bt_pref.discovery_pending = false;
        bt_pref.codec_due = false;
        bt_pref.refresh_due = false;
    }
    pthread_mutex_unlock(&bt_pref.mutex);
}

static uint32_t bt_output_removed_at; /* when the pending removal was seen; watch thread only */

static void bt_output_disconnect_process_line(const char * line, char * pending_path,
                                              uint32_t * pending_deadline,
                                              char added_paths[][256], int * added_count) {
    char path[256];
    if (sscanf(line, "PCMRemoved %255s", path) == 1) {
        if (strstr(path, "/a2dpsrc/sink") != NULL) {
            snprintf(pending_path, 256, "%s", path);
            bt_output_removed_at = bt_control_monotonic_ms();
            *pending_deadline = bt_output_removed_at + BT_OUTPUT_RECONFIGURE_GRACE_MS;
            /* Soft volume only: the PCM is recreated by a reconfigure, so
             * that setting must be re-applied. The codec latch deliberately
             * survives here and is cleared only once a disconnect is
             * actually confirmed -- selecting a codec can itself cycle the
             * PCM, and clearing on every PCMRemoved would let the resulting
             * PCMAdded re-select, cycling it again without end. */
            bluealsa_clear_soft_volume_path(path);
            DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- pending disconnect\n", path);
        }
        return;
    }
    if (strstr(line, "ServiceStopped")) {
        /* A daemon that died may never report PCMRemoved; its PCMs are gone either way. */
        bluealsa_clear_soft_volume_path(NULL);
        return;
    }
    if (strncmp(line, "PCMAdded ", 9) != 0 || sscanf(line, "PCMAdded %255s", path) != 1) return;
    bt_auto_pcm_added(path);
    if (strcmp(pending_path, path) == 0) {
        pending_path[0] = '\0';
        *pending_deadline = 0;
        DBG_LOG("bt_control: output_disconnect_watch: PCMAdded %s -- cancelled pending disconnect\n", path);
    }
    /* A replacement can arrive after the deadline but before the UI consumes
     * the event. Cancel only the event for this exact PCM, under the same
     * short lock used by the UI consumer. Never hold it during control I/O. */
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (strcmp(bt_output_disconnect_confirmed_path, path) == 0) {
        atomic_store(&bt_output_disconnect_flag, false);
        bt_output_disconnect_confirmed_path[0] = '\0';
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (*added_count < BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS)
        snprintf(added_paths[(*added_count)++], 256, "%s", path);
    DBG_LOG("bt_control: output_disconnect_watch: %s\n", line);
}

static bool bt_output_disconnect_read_batch(int fd, char * buf, size_t * used,
                                            char * pending_path, uint32_t * pending_deadline,
                                            char added_paths[][256], int * added_count) {
    if (*used == 511) *used = 0;
    ssize_t n = read(fd, buf + *used, 511 - *used);
    if (n <= 0) return false;
    *used += (size_t)n;
    buf[*used] = '\0';
    char * line = buf;
    char * end;
    while ((end = strchr(line, '\n')) != NULL) {
        *end = '\0';
        bt_output_disconnect_process_line(line, pending_path, pending_deadline,
                                          added_paths, added_count);
        line = end + 1;
    }
    size_t remaining = *used - (size_t)(line - buf);
    memmove(buf, line, remaining);
    *used = remaining;
    return true;
}

static void bt_output_disconnect_publish_if_due(char * pending_path, uint32_t * pending_deadline) {
    if (!pending_path[0] || (int32_t)(bt_control_monotonic_ms() - *pending_deadline) < 0) return;
    /* A codec switch of ours is recreating the transport: keep waiting for
     * the PCM (up to BT_SSC_SWITCH_CAP_MS) instead of calling it a drop. */
    uint32_t hold = bt_auto_reconfigure_deadline(pending_path, bt_output_removed_at);
    if (hold && (int32_t)(hold - bt_control_monotonic_ms()) > 0) {
        *pending_deadline = hold;
        DBG_LOG("bt_control: output_disconnect_watch: %s -- codec switch in flight, waiting\n", pending_path);
        return;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    if (atomic_load(&bt_output_disconnect_active) && !atomic_load(&bt_output_disconnect_flag)) {
        snprintf(bt_output_disconnect_confirmed_path, sizeof(bt_output_disconnect_confirmed_path), "%s", pending_path);
        atomic_store(&bt_output_disconnect_flag, true);
    }
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    DBG_LOG("bt_control: output_disconnect_watch: PCMRemoved %s -- flagging disconnect\n", pending_path);
    bt_auto_drop_confirmed(pending_path, bt_output_removed_at);
    bluealsa_clear_codec_path(pending_path); /* real disconnect: next connect must re-select */
    bt_buds_reset(pending_path);
    bt_pref_forget(pending_path);
    pending_path[0] = '\0';
    *pending_deadline = 0;
}

static void * bt_output_disconnect_thread_func(void * arg) {
    (void) arg;

    char * argv[] = { (char *) BLUEALSA_CTL, (char *) "monitor", NULL };
    pid_t pid;
    int read_fd;
    if (!subprocess_popen(argv, &pid, &read_fd)) {
        DBG_LOG("bt_control: output_disconnect_watch: failed to spawn bluealsactl monitor\n");
        atomic_store(&bt_output_disconnect_active, false);
        return NULL;
    }
    DBG_LOG("bt_control: output_disconnect_watch: monitor started (pid %d)\n", (int) pid);
    uint32_t watch_started_at = bt_control_monotonic_ms();
    atomic_store_explicit(&bt_output_watch_started_at, watch_started_at ? watch_started_at : 1,
                          memory_order_relaxed);

    /* The GUI starts this watch only after it has seen the A2DP link, so the
     * PCM of that connection usually predates the subscription and never
     * produces PCMAdded here. That left a first connection (no BlueZ
     * LastUsed endpoint yet, e.g. right after pairing) on whatever codec
     * BlueZ happened to pick, such as AAC instead of a chosen LDAC, until a
     * reconnect. The worker scans for such a PCM once the monitor has
     * printed its first line: bluealsactl installs its D-Bus matches before
     * printing ServiceRunning/ServiceStopped, so from then on any new PCM is
     * reported below; if nothing arrives the scan still runs after 2 s. */
    bt_pref_start();
    bool scanned_on_ready = false, scanned_on_fallback = false;
    uint32_t scan_fallback_at = bt_control_monotonic_ms() + 2000;

    char buf[512];
    size_t used = 0;
    char pending_path[256] = "";
    uint32_t pending_deadline = 0;
    char added_paths[BT_OUTPUT_DISCONNECT_MAX_ADDED_PATHS][256];
    int added_count = 0;
    while (atomic_load(&bt_output_disconnect_active)) {
        uint32_t now = bt_control_monotonic_ms();
        int timeout_ms = 100;
        if (pending_path[0]) {
            int32_t remaining = (int32_t)(pending_deadline - now);
            timeout_ms = remaining <= 0 ? 0 : (remaining < timeout_ms ? remaining : timeout_ms);
        }
        struct pollfd pfd = { .fd = read_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (pr < 0) { if (errno == EINTR) continue; break; }
        bool terminal = false;
        if (pr > 0 && (pfd.revents & POLLIN))
            terminal = !bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                         &pending_deadline, added_paths, &added_count);
        /* Scan on readiness; a fallback scan (monitor slow to print) does
         * not replace it, since a PCM could appear between the two. */
        if (!scanned_on_ready && pr > 0 && (pfd.revents & POLLIN) && !terminal) {
            scanned_on_ready = true;
            bt_pref_request_scan();
        } else if (!scanned_on_ready && !scanned_on_fallback &&
                   (int32_t) (bt_control_monotonic_ms() - scan_fallback_at) >= 0) {
            scanned_on_fallback = true;
            bt_pref_request_scan();
        }
        if (!terminal) {
            struct pollfd drain = { .fd = read_fd, .events = POLLIN };
            while (atomic_load(&bt_output_disconnect_active) && poll(&drain, 1, 0) > 0) {
                if (!(drain.revents & POLLIN)) {
                    terminal = (drain.revents & (POLLERR | POLLNVAL | POLLHUP)) != 0;
                    break;
                }
                if (!bt_output_disconnect_read_batch(read_fd, buf, &used, pending_path,
                                                     &pending_deadline, added_paths, &added_count)) {
                    terminal = true;
                    break;
                }
                drain.revents = 0;
            }
        }
        if (!atomic_load(&bt_output_disconnect_active)) break;
        if (terminal || (pr > 0 && (pfd.revents & (POLLERR | POLLNVAL | POLLHUP)))) {
            /* A failed monitor may have missed the replacement. Leave an
             * unconfirmed removal to periodic polling rather than inventing
             * a disconnect after its event stream has ended. */
            break;
        }
        bt_output_disconnect_publish_if_due(pending_path, &pending_deadline);
        if (!pending_path[0]) bt_auto_flowing_tick();
        bt_pref_set_paused(pending_path[0] != '\0');
        /* Do not let volume subprocesses delay reading a replacement while
         * a removal is pending. Keep the bounded queue until it settles. */
        if (!pending_path[0]) {
            for (int i = 0; i < added_count; i++) {
                if (!bt_pref_request(added_paths[i])) {
                    /* No worker (thread creation failed): apply inline. */
                    bluealsa_apply_codec_preference(added_paths[i]);
                    bluealsa_apply_rate_preference(added_paths[i]);
                    bluealsa_apply_soft_volume(added_paths[i]);
                }
            }
            added_count = 0;
        }
    }

    DBG_LOG("bt_control: output_disconnect_watch: monitor exited (pid %d)\n", (int) pid);
    bt_pref_stop();
    bt_output_disconnect_finish_pending(pending_path, bt_output_removed_at);
    /* The monitor can exit with a removal still unconfirmed, which would
     * leave the latch set and suppress the re-select on the next connect. */
    bluealsa_clear_codec_path(NULL);
    bt_buds_reset(NULL);
    close(read_fd);
    subprocess_terminate(pid);
    atomic_store(&bt_output_disconnect_active, false);
    return NULL;
}

bool bt_control_output_disconnect_watch_is_running(void) {
    return atomic_load(&bt_output_disconnect_active);
}

void bt_control_output_disconnect_watch_start(void) {
    if (bt_output_disconnect_joinable) {
        if (atomic_load(&bt_output_disconnect_active)) return;
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bt_output_disconnect_active = true;
    bt_output_disconnect_flag = false;
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    if (pthread_create(&bt_output_disconnect_thread, NULL, bt_output_disconnect_thread_func, NULL) == 0)
        bt_output_disconnect_joinable = true;
    else bt_output_disconnect_active = false;
}

void bt_control_output_disconnect_watch_stop(void) {
    bt_output_disconnect_active = false;
    if (bt_output_disconnect_joinable) {
        pthread_join(bt_output_disconnect_thread, NULL);
        bt_output_disconnect_joinable = false;
    }
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    atomic_store(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
}

bool bt_control_output_disconnect_consume(void) {
    pthread_mutex_lock(&bt_output_disconnect_state_mutex);
    bool disconnected = atomic_exchange(&bt_output_disconnect_flag, false);
    bt_output_disconnect_confirmed_path[0] = '\0';
    pthread_mutex_unlock(&bt_output_disconnect_state_mutex);
    return disconnected;
}