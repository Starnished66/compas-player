#include "sd_fsck_run.h"

#include "sd_fsck.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef HOST_BUILD

#include "subprocess.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The repair thread must not call mount helpers. The UI thread observes
 * PHASE_REMOUNT and mounts the saved node, so two threads never mount at
 * once and a hung check is never killed under the filesystem. */
enum {
    PHASE_IDLE = 0,
    PHASE_FSCK = 1,
    PHASE_REMOUNT = 2,
};

#define SD_FSCK_LOG_PATH "/usr/data/sd_fsck.log"
/* Attempt key of a check that has unmounted the card. Internal storage, so
 * it survives the reboot hiby_player.sh does whenever the player exits. */
#define SD_FSCK_MARKER_PATH "/usr/data/sd_fsck.pending"
#define SD_REPAIR_ATTEMPT_SLOTS 8
/* Left for the player and the kernel while the checker runs. */
#define SD_FSCK_MEMORY_RESERVE (4u * 1024u * 1024u)
/* ntfsfix works on a few MFT records; there is no cluster-sized table. */
#define SD_FSCK_NTFS_ESTIMATE (4u * 1024u * 1024u)
#define SD_FSCK_OUTPUT_TAIL 4096

extern void boot_checkpoint(const char * step);

static atomic_int repair_phase;
static atomic_int repair_note;
static atomic_int repair_fsck_code = -1;
static atomic_bool repair_tool_ok;
static sd_fsck_plan_t repair_plan;
static sd_fs_kind_t repair_kind;
static uint64_t repair_memory_limit;
static char repair_mount_point[SD_FSCK_MOUNT_BYTES];
/* Ring of recent attempts. Only the kick path touches it, on the UI thread. */
static char repair_attempted[SD_REPAIR_ATTEMPT_SLOTS][SD_FSCK_ATTEMPT_BYTES];
static int repair_attempted_next;

static void post_note(sd_repair_note_t note) {
    atomic_store_explicit(&repair_note, (int) note, memory_order_release);
}

static bool tool_exists(const char * path, void * ctx) {
    (void) ctx;
    return access(path, X_OK) == 0;
}

static void read_card_cid(char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    FILE * file = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!file) return;
    if (!fgets(out, (int) out_size, file)) out[0] = '\0';
    fclose(file);
    char * end = strpbrk(out, "\r\n");
    if (end) *end = '\0';
}

static bool attempt_remembered(const char * key) {
    for (int i = 0; i < SD_REPAIR_ATTEMPT_SLOTS; i++) {
        if (repair_attempted[i][0] && strcmp(repair_attempted[i], key) == 0) return true;
    }
    return false;
}

/* Overwrites the oldest entry when full, so the newest card is never
 * forgotten and retried on every poll. */
static void remember_attempt(const char * key) {
    if (!key || !key[0] || attempt_remembered(key)) return;
    snprintf(repair_attempted[repair_attempted_next], SD_FSCK_ATTEMPT_BYTES, "%s", key);
    repair_attempted_next = (repair_attempted_next + 1) % SD_REPAIR_ATTEMPT_SLOTS;
}

static bool read_mounts(char * buffer, size_t size) {
    FILE * file = fopen("/proc/mounts", "r");
    if (!file) return false;
    size_t got = fread(buffer, 1, size - 1, file);
    int error = ferror(file);
    fclose(file);
    if (error) return false;
    buffer[got] = '\0';
    return true;
}

static void log_repair_line(const char * text) {
    FILE * file = fopen(SD_FSCK_LOG_PATH, "a");
    if (!file) return;
    fputs(text, file);
    fputc('\n', file);
    fclose(file);
}

/* The loop guard's marker, cached so a poll does not read the file. */
static int marker_state = -1; /* -1 not read yet, 0 none, 1 present */
static char marker_key[SD_FSCK_ATTEMPT_BYTES];

static const char * pending_marker(void) {
    if (marker_state < 0) {
        marker_key[0] = '\0';
        FILE * file = fopen(SD_FSCK_MARKER_PATH, "r");
        if (file) {
            if (!fgets(marker_key, sizeof(marker_key), file)) marker_key[0] = '\0';
            fclose(file);
            char * end = strpbrk(marker_key, "\r\n");
            if (end) *end = '\0';
            /* An empty or unreadable marker still records an unfinished check. */
            if (!marker_key[0]) snprintf(marker_key, sizeof(marker_key), "unknown");
        }
        marker_state = file ? 1 : 0;
    }
    return marker_state == 1 ? marker_key : NULL;
}

