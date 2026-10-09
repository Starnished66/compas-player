#include "lvgl/lvgl.h"
#include "audio.h"
#include "waveform.h"
#include "backlight.h"
#include "metadata.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#ifdef HOST_BUILD
#include <execinfo.h>
#endif

#include "gui.h"
#include "settings.h"
#include "db_log.h"
#include "storage_migration.h"
#include "ui_wake.h"
#include <fcntl.h>

#ifdef HOST_BUILD
  #include "src/drivers/sdl/lv_sdl_window.h"
  #include "src/drivers/sdl/lv_sdl_mouse.h"
  #include "src/drivers/sdl/lv_sdl_keyboard.h"
#else
  #include "src/drivers/display/fb/lv_linux_fbdev.h"
  #include "src/drivers/evdev/lv_evdev.h"
  #include "event_input.h"
  #include "input_device_utils.h"
  #include "hw_buttons.h"
  #include "firmware_update.h"
  #include "subprocess.h"
  #include "boot_trace.h"
  #include "sd_fsck_run.h"
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <linux/fb.h>
#endif

#include "board_config.h"

#define SCREEN_WIDTH BOARD_SCREEN_WIDTH
#define SCREEN_HEIGHT BOARD_SCREEN_HEIGHT

#ifdef HOST_BUILD
static volatile sig_atomic_t running = 1;
#endif

/* Custom tick interface for LVGL timing (replaces older thread-based ticks) */
static uint32_t custom_tick_get(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

#ifdef UI_PERF_TRACE
static uint64_t perf_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000ULL + (uint64_t) ts.tv_nsec / 1000ULL;
}
#endif

#ifdef HOST_BUILD
/* gui.h isn't included in the HOST_BUILD branch above (it pulls in
 * target-only headers transitively), so this is the only declaration
 * gui_init() gets there. */
extern void gui_init(uint32_t screen_width, uint32_t screen_height);
/* Host builds do not install the target crash handler. Shared workers still
 * call this hook, so retain a no-op definition for the simulator. */
void install_thread_crash_altstack(void) {}
#endif

#ifndef HOST_BUILD
#include <ucontext.h>
#include <sys/prctl.h>

/* See scan_one_song_into_db()'s own comment (gui_library.c) -- breadcrumb
 * for the metadata-parser crash handler below. Reading this global char
 * array from a signal handler is safe (no allocation, no lock). */
extern char g_scan_last_path[PATH_MAX];

/* Always-on crash line on internal storage. The SD diag below is opt-in and
 * a full SD card (or logging left off) used to leave no record at all:
 * stderr is not kept, and the bootloader only sees the wait status. This
 * path stays async-signal-safe: stack buffers and open/write/fsync/close. */
#define PLAYER_CRASH_LOG "/usr/data/player_crash.log"
#define PLAYER_CRASH_LOG_PREV "/usr/data/player_crash.log.1"
#define PLAYER_CRASH_LOG_LIMIT (64 * 1024)

#ifndef HOST_BUILD
static lv_indev_t *event_touch;
static int event_touch_fd = -1;
static void event_touch_deleted(lv_event_t *event) {
    (void)event;
    event_touch = NULL;
    event_touch_fd = -1;
}
static lv_indev_t *create_touch(const char *path) {
    if (!ui_wake_enabled()) return lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    /* lv_evdev_create_fd owns fd, including failure paths. */
    lv_indev_t *indev = lv_evdev_create_fd(LV_INDEV_TYPE_POINTER, fd);
    if (indev) {
        event_touch = indev;
        event_touch_fd = fd;
        lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
        lv_indev_add_event_cb(indev, event_touch_deleted, LV_EVENT_DELETE, NULL);
    }
    return indev;
}
#endif

/* Set once main() is past the artwork-helper dispatch. The exec'd artwork
 * helper and the forked tag-parser child inherit this handler, but they
 * exist so a damaged file can crash them without taking the player down;
 * their pid never matches, so they leave no player crash line. */
static volatile pid_t crash_player_pid;

static char * crash_put(char * p, char * end, const char * s) {
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char * crash_put_u(char * p, char * end, unsigned long v) {
    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char) ('0' + (v % 10u));
        v /= 10u;
    } while (v && n < (int) sizeof(tmp));
    while (n > 0 && p < end) *p++ = tmp[--n];
    return p;
}

static char * crash_put_i(char * p, char * end, int v) {
    if (v < 0) {
        if (p < end) *p++ = '-';
        /* v + 1 first so INT_MIN does not negate itself. */
        return crash_put_u(p, end, (unsigned) (-(v + 1)) + 1u);
    }
    return crash_put_u(p, end, (unsigned long) v);
}

