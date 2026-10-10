/* Bootloader entry: installs a pending SD update, then supervises the internal
 * Compás player. Reboots on unexpected exits for crash recovery, or powers
 * off on a clean exit. */

#include "scanner.h"
#include "installer.h"
#include "fb_draw.h"
#include "idle_shutdown.h"
#include "boot_trace.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char ** environ;

/* Background JPEG image from theme assets used for the boot splash. */
#define BOOTLOADER_BG_PATH "/usr/resource/litegui/theme2/boot_animation/en/0.jpg"

#define COLOR_BG fb_rgb(0x12, 0x12, 0x12)

static boot_trace_t boot_trace = BOOT_TRACE_INITIALIZER;

static void bootloader_checkpoint(const char *step) {
    (void)boot_trace_checkpoint(&boot_trace, step);
}

static void record_shutdown_phase(const char * phase, int error);

/* The kernel marks a FAT volume dirty while it is mounted writable and
 * clears the mark only on unmount or a read-only remount. The player has
 * exited here, so its files are closed; finish pending writes and release
 * the volume cleanly before rebooting. */
static void release_sd_card(void) {
    bootloader_checkpoint("shutdown storage sync begin");
    sync();
    bootloader_checkpoint("shutdown storage sync done; SD unmount begin");
    if (umount2(SD_MOUNT_POINT, 0) == 0) {
        record_shutdown_phase("sd unmounted", 0);
        return;
    }
    int error = errno;
    if (error == EINVAL || error == ENOENT) {
        record_shutdown_phase("sd not mounted", error);
        return;
    }
    record_shutdown_phase("sd unmount failed", error);
    if (mount(NULL, SD_MOUNT_POINT, NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0)
        record_shutdown_phase("sd remounted read-only", 0);
    else
        record_shutdown_phase("sd read-only remount failed", errno);
}

/* stderr from the player is not kept across a reboot, so an abnormal exit
 * leaves a bounded record on the writable internal partition. Failures here
 * must not skip the reboot below. */
#define PLAYER_EXIT_LOG "/usr/data/player_exit.log"
#define PLAYER_EXIT_LOG_PREV "/usr/data/player_exit.log.1"
#define PLAYER_EXIT_LOG_LIMIT (64 * 1024)
#define PLAYER_EXIT_KLOG_TAIL 4096

static const char * exit_signal_name(int sig) {
    switch (sig) {
        case SIGHUP: return "SIGHUP";
        case SIGINT: return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL: return "SIGILL";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGUSR1: return "SIGUSR1";
        case SIGSEGV: return "SIGSEGV";
        case SIGUSR2: return "SIGUSR2";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
#ifdef SIGEMT
        case SIGEMT: return "SIGEMT";
#endif
#ifdef SIGSTKFLT
        case SIGSTKFLT: return "SIGSTKFLT";
#endif
        case SIGCHLD: return "SIGCHLD";
        case SIGCONT: return "SIGCONT";
        case SIGSTOP: return "SIGSTOP";
        case SIGTSTP: return "SIGTSTP";
        case SIGTTIN: return "SIGTTIN";
        case SIGTTOU: return "SIGTTOU";
        case SIGURG: return "SIGURG";
        case SIGXCPU: return "SIGXCPU";
        case SIGXFSZ: return "SIGXFSZ";
        case SIGVTALRM: return "SIGVTALRM";
        case SIGPROF: return "SIGPROF";
        case SIGWINCH: return "SIGWINCH";
        case SIGIO: return "SIGIO";
        case SIGPWR: return "SIGPWR";
        case SIGSYS: return "SIGSYS";
        default: return "unknown";
    }
}

static void write_ignoring_errors(int fd, const void * buf, size_t len) {
    const char * p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        p += n;
        len -= (size_t) n;
    }
}

static void rotate_player_exit_log(void) {
    struct stat st;
    if (stat(PLAYER_EXIT_LOG, &st) == 0 && st.st_size > PLAYER_EXIT_LOG_LIMIT)
        (void) rename(PLAYER_EXIT_LOG, PLAYER_EXIT_LOG_PREV);
}