static bool sync_marker_dir(void) {
    int dir = open("/usr/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    bool ok = fsync(dir) == 0;
    if (close(dir) != 0) ok = false;
    return ok;
}

/* Written and synced before anything that could stop the player. */
static bool write_marker(const char * key) {
    int fd = open(SD_FSCK_MARKER_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t len = strlen(key);
    bool ok = write(fd, key, len) == (ssize_t) len && write(fd, "\n", 1) == 1;
    if (fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!sync_marker_dir()) ok = false;
    if (!ok) {
        /* O_TRUNC may have destroyed an older guard. Do not leave an empty
         * marker that would be read as "unknown" on the next boot. */
        unlink(SD_FSCK_MARKER_PATH);
        sync_marker_dir();
        marker_key[0] = '\0';
        marker_state = -1;
        return false;
    }
    snprintf(marker_key, sizeof(marker_key), "%s", key);
    marker_state = 1;
    return true;
}

static void clear_marker(void) {
    if (marker_state == 0) return;
    unlink(SD_FSCK_MARKER_PATH);
    sync_marker_dir();
    marker_key[0] = '\0';
    marker_state = 0;
}

static void close_inherited_fds(void) {
    DIR * dir = opendir("/proc/self/fd");
    if (!dir) return;
    int dir_fd = dirfd(dir);
    struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        int fd = atoi(entry->d_name);
        if (fd > STDERR_FILENO && fd != dir_fd) close(fd);
    }
    closedir(dir);
}

static long log_size(void) {
    struct stat st;
    if (stat(SD_FSCK_LOG_PATH, &st) != 0) return 0;
    return (long) st.st_size;
}

/* The end of what the checker wrote after start_offset, NUL-terminated. */
static void read_log_tail(long start_offset, char * out, size_t out_size) {
    out[0] = '\0';
    FILE * file = fopen(SD_FSCK_LOG_PATH, "r");
    if (!file) return;
    long end = 0;
    if (fseek(file, 0, SEEK_END) == 0) end = ftell(file);
    long from = end - (long) (out_size - 1);
    if (from < start_offset) from = start_offset;
    if (from >= 0 && from < end && fseek(file, from, SEEK_SET) == 0) {
        size_t got = fread(out, 1, out_size - 1, file);
        out[got] = '\0';
    }
    fclose(file);
}

/* Waits until the checker exits. Killing it would leave the card mid-write.
 * The address-space limit makes an oversized allocation fail inside the
 * checker instead of waking the kernel OOM killer, which could pick the
 * player. */
static int run_fsck(char * const argv[], uint64_t memory_limit) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int log_fd = open(SD_FSCK_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        close_inherited_fds();
        if (memory_limit > 0) {
            struct rlimit limit;
            limit.rlim_cur = (rlim_t) memory_limit;
            limit.rlim_max = (rlim_t) memory_limit;
            setrlimit(RLIMIT_AS, &limit);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    for (;;) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, 0);
        if (waited == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            return -1;
        }
        if (waited < 0 && errno != EINTR) return -1;
    }
}

static void * repair_thread(void * arg) {
    (void) arg;
    long start_offset = log_size();
    int code = run_fsck(repair_plan.argv, repair_memory_limit);
    char * tail = malloc(SD_FSCK_OUTPUT_TAIL);
    if (tail) read_log_tail(start_offset, tail, SD_FSCK_OUTPUT_TAIL);
    bool ok = sd_fsck_tool_succeeded(repair_kind, code, tail, repair_plan.device);
    free(tail);
    atomic_store_explicit(&repair_tool_ok, ok, memory_order_relaxed);
    atomic_store_explicit(&repair_fsck_code, code, memory_order_relaxed);
    atomic_store_explicit(&repair_phase, PHASE_REMOUNT, memory_order_release);
    return NULL;
}

static bool unmount_card(const char * mount_point) {
    if (!mount_point || !mount_point[0]) return false;
    sync();
    char * argv[] = { (char *) "umount", (char *) mount_point, NULL };
    int exit_code = -1;
    if (!subprocess_run_checked(argv, NULL, 0, 15000, &exit_code)) return false;
    return exit_code == 0;
}

bool sd_repair_in_progress(void) {
    int phase = atomic_load_explicit(&repair_phase, memory_order_acquire);
    return phase == PHASE_FSCK || phase == PHASE_REMOUNT;
}