static char * crash_put_hex8(char * p, char * end, unsigned long v) {
    const char * digits = "0123456789abcdef";
    char tmp[8];
    for (int i = 7; i >= 0; i--) {
        tmp[i] = digits[v & 0xful];
        v >>= 4;
    }
    for (int i = 0; i < 8 && p < end; i++) *p++ = tmp[i];
    return p;
}

static void crash_write_all(int fd, const char * buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (n == 0) return;
        buf += n;
        len -= (size_t) n;
    }
}

static void record_player_crash(int sig, siginfo_t * info, void * ucontext_v) {
    char thread_name[16];
    char line[224];
    char * p = line;
    char * end = line + sizeof(line) - 1;
    unsigned long pc = 0, ra = 0, sp = 0, t9 = 0, gp = 0;
    int fd;
    off_t sz;

    if (crash_player_pid == 0 || getpid() != crash_player_pid) return;
    for (int i = 0; i < (int) sizeof(thread_name); i++) thread_name[i] = '\0';
    if (prctl(PR_GET_NAME, (unsigned long) thread_name) != 0 || !thread_name[0]) {
        thread_name[0] = '?';
        thread_name[1] = '\0';
    }
    thread_name[sizeof(thread_name) - 1] = '\0';
    for (char * c = thread_name; *c; c++) {
        unsigned char ch = (unsigned char) *c;
        if (ch < 32 || ch > 126) *c = '?';
    }

    if (ucontext_v) {
        ucontext_t * uc = (ucontext_t *) ucontext_v;
        pc = (unsigned long) uc->uc_mcontext.pc;
        ra = (unsigned long) uc->uc_mcontext.gregs[31];
        sp = (unsigned long) uc->uc_mcontext.gregs[29];
        t9 = (unsigned long) uc->uc_mcontext.gregs[25];
        gp = (unsigned long) uc->uc_mcontext.gregs[28];
    }

    p = crash_put(p, end, "sig=");
    p = crash_put_i(p, end, sig);
    p = crash_put(p, end, " si_code=");
    p = crash_put_i(p, end, info ? info->si_code : 0);
    p = crash_put(p, end, " addr=");
    p = crash_put_hex8(p, end, info ? (unsigned long) info->si_addr : 0);
    p = crash_put(p, end, " pc=");
    p = crash_put_hex8(p, end, pc);
    p = crash_put(p, end, " ra=");
    p = crash_put_hex8(p, end, ra);
    p = crash_put(p, end, " sp=");
    p = crash_put_hex8(p, end, sp);
    p = crash_put(p, end, " t9=");
    p = crash_put_hex8(p, end, t9);
    p = crash_put(p, end, " gp=");
    p = crash_put_hex8(p, end, gp);
    p = crash_put(p, end, " thread=");
    p = crash_put(p, end, thread_name);
    if (p < end) *p++ = '\n';

    fd = open(PLAYER_CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    sz = lseek(fd, 0, SEEK_END);
    if (sz > PLAYER_CRASH_LOG_LIMIT) {
        close(fd);
        /* rename(2) is async-signal-safe; it replaces an older .1. */
        (void) rename(PLAYER_CRASH_LOG, PLAYER_CRASH_LOG_PREV);
        fd = open(PLAYER_CRASH_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) return;
    }
    crash_write_all(fd, line, (size_t) (p - line));
    (void) fsync(fd);
    close(fd);
}

/* Signal handler that logs register state and a stack word scan to the
 * opt-in .logs/reload_diag.log, then re-raises the signal with the default
 * handler. */
static void crash_diag_handler(int sig, siginfo_t * info, void * ucontext_v) {
    record_player_crash(sig, info, ucontext_v);
    if (!db_log_enabled()) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    int fd = open("/data/mnt/sd_0/.logs/reload_diag.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ucontext_t * uc = (ucontext_t *) ucontext_v;
        unsigned long pc = (unsigned long) uc->uc_mcontext.pc;
        unsigned long ra = (unsigned long) uc->uc_mcontext.gregs[31];
        unsigned long sp = (unsigned long) uc->uc_mcontext.gregs[29];
        unsigned long a0 = (unsigned long) uc->uc_mcontext.gregs[4];
        unsigned long a1 = (unsigned long) uc->uc_mcontext.gregs[5];
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "[pid=%ld] *** CRASH sig=%d fault_addr=%p pc=%08lx ra=%08lx sp=%08lx a0=%08lx a1=%08lx ***\n",
            (long) getpid(), sig, info ? info->si_addr : NULL, pc, ra, sp, a0, a1);
        if (len > 0) { ssize_t r = write(fd, buf, (size_t) len); (void) r; }
        if (g_scan_last_path[0]) {
            len = snprintf(buf, sizeof(buf), "[pid=%ld]   scan_last_path = %s\n",
                            (long) getpid(), g_scan_last_path);
            if (len > 0) {
                /* snprintf returns the required length even when truncated. */
                size_t written = (size_t) len < sizeof(buf) ? (size_t) len : sizeof(buf) - 1;
                ssize_t r = write(fd, buf, written); (void) r;
            }
        }
        for (unsigned long addr = sp; addr < sp + 2048 && addr >= sp; addr += 4) {
            unsigned long word = *(unsigned long *) addr;
            if (word >= 0x400000UL && word < 0x9d0000UL) {
                len = snprintf(buf, sizeof(buf), "[pid=%ld]   stack[+%04lx] = %08lx\n",
                                (long) getpid(), addr - sp, word);
                if (len > 0) { ssize_t r = write(fd, buf, (size_t) len); (void) r; }
            }
        }
        fsync(fd);
        close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Installs a per-thread signal alternate stack so crash_diag_handler() can
 * run even when the thread's own stack is exhausted. Must be called at
 * thread start for each worker thread that uses a custom stack size. */
void install_thread_crash_altstack(void) {
    static _Thread_local uint8_t altstack_buf[32768];
    stack_t ss;
    ss.ss_sp = altstack_buf;
    ss.ss_size = sizeof(altstack_buf);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
}

/* Polls /dev/fb0 with a real open()+ioctl() check rather than trusting the
 * device node's mere existence -- the node can appear before the underlying
 * driver has finished settling, and using it too early crashes on the first
 * render flush. */
static bool wait_for_fbdev_ready(const char * path, int max_attempts, int delay_ms) {
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            struct fb_var_screeninfo vinfo;
            bool ok = (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0);
            close(fd);
            if (ok) return true;
        }
        usleep(delay_ms * 1000);
    }
    return false;
}

