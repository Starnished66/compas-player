#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "startup_splash.h"
#include "../../system/runtime/board_config.h"
#include "../../persistence/storage_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HOST_BUILD
#define SPLASH_DIR INTERNAL_COMPAS_DIR
#define SPLASH_NAME "startup.bin"
#else
#define SPLASH_NAME "startup.bin"
#endif

#define HEADER_SIZE 12u
#define MAX_IMAGE_SIZE (HEADER_SIZE + BOARD_SCREEN_WIDTH * BOARD_SCREEN_HEIGHT * 2u)

static uint16_t le16(const unsigned char *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static bool read_exact(int fd, void *buffer, size_t size) {
    unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = read(fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += (size_t)n;
    }
    return true;
}

static bool write_exact(int fd, const void *buffer, size_t size) {
    const unsigned char *bytes = buffer;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = write(fd, bytes + offset, size - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += (size_t)n;
    }
    return true;
}

static bool valid_header(const unsigned char h[HEADER_SIZE], int width, int height, size_t size) {
    if (width <= 0 || height <= 0 || width > BOARD_SCREEN_WIDTH || height > BOARD_SCREEN_HEIGHT) return false;
    uint16_t w = le16(h + 4), ht = le16(h + 6), stride = le16(h + 8);
    return h[0] == 0x19 && h[1] == 0x12 && le16(h + 2) == 0 && w == (uint16_t)width &&
           ht == (uint16_t)height && stride == (uint32_t)width * 2u && le16(h + 10) == 0 &&
           size == HEADER_SIZE + (size_t)stride * ht;
}

static int open_data_dir(bool create) {
#ifdef HOST_BUILD
    if (create && mkdir(SPLASH_DIR, 0755) != 0 && errno != EEXIST) return -1;
    return open(SPLASH_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
#else
    int data = open("/usr/data", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (data < 0) return -1;
    if (create && mkdirat(data, COMPAS_STORAGE_DIR, 0755) != 0 && errno != EEXIST) {
        close(data);
        return -1;
    }
    int dir = openat(data, COMPAS_STORAGE_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(data);
    return dir;
#endif
}

static bool load_fd(int fd, int width, int height, uint16_t *pixels) {
    struct stat st;
    unsigned char h[HEADER_SIZE];
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)HEADER_SIZE ||
        (uint64_t)st.st_size > MAX_IMAGE_SIZE) return false;
    size_t total = (size_t)st.st_size;
    if (!read_exact(fd, h, sizeof(h))) return false;
    if (!valid_header(h, width, height, total)) return false;
    if (!read_exact(fd, pixels, total - HEADER_SIZE)) return false;
    unsigned char extra;
    return read(fd, &extra, 1) == 0;
}

bool startup_splash_read(const char *path, int width, int height, uint16_t *pixels) {
    if (!path || !pixels) return false;
    int fd;
    if (strcmp(path, STARTUP_SPLASH_PATH) == 0) {
        int dir = open_data_dir(false);
        if (dir < 0) return false;
        fd = openat(dir, SPLASH_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        close(dir);
    } else {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    }
    if (fd < 0) return false;
    bool ok = load_fd(fd, width, height, pixels);
    close(fd);
    return ok;
}

bool startup_splash_configured(void) {
    int dir = open_data_dir(false);
    if (dir < 0) return false;
    int fd = openat(dir, SPLASH_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(dir);
    if (fd < 0) return false;
    /* Only header/size validation is required for UI availability. */
    struct stat st;
    unsigned char h[HEADER_SIZE];
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= HEADER_SIZE &&
              (uint64_t)st.st_size <= MAX_IMAGE_SIZE;
    if (ok) ok = read_exact(fd, h, sizeof(h));
    if (ok) ok = valid_header(h, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, (size_t)st.st_size);
    close(fd);
    return ok;
}

bool startup_splash_install(const char *source, int width, int height, const char **reason) {
    if (reason) *reason = "Unable to save startup image";
    int dir = open_data_dir(true);
    if (dir < 0) return false;
    if (!source) {
        struct stat st;
        if (fstatat(dir, SPLASH_NAME, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode)) {
            close(dir);
            return false;
        }
        bool ok = unlinkat(dir, SPLASH_NAME, 0) == 0 || errno == ENOENT;
        if (ok) ok = fsync(dir) == 0;
        close(dir);
        if (ok && reason) *reason = NULL;
        return ok;
    }
    int in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) { close(dir); if (reason) *reason = "Cannot open startup image"; return false; }
    struct stat st;
    unsigned char hdr[HEADER_SIZE];
    bool ok = fstat(in, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= HEADER_SIZE &&
              (uint64_t)st.st_size <= MAX_IMAGE_SIZE;
    if (ok) ok = read_exact(in, hdr, sizeof(hdr));
    ok = ok && valid_header(hdr, width, height, (size_t)st.st_size);
    if (!ok) {
        close(in); close(dir);
        if (reason) *reason = "Image must be an uncompressed RGB565 screen-sized LVGL image";
        return false;
    }
    const char *temp = ".startup.bin.tmp";
    if (unlinkat(dir, temp, 0) != 0 && errno != ENOENT) {
        close(in);
        close(dir);
        return false;
    }
    int out = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0) { close(in); close(dir); return false; }
    ok = write_exact(out, hdr, HEADER_SIZE);
    unsigned char buf[8192];
    uint64_t remaining = (uint64_t)st.st_size - HEADER_SIZE;
    while (ok && remaining) {
        size_t want = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        ssize_t n = read(in, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        ok = write_exact(out, buf, (size_t)n);
        remaining -= (uint64_t)n;
    }
    unsigned char extra;
    if (ok && read(in, &extra, 1) != 0) ok = false;
    if (ok) ok = fsync(out) == 0;
    if (close(out) != 0) ok = false;
    close(in);
    if (ok) ok = renameat(dir, temp, dir, SPLASH_NAME) == 0;
    if (ok) ok = fsync(dir) == 0;
    if (!ok) unlinkat(dir, temp, 0);
    close(dir);
    if (ok && reason) *reason = NULL;
    return ok;
}

void startup_splash_get_tint(bool *enabled, uint32_t *accent) {
    if (enabled) *enabled = false;
    if (accent) *accent = 0xB0142B; /* DEFAULT_ACCENT_COLOR */
    int dir = open_data_dir(false);
    if (dir < 0) return;
    const char *settings_name = strrchr(SETTINGS_FILE_PATH, '/');
    settings_name = settings_name ? settings_name + 1 : SETTINGS_FILE_PATH;
    int fd = openat(dir, settings_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(dir);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return;
    }
    char snapshot[65536];
    size_t used = 0;
    while (used < sizeof(snapshot)) {
        ssize_t n = read(fd, snapshot + used, sizeof(snapshot) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);

    bool tint = false;
    /* Settings predating setup belong to existing users, as in settings_load(). */
    bool setup_complete = true;
    uint32_t color = 0xB0142B; /* DEFAULT_ACCENT_COLOR */
    size_t start = 0;
    while (start < used) {
        size_t end = start;
        while (end < used && snapshot[end] != '\n') end++;
        if (end == used && used == sizeof(snapshot)) break;
        size_t line_end = end;
        if (line_end > start && snapshot[line_end - 1] == '\r') line_end--;
        size_t line_size = line_end - start;
        const char *line = snapshot + start;
        if (line_size >= 15 && !memcmp(line, "setup_complete=", 15)) {
            setup_complete = line_size == 16 && line[15] == '1';
        } else if (line_size >= 20 && !memcmp(line, "startup_splash_tint=", 20)) {
            const char *value = line + 20;
            size_t value_size = line_size - 20;
            tint = (value_size == 1 && value[0] == '1') ||
                   (value_size == 4 && memcmp(value, "true", 4) == 0);
        } else if (line_size == 19 && !memcmp(line, "accent_color=", 13)) {
            uint32_t parsed = 0;
            bool valid = true;
            for (size_t i = 13; i < line_size; i++) {
                unsigned char c = (unsigned char)line[i];
                unsigned digit;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                else { valid = false; break; }
                parsed = (parsed << 4) | digit;
            }
            if (valid) color = parsed;
        }
        start = end < used ? end + 1 : used;
    }
    if (enabled) *enabled = tint && setup_complete;
    if (accent) *accent = color;
}
