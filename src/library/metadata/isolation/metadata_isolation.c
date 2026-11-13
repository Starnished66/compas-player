#define _FILE_OFFSET_BITS 64
#include "../parsing/metadata.h"
#include "../parsing/metadata_parser_internal.h"
#include "../../artwork/coordinator/artwork_memory.h"
#include "../../../core/process/subprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Isolates decoder-based metadata parsing in one persistent worker process
 * (compas_player --metadata-scan-worker) so a timeout or decoder crash is
 * contained: the worker is SIGKILLed and the next read starts a fresh one.
 * The worker is reused for every file of a scan and stopped when it ends
 * (metadata_scan_worker_stop()).
 *
 * MP3 and raw AAC files skip the worker because they use internal bounded ID3
 * readers. Embedded pictures and lyrics are omitted in isolated reads to keep
 * IPC fixed-size. */
static bool isolated_needs_child(const char * path) {
    const char * ext = strrchr(path, '.');
    if (!ext) return false;
    if (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0) return false;
    return true;
}

/* Children stuck in uninterruptible SD-card I/O cannot be synchronously
 * reaped even after SIGKILL. Keep a small explicit set and retry WNOHANG on
 * later scan calls; never use waitpid(-1), which could steal a child owned
 * by another subsystem. Once the set is full, new isolated reads fail
 * closed instead of creating an unbounded number of unreapable children. */
#define METADATA_ABANDONED_CHILD_MAX 8
typedef struct {
    pthread_mutex_t mutex;
    pid_t children[METADATA_ABANDONED_CHILD_MAX];
    int capacity;
    const char * label;
} metadata_child_pool_t;

static metadata_child_pool_t scan_child_pool = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .capacity = METADATA_ABANDONED_CHILD_MAX,
    .label = "scan",
};
static metadata_child_pool_t artwork_child_pool = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .capacity = 2,
    .label = "artwork",
};
static pthread_mutex_t artwork_isolation_mutex = PTHREAD_MUTEX_INITIALIZER;

static void reap_abandoned_metadata_children(metadata_child_pool_t * pool) {
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        pid_t pid = pool->children[i];
        if (pid <= 0) continue;
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD)) pool->children[i] = 0;
    }
    pthread_mutex_unlock(&pool->mutex);
}

static bool abandoned_metadata_child_capacity_available(metadata_child_pool_t * pool) {
    bool available = false;
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        if (pool->children[i] == 0) {
            available = true;
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    return available;
}

static void remember_abandoned_metadata_child(metadata_child_pool_t * pool, pid_t pid) {
    pthread_mutex_lock(&pool->mutex);
    for (int i = 0; i < pool->capacity; i++) {
        if (pool->children[i] == 0) {
            pool->children[i] = pid;
            pid = 0;
            break;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    if (pid > 0)
        fprintf(stderr, "metadata %s: abandoned-child set full; pid %ld remains for init to reap\n",
                pool->label, (long) pid);
}

static void metadata_read_scan_tags(const char * path, track_metadata_t * out) {
    metadata_read_without_artwork(path, out);
    free(out->picture_data);
    out->picture_data = NULL;
    out->picture_size = 0;
    free(out->lyrics);
    out->lyrics = NULL;
}

static bool read_with_deadline(int fd, void * dst, size_t size,
                               const struct timespec * start, int timeout_ms) {
    size_t total = 0;
    while (total < size) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ms = (int64_t) (now.tv_sec - start->tv_sec) * 1000 +
                             (int64_t) (now.tv_nsec - start->tv_nsec) / 1000000;
        int remaining_ms = timeout_ms - (int) elapsed_ms;
        if (remaining_ms <= 0) return false;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr;
        do pr = poll(&pfd, 1, remaining_ms); while (pr < 0 && errno == EINTR);
        if (pr <= 0) return false;
        ssize_t n;
        do n = read(fd, (char *) dst + total, size - total); while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        total += (size_t) n;
    }
    return true;
}

static bool write_all(int fd, const void * src, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n;
        do n = write(fd, (const char *) src + total, size - total);
        while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        total += (size_t) n;
    }
    return true;
}

/* A helper process is disposable; under unexpected memory pressure the
 * kernel should discard it instead of the interactive player. */
static void metadata_helper_make_disposable(void) {
    int oom_fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (oom_fd >= 0) {
        const char score[] = "500";
        ssize_t ignored = write(oom_fd, score, sizeof(score) - 1);
        (void) ignored;
        close(oom_fd);
    }
}

static bool read_all(int fd, void * dst, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n;
        do n = read(fd, (char *) dst + total, size - total);
        while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        total += (size_t) n;
    }
    return true;
}