/* Internal persistent storage remains available after releasing the SD card.
 * Diagnostics must not prevent a reboot if the log cannot be written. */
static void record_shutdown_phase(const char * phase, int error) {
    struct timespec now = {0};
    (void) clock_gettime(CLOCK_BOOTTIME, &now);
    char line[256];
    int len = snprintf(line, sizeof(line), "shutdown boottime=%lld phase=%s errno=%d\n",
                       (long long) now.tv_sec, phase, error);
    if (len <= 0) return;
    if (len >= (int) sizeof(line)) len = sizeof(line) - 1;
    char trace_step[192];
    (void)snprintf(trace_step, sizeof(trace_step), "shutdown phase=%s errno=%d", phase, error);
    bootloader_checkpoint(trace_step);
    fprintf(stderr, "%s", line);
    rotate_player_exit_log();
    int fd = open(PLAYER_EXIT_LOG, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    write_ignoring_errors(fd, line, (size_t) len);
    (void) fsync(fd);
    close(fd);
}

static void append_kernel_log_tail(int fd) {
    int size = klogctl(10, NULL, 0); /* SYSLOG_ACTION_SIZE_BUFFER */
    if (size <= 0) return;
    int want = size < PLAYER_EXIT_KLOG_TAIL ? size : PLAYER_EXIT_KLOG_TAIL;
    char * buf = malloc((size_t) want);
    if (!buf) return;
    int n = klogctl(3, buf, want); /* SYSLOG_ACTION_READ_ALL, last `want` bytes */
    if (n > 0) {
        if (n > want) n = want;
        write_ignoring_errors(fd, buf, (size_t) n);
        if (buf[n - 1] != '\n') write_ignoring_errors(fd, "\n", 1);
    }
    free(buf);
}

/* One line, then the tail of the kernel log (an OOM-killer line lives there).
 * Exit 75 is the player's intentional restart; it still reboots, but the
 * line says so. Rotate before appending so the file stays within one
 * generation plus the previous one. */
static void record_player_exit(int status) {
    struct timespec realtime;
    struct timespec boottime;
    char line[320];
    int len;

    if (clock_gettime(CLOCK_REALTIME, &realtime) != 0) realtime.tv_sec = 0;
    if (clock_gettime(CLOCK_BOOTTIME, &boottime) != 0) boottime.tv_sec = 0;

    if (WIFSIGNALED(status)) {
        len = snprintf(line, sizeof(line),
                       "realtime=%lld boottime=%lld status=0x%x signal %d (%s)%s\n",
                       (long long) realtime.tv_sec, (long long) boottime.tv_sec,
                       (unsigned) status, WTERMSIG(status), exit_signal_name(WTERMSIG(status)),
                       WCOREDUMP(status) ? " core" : "");
    } else if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        len = snprintf(line, sizeof(line),
                       "realtime=%lld boottime=%lld status=0x%x exit code %d%s\n",
                       (long long) realtime.tv_sec, (long long) boottime.tv_sec,
                       (unsigned) status, code,
                       code == IDLE_SHUTDOWN_REBOOT_EXIT_CODE ? " requested restart" : "");
    } else {
        len = snprintf(line, sizeof(line),
                       "realtime=%lld boottime=%lld status=0x%x unrecognized\n",
                       (long long) realtime.tv_sec, (long long) boottime.tv_sec,
                       (unsigned) status);
    }
    if (len <= 0) return;
    if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;

    rotate_player_exit_log();
    int fd = open(PLAYER_EXIT_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write_ignoring_errors(fd, line, (size_t) len);
    append_kernel_log_tail(fd);
    (void) fsync(fd);
    close(fd);
}

static bool init_is_busybox(void) {
    char executable[256];
    ssize_t length = readlink("/proc/1/exe", executable, sizeof(executable) - 1);
    if (length <= 0 || length >= (ssize_t) sizeof(executable) - 1) return false;
    executable[length] = '\0';
    const char * name = strrchr(executable, '/');
    return strcmp(name ? name + 1 : executable, "busybox") == 0;
}

static void reboot_device(void) {
    sleep(1);
    release_sd_card();
    /* The child has been reaped, closing its audio and storage handles.
     * Ask BusyBox init to run rcK and the remaining shutdown hooks. Sending
     * SIGTERM here cannot be misread as a clean player exit/poweroff.
     * Keep the normal SIGTERM disposition: init eventually terminates this
     * supervisor too. The fallback covers hooks that stall before that. */
    if (init_is_busybox()) {
        struct sigaction action = {0};
        sigset_t unblocked;
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigemptyset(&unblocked);
        sigaddset(&unblocked, SIGTERM);
        /* An ignored or blocked signal can survive the launcher exec. Init
         * must be able to terminate us after its shutdown hooks finish. */
        if (sigaction(SIGTERM, &action, NULL) != 0 ||
            sigprocmask(SIG_UNBLOCK, &unblocked, NULL) != 0) {
            record_shutdown_phase("init signal setup failed; direct fallback", errno);
        } else {
            record_shutdown_phase("requesting init reboot", 0);
            /* Release writable userdata descriptors before init's unmount and
             * read-only remount hooks. Fallback diagnostics still use the
             * existing short-lived player_exit log writes. */
            boot_trace_close(&boot_trace);
            if (kill(1, SIGTERM) == 0) {
                struct timespec remaining = {20, 0};
                while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
                record_shutdown_phase("init reboot timed out; direct fallback", 0);
            } else {
                record_shutdown_phase("init reboot request failed; direct fallback", errno);
            }
        }
    } else {
        record_shutdown_phase("unsupported init; direct fallback", 0);
    }
    boot_trace_close(&boot_trace);
    reboot(RB_AUTOBOOT);
    record_shutdown_phase("direct reboot failed", errno);
    _exit(1);
}

static void poweroff_device(void) {
    release_sd_card();
    bootloader_checkpoint("requesting direct poweroff");
    boot_trace_close(&boot_trace);
    reboot(RB_POWER_OFF);
    /* If poweroff syscall fails, pause indefinitely rather than rebooting. */
    record_shutdown_phase("direct poweroff failed", errno);
    perror("compas_bootloader: poweroff syscall failed");
    for (;;) pause();
}

/* Measured OOM, about 30s after boot: the player (~20 MB RSS) was killed in
 * fat_write_begin. free was the 954 KB min watermark, writeback was 9.4 MB,
 * dirty 0.85 MB, and the file LRU was unreclaimable. Defaults (dirty_ratio
 * 20 and dirty_background_ratio 10 of ~58 MB, expire 30s, min_free_kbytes
 * 954) let writeback pin more RAM than the player can spare. Cap dirty pages
 * at a few MB so writers stall first, expire a little sooner so that 30s
 * burst cannot rebuild, and start reclaim slightly earlier. Best-effort:
 * a failure is logged and boot continues. */
static void tune_vm_writeback(void) {
    static const struct { const char * path; const char * value; } settings[] = {
        /* Flusher starts at 1 MiB instead of ~10% (~6 MiB). */
        { "/proc/sys/vm/dirty_background_bytes", "1048576\n" },
        /* Writers block at 4 MiB instead of ~20% (~12 MiB). Must stay above
         * the background threshold; 4 MiB is under the 9.4 MiB of writeback
         * that was stuck when the player was killed. */
        { "/proc/sys/vm/dirty_bytes", "4194304\n" },
        /* 15s instead of 30s: still coarse on a slow card, but dirty pages
         * cannot sit for the whole window that matched the OOM. */
        { "/proc/sys/vm/dirty_expire_centisecs", "1500\n" },
        /* 2560 KB. The kill happened with free == the 954 KB watermark;
         * this starts reclaim ~1.6 MB sooner and is ~4% of 58 MB. */
        { "/proc/sys/vm/min_free_kbytes", "2560\n" },
    };
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        int fd = open(settings[i].path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "compas_bootloader: %s: %s\n", settings[i].path, strerror(errno));
            continue;
        }
        size_t len = strlen(settings[i].value);
        ssize_t wrote = write(fd, settings[i].value, len);
        if (wrote < 0)
            fprintf(stderr, "compas_bootloader: write %s failed: %s\n", settings[i].path, strerror(errno));
        else if ((size_t) wrote != len)
            fprintf(stderr, "compas_bootloader: write %s short (%zd)\n", settings[i].path, wrote);
        close(fd);
    }
}

