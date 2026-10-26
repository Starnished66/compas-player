#define _POSIX_C_SOURCE 200809L

#include "rockbox_bridge.h"

#include "../../core/system/runtime/board_config.h"
#include "../app/compas_loader.h"
#include "../display/fb_draw.h"
#include "../scan/scanner.h"
#include "../storage/sd_ready.h"
#include "../../core/process/subprocess.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

extern char ** environ;

#define ROCKBOX_MAX_BYTES (32u * 1024u * 1024u)
#define ROCKBOX_INFO_MAX 4096
#ifndef COMPAS_RB_DATA_ROOT
#define COMPAS_RB_DATA_ROOT "/usr/data"
#endif
#ifndef COMPAS_RB_SD_ROOT
#define COMPAS_RB_SD_ROOT SD_MOUNT_POINT
#endif
#define ROCKBOX_SCRATCH COMPAS_RB_DATA_ROOT "/.compas/rockbox"

#if defined(BOARD_R1)
#define RB_SD_EXEC_NAME "rockbox.r1"
#define RB_STAGE_EXEC ROCKBOX_SCRATCH "/rockbox.r1"
#define RB_TARGET "hibyr1"
#define RB_TARGET_ID "124"
#elif defined(BOARD_R3PROII)
#define RB_SD_EXEC_NAME "rockbox.r3proii"
#define RB_STAGE_EXEC ROCKBOX_SCRATCH "/rockbox.r3proii"
#define RB_TARGET "hibyr3proii"
#define RB_TARGET_ID "123"
#else
#define RB_SUPPORTED 0
#endif

#ifndef RB_SUPPORTED
#define RB_SUPPORTED 1
#endif

#if RB_SUPPORTED
static uint16_t u16le(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool same_source_snapshot(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Checks a MIPS32 little-endian executable through an already-open descriptor.
 * ET_EXEC and ET_DYN are accepted because Rockbox builds can be PIE. This
 * descriptor-based validator keeps the Rockbox source pinned across card
 * removal; player updates are handled separately by the running player. */
static bool rockbox_elf_fd_valid(int fd, off_t size) {
    enum { EHDR = 52, PHDR = 32 };
    unsigned char h[EHDR];
    if (size < EHDR || pread(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h)) return false;
    if (memcmp(h, "\177ELF", 4) || h[4] != 1 || h[5] != 1 || h[6] != 1) return false;
    uint16_t type = u16le(h + 16);
    if ((type != 2 && type != 3) || u16le(h + 18) != 8 || u32le(h + 20) != 1) return false;
    uint32_t flags = u32le(h + 36);
    uint32_t abi = flags & 0x0000F000u;
    uint32_t arch = flags & 0xF0000000u;
    /* HiBy's kernel runs O32 MIPS1/2/32/32r2 code. ELFCLASS32 alone also
     * admits N32/O64, MIPS64, r6 and microMIPS binaries that it cannot run. */
    if ((flags & 0x00000020u) != 0 || (abi != 0 && abi != 0x00001000u) ||
        (arch != 0x00000000u && arch != 0x10000000u &&
         arch != 0x50000000u && arch != 0x70000000u) ||
        (flags & 0x0F000000u) != 0)
        return false;
    uint32_t phoff = u32le(h + 28);
    uint16_t phentsize = u16le(h + 42), phnum = u16le(h + 44);
    if (phnum == 0 || phentsize < PHDR || (uint64_t)phoff + (uint64_t)phentsize * phnum > (uint64_t)size)
        return false;
    bool load = false;
    unsigned char p[PHDR];
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t off = (uint64_t)phoff + (uint64_t)i * phentsize;
        if (pread(fd, p, sizeof(p), (off_t)off) != (ssize_t)sizeof(p)) return false;
        if (u32le(p) == 1) {
            load = true;
            if ((uint64_t)u32le(p + 4) + u32le(p + 16) > (uint64_t)size) return false;
        } else if (u32le(p) == 2 || u32le(p) == 3) {
            if ((uint64_t)u32le(p + 4) + u32le(p + 16) > (uint64_t)size) return false;
        }
    }
    return load;
}

static bool fd_regular_readable_executable(int fd, struct stat *st) {
    return fstat(fd, st) == 0 && S_ISREG(st->st_mode) && st->st_size > 0 &&
           (st->st_mode & 0444) != 0 && (st->st_mode & 0111) != 0 &&
           st->st_size <= ROCKBOX_MAX_BYTES && rockbox_elf_fd_valid(fd, st->st_size);
}

static bool rockbox_info_matches(int dirfd) {
    int fd = openat(dirfd, "rockbox-info.txt", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > ROCKBOX_INFO_MAX) {
        close(fd);
        return false;
    }
    char buf[ROCKBOX_INFO_MAX + 1];
    ssize_t n = read(fd, buf, (size_t)st.st_size);
    bool ok = n == st.st_size;
    if (close(fd) != 0) ok = false;
    if (!ok) return false;
    buf[n] = '\0';
    bool target = false;
    bool target_id = false;
    bool has_target_id = false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (strncmp(line, "Target:", 7) == 0) {
            char *value = line + 7;
            while (*value == ' ' || *value == '\t') value++;
            target = strcmp(value, RB_TARGET) == 0;
        } else if (strncmp(line, "Target id:", 10) == 0 || strncmp(line, "Target ID:", 10) == 0 ||
                   strncmp(line, "TargetID:", 9) == 0) {
            has_target_id = true;
            char *value = strchr(line, ':') + 1;
            while (*value == ' ' || *value == '\t') value++;
            target_id = strcmp(value, RB_TARGET_ID) == 0;
        }
    }
    /* Some official archives omit Target ID; if present it must agree. */
    return target && (!has_target_id || target_id);
}

