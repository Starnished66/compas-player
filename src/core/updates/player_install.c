#define _POSIX_C_SOURCE 200809L
#include "player_install.h"
#include "../../bootloader/scan/scanner.h"
#include "mbedtls/sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEMP_NAME ".compas_player.installing"
#define ARCHIVE_BASE "compas_player.previous"

/* This module is also linked into the tiny standalone loader. */
#ifdef COMPAS_LOADER_BUILD
#define ERR(s) (s)
#else
#include "../../ui/text/i18n/i18n.h"
#define ERR(s) TR(s)
#endif

const char *player_install_board_asset(void) {
#if defined(BOARD_R3PROII)
    return "compas_player_r3proii";
#elif defined(BOARD_R3II_2025)
    return "compas_player_r3ii_2025";
#else
    return "compas_player";
#endif
}

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool elf_valid(int fd, const struct stat *st) {
    unsigned char h[52], p[32];
    if (!S_ISREG(st->st_mode) || st->st_nlink != 1 || st->st_size < 84 ||
        st->st_size > PLAYER_INSTALL_MAX_BYTES || pread(fd, h, sizeof(h), 0) != (ssize_t)sizeof(h))
        return false;
    if (memcmp(h, "\177ELF", 4) || h[4] != 1 || h[5] != 1 || h[6] != 1 || u16(h + 16) != 2 ||
        u16(h + 18) != 8 || u32(h + 20) != 1)
        return false;
    uint32_t flags = u32(h + 36), abi = flags & 0xf000u, arch = flags & 0xf0000000u;
    if ((flags & 0x20u) || (abi && abi != 0x1000u) ||
        (arch && arch != 0x10000000u && arch != 0x50000000u && arch != 0x70000000u) ||
        (flags & 0x0f000000u))
        return false;
    uint32_t phoff = u32(h + 28), entry = u32(h + 24);
    uint16_t ents = u16(h + 42), count = u16(h + 44);
    if (ents < 32 || !count || (uint64_t)phoff + (uint64_t)ents * count > (uint64_t)st->st_size)
        return false;
    bool entry_ok = false;
    for (uint16_t i = 0; i < count; i++) {
        if (pread(fd, p, sizeof(p), (off_t)phoff + (off_t)i * ents) != (ssize_t)sizeof(p))
            return false;
        uint32_t type = u32(p), off = u32(p + 4), va = u32(p + 8), fs = u32(p + 16),
                 ms = u32(p + 20), fl = u32(p + 24);
        if (type == 3)
            return false;
        if (type != 1)
            continue;
        if (fs > ms || (uint64_t)off + fs > (uint64_t)st->st_size ||
            (uint64_t)va + ms > (uint64_t)UINT32_MAX + 1u)
            return false;
        if ((fl & 1u) && entry >= va && (uint64_t)entry < (uint64_t)va + ms)
            entry_ok = true;
    }
    return entry_ok;
}

bool player_install_sha256_fd(int fd, uint64_t max_size, uint64_t *size_out, char hex[65]) {
    if (fd < 0 || !size_out || !hex)
        return false;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    unsigned char b[16384], digest[32];
    uint64_t total = 0;
    while (ok) {
        ssize_t n = pread(fd, b, sizeof(b), (off_t)total);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 || (n == 0 && total == 0)) {
            ok = false;
            break;
        }
        if (!n)
            break;
        if ((uint64_t)n > max_size - total) {
            ok = false;
            break;
        }
        ok = mbedtls_sha256_update(&c, b, (size_t)n) == 0;
        total += (uint64_t)n;
    }
    if (ok)
        ok = mbedtls_sha256_finish(&c, digest) == 0;
    mbedtls_sha256_free(&c);
    if (!ok)
        return false;
    for (size_t i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    *size_out = total;
    return true;
}