/* A clean player exit powers off; signals and non-zero exits reboot. */
static void run_player_supervised(const char * player_path) {
    bootloader_checkpoint("player fork begin");
    pid_t pid = fork();
    if (pid < 0) {
        /* If fork fails, attempt direct execve before rebooting. */
        perror("compas_bootloader: fork failed, execve'ing directly (no reboot-on-crash this launch)");
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        perror("compas_bootloader: execve failed");
        reboot_device();
    }

    if (pid == 0) {
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        /* Fall back to the packaged internal player if the installed copy fails. */
        perror("compas_bootloader: execve failed, falling back to packaged player");
        if (strcmp(player_path, INTERNAL_PLAYER_PATH) != 0) {
            execve(INTERNAL_PLAYER_PATH, (char * []) { (char *) INTERNAL_PLAYER_PATH, NULL }, environ);
        }
        _exit(127);
    }

    bootloader_checkpoint("player fork done; supervisor waitpid begin");

    /* Wait for child process, retrying on EINTR. */
    int status;
    int wait_error;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);
    wait_error = errno;

    {
        char phase[128];
        if (reaped == pid)
            snprintf(phase, sizeof(phase), "supervisor waitpid done status=0x%x", (unsigned)status);
        else
            snprintf(phase, sizeof(phase), "supervisor waitpid failed errno=%d", wait_error);
        bootloader_checkpoint(phase);
    }
    if (reaped != pid) {
        /* Child wait failed unexpectedly; fall through to reboot. */
        errno = wait_error;
        perror("compas_bootloader: waitpid failed unexpectedly");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "compas_bootloader: %s exited cleanly -- powering off\n", player_path);
        poweroff_device();
    } else {
        fprintf(stderr, "compas_bootloader: %s exited abnormally (status=0x%x) -- rebooting\n",
                player_path, (unsigned) status);
        record_player_exit(status);
    }
    reboot_device();
}