static bool open_rockbox_source(int *dirfd_out, int *execfd_out, struct stat *execst_out) {
    struct stat rootst, dirst;
    int rootfd = open(COMPAS_RB_SD_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootfd < 0) return false;
    if (fstat(rootfd, &rootst) != 0) { close(rootfd); return false; }
    int dirfd = openat(rootfd, ".rockbox", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(rootfd);
    if (dirfd < 0 || fstat(dirfd, &dirst) != 0 || dirst.st_dev != rootst.st_dev) {
        if (dirfd >= 0) close(dirfd);
        return false;
    }
    if (!rockbox_info_matches(dirfd)) { close(dirfd); return false; }
    int fd = openat(dirfd, RB_SD_EXEC_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || !fd_regular_readable_executable(fd, execst_out) ||
        execst_out->st_dev != dirst.st_dev) {
        if (fd >= 0) close(fd);
        close(dirfd);
        return false;
    }
    *dirfd_out = dirfd;
    *execfd_out = fd;
    return true;
}

/* The open source descriptor can remain readable after an SD unmount or card
 * swap. Before publishing or reusing a staged binary, require the mounted SD
 * path to still resolve to the same Rockbox directory and executable inode. */
static bool source_path_still_current(int original_dirfd, int original_fd, const struct stat *original_file) {
    int current_dirfd = -1, current_fd = -1;
    struct stat original_dir, current_dir, original_now, current_file;
    bool same = open_rockbox_source(&current_dirfd, &current_fd, &current_file) &&
                fstat(original_dirfd, &original_dir) == 0 && fstat(current_dirfd, &current_dir) == 0 &&
                fstat(original_fd, &original_now) == 0 &&
                original_dir.st_dev == current_dir.st_dev && original_dir.st_ino == current_dir.st_ino &&
                same_source_snapshot(original_file, &original_now) &&
                same_source_snapshot(original_file, &current_file);
    if (current_fd >= 0) close(current_fd);
    if (current_dirfd >= 0) close(current_dirfd);
    return same;
}

/* Recognize only names this bridge can create, and only remove regular files.
 * The stage lock prevents this cleanup from racing another bridge instance. */
static bool is_bridge_temp_name(const char *name) {
    if (strncmp(name, ".rb-", 4) != 0) return false;
    const unsigned char *p = (const unsigned char *)name + 4;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '-') {
        p++;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') p++;
    }
    return strcmp((const char *)p, ".tmp") == 0;
}

