#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* Closes all file descriptors above stderr in the child after fork().
 * Many open()/pcm_open() calls in this codebase (including tinyalsa's
 * pcm_hw_open()) do not set O_CLOEXEC, so every fd is duplicated into
 * any forked child. Long-lived daemons spawned via exec can permanently
 * hold duplicates of, e.g., the ALSA PCM fd, causing EBUSY on every
 * subsequent pcm_open() in the parent even after its own handle is closed.
 * Closing inherited fds in the child right before exec protects every
 * subprocess call regardless of what the parent has open at the time.
 * Safe after fork(): the child is single-threaded at that point. */
static void close_inherited_fds(void) {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) return;
    int dir_fd = dirfd(dir);

    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue; /* skip "." / ".." */
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != dir_fd) close(fd);
    }
    closedir(dir);
}

extern char ** environ;

/* posix_spawn() equivalent of close_inherited_fds(): queues a close in the
 * child for every fd above stderr that lacks FD_CLOEXEC (those are the ones
 * that would otherwise survive exec -- see close_inherited_fds()). fds that
 * are already close-on-exec are skipped, so a descriptor another thread
 * closes between this scan and the spawn is the only possible stale entry.
 * Must be queued AFTER any adddup2() whose source it would close. Best
 * effort like the fork version: if /proc is unavailable nothing is queued. */
int subprocess_close_inherited_fds_action(posix_spawn_file_actions_t * fa) {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) return 0;
    int dir_fd = dirfd(dir);

    int rc = 0;
    struct dirent * entry;
    while (rc == 0 && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue; /* skip "." / ".." */
        int fd = atoi(entry->d_name);
        if (fd <= STDERR_FILENO || fd == dir_fd) continue;
        int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC)) continue;
        rc = posix_spawn_file_actions_addclose(fa, fd);
    }
    closedir(dir);
    return rc;
}

#define SUBPROCESS_STDIN_INHERIT (-1)
#define SUBPROCESS_STDIN_DEVNULL (-2)

/* Common spawn path: stdin comes from stdin_fd (or is inherited / /dev/null),
 * stdout from stdout_fd (-1 = /dev/null), stderr is always /dev/null, then
 * every other inheritable fd is closed. The pipe fds passed in should be
 * O_CLOEXEC so the dup2 copies are the only ones the child keeps. posix_spawnp() searches PATH exactly
 * like the execvp() the fork versions used, with the same environ.
 *
 * Unlike fork(), clone(CLONE_VM|CLONE_VFORK) inside posix_spawn does not
 * copy the 20 MiB player address space, which on this 56 MiB no-swap device
 * both spikes memory and costs page-table copying on a slow CPU. Returns 0
 * or an errno value; on failure *pid is not valid. */
static int subprocess_spawn(char * const argv[], int stdin_fd, int stdout_fd, pid_t * pid, bool process_group) {
    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) return rc;

    if (stdin_fd >= 0) rc = posix_spawn_file_actions_adddup2(&fa, stdin_fd, STDIN_FILENO);
    else if (stdin_fd == SUBPROCESS_STDIN_DEVNULL)
        rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc == 0) {
        if (stdout_fd >= 0) rc = posix_spawn_file_actions_adddup2(&fa, stdout_fd, STDOUT_FILENO);
        else rc = posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (rc == 0) rc = posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0) rc = subprocess_close_inherited_fds_action(&fa);
    posix_spawnattr_t attrs;
    bool attrs_ready = false;
    if (rc == 0 && process_group) {
        rc = posix_spawnattr_init(&attrs);
        attrs_ready = rc == 0;
        if (rc == 0) rc = posix_spawnattr_setpgroup(&attrs, 0);
        if (rc == 0) rc = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
    }
    if (rc == 0) rc = posix_spawnp(pid, argv[0], &fa, attrs_ready ? &attrs : NULL, argv, environ);
    if (attrs_ready) posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&fa);
    return rc;
}

/* Default subprocess execution timeout (15s) to ensure hung commands do not block the UI.
 * Commands requiring longer budgets (e.g. bt_init) call subprocess_run_timeout() directly. */
#define SUBPROCESS_TIMEOUT_MS 15000

/* Nice value for subprocess_run_low_priority(). These are periodic UI-status
 * reads with no deadline, and several of them (bluetoothctl/bluealsactl) are
 * D-Bus clients of bluealsad -- the same process encoding LDAC on this
 * single-core SoC. Letting the kernel prefer the encoder whenever both are
 * runnable costs nothing here: the worst case is a status icon updating a
 * fraction of a second later. */