bool sd_repair_needs_remount(void) {
    return atomic_load_explicit(&repair_phase, memory_order_acquire) == PHASE_REMOUNT;
}

const char * sd_repair_device(void) { return repair_plan.device; }

bool sd_repair_current_readonly(bool * readonly) {
    if (readonly) *readonly = false;
    char * mounts = malloc(16384);
    if (!mounts) return false;
    bool loaded = read_mounts(mounts, 16384);
    sd_mount_info_t info;
    bool found = loaded && sd_fsck_parse_mounts(mounts, &info) && info.found;
    free(mounts);
    if (!found) return false;
    if (readonly) *readonly = info.readonly;
    return true;
}

void sd_repair_complete(bool mounted, bool readonly) {
    int code = atomic_load_explicit(&repair_fsck_code, memory_order_relaxed);
    bool tool_ok = atomic_load_explicit(&repair_tool_ok, memory_order_relaxed);
    char line[160];
    snprintf(line, sizeof(line), "fsck exit %d, finished=%d, mounted=%d, readonly=%d", code, tool_ok, mounted,
             readonly);
    log_repair_line(line);
    /* The check ran to the end without the player exiting. */
    clear_marker();
    post_note(sd_fsck_outcome(tool_ok, mounted, readonly));
    boot_checkpoint(mounted && !readonly ? "sd repair remounted read-write" : "sd repair left card unusable");
    atomic_store_explicit(&repair_phase, PHASE_IDLE, memory_order_release);
}

sd_repair_note_t sd_repair_take_note(void) {
    return (sd_repair_note_t) atomic_exchange_explicit(&repair_note, SD_REPAIR_NOTE_NONE, memory_order_acq_rel);
}

static uint64_t read_mem_available(void) {
    FILE * file = fopen("/proc/meminfo", "r");
    if (!file) return 0;
    char line[128];
    unsigned long long kib = 0;
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) break;
        kib = 0;
    }
    fclose(file);
    return (uint64_t) kib * 1024u;
}

