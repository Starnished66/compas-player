/* Bootloader entry: selects and supervises the internal
 * Compás player. Reboots on unexpected exits for crash recovery, or powers
 * off on a clean exit. */

#include "../scan/scanner.h"
#include "../install/installer.h"
#include "../display/fb_draw.h"
#include "../../core/system/lifecycle/idle_shutdown.h"
#include "../../core/system/logging/boot_trace.h"
#include "../startup/boot_tuning.h"
#include "../../core/system/lifecycle/reboot_handoff.h"
#include "../storage/sd_ready.h"
#include "compas_loader.h"
#include "../install/internal_storage.h"
#include "../selector/ui/controller/selector_controller.h"
#include "../../core/process/subprocess.h"

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

#define COLOR_BG fb_rgb(0x12, 0x12, 0x12)

static boot_trace_t boot_trace = BOOT_TRACE_INITIALIZER;

static void bootloader_checkpoint(const char *step) {
    (void)boot_trace_checkpoint(&boot_trace, step);
}

/* Shared filesystem-repair runner records through this loader trace. */
void boot_checkpoint(const char *step) { bootloader_checkpoint(step); }

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

static void before_init_shutdown_request(void) {
    record_shutdown_phase("requesting init reboot", 0);
    /* Close persistent trace storage before init runs its unmount hooks. */
    boot_trace_close(&boot_trace);
}

static void reboot_device(bool requested_restart) {
    if (!requested_restart) sleep(1);
    release_sd_card();
    /* The child has been reaped, closing its audio and storage handles.
     * Ask BusyBox init to run rcK and the remaining shutdown hooks. Sending
     * SIGTERM here cannot be misread as a clean player exit/poweroff.
     * Keep the normal SIGTERM disposition: init eventually terminates this
     * supervisor too. The fallback covers hooks that stall before that. */
    reboot_handoff_result_t handoff = reboot_request_orderly_shutdown(before_init_shutdown_request);
    if (handoff == REBOOT_HANDOFF_REQUESTED) {
        record_shutdown_phase("init reboot timed out; direct fallback", 0);
    } else if (handoff == REBOOT_HANDOFF_SIGNAL_SETUP_FAILED) {
        record_shutdown_phase("init signal setup failed; direct fallback", errno);
    } else if (handoff == REBOOT_HANDOFF_REQUEST_FAILED) {
        record_shutdown_phase("init reboot request failed; direct fallback", errno);
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
    perror("compas_loader: poweroff syscall failed");
    for (;;) pause();
}

void compas_loader_restart(void) { reboot_device(true); }
void compas_loader_poweroff(void) { poweroff_device(); }

/* A clean player exit powers off; signals and non-zero exits reboot. */
static void run_player_supervised(const char * player_path) {
    bootloader_checkpoint("player fork begin");
    pid_t pid = fork();
    if (pid < 0) {
        /* If fork fails, attempt direct execve before rebooting. */
        perror("compas_loader: fork failed, execve'ing directly (no reboot-on-crash this launch)");
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        perror("compas_loader: execve failed");
        reboot_device(false);
    }

    if (pid == 0) {
        execve(player_path, (char * []) { (char *) player_path, NULL }, environ);
        /* Fall back to the packaged internal player if the installed copy fails. */
        perror("compas_loader: execve failed, falling back to packaged player");
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
        perror("compas_loader: waitpid failed unexpectedly");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "compas_loader: %s exited cleanly -- powering off\n", player_path);
        poweroff_device();
    } else {
        fprintf(stderr, "compas_loader: %s exited abnormally (status=0x%x) -- rebooting\n",
                player_path, (unsigned) status);
        record_player_exit(status);
    }
    bool requested_restart = reaped == pid && WIFEXITED(status) &&
                             WEXITSTATUS(status) == IDLE_SHUTDOWN_REBOOT_EXIT_CODE;
    reboot_device(requested_restart);
}

/* Kernel log, readable over ADB with dmesg. Best effort: failures are ignored. */
static void log_kernel(const char * text) {
    char line[256];
    int len = snprintf(line, sizeof(line), "<3>compas_loader: %s\n", text);
    fprintf(stderr, "compas_loader: %s\n", text);
    if (len <= 0) return;
    if (len >= (int) sizeof(line)) len = (int) sizeof(line) - 1;
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    (void) write(fd, line, (size_t) len);
    close(fd);
}

/* The player normally starts ADB, and the player is blocked on this path, so
 * run the same stock ADB sequence as usb_mode_control. No other USB function
 * is started or stopped. */
