/* Bootloader entry: installs a pending SD update, then supervises the internal
 * Compás player. Reboots on unexpected exits for crash recovery, or powers
 * off on a clean exit. */

#include "scanner.h"
#include "installer.h"
#include "fb_draw.h"
#include "idle_shutdown.h"

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

/* The kernel marks a FAT volume dirty while it is mounted writable and
 * clears the mark only on unmount or a read-only remount. The player has
 * exited here, so its files are closed; finish pending writes and release
 * the volume cleanly before rebooting. */
static void release_sd_card(void) {
    sync();
    if (umount2(SD_MOUNT_POINT, 0) == 0) return;
    if (errno == EINVAL || errno == ENOENT) return; /* not mounted */
    mount(NULL, SD_MOUNT_POINT, NULL, MS_REMOUNT | MS_RDONLY, NULL);
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
    struct stat st;

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

    if (stat(PLAYER_EXIT_LOG, &st) == 0 && st.st_size > PLAYER_EXIT_LOG_LIMIT)
        (void) rename(PLAYER_EXIT_LOG, PLAYER_EXIT_LOG_PREV);

    int fd = open(PLAYER_EXIT_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    write_ignoring_errors(fd, line, (size_t) len);
    append_kernel_log_tail(fd);
    (void) fsync(fd);
    close(fd);
}

/* Supervises player execution. A clean exit status (0) triggers device poweroff,
 * while abnormal termination (signals or non-zero exit) triggers a reboot. */
static void reboot_device(void) {
    sleep(1);
    release_sd_card();
    reboot(RB_AUTOBOOT);
    /* reboot() is expected to terminate the process; exit if it returns. */
    _exit(1);
}

static void poweroff_device(void) {
    release_sd_card();
    reboot(RB_POWER_OFF);
    /* If poweroff syscall fails, pause indefinitely rather than rebooting. */
    perror("compas_bootloader: poweroff syscall failed");
    for (;;) pause();
}

static void run_player_supervised(const char * player_path) {
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

    /* Wait for child process, retrying on EINTR. */
    int status;
    pid_t reaped;
    do {
        reaped = waitpid(pid, &status, 0);
    } while (reaped == -1 && errno == EINTR);

    if (reaped != pid) {
        /* Child wait failed unexpectedly; fall through to reboot. */
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
    /* Open framebuffer and paint the splash before waiting for the SD card. */
    bool fb_ready = fb_open();
    if (fb_ready) {
        if (!fb_draw_background_jpeg(BOOTLOADER_BG_PATH)) fb_fill(COLOR_BG);
        fb_flush();
    }

    scan_result_t scan;
    scanner_scan(&scan);

    /* Install a pending SD update before choosing the internal player copy. */
    installer_run(&scan, fb_ready);

    const char * internal_path = installer_internal_player_path();
    if (fb_ready) fb_close();
    run_player_supervised(internal_path);
    return 1; /* unreachable -- run_player_supervised() never returns */
}