#define SUBPROCESS_BACKGROUND_NICE 10

static bool subprocess_run_impl(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms,
                                 int * out_exit_code, int child_nice, bool process_group);

bool subprocess_run(char * const argv[], char * out_buf, size_t out_buf_size) {
    return subprocess_run_timeout(argv, out_buf, out_buf_size, SUBPROCESS_TIMEOUT_MS);
}

bool subprocess_run_low_priority(char * const argv[], char * out_buf, size_t out_buf_size) {
    return subprocess_run_impl(argv, out_buf, out_buf_size, SUBPROCESS_TIMEOUT_MS, NULL,
                               SUBPROCESS_BACKGROUND_NICE, false);
}

bool subprocess_run_timeout(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms) {
    return subprocess_run_checked(argv, out_buf, out_buf_size, timeout_ms, NULL);
}

bool subprocess_run_checked(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms,
                             int * out_exit_code) {
    return subprocess_run_impl(argv, out_buf, out_buf_size, timeout_ms, out_exit_code, 0, false);
}

bool subprocess_run_checked_group(char * const argv[], char * out_buf, size_t out_buf_size,
                                  int timeout_ms, int * out_exit_code) {
    return subprocess_run_impl(argv, out_buf, out_buf_size, timeout_ms, out_exit_code, 0, true);
}

static int64_t subprocess_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Keep the group leader unreaped during the grace period so its PID/PGID
 * cannot be recycled. TERM lets shell EXIT traps stop their own sessions;
 * KILL then removes remaining members of this group. */
static void subprocess_cancel(pid_t pid, bool process_group) {
    if (process_group) {
        (void) kill(-pid, SIGTERM);
        int64_t grace_end = subprocess_monotonic_ms() + 1500;
        while (subprocess_monotonic_ms() < grace_end) usleep(10000);
        (void) kill(-pid, SIGKILL);
    } else {
        (void) kill(pid, SIGKILL);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
}

/* child_nice is applied to the child right after it is spawned. 0 leaves
 * scheduling alone, which is what every caller except the explicitly
 * low-priority ones gets. */
static bool subprocess_run_impl(char * const argv[], char * out_buf, size_t out_buf_size, int timeout_ms,
                                 int * out_exit_code, int child_nice, bool process_group) {
    if (out_exit_code) *out_exit_code = -1;
    if (timeout_ms < 0) return false;
    /* One deadline covers stdout and child exit. Repeated output must not
     * reset the budget (radio clients can keep printing while unresponsive). */
    int64_t deadline = subprocess_monotonic_ms() + timeout_ms;

    int pipefd[2] = { -1, -1 };
    if (out_buf && out_buf_size > 0) {
        if (pipe2(pipefd, O_CLOEXEC) != 0) return false;
        out_buf[0] = '\0';
    }

    pid_t pid;
    int spawn_rc = subprocess_spawn(argv, SUBPROCESS_STDIN_INHERIT, pipefd[1], &pid, process_group);
    if (spawn_rc != 0) {
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        /* The fork version reported a command that could not be executed as
         * a completed run with exit status 127; keep that. Only resource
         * exhaustion (the old fork() failure) is a failed spawn. */
        if (spawn_rc == ENOMEM || spawn_rc == EAGAIN || spawn_rc == EMFILE || spawn_rc == ENFILE)
            return false;
        if (out_exit_code) *out_exit_code = 127;
        return true;
    }
    /* A nice value is inherited across exec, so setting it from here right
     * after the spawn is equivalent to the old child-side call. Best-effort
     * for the same reason: failure just leaves normal priority. */
    if (child_nice != 0) (void) setpriority(PRIO_PROCESS, (id_t) pid, child_nice);

    if (pipefd[1] >= 0) close(pipefd[1]);

    bool timed_out = false;
    if (pipefd[0] >= 0) {
        size_t total = 0;
        for (;;) {
            if (total + 1 >= out_buf_size) break;
            struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
            int64_t remaining = deadline - subprocess_monotonic_ms();
            if (remaining <= 0) { timed_out = true; break; }
            int pr = poll(&pfd, 1, (int)remaining);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) {
                timed_out = (pr == 0); /* 0 = timeout; <0 = poll() error, treat like EOF */
                break;
            }
            ssize_t n = read(pipefd[0], out_buf + total, out_buf_size - 1 - total);
            if (n <= 0) break;
            total += (size_t) n;
        }
        out_buf[total] = '\0';
        close(pipefd[0]);
    }

    if (timed_out) {
        subprocess_cancel(pid, process_group);
        return false;
    }

    /* Bounds the final exit-wait too -- the read loop above only covers
     * stdout; a child that closes/never had stdout piped (out_buf == NULL)
     * but keeps running could still hang here forever otherwise. Polled in
     * small slices rather than a single blocking waitpid() since there's no
     * "wait with timeout" syscall. */
    for (;;) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (out_exit_code && WIFEXITED(status)) *out_exit_code = WEXITSTATUS(status);
            return true;
        }
        if (r < 0 && errno != EINTR) break;
        int64_t remaining = deadline - subprocess_monotonic_ms();
        if (remaining <= 0) break;
        usleep((useconds_t)(remaining < 50 ? remaining : 50) * 1000);
    }
    subprocess_cancel(pid, process_group);
    return false;
}