static void start_recovery_adb(void) {
    int exit_code = -1;
    char * start_argv[] = { (char *) "/etc/init.d/adb/S440adb", (char *) "start", NULL };
    if (!subprocess_run_checked(start_argv, NULL, 0, 15000, &exit_code) || exit_code != 0) {
        log_kernel("recovery ADB start failed");
        return;
    }
    for (int i = 0; i < 20 && access("/dev/usb-ffs/adb", F_OK) != 0; i++) usleep(50000);
    char * bind_argv[] = { (char *) "/sbin/usb_adb_enable.sh", NULL };
    exit_code = -1;
    bool bound = subprocess_run_checked(bind_argv, NULL, 0, 5000, &exit_code);
    char msg[64];
    if (bound)
        snprintf(msg, sizeof(msg), "recovery ADB bind exit=%d", exit_code);
    else
        snprintf(msg, sizeof(msg), "recovery ADB bind did not complete");
    log_kernel(msg);
}

#define INTERNAL_STORAGE_RECOVERY_SECONDS (10 * 60)

static void internal_storage_recovery(const char * reason) __attribute__((noreturn));

/* Blocks the player, updates and selector when /usr/data is not the writable
 * internal userdata mount. There is no automatic reboot: the same mount would
 * fail again in a loop. Poweroff is safe because the loader has mounted
 * nothing writable, so userdata is left untouched for a later boot or manual
 * recovery. ADB stays reachable during the wait. This path must not reach the
 * reboot, SD, or boot-trace helpers, which write under /usr/data. */
static void internal_storage_recovery(const char * reason) {
    char msg[128];
    snprintf(msg, sizeof(msg), "internal storage not ready: %s", reason);
    log_kernel(msg);
    log_kernel("data not erased; player, updates and selector blocked");

    if (fb_open()) {
        fb_fill(COLOR_BG);
        static const char * const text[] = {
            "INTERNAL STORAGE", "COULD NOT MOUNT", "", "YOUR DATA WAS",
            "NOT ERASED", "", "TURNING OFF SOON", "TURN ON TO RETRY",
        };
        const int count = (int) (sizeof(text) / sizeof(text[0]));
        const int pitch = fb_text_height() * 2;
        const int block = (count - 1) * pitch + fb_text_height();
        const int top = (FB_HEIGHT - block) / 2;
        for (int i = 0; i < count; i++)
            fb_draw_text_centered(top + i * pitch, text[i], fb_rgb(0xFF, 0xFF, 0xFF));
        fb_flush();
    }

    start_recovery_adb();

    unsigned int remaining = INTERNAL_STORAGE_RECOVERY_SECONDS;
    while (remaining > 0) remaining = sleep(remaining);

    log_kernel("powering off after internal storage failure");
    sync();
    reboot(RB_POWER_OFF);
    int error = errno;
    snprintf(msg, sizeof(msg), "poweroff failed errno=%d", error);
    log_kernel(msg);
    for (;;) pause();
}

static void require_internal_storage(void) {
    char reason[64];
    if (!internal_storage_ready(reason, sizeof(reason))) internal_storage_recovery(reason);
}

bool compas_loader_prepare(void) {
    require_internal_storage();
    (void)boot_trace_open(&boot_trace, "/usr/data/bootloader_boot.log");
    bootloader_checkpoint("bootloader prepare entered; framebuffer open begin");
    bool fb_ready = fb_open();
    bootloader_checkpoint(fb_ready ? "framebuffer open done" : "framebuffer open failed");
    if (fb_ready) {
        bootloader_checkpoint("bootloader splash draw begin");
        if (!fb_draw_startup_background()) fb_fill(COLOR_BG);
        bootloader_checkpoint("bootloader splash draw done; flush begin");
        fb_flush();
        bootloader_checkpoint("bootloader splash flush done");
    }

    bootloader_checkpoint("SD mount begin");
    mount_sd_card_if_needed();
    bootloader_checkpoint("SD mount done");
    return fb_ready;
}

void compas_loader_run(bool startup_prepared) {
    bool fb_ready;
    if (startup_prepared) {
        require_internal_storage();
        (void)boot_trace_open(&boot_trace, "/usr/data/bootloader_boot.log");
        bootloader_checkpoint("bootloader main entered after selector; framebuffer open begin");
        /* The selector already drew the splash and waited for SD. Reopen the
         * framebuffer only so an SD update can display its progress screen. */
        fb_ready = fb_open();
        bootloader_checkpoint(fb_ready ? "framebuffer reopened" : "framebuffer reopen failed");
    } else {
        fb_ready = compas_loader_prepare();
    }

    scan_result_t scan = {
        .sd_update_present = access(SD_UPDATE_PLAYER_PATH, F_OK) == 0,
        .sd_update_path = SD_UPDATE_PLAYER_PATH,
    };
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
    boot_tune_vm_writeback();
    run_player_supervised(internal_path);
}

#ifndef COMPAS_LOADER_EMBEDDED
int main(void) {
    (void)compas_loader_prepare();
    fb_close();
    (void)selector_controller_run(false);
    compas_loader_run(true);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
#endif