static void remove_stale_bridge_temps(int stagefd) {
    int scanfd = dup(stagefd);
    if (scanfd < 0) return;
    DIR *dir = fdopendir(scanfd);
    if (!dir) { close(scanfd); return; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_bridge_temp_name(entry->d_name)) continue;
        struct stat st;
        if (fstatat(stagefd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode))
            (void)unlinkat(stagefd, entry->d_name, 0);
    }
    closedir(dir);
}
#endif

void compas_boot_prepare(void) {
    if (compas_loader_prepare()) fb_close();
}

void compas_boot_mount_sd(void) {
    mount_sd_card_if_needed();
}

bool compas_rockbox_available(void) {
#if RB_SUPPORTED
    int dirfd = -1, fd = -1;
    struct stat st;
    bool ok = open_rockbox_source(&dirfd, &fd, &st);
    if (fd >= 0) close(fd);
    if (dirfd >= 0) close(dirfd);
    return ok;
#else
    return false;
#endif
}

#if RB_SUPPORTED
static uint32_t crc_step(uint32_t crc, const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc;
}

static bool fd_crc32(int fd, off_t size, uint32_t *out_crc) {
    unsigned char buf[32768];
    uint64_t offset = 0;
    uint32_t crc = 0xFFFFFFFFu;
    while (offset < (uint64_t)size) {
        size_t want = sizeof(buf);
        if ((uint64_t)size - offset < want) want = (size_t)((uint64_t)size - offset);
        ssize_t n = pread(fd, buf, want, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        crc = crc_step(crc, buf, (size_t)n);
        offset += (uint64_t)n;
    }
    *out_crc = crc ^ 0xFFFFFFFFu;
    return true;
}
#endif

const char *compas_rockbox_stage(void) {
#if RB_SUPPORTED
    int dirfd = -1, srcfd = -1, tmpfd = -1, stage = -1, lockfd = -1;
    struct stat before, after, tmpst;
    char tmpname[32] = { 0 };
    bool ok = false;
    bool temp_owned = false;
    if (!open_rockbox_source(&dirfd, &srcfd, &before)) goto done;

    int parent = open(COMPAS_RB_DATA_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) goto done;
    if (mkdirat(parent, ".compas", 0755) != 0 && errno != EEXIST) { close(parent); goto done; }
    int compas = openat(parent, ".compas", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (compas < 0) { close(parent); goto done; }
    if (mkdirat(compas, "rockbox", 0755) != 0 && errno != EEXIST) {
        close(compas);
        close(parent);
        goto done;
    }
    stage = openat(compas, "rockbox", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stage < 0) {
        close(compas);
        close(parent);
        goto done;
    }
    if (fsync(compas) != 0 || fsync(parent) != 0) {
        close(stage);
        stage = -1;
        close(compas);
        close(parent);
        goto done;
    }
    close(compas);
    close(parent);
    lockfd = openat(stage, "stage.lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lockst;
    if (lockfd < 0 || fstat(lockfd, &lockst) != 0 || !S_ISREG(lockst.st_mode) || lockst.st_nlink != 1 ||
        fchmod(lockfd, 0600) != 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0 || fsync(stage) != 0)
        goto stage_done;
    remove_stale_bridge_temps(stage);
    if (fsync(stage) != 0) goto stage_done;

    uint32_t source_crc;
    if (!fd_crc32(srcfd, before.st_size, &source_crc)) goto stage_done;
    if (fstat(srcfd, &after) != 0 || !same_source_snapshot(&before, &after)) goto stage_done;

    /* Reuse a byte-identical, valid persistent copy to avoid writing the
     * internal flash on every boot. The source remains pinned and checked. */
    int existing = openat(stage, strrchr(RB_STAGE_EXEC, '/') + 1,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (existing >= 0) {
        struct stat existing_st, source_after;
        uint32_t existing_crc;
        bool reusable = fstat(existing, &existing_st) == 0 && S_ISREG(existing_st.st_mode) &&
                        (existing_st.st_mode & 0111) != 0 && existing_st.st_size == before.st_size &&
                        rockbox_elf_fd_valid(existing, existing_st.st_size) &&
                        fd_crc32(existing, existing_st.st_size, &existing_crc) && existing_crc == source_crc &&
                        fstat(srcfd, &source_after) == 0 && same_source_snapshot(&before, &source_after) &&
                        source_path_still_current(dirfd, srcfd, &before);
        close(existing);
        if (reusable) {
            ok = true;
            goto stage_done;
        }
    }
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        (void)snprintf(tmpname, sizeof(tmpname), ".rb-%ld-%u.tmp", (long)getpid(), attempt);
        tmpfd = openat(stage, tmpname, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (tmpfd >= 0 || errno != EEXIST) break;
    }
    if (tmpfd < 0) goto stage_done;
    temp_owned = true;
    unsigned char buf[32768];
    uint32_t crc = 0xFFFFFFFFu;
    uint64_t copied = 0;
    while (copied < (uint64_t)before.st_size) {
        size_t want = sizeof(buf);
        if ((uint64_t)before.st_size - copied < want) want = (size_t)((uint64_t)before.st_size - copied);
        ssize_t n = read(srcfd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto stage_done;
        crc = crc_step(crc, buf, (size_t)n);
        copied += (uint64_t)n;
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(tmpfd, buf + off, (size_t)n - off);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) goto stage_done;
            off += (size_t)w;
        }
    }
    uint32_t copied_crc = crc ^ 0xFFFFFFFFu;
    uint32_t copy_crc = 0xFFFFFFFFu;
    uint64_t checked = 0;
    if (lseek(tmpfd, 0, SEEK_SET) < 0) goto stage_done;
    for (;;) {
        ssize_t n = read(tmpfd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto stage_done;
        if (n == 0) break;
        copy_crc = crc_step(copy_crc, buf, (size_t)n);
        checked += (uint64_t)n;
    }
    copy_crc ^= 0xFFFFFFFFu;
    if (fstat(srcfd, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        !same_source_snapshot(&before, &after) || copied != (uint64_t)before.st_size ||
        checked != copied || copy_crc != copied_crc || copied_crc != source_crc ||
        fstat(tmpfd, &tmpst) != 0 || tmpst.st_size != before.st_size ||
        fchmod(tmpfd, 0755) != 0 || fsync(tmpfd) != 0 ||
        !rockbox_elf_fd_valid(tmpfd, tmpst.st_size) ||
        !source_path_still_current(dirfd, srcfd, &before))
        goto stage_done;
    if (close(tmpfd) != 0) { tmpfd = -1; goto stage_done; }
    tmpfd = -1;
    if (renameat(stage, tmpname, stage, strrchr(RB_STAGE_EXEC, '/') + 1) != 0 || fsync(stage) != 0) goto stage_done;
    ok = true;
stage_done:
    if (tmpfd >= 0) { close(tmpfd); tmpfd = -1; }
    if (!ok && temp_owned && tmpname[0]) unlinkat(stage, tmpname, 0);
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
    if (stage >= 0) close(stage);
done:
    if (srcfd >= 0) close(srcfd);
    if (dirfd >= 0) close(dirfd);
    return ok ? RB_STAGE_EXEC : NULL;
#else
    return NULL;
#endif
}

void compas_boot_start_compas(void) {
    char *const argv[] = { (char *)"/proc/self/exe", (char *)COMPAS_LOADER_ARG, NULL };
    execve("/proc/self/exe", argv, environ);
    fprintf(stderr, "compas boot: execve(/proc/self/exe --compas-loader) failed: %s\n", strerror(errno));
    compas_loader_run(true);
}

int compas_boot_run_bounded(char * const argv[], unsigned timeout_seconds) {
    if (!argv || !argv[0] || timeout_seconds == 0 || timeout_seconds > 60) return -1;
    int exit_code = -1;
    bool completed = subprocess_run_checked_group(argv, NULL, 0,
                                                   (int)(timeout_seconds * 1000u), &exit_code);
    return completed && exit_code == 0 ? 0 : -1;
}