extern char ** environ;

int metadata_scan_worker_run(int request_fd, int response_fd) {
    if (request_fd < 0 || response_fd < 0) return 1;
    (void) setpriority(PRIO_PROCESS, 0, 19);
    metadata_helper_make_disposable();

    for (;;) {
        uint32_t len = 0;
        if (!read_all(request_fd, &len, sizeof(len))) return 0; /* EOF: scan over */
        if (len == 0 || len >= PATH_MAX) return 1;
        char path[PATH_MAX];
        if (!read_all(request_fd, path, len)) return 1;
        path[len] = '\0';

        track_metadata_t result;
        metadata_read_scan_tags(path, &result);
        if (!write_all(response_fd, &result, sizeof(result))) return 1;
    }
}

/* Parent side: one worker, guarded by scan_worker_mutex. */
static pthread_mutex_t scan_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pid_t scan_worker_pid = 0;
static int scan_worker_request_fd = -1;
static int scan_worker_response_fd = -1;

static bool scan_worker_start_locked(void) {
    int req[2], resp[2];
    if (pipe2(req, O_CLOEXEC) != 0) return false;
    if (pipe2(resp, O_CLOEXEC) != 0) {
        close(req[0]);
        close(req[1]);
        return false;
    }
    char req_arg[24], resp_arg[24];
    snprintf(req_arg, sizeof(req_arg), "%d", req[0]);
    snprintf(resp_arg, sizeof(resp_arg), "%d", resp[1]);
    char * const argv[] = {
        (char *) "compas_player",
        (char *) "--metadata-scan-worker",
        req_arg,
        resp_arg,
        NULL,
    };
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc == 0) {
        rc = posix_spawn_file_actions_adddup2(&fa, req[0], req[0]);
        if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, resp[1], resp[1]);
        if (rc == 0) rc = subprocess_close_inherited_fds_action(&fa);
        if (rc == 0) rc = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
    }
    close(req[0]);
    close(resp[1]);
    if (rc != 0) {
        close(req[1]);
        close(resp[0]);
        fprintf(stderr, "metadata scan: worker spawn failed: %s\n", strerror(rc));
        return false;
    }
    scan_worker_pid = pid;
    scan_worker_request_fd = req[1];
    scan_worker_response_fd = resp[0];
    return true;
}

/* The worker is idle or stuck whenever this runs, so it is killed rather
 * than asked to exit. Bounded reap, as in the artwork helper: a process stuck
 * in uninterruptible I/O stays unreapable even after SIGKILL, so never block
 * on it; remember it and retry later. */
static void scan_worker_stop_locked(void) {
    if (scan_worker_pid > 0) {
        pid_t pid = scan_worker_pid;
        kill(pid, SIGKILL);
        close(scan_worker_request_fd);
        close(scan_worker_response_fd);
        scan_worker_pid = 0;
        scan_worker_request_fd = -1;
        scan_worker_response_fd = -1;

        struct timespec start, now;
        clock_gettime(CLOCK_MONOTONIC, &start);
        bool reaped = false;
        for (;;) {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) { reaped = true; break; }
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsed_ms = (int64_t) (now.tv_sec - start.tv_sec) * 1000 +
                                 (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed_ms >= 1000) break;
            usleep(1000);
        }
        if (!reaped) remember_abandoned_metadata_child(&scan_child_pool, pid);
    }
    reap_abandoned_metadata_children(&scan_child_pool);
}

static bool scan_worker_send_locked(const char * path, uint32_t len) {
    return write_all(scan_worker_request_fd, &len, sizeof(len)) &&
           write_all(scan_worker_request_fd, path, len);
}