/* True if a real filesystem is mounted at /data/mnt/sd_0 (its st_dev differs
 * from its parent's), as opposed to just an empty directory sitting there. */
static bool sd_mount_point_mounted(void) {
    struct stat parent_st, mnt_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat("/data/mnt/sd_0", &mnt_st) != 0) return false;
    return parent_st.st_dev != mnt_st.st_dev;
}

/* mdev normally creates these nodes when hardware appears.  If mdev died or
 * missed the hotplug event, an old node can remain in /dev with the previous
 * major/minor pair.  Read the kernel's current pair from sysfs and repair
 * only nodes explicitly requested by the callers below.  Never replace a
 * non-device entry: an unexpected file or symlink must remain untouched. */
static void sync_device_node_from_sysfs(const char * sysfs_class,
                                        const char * device_name,
                                        const char * device_path,
                                        mode_t device_type) {
    char sysfs_path[PATH_MAX];
    unsigned int major_num, minor_num;
    struct stat node_st;
    FILE * sysfs;

    if (snprintf(sysfs_path, sizeof(sysfs_path),
                 "/sys/class/%s/%s/dev", sysfs_class, device_name) >= (int) sizeof(sysfs_path)) {
        return;
    }
    sysfs = fopen(sysfs_path, "r");
    if (!sysfs) return;
    if (fscanf(sysfs, "%u:%u", &major_num, &minor_num) != 2) {
        fclose(sysfs);
        return;
    }
    fclose(sysfs);

    dev_t expected = makedev(major_num, minor_num);
    if (lstat(device_path, &node_st) == 0) {
        if ((node_st.st_mode & S_IFMT) != device_type) return;
        if (node_st.st_rdev == expected) return;
        if (unlink(device_path) != 0) return;
    } else if (errno != ENOENT) {
        return;
    }

    (void) mknod(device_path, device_type | 0660, expected);
}

static void sync_sd_device_nodes(void) {
    sync_device_node_from_sysfs("block", "mmcblk0", "/dev/mmcblk0", S_IFBLK);
    sync_device_node_from_sysfs("block", "mmcblk0p1", "/dev/mmcblk0p1", S_IFBLK);
}

/* Recover the two internal ALSA nodes needed by audio_init() when mdev has
 * missed the sound-card event.  mkdir() is intentionally non-destructive:
 * an existing unexpected /dev/snd entry is left alone. */
static void sync_sound_device_nodes(void) {
    struct stat snd_dir_st;

    if (lstat("/dev/snd", &snd_dir_st) != 0) {
        if (errno != ENOENT || mkdir("/dev/snd", 0755) != 0) return;
    }
    if (lstat("/dev/snd", &snd_dir_st) != 0 || !S_ISDIR(snd_dir_st.st_mode)) return;

    sync_device_node_from_sysfs("sound", "controlC0", "/dev/snd/controlC0", S_IFCHR);
    sync_device_node_from_sysfs("sound", "pcmC0D0p", "/dev/snd/pcmC0D0p", S_IFCHR);
}