bool subprocess_spawn_daemon(char * const argv[]) {
    return subprocess_spawn_daemon_logged(argv, NULL);
}

bool subprocess_spawn_daemon_logged(char * const argv[], const char * log_path) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        /* First child: fork again then exit immediately -- the grandchild
         * (the actual daemon) gets reparented to init, which reaps it when
         * it eventually exits, so it never becomes a zombie under this
         * process either. */
        pid_t pid2 = fork();
        if (pid2 == 0) {
            setsid();
            int devnull_in = open("/dev/null", O_RDONLY);
            if (devnull_in >= 0) {
                dup2(devnull_in, STDIN_FILENO);
                close(devnull_in);
            }
            /* log_path (truncated fresh each spawn, not appended -- callers
             * that retry a spawn want just the latest attempt's output, not
             * a growing file mixing every previous one) instead of
             * /dev/null when the caller wants to inspect what the daemon
             * actually printed on startup, e.g. to detect and retry a known
             * flaky initialization failure that doesn't surface any other
             * way. */
            int out_fd = log_path ? open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : open("/dev/null", O_WRONLY);
            if (out_fd < 0) out_fd = open("/dev/null", O_WRONLY);
            if (out_fd >= 0) {
                dup2(out_fd, STDOUT_FILENO);
                dup2(out_fd, STDERR_FILENO);
                close(out_fd);
            }
            close_inherited_fds();
            execvp(argv[0], argv);
            _exit(127); /* execvp only returns on failure */
        }
        _exit(0);
    }

    int wait_status;
    while (waitpid(pid, &wait_status, 0) < 0 && errno == EINTR) {
        /* retry -- interrupted by a signal, not an indication the child is still running */
    }
    return true;
}

bool subprocess_popen(char * const argv[], pid_t * out_pid, int * out_read_fd) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return false;

    pid_t pid;
    int spawn_rc = subprocess_spawn(argv, SUBPROCESS_STDIN_DEVNULL, pipefd[1], &pid, false);
    if (spawn_rc != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    close(pipefd[1]);
    *out_pid = pid;
    *out_read_fd = pipefd[0];
    return true;
}

bool subprocess_popen_stdin(char * const argv[], pid_t * out_pid, int * out_write_fd) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return false;

    pid_t pid;
    int spawn_rc = subprocess_spawn(argv, pipefd[0], -1, &pid, false);
    if (spawn_rc != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    close(pipefd[0]);
    *out_pid = pid;
    *out_write_fd = pipefd[1];
    return true;
}

void subprocess_terminate(pid_t pid) {
    kill(pid, SIGTERM);
    for (int waited_ms = 0; waited_ms < 1000; waited_ms += 50) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void subprocess_kill_all_matching(const char * needle) {
    char out[4096];
    char * argv[] = { (char *) "ps", NULL };
    if (!subprocess_run(argv, out, sizeof(out))) return;

    int pids[16];
    int pid_count = 0;
    char * line_save = NULL;
    char * line = strtok_r(out, "\n", &line_save);
    while (line && pid_count < 16) {
        if (strstr(line, needle)) {
            int pid = atoi(line);
            if (pid > 0) pids[pid_count++] = pid;
        }
        line = strtok_r(NULL, "\n", &line_save);
    }
    for (int i = 0; i < pid_count; i++) kill(pids[i], SIGKILL);
}