int main(void) {
    (void)boot_trace_open(&boot_trace, "/usr/data/bootloader_boot.log");
    bootloader_checkpoint("bootloader main entered; framebuffer open begin");
    /* Open framebuffer and paint the splash before waiting for the SD card. */
    bool fb_ready = fb_open();
    bootloader_checkpoint(fb_ready ? "framebuffer open done" : "framebuffer open failed");
    if (fb_ready) {
        bootloader_checkpoint("bootloader splash draw begin");
        if (!fb_draw_background_jpeg(BOOTLOADER_BG_PATH)) fb_fill(COLOR_BG);
        bootloader_checkpoint("bootloader splash draw done; flush begin");
        fb_flush();
        bootloader_checkpoint("bootloader splash flush done");
    }

    scan_result_t scan;
    bootloader_checkpoint("SD scanner begin");
    scanner_scan(&scan);
    bootloader_checkpoint("SD scanner done");

    /* Install a pending SD update before choosing the internal player copy. */
    bootloader_checkpoint("player installer begin");
    installer_run(&scan, fb_ready);
    bootloader_checkpoint("player installer done");

    bootloader_checkpoint("internal player selection begin");
    const char * internal_path = installer_internal_player_path();
    bootloader_checkpoint("internal player selection done");
    if (fb_ready) {
        bootloader_checkpoint("framebuffer close begin");
        fb_close();
        bootloader_checkpoint("framebuffer close done");
    }
    tune_vm_writeback();
    run_player_supervised(internal_path);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