/* Tries each supported filesystem type (vfat, exfat, NTFS) against one
 * device node in turn, stopping at the first that actually mounts.
 *
 * NTFS is intentionally mounted through the ntfs-3g binary included in the
 * firmware image.  This kernel/BusyBox combination has no usable in-kernel
 * `ntfs` mount path (and no mount.ntfs helper), so `mount -t ntfs` never
 * reached the driver that is actually shipped on the device. */
static void try_mount_sd_device_node(const char * device_node) {
    char * vfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "vfat", (char *) "-o",
                            (char *) "rw,relatime,fmask=0022,dmask=0022,codepage=936,iocharset=utf8,shortname=mixed",
                            (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(vfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * exfat_argv[] = { (char *) "mount", (char *) "-t", (char *) "exfat", (char *) "-o",
                             (char *) "rw,relatime", (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(exfat_argv, NULL, 0);
    if (sd_mount_point_mounted()) return;

    char * ntfs_argv[] = { (char *) "/usr/bin/ntfs-3g", (char *) "-o",
                            (char *) "rw,relatime,big_writes,umask=0022",
                            (char *) device_node, (char *) "/data/mnt/sd_0", NULL };
    subprocess_run(ntfs_argv, NULL, 0);
}

/* The repair thread asks for this after fsck. It stays on the thread that
 * already owns mounting, and it does not start another repair. */
static void finish_sd_repair_remount(void) {
    const char * device = sd_repair_device();
    if (device && device[0]) try_mount_sd_device_node(device);
    bool mounted = sd_mount_point_mounted();
    bool readonly = false;
    if (mounted) {
        bool parsed_readonly = false;
        if (sd_repair_current_readonly(&parsed_readonly)) readonly = parsed_readonly;
    }
    sd_repair_complete(mounted, readonly);
}

/* Ensures the SD card is mounted at /data/mnt/sd_0. Creates the mount point
 * first (best-effort -- /data/mnt may not exist yet on a fresh boot), then
 * tries the card's partition node, falling back to the whole-disk node for
 * a partition-less ("superfloppy") card. A read-only mount is unmounted
 * and checked in the background; this returns with the card detached until
 * a later call remounts it. */
void mount_sd_card_if_needed(void) {
    mkdir("/data/mnt", 0755);
    mkdir("/data/mnt/sd_0", 0755);
    if (sd_repair_needs_remount()) {
        finish_sd_repair_remount();
        return;
    }
    /* fsck still has the block device. Mounting over it would race the check. */
    if (sd_repair_in_progress()) return;
    if (!sd_mount_point_mounted()) {
        sync_sd_device_nodes();
        try_mount_sd_device_node("/dev/mmcblk0p1");
        if (!sd_mount_point_mounted()) try_mount_sd_device_node("/dev/mmcblk0");
    }
    /* Only a read-only card is checked. A card that was not safely removed
     * is left alone: that is every pulled card and unclean shutdown. */
    sd_readonly_repair_kick(NULL);
    if (sd_repair_needs_remount()) finish_sd_repair_remount();
}

/* A software-triggered reboot can start this S92 process before the SD
 * block node has reappeared. The one early mount attempt above then misses
 * it, and plugin_manager_init() later sees no .plugins directory; unlike the
 * library, plugin-backed screens are built only once and cannot recover on
 * the later hotplug poll. Called immediately after painting the splash, so
 * this bounded wait overlaps the player's own splash and remains useful for
 * standalone test launches that bypass the bootloader.
 * Avoid running mount helpers until a real node exists. */
static void settle_sd_mount_during_splash(void) {
    const int attempts = 25;
    for (int i = 0; i < attempts; i++) {
        if (sd_mount_point_mounted()) return;
        if (access("/dev/mmcblk0p1", F_OK) == 0 || access("/dev/mmcblk0", F_OK) == 0) {
            mount_sd_card_if_needed();
            if (sd_mount_point_mounted()) return;
        }
        usleep(100000);
    }
}

/* Startup histories stay on internal flash across reboots. The shared logger
 * retains prior launches and flushes each stage before its operation begins. */
static boot_trace_t player_boot_trace = BOOT_TRACE_INITIALIZER;
static bool player_boot_trace_attempted;

void boot_checkpoint(const char * step) {
    if (!player_boot_trace_attempted) {
        player_boot_trace_attempted = true;
        (void)boot_trace_open(&player_boot_trace, "/usr/data/boot_debug.log");
    }
    (void)boot_trace_checkpoint(&player_boot_trace, step);
}

#endif

#ifdef HOST_BUILD
/* Host-only dev convenience: provides backtraces on crashes during host simulation.
 * Device builds use the async-signal-safe crash_diag_handler below. */
// handler for signals that indicate crashes like SIGSEGV or SIGABRT
void crash_handler(int sig) {
    void *buffer[128];
    int size;

    size = backtrace(buffer, 128);

    // Write directly to stderr (safe in signal handlers)
    write(STDERR_FILENO, "Crashed! Backtrace:\n", 20);
    backtrace_symbols_fd(buffer, size, STDERR_FILENO);
    exit(1);
}

/* Host-only: clean shutdown on SIGINT (Ctrl-C) in terminal for SDL simulator.
 * On device builds, the main loop runs continuously and shutdown is supervised. */
// handler for SIGINT
static void sigint_handler(int sig) {
	(void)sig;
	running = 0;

	// TODO: add more cleanup (like turning off the screen)
}
#endif

int main(int argc, char ** argv) {
    /* Ignore SIGPIPE process-wide: a Bluetooth disconnect during playback
     * kills the `aplay -D bluealsa` child audio_output.c writes into, and
     * without this the next write() would kill the whole process instead of
     * just returning EPIPE (which audio_output_write() already handles
     * correctly). Must run before anything else opens a subprocess pipe. */
    signal(SIGPIPE, SIG_IGN);
#ifdef HOST_BUILD
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGINT, sigint_handler);  // interrupt handling (ctrl-c in terminal)
#endif

#ifndef HOST_BUILD
    struct sigaction crash_sa;
    memset(&crash_sa, 0, sizeof(crash_sa));
    crash_sa.sa_sigaction = crash_diag_handler;
    /* SA_ONSTACK: required so crash_diag_handler() can run on a thread whose
     * own stack is exhausted. Only takes effect for threads that have called
     * sigaltstack() (install_thread_crash_altstack()). */
    crash_sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    /* One handler for every fatal signal. Block the set so a second fault
     * inside the handler cannot re-enter it. */
    sigaddset(&crash_sa.sa_mask, SIGSEGV);
    sigaddset(&crash_sa.sa_mask, SIGBUS);
    sigaddset(&crash_sa.sa_mask, SIGILL);
    sigaddset(&crash_sa.sa_mask, SIGFPE);
    sigaddset(&crash_sa.sa_mask, SIGABRT);
    sigaction(SIGSEGV, &crash_sa, NULL);
    sigaction(SIGBUS, &crash_sa, NULL);
    sigaction(SIGILL, &crash_sa, NULL);
    sigaction(SIGFPE, &crash_sa, NULL);
    sigaction(SIGABRT, &crash_sa, NULL);
#endif

    /* Recognized before validating the full argument count, so a malformed
     * helper invocation (wrong arg count, unparsable fd/priority) fails
     * fast here rather than falling through into normal application
     * startup with a leftover "--metadata-artwork-helper" argv entry. */
    if (argc >= 2 && strcmp(argv[1], "--metadata-artwork-helper") == 0) {
        if (argc != 5) return 1;
        char * end = NULL;
        errno = 0;
        long fd = strtol(argv[2], &end, 10);
        if (errno != 0 || !end || end == argv[2] || *end != '\0' || fd < 0 || fd > INT_MAX) return 1;
        /* argv[4]: the parent's real artwork_priority_t (see
         * metadata_read_artwork_isolated_impl()'s own comment on why this
         * crosses the fork/exec boundary as a decimal string) -- any
         * unparsable or out-of-range value defaults to the most
         * conservative ARTWORK_PRIO_WARMER rather than being trusted, since
         * a malformed value here must never grant a LARGER memory ceiling
         * than intended. */
        end = NULL;
        errno = 0;
        long prio_val = strtol(argv[4], &end, 10);
        artwork_priority_t prio = ARTWORK_PRIO_WARMER;
        if (errno == 0 && end && end != argv[4] && *end == '\0' &&
            (prio_val == ARTWORK_PRIO_WARMER || prio_val == ARTWORK_PRIO_THUMBNAIL ||
             prio_val == ARTWORK_PRIO_PLAYER)) {
            prio = (artwork_priority_t) prio_val;
        }
        return metadata_artwork_helper_run(argv[3], (int) fd, prio);
    }
#ifndef HOST_BUILD
    crash_player_pid = getpid();
#endif

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("Starting compas_player...\n");

#ifndef HOST_BUILD
    boot_checkpoint("main entered");

    /* Fallback library search path on the writable partition, for restoring
     * a shared library the read-only squashfs rootfs is missing without
     * needing a reflash (e.g. libldacdec.so, required by bluealsa). */
    setenv("LD_LIBRARY_PATH", "/usr/data/lib", 1);

    /* ALSA looks for $HOME/.asoundrc, and the rootfs is a read-only
     * squashfs, so point HOME at the writable partition. Bluetooth playback
     * writes a rate-pinned PCM definition there; the file only adds a PCM
     * name, leaving local and USB output untouched. */
    setenv("HOME", "/usr/data/alsa", 1);

    /* Must run before firmware_update_check_boot_combo() and gui_init() --
     * both read from the SD card. */
    boot_checkpoint("mount_sd_card_if_needed begin");
    mount_sd_card_if_needed();
    boot_checkpoint("mount_sd_card_if_needed done");
    boot_checkpoint("storage migration begin");
    storage_migrate_internal_data();
    if (sd_card_root_is_mounted()) storage_migrate_legacy_data();
    boot_checkpoint("storage migration done");

    /* Deliberately no startup-time Bluetooth/D-Bus cleanup call here -- see
     * bluetooth_control.h's comment above bt_control_is_powered(): doing
     * that work at this point in boot reliably hangs the app before it
     * reaches the main loop. */

    /* Must run before any display/GUI setup -- if Power+Volume Up are held
     * (the recovery-boot gesture) and a *.upt update file is on the SD
     * card, this reboots straight into recovery and never returns. */
    boot_checkpoint("firmware_update_check_boot_combo begin");
    firmware_update_check_boot_combo();
    boot_checkpoint("firmware_update_check_boot_combo done");
#endif

    /* 1. Initialize LVGL core */
#ifndef HOST_BUILD
    boot_checkpoint("lv_init begin");
#endif
    lv_init();

    /* Register the custom tick source */
    lv_tick_set_cb(custom_tick_get);
#ifndef HOST_BUILD
    boot_checkpoint("lv_init done");
#endif

#ifdef HOST_BUILD
    printf("Initializing Host Build (SDL2 Simulation at %dx%d)...\n", SCREEN_WIDTH, SCREEN_HEIGHT);

    /* Create SDL2 window and display */
    lv_display_t * disp = lv_sdl_window_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    if (!disp) {
        fprintf(stderr, "Error: Failed to create SDL2 window\n");
        return 1;
    }
    settings_load(&current_settings);
    if (current_settings.screen_upside_down) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
    }

    /* Register Mouse as Pointer device (maps mouse click -> touch) */
    lv_indev_t * mouse = lv_sdl_mouse_create();
    if (mouse) {
        lv_indev_set_display(mouse, disp);
    }

    /* Register Keyboard as Keypad device */
    lv_indev_t * kbd = lv_sdl_keyboard_create();
    if (kbd) {
        lv_indev_set_display(kbd, disp);
    }
#else
    printf("Initializing Target Build (Linux Framebuffer and EVDEV touch)...\n");

    /* Create Framebuffer display */
    boot_checkpoint("lv_linux_fbdev_create begin");
    lv_display_t * disp = lv_linux_fbdev_create();
    if (!disp) {
        fprintf(stderr, "Error: Failed to create Linux framebuffer display\n");
        boot_checkpoint("lv_linux_fbdev_create FAILED, returning 1");
        return 1;
    }
    boot_checkpoint("lv_linux_fbdev_create done");
    boot_checkpoint("wait_for_fbdev_ready begin");
    if (!wait_for_fbdev_ready("/dev/fb0", 50, 100)) {
        fprintf(stderr, "Warning: /dev/fb0 not ready after 5s of retries, proceeding anyway...\n");
    }
    boot_checkpoint("wait_for_fbdev_ready done");
    boot_checkpoint("lv_linux_fbdev_set_file begin");
    if (lv_linux_fbdev_set_file(disp, "/dev/fb0") != LV_RESULT_OK) {
        fprintf(stderr, "Error: Failed to open framebuffer device /dev/fb0\n");
        boot_checkpoint("lv_linux_fbdev_set_file FAILED, returning 1");
        return 1;
    }
    boot_checkpoint("lv_linux_fbdev_set_file done");

    boot_checkpoint("display settings_load begin");
    settings_load(&current_settings);
    boot_checkpoint("display settings_load done");
    if (current_settings.screen_upside_down) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_180);
    }

    /* As early as this process can paint anything -- see gui_show_boot_
     * splash()'s own comment (gui.c) for why this exists and how gui_init()
     * further down uses the tick recorded here. */
    boot_checkpoint("gui_show_boot_splash begin");
    gui_show_boot_splash();
    boot_checkpoint("gui_show_boot_splash done");

    boot_checkpoint("settle_sd_mount_during_splash begin");
    settle_sd_mount_during_splash();
    boot_checkpoint("settle_sd_mount_during_splash done");

    boot_checkpoint("ui_wake_init begin");
    ui_wake_init();
    boot_checkpoint("ui_wake_init done");

    /* Create Touch Input device via evdev. Auto-detect the touch controller
     * by name first (works on the R1's Hynitron "hyn_ts"); fall back to
     * guessing event0/event1 for variants where that lookup doesn't match. */
    boot_checkpoint("touch discovery begin");
    lv_indev_t * touch = NULL;
    char touch_path[64];
    if (find_input_device_by_name("hyn_ts", touch_path, sizeof(touch_path)) || find_input_device_by_name("goodix-ts", touch_path, sizeof(touch_path))) {
        printf("Detected touch controller at %s\n", touch_path);
        touch = create_touch(touch_path);
    }
    boot_checkpoint("touch auto-detect attempt done");
    if (!touch) {
        fprintf(stderr, "Warning: Touch controller not auto-detected. Guessing /dev/input/event0...\n");
        snprintf(touch_path, sizeof(touch_path), "/dev/input/event0");
        touch = create_touch(touch_path);
    }
    if (!touch) {
        fprintf(stderr, "Warning: Failed to open /dev/input/event0. Trying event1...\n");
        snprintf(touch_path, sizeof(touch_path), "/dev/input/event1");
        touch = create_touch(touch_path);
    }
    boot_checkpoint("touch setup done");

    if (touch) {
        lv_indev_set_display(touch, disp);
        printf("Touch screen input driver successfully registered.\n");
    } else {
        fprintf(stderr, "Warning: No touch input device found.\n");
    }

    /* Physical volume/skip/play-pause buttons: a separate poller thread,
     * since they're media-key shortcuts (act regardless of what's focused),
     * not keypad navigation for the UI. */
    boot_checkpoint("hw_buttons_init begin");
    hw_buttons_init();
    boot_checkpoint("hw_buttons_init done");
#endif

    /* 2. Initialize audio playback and the application GUI */
#ifndef HOST_BUILD
    boot_checkpoint("sync_sound_device_nodes begin");
    sync_sound_device_nodes();
    boot_checkpoint("sync_sound_device_nodes done");
    boot_checkpoint("audio_init begin");
#endif
    audio_init();
#ifndef HOST_BUILD
    boot_checkpoint("audio_init done");
#endif
#ifndef HOST_BUILD
    boot_checkpoint("gui_init begin");
#endif
    gui_init(SCREEN_WIDTH, SCREEN_HEIGHT);
#ifndef HOST_BUILD
    boot_checkpoint("gui_init done");
#endif

    /* 3. Main event loop */
    printf("Entering main event loop...\n");
#ifndef HOST_BUILD
    boot_checkpoint("entering main loop");
#endif
    /* LVGL normally calls its registered tick callback thousands of times
     * per second. On this old 32-bit kernel, musl implements each call as a
     * failing clock_gettime64 syscall followed by a legacy clock_gettime
     * fallback. Switch LVGL to its in-memory tick counter after setup and
     * advance it once per handler iteration from one real clock read. Timer
     * semantics remain identical: the tick is stable while one handler pass
     * runs, just like the conventional interrupt-driven lv_tick_inc model. */
    /* Preserve LVGL's exact clock domain across the handoff to prevent
     * wraparound differences with interactive timeout baselines. */
    uint32_t lvgl_handoff_tick = lv_tick_get();
    uint32_t last_real_tick = custom_tick_get();
    lv_tick_inc(lvgl_handoff_tick);
    lv_tick_set_cb(NULL);
#ifndef HOST_BUILD
    /* Must be after lv_tick_set_cb(NULL): this baseline must belong to the
     * runtime clock that update_timer_cb will read, not the boot clock. */
    gui_reset_interactive_timeout_baseline();
#endif
#ifdef UI_PERF_TRACE
    uint32_t perf_report_tick = last_real_tick;
    uint64_t perf_handler_total_us = 0;
    uint64_t perf_handler_max_us = 0;
    unsigned perf_handler_calls = 0;
    unsigned perf_handler_over_16ms = 0;
#endif
    /* HOST_BUILD only: a stray SIGINT on the real device must NOT reach this
     * loop's exit path -- falling through to gui_deinit(); return 0; makes
     * run_player_supervised() (src/bootloader/main.c) treat it as a clean,
     * intentional shutdown request and power the device off. sigint_handler
     * is only ever wired up under HOST_BUILD (see main()'s own signal()
     * call above), so `running` never leaves 1 on-device either way, but
     * looping on the literal constant here keeps that guarantee visible at
     * the one place it actually matters instead of relying on a variable
     * defined and set far away. */
#ifdef HOST_BUILD
    while (running) {
#else
    while (1) {
#endif
        uint32_t real_tick = custom_tick_get();
        lv_tick_inc(real_tick - last_real_tick);
        last_real_tick = real_tick;
#ifdef UI_PERF_TRACE
        uint64_t perf_handler_start_us = perf_now_us();
#endif
        uint32_t time_till_next = lv_timer_handler();
        /* After lv_tick_set_cb(NULL), LVGL's tick stays frozen at the loop-top
         * lv_tick_inc() value for this entire handler pass. lv_timer_exec()
         * stamps timer->last_run = lv_tick_get() (that frozen value)
         * immediately before invoking the callback, so a blocking callback --
         * the transition compositor waiting on FBIOPAN_DISPLAY vsync,
         * commonly ~16ms -- is invisible to LVGL's time_till_next math, which
         * then returns the timer's full period again. Sleeping that full
         * amount on top of time already spent doubles the cycle. One extra
         * custom_tick_get() here is cheap next to the thousands of
         * clock_gettime() calls the NULL-cb handoff was designed to avoid.
         * last_real_tick is left at the loop-top read: the next lv_tick_inc()
         * must still measure loop-top-to-loop-top, independent of this
         * sleep-only correction. */
        uint32_t handler_elapsed_ms = custom_tick_get() - real_tick;
        if (time_till_next > handler_elapsed_ms) time_till_next -= handler_elapsed_ms;
        else time_till_next = 0;
#ifdef UI_PERF_TRACE
        uint64_t perf_handler_us = perf_now_us() - perf_handler_start_us;
        perf_handler_total_us += perf_handler_us;
        if (perf_handler_us > perf_handler_max_us) perf_handler_max_us = perf_handler_us;
        perf_handler_calls++;
        if (perf_handler_us >= 16000) perf_handler_over_16ms++;
        if ((uint32_t) (real_tick - perf_report_tick) >= 1000) {
            printf("PERF main handler calls=%u avg_us=%llu max_us=%llu over16=%u next_ms=%u screen_on=%d\n",
                   perf_handler_calls,
                   (unsigned long long) (perf_handler_calls ? perf_handler_total_us / perf_handler_calls : 0),
                   (unsigned long long) perf_handler_max_us, perf_handler_over_16ms, time_till_next,
                   backlight_screen_is_on());
            perf_report_tick = real_tick;
            perf_handler_total_us = 0;
            perf_handler_max_us = 0;
            perf_handler_calls = 0;
            perf_handler_over_16ms = 0;
        }
#endif
        /* Cap idle sleep duration: lv_timer_handler() returns LV_NO_TIMER_READY
         * (0xFFFFFFFF) when no unpaused timers are ready, which would overflow
         * microsecond conversion. */
        /* Screen-off CPU/battery optimization: while the backlight is off,
         * gui.c's own update_timer_cb() (still running every 500ms -- see
         * its own comment on why that keeps going regardless) is the only
         * timer that still needs prompt service; LVGL's display-refresh
         * timer is paused for the same duration (see gui.c's screen_just_
         * woke/screen_on_now handling), so there's nothing display-related
         * this loop needs to wake up quickly for. Raising the idle cap to
         * match that 500ms cadence while off (kept at 100ms while on, for
         * touch/animation responsiveness) is what actually cuts main-loop
         * wakeups -- backlight_screen_is_on() is the same shared source of
         * truth gui.c itself uses, so this can never disagree with the
         * paused/resumed state of the refresh timer. */
        uint32_t idle_cap_ms = backlight_screen_is_on() ? 100 : 500;
        if (time_till_next > idle_cap_ms) time_till_next = idle_cap_ms;
#ifndef HOST_BUILD
        if (ui_wake_enabled()) {
            /* LVGL has no public enabled/transition getter. Mirror its read
             * preconditions here: a disabled reader cannot drain a ready fd. */
            bool can_read_touch = ui_event_input_can_read(event_touch);
            unsigned ready = ui_wake_wait(can_read_touch ? event_touch_fd : -1, time_till_next);
            if (ready) {
                /* Advance ticks before dispatch: gestures must see real event
                 * time even after an idle wait. The handler accounts the rest. */
                uint32_t input_tick = custom_tick_get();
                lv_tick_inc(input_tick - last_real_tick);
                last_real_tick = input_tick;
                if ((ready & UI_WAKE_TOUCH_ERROR) && event_touch) {
                    /* Persistent error readiness cannot be consumed as data. */
                    fprintf(stderr, "Touch input: descriptor error, removing device\n");
                    lv_evdev_delete(event_touch);
                } else if ((ready & UI_WAKE_TOUCH) && event_touch) {
                    lv_indev_read(event_touch);
                    /* Explicitly preserve stationary long presses. LVGL also
                     * resumes this timer in its pointer press handler. */
                    ui_input_set_runtime_timer(event_touch, true);
                }
                if (ready & UI_WAKE_WORKER) gui_process_input_wake();
            }
        } else
#endif
        usleep(time_till_next * 1000);
    }

    waveform_shutdown();
    gui_deinit();
    return 0;
}