static bool hash_text(const char *s) {
    if (!s || strlen(s) != 64)
        return false;
    for (int i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}
static bool same_stat(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_nlink == b->st_nlink && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static bool same_file_contents(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
           a->st_nlink == b->st_nlink && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* Marker strings are retained in every app binary so SD/manual installs can
 * be bound to this exact board. Online assets are separately authenticated. */
static bool board_marker_valid(int fd, uint64_t size, bool allow_missing) {
#if defined(BOARD_R3PROII)
    static const char own[] = "r3proii";
#elif defined(BOARD_R3II_2025)
    static const char own[] = "r3ii_2025";
#else
    static const char own[] = "r1";
#endif
    static const char prefix[] = "COMPAS_PLAYER_BOARD:";
    unsigned char b[8192];
    uint64_t off = 0;
    size_t carry = 0;
    char window[8192 + 64];
    bool found = false;
    while (off < size) {
        size_t want = sizeof(b);
        if (size - off < want)
            want = (size_t)(size - off);
        ssize_t n = pread(fd, b, want, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        memcpy(window + carry, b, (size_t)n);
        size_t total = carry + (size_t)n;
        for (size_t i = 0; i + sizeof(prefix) - 1 <= total; i++)
            if (memcmp(window + i, prefix, sizeof(prefix) - 1) == 0) {
                const char *suffix = window + i + sizeof(prefix) - 1;
                size_t left = total - (i + sizeof(prefix) - 1);
                if (left == 0)
                    continue;
                if (suffix[0] == '\0')
                    continue; /* the validator's own prefix constant */
                const char *end = memchr(suffix, '\0', left);
                if (!end)
                    continue; /* retain this partial token in carry for the next read */
                size_t token_size = (size_t)(end - suffix);
                bool recognized = (token_size == 2 && memcmp(suffix, "r1", 2) == 0) ||
                                  (token_size == 7 && memcmp(suffix, "r3proii", 7) == 0) ||
                                  (token_size == 9 && memcmp(suffix, "r3ii_2025", 9) == 0);
                if (!recognized)
                    continue;
                if (token_size != strlen(own) || memcmp(suffix, own, token_size) != 0)
                    return false;
                found = true;
            }
        carry = total < 63 ? total : 63;
        memmove(window, window + total - carry, carry);
        off += (uint64_t)n;
    }
    return found || allow_missing;
}

static const char *packaged_player_path(void) {
#ifdef HOST_BUILD
    return PLAYER_INSTALL_DATA_ROOT "/packaged_player";
#else
    return INTERNAL_PLAYER_PATH;
#endif
}

bool player_install_preflight_fd(int fd, bool allow_missing_board_marker, char *error,
                                 size_t error_size) {
    if (!error || !error_size)
        return false;
    error[0] = '\0';
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > PLAYER_INSTALL_MAX_BYTES || !elf_valid(fd, &st)) {
        snprintf(error, error_size, "%s", ERR("Player image validation failed (ELF mismatch)."));
        return false;
    }
    if (!board_marker_valid(fd, (uint64_t)st.st_size, allow_missing_board_marker)) {
        snprintf(error, error_size, "%s",
                 ERR("This player image is for a different device model or is missing its model "
                     "marker."));
        return false;
    }
    char source_path[64];
    int n = snprintf(source_path, sizeof(source_path), "/proc/self/fd/%d", fd);
    char source_stamp[BOOT_BUILD_STAMP_LEN + 1];
    char packaged_stamp[BOOT_BUILD_STAMP_LEN + 1];
    if (n > 0 && (size_t)n < sizeof(source_path) &&
        scanner_read_build_stamp(source_path, source_stamp, sizeof(source_stamp)) &&
        scanner_read_build_stamp(packaged_player_path(), packaged_stamp, sizeof(packaged_stamp)) &&
        strcmp(source_stamp, packaged_stamp) < 0) {
        snprintf(error, error_size, "%s", ERR("This player is older than the installed firmware."));
        return false;
    }
    return true;
}

static int open_parent(const char *path, char *leaf, size_t leafsz) {
    if (!path)
        return -1;
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1] || strlen(slash + 1) >= leafsz) {
        errno = EINVAL;
        return -1;
    }
    memcpy(leaf, slash + 1, strlen(slash + 1) + 1);
    size_t plen = (size_t)(slash - path);
    if (plen == 0)
        plen = 1;
    char parent[512];
    if (plen >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';
    return open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static bool archive_source(const char *path, const struct stat *snapshot, int *dir_out,
                           char archived[64], struct stat *archived_snapshot) {
    *dir_out = -1;
    if (!path)
        return true;
    char leaf[64];
    int dir = open_parent(path, leaf, sizeof(leaf));
    if (dir < 0)
        return false;
    struct stat current;
    if (fstatat(dir, leaf, &current, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(current.st_mode) ||
        current.st_nlink != 1 || !same_stat(&current, snapshot)) {
        close(dir);
        errno = ESTALE;
        return false;
    }
    char candidate[64];
    for (unsigned i = 0; i < 128; i++) {
        if (i == 0)
            snprintf(candidate, sizeof(candidate), "%s", ARCHIVE_BASE);
        else
            snprintf(candidate, sizeof(candidate), "%s.%u", ARCHIVE_BASE, i);
        struct stat dst;
        if (fstatat(dir, candidate, &dst, AT_SYMLINK_NOFOLLOW) == 0)
            continue;
        if (errno != ENOENT) {
            close(dir);
            return false;
        }
        if (renameat(dir, leaf, dir, candidate) == 0) {
            snprintf(archived, 64, "%s", candidate);
            struct stat moved;
            if (fstatat(dir, candidate, &moved, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(moved.st_mode) || moved.st_nlink != 1 ||
                !same_file_contents(snapshot, &moved)) {
                (void)renameat(dir, candidate, dir, leaf);
                close(dir);
                errno = ESTALE;
                return false;
            }
            if (fsync(dir) != 0) {
                (void)renameat(dir, candidate, dir, leaf);
                close(dir);
                return false;
            }
            if (archived_snapshot)
                *archived_snapshot = moved;
            *dir_out = dir;
            return true;
        }
        if (errno != EEXIST) {
            close(dir);
            return false;
        }
    }
    close(dir);
    errno = EEXIST;
    return false;
}

/* A legacy loader treats the canonical SD filename as an automatic update.
 * For an online install, archive it when a mounted card currently has one;
 * absence of a card is normal. Never follow a file or directory symlink. */
static bool archive_sd_if_present(const char *requested, const struct stat *snapshot, int *dir_out,
                                  char archived[64], struct stat *archived_snapshot) {
    if (requested)
        return archive_source(requested, snapshot, dir_out, archived, archived_snapshot);
    char leaf[64];
    int dir = open_parent(PLAYER_INSTALL_SD_SOURCE, leaf, sizeof(leaf));
    if (dir < 0)
        return errno == ENOENT || errno == ENODEV;
    struct stat st;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        int saved = errno;
        close(dir);
        return saved == ENOENT;
    }
    close(dir);
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
        errno = EPERM;
        return false;
    }
    return archive_source(PLAYER_INSTALL_SD_SOURCE, &st, dir_out, archived, archived_snapshot);
}

bool player_install_fd(int source_fd, uint64_t size, const char *expected_sha256,
                       bool allow_missing_board_marker, bool preflighted,
                       const char *sd_source_path, const struct stat *sd_snapshot, char *error,
                       size_t error_size) {
    if (!error || !error_size)
        return false;
    error[0] = '\0';
    if (source_fd < 0 || !size || size > PLAYER_INSTALL_MAX_BYTES ||
        (expected_sha256 && !hash_text(expected_sha256)) ||
        (!expected_sha256 && (!sd_source_path || !sd_snapshot || allow_missing_board_marker)) ||
        (preflighted && (!sd_source_path || !sd_snapshot || allow_missing_board_marker)) ||
        ((sd_source_path != NULL) != (sd_snapshot != NULL))) {
        snprintf(error, error_size, "%s", ERR("Invalid player image or checksum."));
        return false;
    }
    struct stat source_before;
    if (!preflighted &&
        !player_install_preflight_fd(source_fd, allow_missing_board_marker, error, error_size))
        return false;
    uint64_t source_size = 0;
    char digest[65];
    if (fstat(source_fd, &source_before) != 0 || source_before.st_size < 0 ||
        (uint64_t)source_before.st_size != size || !elf_valid(source_fd, &source_before) ||
        !player_install_sha256_fd(source_fd, PLAYER_INSTALL_MAX_BYTES, &source_size, digest) ||
        source_size != size || (expected_sha256 && strcmp(digest, expected_sha256)) ||
        ((sd_snapshot != NULL) && !same_stat(&source_before, sd_snapshot))) {
        snprintf(error, error_size, "%s",
                 ERR("Player image validation failed (ELF or SHA-256 mismatch)."));
        return false;
    }
    const char *expected = expected_sha256 ? expected_sha256 : digest;
    int data = open(PLAYER_INSTALL_DATA_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (data < 0) {
        snprintf(error, error_size, "%s", ERR("Internal storage is unavailable."));
        return false;
    }
    struct stat old;
    bool had_old = fstatat(data, "compas_player", &old, AT_SYMLINK_NOFOLLOW) == 0;
    if ((had_old && (!S_ISREG(old.st_mode) || old.st_nlink != 1)) ||
        (!had_old && errno != ENOENT)) {
        close(data);
        snprintf(error, error_size, "%s", ERR("Installed player path is unsafe."));
        return false;
    }
    struct stat tmpst;
    if (fstatat(data, TEMP_NAME, &tmpst, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(tmpst.st_mode) || tmpst.st_nlink != 1 || unlinkat(data, TEMP_NAME, 0) != 0) {
            close(data);
            snprintf(error, error_size, "%s", ERR("Player update temporary is unsafe."));
            return false;
        }
    } else if (errno != ENOENT) {
        close(data);
        snprintf(error, error_size, "%s", ERR("Cannot inspect player update storage."));
        return false;
    }
    int out = openat(data, TEMP_NAME, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) {
        close(data);
        snprintf(error, error_size, "%s", ERR("Could not prepare the player update."));
        return false;
    }
    bool ok = true;
    unsigned char buf[32768];
    uint64_t copied = 0;
    while (copied < size) {
        size_t want = sizeof(buf);
        if (size - copied < want)
            want = (size_t)(size - copied);
        ssize_t n = pread(source_fd, buf, want, (off_t)copied);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        size_t at = 0;
        while (at < (size_t)n) {
            ssize_t w = write(out, buf + at, (size_t)n - at);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            at += (size_t)w;
        }
        if (!ok)
            break;
        copied += (uint64_t)n;
    }
    struct stat outst;
    uint64_t copied_size = 0;
    char copied_sha[65];
    if (ok)
        ok = copied == size && fchmod(out, 0755) == 0 && fsync(out) == 0 &&
             fstat(out, &outst) == 0 && elf_valid(out, &outst) &&
             player_install_sha256_fd(out, PLAYER_INSTALL_MAX_BYTES, &copied_size, copied_sha) &&
             copied_size == size && !strcmp(copied_sha, expected);
    struct stat srcnow;
    if (ok)
        ok = fstat(source_fd, &srcnow) == 0 && same_stat(&source_before, &srcnow);
    if (close(out) != 0)
        ok = false;
    if (!ok) {
        unlinkat(data, TEMP_NAME, 0);
        close(data);
        snprintf(error, error_size, "%s", ERR("Player copy could not be verified."));
        return false;
    }
    int sddir = -1;
    char archived[64] = {0};
    struct stat archived_snapshot;
    if (!archive_sd_if_present(sd_source_path, sd_snapshot, &sddir, archived, &archived_snapshot)) {
        unlinkat(data, TEMP_NAME, 0);
        close(data);
        snprintf(error, error_size, "%s", ERR("Could not safely archive the SD player update."));
        return false;
    }
    struct stat now;
    bool exists = fstatat(data, "compas_player", &now, AT_SYMLINK_NOFOLLOW) == 0;
    ok = had_old ? (exists && S_ISREG(now.st_mode) && now.st_nlink == 1 &&
                    now.st_dev == old.st_dev && now.st_ino == old.st_ino)
                 : (!exists && errno == ENOENT);
    bool committed = ok && renameat(data, TEMP_NAME, data, "compas_player") == 0;
    bool durable = committed && fsync(data) == 0;
    if (committed && !durable)
        fprintf(stderr, "player_install: installed player; directory fsync failed\n");
    if (!committed) {
        unlinkat(data, TEMP_NAME, 0);
        if (sddir >= 0) {
            struct stat a, leaf;
            if (fstatat(sddir, archived, &a, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(a.st_mode) &&
                a.st_nlink == 1 &&
                fstatat(sddir, "compas_player", &leaf, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT)
                (void)renameat(sddir, archived, sddir, "compas_player");
            (void)fsync(sddir);
            close(sddir);
        }
        close(data);
        snprintf(
            error, error_size, "%s",
            ERR("Player update could not be committed; the previous player remains installed."));
        return false;
    }
    if (sddir >= 0 && durable) {
        struct stat archived_now;
        if (fstatat(sddir, archived, &archived_now, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(archived_now.st_mode) && archived_now.st_nlink == 1 &&
            same_stat(&archived_snapshot, &archived_now)) {
            if (unlinkat(sddir, archived, 0) == 0)
                (void)fsync(sddir);
        }
    }
    if (sddir >= 0)
        close(sddir);
    close(data);
    return true;
}