/* Bytes the checker needs, or 0 when the boot sector cannot be read. */
static uint64_t checker_memory_estimate(const sd_mount_info_t * info) {
    if (info->kind == SD_FS_KIND_NTFS) return SD_FSCK_NTFS_ESTIMATE;
    unsigned char sector[512];
    int fd = open(info->device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t got = pread(fd, sector, sizeof(sector), 0);
    close(fd);
    sd_fs_geometry_t geometry;
    if (got != (ssize_t) sizeof(sector) || !sd_fsck_parse_boot_sector(sector, sizeof(sector), &geometry)) return 0;
    if (geometry.kind != info->kind) return 0;
    return sd_fsck_memory_estimate(&geometry);
}

sd_repair_kick_result_t sd_readonly_repair_kick(void (* release_handles)(void)) {
    sd_repair_kick_result_t result = { false, false };
    if (sd_repair_in_progress()) return result;

    char * mounts = malloc(16384);
    if (!mounts) return result;
    bool loaded = read_mounts(mounts, 16384);
    sd_mount_info_t info;
    bool parsed = loaded && sd_fsck_parse_mounts(mounts, &info);
    free(mounts);
    if (!parsed || !info.found) return result;
    if (!sd_fsck_device_allowed(info.device)) return result;

    char cid[64];
    char key[SD_FSCK_ATTEMPT_BYTES];
    const char * marker = pending_marker();
    if (!info.readonly) {
        /* A damaged FAT card mounts writable again after a reboot and goes
         * read-only only when the kernel meets the damage, so a writable
         * mount does not clear the marker. A different card does. */
        if (marker) {
            read_card_cid(cid, sizeof(cid));
            sd_repair_attempt_key(info.device, cid, key, sizeof(key));
            if (!sd_repair_marker_blocks(marker, key)) clear_marker();
        }
        return result;
    }

    read_card_cid(cid, sizeof(cid));
    sd_repair_attempt_key(info.device, cid, key, sizeof(key));
    if (attempt_remembered(key)) return result;

    if (sd_repair_marker_blocks(marker, key)) {
        /* The last check of this card never finished: the player exited
         * while the card was detached, and hiby_player.sh rebooted. Running
         * it again would repeat that on every boot. */
        remember_attempt(key);
        post_note(SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER);
        char line[320];
        snprintf(line, sizeof(line), "not checking %s (%s): the previous check of this card did not finish",
                 info.device, info.fstype);
        log_repair_line(line);
        boot_checkpoint("sd repair skipped, previous check did not finish");
        return result;
    }

    const char * tool = sd_fsck_select_tool(info.kind, tool_exists, NULL);
    if (!tool || !sd_fsck_plan(info.kind, tool, info.device, &repair_plan)) {
        remember_attempt(key);
        post_note(SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER);
        char line[320];
        snprintf(line, sizeof(line), "not checking %s (%s): no checker for this filesystem", info.device, info.fstype);
        log_repair_line(line);
        boot_checkpoint("sd repair skipped, no checker for this filesystem");
        return result;
    }

    /* Checked before the unmount: a checker that runs out of memory would
     * leave the card detached for nothing. */
    uint64_t needed = checker_memory_estimate(&info);
    uint64_t available = read_mem_available();
    uint64_t budget = available > SD_FSCK_MEMORY_RESERVE ? available - SD_FSCK_MEMORY_RESERVE : 0;
    if (needed == 0 || needed > budget) {
        remember_attempt(key);
        post_note(SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER);
        char line[320];
        snprintf(line, sizeof(line), "not checking %s (%s, read-only): needs %llu KiB, %llu KiB available", info.device,
                 info.fstype, (unsigned long long) (needed / 1024), (unsigned long long) (available / 1024));
        log_repair_line(line);
        boot_checkpoint("sd repair skipped, not enough memory");
        return result;
    }
    snprintf(repair_mount_point, sizeof(repair_mount_point), "%s", info.mount_point);

    /* From here the player closes files and detaches the card; if it exits
     * before sd_repair_complete(), the next boot finds this marker. */
    if (!write_marker(key)) {
        remember_attempt(key);
        post_note(SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER);
        log_repair_line("not checking read-only card: could not persist interrupted-check marker");
        boot_checkpoint("sd repair skipped, loop guard unavailable");
        return result;
    }
    bool unmounted = unmount_card(repair_mount_point);
    if (!unmounted && release_handles) {
        release_handles();
        result.released_handles = true;
        unmounted = unmount_card(repair_mount_point);
    }
    if (!unmounted) {
        /* Nothing was detached, so there is no unfinished check to record.
         * A busy card with nobody to close is retried on a later poll.
         * After the files are closed, another failure is final so the
         * poll does not tear the library down every cycle. */
        clear_marker();
        if (result.released_handles) {
            remember_attempt(key);
            post_note(SD_REPAIR_NOTE_FAILED);
            boot_checkpoint("sd repair unmount failed");
        }
        return result;
    }

    remember_attempt(key);
    repair_kind = info.kind;
    repair_memory_limit = budget;
    atomic_store_explicit(&repair_fsck_code, -1, memory_order_relaxed);
    atomic_store_explicit(&repair_tool_ok, false, memory_order_relaxed);
    atomic_store_explicit(&repair_phase, PHASE_FSCK, memory_order_release);
    pthread_t thread;
    if (pthread_create(&thread, NULL, repair_thread, NULL) != 0) {
        /* The card is already unmounted. Ask the UI thread to mount it
         * again instead of leaving it detached. */
        atomic_store_explicit(&repair_phase, PHASE_REMOUNT, memory_order_release);
        boot_checkpoint("sd repair thread failed");
        return result;
    }
    pthread_detach(thread);
    post_note(SD_REPAIR_NOTE_STARTED);
    boot_checkpoint("sd repair started");
    char line[400];
    snprintf(line, sizeof(line), "checking %s (%s, read-only) with %s, limit %llu KiB", repair_plan.device, info.fstype,
             repair_plan.tool, (unsigned long long) (budget / 1024));
    log_repair_line(line);
    result.started = true;
    return result;
}

#else

bool sd_repair_in_progress(void) { return false; }
bool sd_repair_needs_remount(void) { return false; }
const char * sd_repair_device(void) { return ""; }
bool sd_repair_current_readonly(bool * readonly) {
    if (readonly) *readonly = false;
    return false;
}
void sd_repair_complete(bool mounted, bool readonly) { (void) mounted; (void) readonly; }
sd_repair_note_t sd_repair_take_note(void) { return SD_REPAIR_NOTE_NONE; }
sd_repair_kick_result_t sd_readonly_repair_kick(void (* release_handles)(void)) {
    (void) release_handles;
    sd_repair_kick_result_t result = { false, false };
    return result;
}

#endif