bool metadata_read_isolated(const char * path, track_metadata_t * out, int timeout_ms) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) return false;
    if (!isolated_needs_child(path)) {
        metadata_read_scan_tags(path, out);
        return true;
    }
    /* Scans name files through the player's open root directory
     * (/proc/self/fd/N/...), which the worker does not inherit. Point the
     * worker at the player's own /proc entry so it resolves the same file. */
    char worker_path[PATH_MAX];
    const char * request_path = path;
    const char self_prefix[] = "/proc/self/";
    if (strncmp(path, self_prefix, sizeof(self_prefix) - 1) == 0) {
        int n = snprintf(worker_path, sizeof(worker_path), "/proc/%ld/%s",
                         (long) getpid(), path + sizeof(self_prefix) - 1);
        if (n < 0 || (size_t) n >= sizeof(worker_path)) return false;
        request_path = worker_path;
    }
    size_t len = strlen(request_path);
    if (len >= PATH_MAX) return false;

    pthread_mutex_lock(&scan_worker_mutex);
    bool ok = false;
    /* A worker that died while idle (the OOM killer prefers it) fails the
     * request write. That file was never tried, so start a fresh worker and
     * send it once more; only a failed parse counts against the file. */
    bool sent = false;
    for (int attempt = 0; attempt < 2 && !sent; attempt++) {
        if (scan_worker_pid <= 0) {
            reap_abandoned_metadata_children(&scan_child_pool);
            if (!abandoned_metadata_child_capacity_available(&scan_child_pool)) {
                fprintf(stderr, "metadata scan: too many unreaped parser children; skipping %s\n", path);
                break;
            }
            if (!scan_worker_start_locked()) break;
        }
        sent = scan_worker_send_locked(request_path, (uint32_t) len);
        if (!sent) {
            fprintf(stderr, "metadata scan: parser worker unreachable for %s\n", path);
            scan_worker_stop_locked();
        }
    }
    if (sent) {
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        ok = read_with_deadline(scan_worker_response_fd, out, sizeof(*out), &start, timeout_ms);
        if (!ok) {
            fprintf(stderr, "metadata scan: parser worker failed for %s\n", path);
            memset(out, 0, sizeof(*out));
            scan_worker_stop_locked();
        }
    }
    pthread_mutex_unlock(&scan_worker_mutex);
    return ok;
}

void metadata_scan_worker_stop(void) {
    pthread_mutex_lock(&scan_worker_mutex);
    scan_worker_stop_locked();
    pthread_mutex_unlock(&scan_worker_mutex);
}

static bool metadata_artwork_limit_memory(artwork_priority_t prio) {
    /* Bound ALL parser allocations, including third-party container readers.
     * The limit is headroom, not an allocation: a small picture only uses
     * its actual bytes. Reserve matches the CALLER's real priority (see
     * artwork_reserve_bytes_for_priority()'s own comment) -- this used to
     * always use the warmer's own 8 MiB reserve regardless of priority,
     * refusing to even attempt an interactive PRIO_PLAYER extraction under
     * the same ~9MB-available threshold that only the background warmer
     * should be held to. */
    size_t available = system_get_mem_available_bytes();
    const size_t reserve = artwork_reserve_bytes_for_priority(prio);
    if (available <= reserve + 1024U * 1024U) return false;
    size_t budget = available - reserve;
    if (budget > 12U * 1024U * 1024U) budget = 12U * 1024U * 1024U;
    FILE * statm = fopen("/proc/self/statm", "r");
    unsigned long pages = 0;
    bool have_size = statm && fscanf(statm, "%lu", &pages) == 1;
    if (statm) fclose(statm);
    long page_size = sysconf(_SC_PAGESIZE);
    struct rlimit limit;
    if (!have_size || page_size <= 0 || getrlimit(RLIMIT_AS, &limit) != 0) return false;
    uint64_t ceiling = (uint64_t) pages * (unsigned long) page_size + budget;
    if (ceiling > SIZE_MAX) return false;
    if (limit.rlim_cur == RLIM_INFINITY || ceiling < limit.rlim_cur)
        limit.rlim_cur = (rlim_t) ceiling;
    return setrlimit(RLIMIT_AS, &limit) == 0;
}

int metadata_artwork_helper_run(const char * path, int output_fd, artwork_priority_t prio) {
    if (!path || !path[0] || output_fd < 0) return 1;
    if (!metadata_artwork_limit_memory(prio)) return 1;
    metadata_parser_enable_artwork_only();

    metadata_helper_make_disposable();

    track_metadata_t result;
    errno = 0;
    metadata_read(path, &result);
    if (!result.picture_data && errno == ENOMEM) {
        free(result.lyrics);
        return 1;
    }
    uint8_t * picture = result.picture_data;
    uint32_t picture_size = result.picture_size;
    char * lyrics = result.lyrics;
    result.picture_data = NULL;
    result.lyrics = NULL;

    bool ok = write_all(output_fd, &result, sizeof(result));
    if (ok && picture_size > 0)
        ok = picture && write_all(output_fd, picture, picture_size);
    free(picture);
    free(lyrics);
    close(output_fd);
    return ok ? 0 : 1;
}

static metadata_artwork_result_t metadata_read_artwork_isolated_impl(const char * path,
                                                                     track_metadata_t * out,
                                                                     int timeout_ms,
                                                                     artwork_priority_t prio) {
    memset(out, 0, sizeof(*out));
    if (!path || !path[0] || timeout_ms <= 0) return METADATA_ARTWORK_TEMPORARY_FAILURE;

    reap_abandoned_metadata_children(&artwork_child_pool);
    if (!abandoned_metadata_child_capacity_available(&artwork_child_pool))
        return METADATA_ARTWORK_TEMPORARY_FAILURE;

    /* O_CLOEXEC so neither end leaks into any other child spawned meanwhile;
     * the write end is explicitly re-inherited by the helper below. */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return METADATA_ARTWORK_TEMPORARY_FAILURE;
    char output_fd_arg[24];
    snprintf(output_fd_arg, sizeof(output_fd_arg), "%d", pipefd[1]);
    char prio_arg[8];
    snprintf(prio_arg, sizeof(prio_arg), "%d", (int) prio);

    /* posix_spawn() (CLONE_VM|CLONE_VFORK) instead of fork(): no copy of the
     * player's address space while the audio/UI threads keep dirtying it. The
     * helper applies its own RLIMIT_AS and oom_score_adj at startup
     * (metadata_artwork_helper_run()), so nothing needs to run in the child
     * before exec. The helper keeps the same fd number as before: dup2 of an
     * fd onto itself clears its close-on-exec flag. Every other fd without
     * close-on-exec is closed in the child. */
    char * const argv[] = {
        (char *) "compas_player",
        (char *) "--metadata-artwork-helper",
        output_fd_arg,
        (char *) path,
        prio_arg,
        NULL,
    };
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int spawn_rc = posix_spawn_file_actions_init(&fa);
    if (spawn_rc == 0) {
        spawn_rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], pipefd[1]);
        if (spawn_rc == 0) spawn_rc = subprocess_close_inherited_fds_action(&fa);
        if (spawn_rc == 0) spawn_rc = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
    }
    if (spawn_rc != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return METADATA_ARTWORK_TEMPORARY_FAILURE;
    }

    close(pipefd[1]);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    track_metadata_t result;
    bool ok = read_with_deadline(pipefd[0], &result, sizeof(result), &start, timeout_ms);
    metadata_artwork_result_t outcome = METADATA_ARTWORK_TEMPORARY_FAILURE;
    uint8_t * picture = NULL;
    if (ok && result.picture_size > EMBEDDED_COVER_MAX_BYTES) {
        outcome = METADATA_ARTWORK_INVALID;
        ok = false;
    }
    if (ok && result.picture_too_large && result.picture_size == 0) {
        outcome = METADATA_ARTWORK_TOO_LARGE;
    } else if (ok && result.picture_size > 0) {
        /* The helper still owns its copy. Charge only the additional copy
         * against current free memory, not a fixed worst-case tag size --
         * against the CALLER's real reserve (`prio`), not always the
         * warmer's own stricter one (see this function's own header
         * comment). */
        picture = artwork_check_memory_admission(prio, result.picture_size)
            ? malloc(result.picture_size) : NULL;
        ok = picture && read_with_deadline(pipefd[0], picture, result.picture_size, &start, timeout_ms);
    }
    close(pipefd[0]);

    if (ok) {
        result.picture_data = picture;
        result.lyrics = NULL;
        *out = result;
        if (result.picture_too_large && !picture) outcome = METADATA_ARTWORK_TOO_LARGE;
        else outcome = picture ? METADATA_ARTWORK_FOUND : METADATA_ARTWORK_NOT_FOUND;
    } else {
        free(picture);
        kill(pid, SIGKILL);
    }

    for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                free(out->picture_data);
                memset(out, 0, sizeof(*out));
                return outcome == METADATA_ARTWORK_INVALID
                    ? METADATA_ARTWORK_INVALID
                    : outcome == METADATA_ARTWORK_TOO_LARGE
                    ? METADATA_ARTWORK_TOO_LARGE
                    : METADATA_ARTWORK_TEMPORARY_FAILURE;
            }
            return outcome;
        }
        usleep(50000);
    }
    kill(pid, SIGKILL);
    remember_abandoned_metadata_child(&artwork_child_pool, pid);
    return outcome;
}

metadata_artwork_result_t metadata_read_artwork_isolated(const char * path,
                                                         track_metadata_t * out,
                                                         int timeout_ms,
                                                         artwork_priority_t prio) {
    if (!out) return METADATA_ARTWORK_TEMPORARY_FAILURE;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&artwork_isolation_mutex);
    metadata_artwork_result_t result =
        metadata_read_artwork_isolated_impl(path, out, timeout_ms, prio);
    pthread_mutex_unlock(&artwork_isolation_mutex);
    return result;
}
